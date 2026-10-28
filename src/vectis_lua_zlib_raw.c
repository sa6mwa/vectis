#include "vectis_lua_zlib_raw.h"
#include "vectis_lua_buffer.h"
#include <lauxlib.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vectis/zlib.h>
#include <vectis/zlib_raw.h>
#define RAW "vectis.zlib.raw"
#define HEADER "vectis.zlib.gzip_header"
#define ALLOCATOR "vectis.zlib.allocator"
#define ALLOCATOR_MAP "vectis.zlib.allocator_map"
#define ALLOCATOR_PINS "vectis.zlib.allocator_pins"
#define ALLOCATOR_BUFFER_FACTORY "vectis.zlib.buffer_factory"
#define ALLOCATOR_OWNER_KEY 6
#define CRC_TABLE "vectis.zlib.crc_table"
typedef struct allocation {
  vectis_buffer *buffer;
  void *bytes;
  size_t size;
  struct allocation *next;
} allocation;
typedef struct allocator_context {
  lua_State *L;
  allocation *blocks;
  size_t owners;
  int active;
  int failed;
  int has_error;
  int custom;
} allocator_context;
typedef struct header_owner {
  vectis_zlib_gzip_header *receiver;
  vectis_buffer_span *temporary[3];
  int busy;
} header_owner;
typedef struct raw_owner {
  vectis_zlib_raw *receiver;
  int compress;
  int busy;
  int back_mode;
  vectis_buffer_span *back_input;
  vectis_buffer_span *temporary_window;
  int back_advanced;
  allocator_context *allocator;
  int allocator_attached;
} raw_owner;

static int integer(lua_State *L, int index) {
  lua_Integer value = luaL_checkinteger(L, index);
  if (value < INT_MIN || value > INT_MAX)
    luaL_argerror(L, index, "outside native int domain");
  return (int)value;
}
static const char *cstring(lua_State *L, int index) {
  size_t size;
  const char *value;
  luaL_checktype(L, index, LUA_TSTRING);
  value = lua_tolstring(L, index, &size);
  if (memchr(value, 0, size))
    luaL_argerror(L, index, "C string contains NUL");
  return value;
}
static void unsigned_value(lua_State *L, uint64_t value) {
  char text[3 * sizeof(uint64_t) + 1];
  size_t offset = sizeof(text);
  if (value <= (uint64_t)LUA_MAXINTEGER) {
    lua_pushinteger(L, (lua_Integer)value);
    return;
  }
  text[--offset] = '\0';
  do {
    text[--offset] = (char)('0' + value % 10u);
    value /= 10u;
  } while (value);
  lua_pushstring(L, text + offset);
}
static uint64_t unsigned_argument(lua_State *L, int index, uint64_t maximum) {
  uint64_t value = 0;
  const char *text;
  size_t length, i;
  unsigned digit;
  if (lua_istable(L, index)) {
    uint64_t high, low;
    index = lua_absindex(L, index);
    lua_pushliteral(L, "high");
    lua_rawget(L, index);
    if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 0 ||
        (uint64_t)lua_tointeger(L, -1) > UINT32_MAX)
      luaL_argerror(L, index, "high must be uint32");
    high = (uint64_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_pushliteral(L, "low");
    lua_rawget(L, index);
    if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 0 ||
        (uint64_t)lua_tointeger(L, -1) > UINT32_MAX)
      luaL_argerror(L, index, "low must be uint32");
    low = (uint64_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    value = (high << 32u) | low;
    if (value > maximum)
      luaL_argerror(L, index, "outside native unsigned domain");
    return value;
  }
  if (lua_isinteger(L, index)) {
    lua_Integer number = lua_tointeger(L, index);
    if (number < 0 || (uint64_t)number > maximum)
      luaL_argerror(L, index, "outside native unsigned domain");
    return (uint64_t)number;
  }
  luaL_checktype(L, index, LUA_TSTRING);
  text = lua_tolstring(L, index, &length);
  if (!length)
    luaL_argerror(L, index, "unsigned decimal size required");
  for (i = 0; i < length; ++i) {
    if (text[i] < '0' || text[i] > '9')
      luaL_argerror(L, index, "unsigned decimal size required");
    digit = (unsigned)(text[i] - '0');
    if (digit > maximum || value > (maximum - digit) / 10u)
      luaL_argerror(L, index, "outside native unsigned domain");
    value = value * 10u + digit;
  }
  return value;
}
static size_t size_argument(lua_State *L, int index) {
  return (size_t)unsigned_argument(L, index, (uint64_t)SIZE_MAX);
}
static void push_io(lua_State *L, raw_owner *owner, int input) {
  void *token = vectis_lua_buffer_push_scope(L);
  vectis_buffer_span *span = NULL;
  vectis_error error;
  if (vectis_zlib_raw_io(owner->receiver, input, &span, &error) != VECTIS_OK)
    luaL_error(L, "%s", error.message);
  if (!span) {
    lua_pop(L, 1);
    lua_pushnil(L);
    return;
  }
  vectis_lua_buffer_scope_adopt_span(token, span);
}
static int error_result(lua_State *L, const vectis_error *error) {
  lua_pushnil(L);
  lua_createtable(L, 0, 3);
  lua_pushinteger(L, error->code);
  lua_setfield(L, -2, "status");
  lua_pushinteger(L, error->dependency_code);
  lua_setfield(L, -2, "zlib_status");
  lua_pushstring(L, error->message);
  lua_setfield(L, -2, "message");
  return 2;
}
static int header_close(lua_State *L) {
  header_owner *owner = (header_owner *)luaL_testudata(L, 1, HEADER);
  int index;
  if (!owner)
    return 0;
  if (owner->busy)
    return luaL_error(L, "gzip header fields are active");
  vectis_zlib_gzip_header_close(owner->receiver);
  owner->receiver = NULL;
  for (index = 0; index < 3; ++index) {
    vectis_buffer_span_close(owner->temporary[index]);
    owner->temporary[index] = NULL;
  }
  return 0;
}
static void raw_field(lua_State *L, int table, const char *name) {
  table = lua_absindex(L, table);
  lua_pushstring(L, name);
  lua_rawget(L, table);
}
static int push_allocator(lua_State *L, allocator_context *context) {
  lua_getfield(L, LUA_REGISTRYINDEX, ALLOCATOR_PINS);
  if (lua_istable(L, -1)) {
    lua_pushlightuserdata(L, context);
    lua_rawget(L, -2);
    lua_remove(L, -2);
    if (lua_isuserdata(L, -1))
      return 1;
  }
  lua_pop(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, ALLOCATOR_MAP);
  lua_pushlightuserdata(L, context);
  lua_rawget(L, -2);
  lua_remove(L, -2);
  if (!lua_isuserdata(L, -1)) {
    lua_pop(L, 1);
    return 0;
  }
  return 1;
}
static void allocator_value(lua_State *L, allocator_context *context, int key) {
  if (!push_allocator(L, context)) {
    lua_pushnil(L);
    return;
  }
  lua_getiuservalue(L, -1, 1);
  lua_remove(L, -2);
  lua_rawgeti(L, -1, key);
  lua_remove(L, -2);
}
static void allocator_pin(lua_State *L, raw_owner *owner, int index, int pin) {
  if (!owner->allocator)
    return;
  index = lua_absindex(L, index);
  lua_getfield(L, LUA_REGISTRYINDEX, ALLOCATOR_PINS);
  lua_pushlightuserdata(L, owner->allocator);
  if (pin) {
    lua_getiuservalue(L, index, 1);
    lua_rawgeti(L, -1, ALLOCATOR_OWNER_KEY);
    lua_remove(L, -2);
  } else
    lua_pushboolean(L, 0);
  lua_rawset(L, -3);
  lua_pop(L, 1);
}
typedef struct allocator_frame {
  allocator_context *context;
  allocation *block;
  uInt items, size;
  size_t bytes;
  vectis_buffer *result;
  void *data;
  void *scope;
  int rooted;
} allocator_frame;
static int allocator_frame_driver(lua_State *L) {
  allocator_frame *frame = (allocator_frame *)lua_touserdata(L, 1);
  allocator_context *context = frame->context;
  vectis_error error;
  size_t capacity;
  if (!push_allocator(L, context))
    return luaL_error(L, "allocator context expired");
  lua_getiuservalue(L, -1, 1);
  lua_remove(L, -2);
  lua_rawgeti(L, -1, frame->block ? 2 : 1);
  if (frame->block) {
    frame->scope = vectis_lua_buffer_push_scope(L);
    lua_pushvalue(L, -1);
    lua_rawseti(L, 2, 4);
    frame->rooted = 1;
    if (vectis_lua_buffer_scope_init(frame->scope, frame->block->buffer,
                                     frame->block->size, 1,
                                     &error) != VECTIS_OK)
      return luaL_error(L, "%s", error.message);
  } else {
    unsigned_value(L, frame->items);
    unsigned_value(L, frame->size);
  }
  lua_rawgeti(L, 2, 3);
  lua_call(L, frame->block ? 2 : 3, 2);
  if (!lua_isnil(L, -1))
    return lua_error(L);
  if (frame->block) {
    if (!lua_isnil(L, -2) && !(lua_isboolean(L, -2) && lua_toboolean(L, -2)) &&
        !(lua_isinteger(L, -2) && lua_tointeger(L, -2) == 0))
      return luaL_error(L, "allocator free rejected cleanup");
    return 0;
  }
  if (lua_isnil(L, -2)) {
    context->failed = 1;
    return 0;
  }
  if (vectis_lua_buffer_owned_info(L, -2, frame->bytes, &frame->data, &capacity,
                                   &error) != VECTIS_OK)
    return luaL_error(L, "%s", error.message);
  if (vectis_lua_buffer_take(L, -2, frame->bytes, &frame->result, &error) !=
      VECTIS_OK)
    return luaL_error(L, "%s", error.message);
  return 0;
}
static int allocator_protect(allocator_frame *frame) {
  allocator_context *context = frame->context;
  lua_State *L = context->L;
  int top = lua_gettop(L), status;
  ++context->active;
  if (!lua_checkstack(L, 10)) {
    context->failed = 1;
    --context->active;
    return 0;
  }
  lua_pushcfunction(L, allocator_frame_driver);
  lua_pushlightuserdata(L, frame);
  status = lua_pcall(L, 1, 0, 0);
  if (frame->rooted)
    vectis_lua_buffer_scope_close(frame->scope);
  if (status != LUA_OK) {
    if (!context->has_error && push_allocator(L, context)) {
      lua_getiuservalue(L, -1, 1);
      lua_pushvalue(L, top + 1);
      lua_rawseti(L, -2, 5);
      lua_pop(L, 2);
    }
    context->failed = 1;
    context->has_error = 1;
  }
  lua_settop(L, top);
  --context->active;
  return status == LUA_OK;
}
static voidpf allocator_alloc(voidpf user, uInt items, uInt size) {
  allocator_context *context = (allocator_context *)user;
  allocator_frame frame;
  allocation *block;
  if (context->failed || (size && (size_t)items > SIZE_MAX / (size_t)size)) {
    context->failed = 1;
    return Z_NULL;
  }
  block = (allocation *)calloc(1, sizeof(*block));
  if (!block) {
    context->failed = 1;
    return Z_NULL;
  }
  memset(&frame, 0, sizeof(frame));
  frame.context = context;
  frame.items = items;
  frame.size = size;
  frame.bytes = (size_t)items * (size_t)size;
  if (!frame.bytes)
    frame.bytes = 1;
  if (!allocator_protect(&frame) || !frame.result) {
    vectis_buffer_close(frame.result);
    free(block);
    return Z_NULL;
  }
  block->buffer = frame.result;
  block->bytes = frame.data;
  block->size = frame.bytes;
  block->next = context->blocks;
  context->blocks = block;
  return frame.data;
}
static void allocator_free(voidpf user, voidpf bytes) {
  allocator_context *context = (allocator_context *)user;
  allocation **entry = &context->blocks, *block;
  allocator_frame frame;
  if (!bytes)
    return;
  while (*entry && (*entry)->bytes != bytes)
    entry = &(*entry)->next;
  block = *entry;
  if (!block) {
    context->failed = 1;
    return;
  }
  memset(&frame, 0, sizeof(frame));
  frame.context = context;
  frame.block = block;
  (void)allocator_protect(&frame);
  *entry = block->next;
  vectis_buffer_close(block->buffer);
  free(block);
}
static int allocator_gc(lua_State *L) {
  allocator_context *context =
      (allocator_context *)luaL_testudata(L, 1, ALLOCATOR);
  if (!context || context->owners)
    return 0;
  while (context->blocks) {
    allocation *block = context->blocks;
    context->blocks = block->next;
    vectis_buffer_close(block->buffer);
    free(block);
  }
  lua_getfield(L, LUA_REGISTRYINDEX, ALLOCATOR_PINS);
  lua_pushlightuserdata(L, context);
  lua_pushnil(L);
  lua_rawset(L, -3);
  lua_pop(L, 1);
  return 0;
}
static allocator_context *allocator_create(lua_State *L, int argument) {
  int input, top = lua_gettop(L), has_alloc, has_free;
  allocator_context *context;
  if (lua_isnoneornil(L, argument))
    return NULL;
  luaL_checktype(L, argument, LUA_TTABLE);
  input = lua_absindex(L, argument);
  raw_field(L, input, "zalloc");
  has_alloc = !lua_isnil(L, -1);
  if (has_alloc)
    luaL_checktype(L, -1, LUA_TFUNCTION);
  lua_pop(L, 1);
  raw_field(L, input, "zfree");
  has_free = !lua_isnil(L, -1);
  if (has_free)
    luaL_checktype(L, -1, LUA_TFUNCTION);
  lua_pop(L, 1);
  if (has_alloc != has_free) {
    luaL_error(L, "zalloc/zfree must form a pair");
    return NULL;
  }
  context = (allocator_context *)lua_newuserdatauv(L, sizeof(*context), 1);
  memset(context, 0, sizeof(*context));
  context->L = L;
  context->custom = has_alloc;
  luaL_setmetatable(L, ALLOCATOR);
  lua_createtable(L, 5, 0);
  raw_field(L, input, "zalloc");
  lua_rawseti(L, -2, 1);
  raw_field(L, input, "zfree");
  lua_rawseti(L, -2, 2);
  raw_field(L, input, "opaque");
  lua_rawseti(L, -2, 3);
  lua_pushboolean(L, 0);
  lua_rawseti(L, -2, 4);
  lua_pushboolean(L, 0);
  lua_rawseti(L, -2, 5);
  lua_setiuservalue(L, -2, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, ALLOCATOR_MAP);
  lua_pushlightuserdata(L, context);
  lua_pushvalue(L, top + 1);
  lua_rawset(L, -3);
  lua_pop(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, ALLOCATOR_PINS);
  lua_pushlightuserdata(L, context);
  lua_pushboolean(L, 0);
  lua_rawset(L, -3);
  lua_pop(L, 1);
  return context;
}
static void allocator_attach(lua_State *L, raw_owner *owner, int owner_index,
                             int context_index) {
  if (!owner->allocator)
    return;
  owner_index = lua_absindex(L, owner_index);
  context_index = lua_absindex(L, context_index);
  lua_getiuservalue(L, owner_index, 1);
  lua_pushvalue(L, context_index);
  lua_rawseti(L, -2, ALLOCATOR_OWNER_KEY);
  lua_pop(L, 1);
}
static int allocator_take_error(lua_State *L, allocator_context *context) {
  if (!context || !context->failed)
    return 0;
  context->failed = 0;
  if (!context->has_error)
    return 0;
  context->has_error = 0;
  if (!push_allocator(L, context)) {
    lua_pushnil(L);
    return 1;
  }
  lua_getiuservalue(L, -1, 1);
  lua_rawgeti(L, -1, 5);
  lua_pushboolean(L, 0);
  lua_rawseti(L, -3, 5);
  lua_remove(L, -2);
  lua_remove(L, -2);
  return 1;
}
static int allocator_raise(lua_State *L, raw_owner *owner) {
  return allocator_take_error(L, owner->allocator) ? lua_error(L) : 0;
}
static void allocator_detach(lua_State *L, raw_owner *owner, int index) {
  if (!owner->allocator)
    return;
  if (owner->allocator_attached) {
    --owner->allocator->owners;
    owner->allocator_attached = 0;
  }
  lua_getiuservalue(L, index, 1);
  lua_pushnil(L);
  lua_rawseti(L, -2, ALLOCATOR_OWNER_KEY);
  lua_pop(L, 1);
  owner->allocator = NULL;
}

static int header_create_driver(lua_State *L) {
  static const char *const names[3] = {"extra", "name", "comment"};
  vectis_zlib_gzip_header_options options;
  header_owner *owner;
  vectis_error error;
  vectis_status status;
  size_t size, value;
  int index, read;
  int has_length, has_max[3];
  owner = (header_owner *)luaL_checkudata(L, 1, HEADER);
  luaL_checktype(L, 2, LUA_TTABLE);
  read = 0;
  if (!lua_isnoneornil(L, 3)) {
    luaL_checktype(L, 3, LUA_TBOOLEAN);
    read = lua_toboolean(L, 3);
  }
  vectis_zlib_gzip_header_options_init(&options);
  options.read_mode = read;
#define HEADER_INT(name)                                                       \
  do {                                                                         \
    raw_field(L, 2, #name);                                                    \
    if (!lua_isnil(L, -1))                                                     \
      options.fields.name = integer(L, -1);                                    \
    lua_pop(L, 1);                                                             \
  } while (0)
#define HEADER_UINT(name, present)                                             \
  do {                                                                         \
    raw_field(L, 2, #name);                                                    \
    present = !lua_isnil(L, -1);                                               \
    if (present) {                                                             \
      value = size_argument(L, -1);                                            \
      if (value > UINT_MAX)                                                    \
        luaL_error(L, "gzip " #name " exceeds uInt");                          \
      options.fields.name = (uInt)value;                                       \
    }                                                                          \
    lua_pop(L, 1);                                                             \
  } while (0)
  HEADER_INT(text);
  HEADER_INT(xflags);
  HEADER_INT(os);
  HEADER_INT(hcrc);
  HEADER_INT(done);
  raw_field(L, 2, "time");
  if (!lua_isnil(L, -1)) {
    value = size_argument(L, -1);
    if ((uint64_t)value > (uint64_t)ULONG_MAX)
      luaL_error(L, "gzip time exceeds uLong");
    options.fields.time = (uLong)value;
  }
  lua_pop(L, 1);
  HEADER_UINT(extra_len, has_length);
  HEADER_UINT(extra_max, has_max[0]);
  HEADER_UINT(name_max, has_max[1]);
  HEADER_UINT(comm_max, has_max[2]);
#undef HEADER_INT
#undef HEADER_UINT

  for (index = 0; index < 3; ++index) {
    raw_field(L, 2, names[index]);
    if (!lua_isnil(L, -1)) {
      status = vectis_lua_buffer_acquire_span(L, -1, !read,
                                              &owner->temporary[index], &error);
      if (status != VECTIS_OK) {
        lua_pop(L, 1);
        return error_result(L, &error);
      }
      status = vectis_buffer_span_size(owner->temporary[index], &size, &error);
      if (status != VECTIS_OK) {
        lua_pop(L, 1);
        return error_result(L, &error);
      }
      if (size > UINT_MAX &&
          ((read && !has_max[index]) || (!read && index == 0 && !has_length)))
        luaL_error(L, "gzip default byte size exceeds uInt");
      if (read && !has_max[index]) {
        if (index == 0)
          options.fields.extra_max = (uInt)size;
        else if (index == 1)
          options.fields.name_max = (uInt)size;
        else
          options.fields.comm_max = (uInt)size;
      }
      if (!read && index == 0 && !has_length)
        options.fields.extra_len = (uInt)size;
    }
    lua_pop(L, 1);
  }
  options.extra = owner->temporary[0];
  options.name = owner->temporary[1];
  options.comment = owner->temporary[2];
  status = vectis_zlib_gzip_header_new(&options, &owner->receiver, &error);
  for (index = 0; index < 3; ++index) {
    vectis_buffer_span_close(owner->temporary[index]);
    owner->temporary[index] = NULL;
  }
  if (status != VECTIS_OK)
    return error_result(L, &error);
  lua_pushvalue(L, 1);
  return 1;
}
static int header_create(lua_State *L) {
  header_owner *owner;
  int status, index;
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 2);
  owner = (header_owner *)lua_newuserdatauv(L, sizeof(*owner), 0);
  memset(owner, 0, sizeof(*owner));
  luaL_setmetatable(L, HEADER);
  lua_pushcfunction(L, header_create_driver);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  status = lua_pcall(L, 3, 2, 0);
  for (index = 0; index < 3; ++index) {
    vectis_buffer_span_close(owner->temporary[index]);
    owner->temporary[index] = NULL;
  }
  if (status != LUA_OK) {
    vectis_zlib_gzip_header_close(owner->receiver);
    owner->receiver = NULL;
    return lua_error(L);
  }
  return 2;
}
static int header_fields_driver(lua_State *L) {
  header_owner *owner = (header_owner *)luaL_checkudata(L, 1, HEADER);
  gz_headerp fields;
  vectis_error error;
  vectis_buffer_span *span;
  void *token;
  int index;
  static const char *const names[3] = {"extra", "name", "comment"};
  if (vectis_zlib_gzip_header_native(owner->receiver, &fields, &error) !=
      VECTIS_OK)
    return error_result(L, &error);
  lua_createtable(L, 0, 13);
#define HEADER_VALUE(name)                                                     \
  do {                                                                         \
    lua_pushinteger(L, fields->name);                                          \
    lua_setfield(L, -2, #name);                                                \
  } while (0)
  HEADER_VALUE(text);
  HEADER_VALUE(xflags);
  HEADER_VALUE(os);
  HEADER_VALUE(hcrc);
  HEADER_VALUE(done);
  unsigned_value(L, fields->time);
  lua_setfield(L, -2, "time");
  unsigned_value(L, fields->extra_len);
  lua_setfield(L, -2, "extra_len");
  unsigned_value(L, fields->extra_max);
  lua_setfield(L, -2, "extra_max");
  unsigned_value(L, fields->name_max);
  lua_setfield(L, -2, "name_max");
  unsigned_value(L, fields->comm_max);
  lua_setfield(L, -2, "comm_max");
#undef HEADER_VALUE
  for (index = 0; index < 3; ++index) {
    token = vectis_lua_buffer_push_scope(L);
    span = NULL;
    if (vectis_zlib_gzip_header_io(owner->receiver, index, &span, &error) !=
        VECTIS_OK)
      return luaL_error(L, "%s", error.message);
    if (span)
      vectis_lua_buffer_scope_adopt_span(token, span);
    else {
      lua_pop(L, 1);
      lua_pushnil(L);
    }
    lua_setfield(L, -2, names[index]);
  }
  return 1;
}
static int header_fields(lua_State *L) {
  header_owner *owner = (header_owner *)luaL_checkudata(L, 1, HEADER);
  int status;
  if (!owner->receiver || owner->busy)
    return luaL_error(L, "gzip header is closed or active");
  owner->busy = 1;
  lua_pushcfunction(L, header_fields_driver);
  lua_pushvalue(L, 1);
  status = lua_pcall(L, 1, 2, 0);
  owner->busy = 0;
  if (status != LUA_OK)
    return lua_error(L);
  return 2;
}
static raw_owner *userdata(lua_State *L, int index) {
  return (raw_owner *)luaL_checkudata(L, index, RAW);
}
static raw_owner *live(lua_State *L) {
  raw_owner *owner = userdata(L, 1);
  if (owner->allocator && owner->allocator->active)
    luaL_error(L, "shared zlib allocator callback is active");
  if (owner->busy)
    luaL_error(L, "raw zlib owner is active");
  if (!owner->receiver)
    luaL_error(L, "raw zlib owner is closed");
  if (owner->allocator)
    owner->allocator->L = L;
  return owner;
}
static z_streamp native(lua_State *L, raw_owner *owner) {
  z_streamp stream;
  vectis_error error;
  if (owner->receiver->native(owner->receiver, &stream, &error) != VECTIS_OK)
    luaL_error(L, "%s", error.message);
  return stream;
}
static int close_owner(lua_State *L) {
  raw_owner *owner = (raw_owner *)luaL_testudata(L, 1, RAW);
  vectis_error error;
  int has_error;
  if (!owner)
    return 0;
  if (owner->busy)
    return luaL_error(L, "cannot close an active raw zlib owner");
  if (owner->allocator && owner->allocator->active)
    return luaL_error(L, "cannot close from shared allocator callback");
  if (owner->allocator)
    owner->allocator->L = L;
  allocator_pin(L, owner, 1, 1);
  owner->busy = 1;
  if (vectis_zlib_raw_close(owner->receiver, &error) != VECTIS_OK) {
    owner->busy = 0;
    allocator_pin(L, owner, 1, 0);
    return luaL_error(L, "%s", error.message);
  }
  owner->busy = 0;
  owner->receiver = NULL;
  vectis_buffer_span_close(owner->back_input);
  owner->back_input = NULL;
  vectis_buffer_span_close(owner->temporary_window);
  owner->temporary_window = NULL;
  lua_getiuservalue(L, 1, 1);
  if (lua_istable(L, -1)) {
    int key;
    for (key = 1; key <= 5; ++key) {
      lua_pushnil(L);
      lua_rawseti(L, -2, key);
    }
  }
  lua_pop(L, 1);
  allocator_pin(L, owner, 1, 0);
  has_error = allocator_take_error(L, owner->allocator);
  allocator_detach(L, owner, 1);
  if (has_error)
    return lua_error(L);
  return 0;
}
static raw_owner *new_owner(lua_State *L, int compress) {
  raw_owner *owner = (raw_owner *)lua_newuserdatauv(L, sizeof(*owner), 1);
  memset(owner, 0, sizeof(*owner));
  owner->compress = compress;
  luaL_setmetatable(L, RAW);
  lua_newtable(L);
  lua_setiuservalue(L, -2, 1);
  return owner;
}
static int default_alloc(lua_State *L) {
  size_t items = size_argument(L, 1), size = size_argument(L, 2), bytes;
  if (items > UINT_MAX || size > UINT_MAX || (size && items > SIZE_MAX / size))
    return luaL_error(L, "zalloc product outside native domain");
  bytes = items * size;
  if (!bytes)
    bytes = 1;
  lua_getfield(L, LUA_REGISTRYINDEX, ALLOCATOR_BUFFER_FACTORY);
  unsigned_value(L, bytes);
  lua_call(L, 1, 2);
  return 2;
}
static int default_free(lua_State *L) {
  vectis_error error;
  if (lua_isnoneornil(L, 1))
    return 0;
  if (vectis_lua_buffer_close_resource(L, 1, &error) != VECTIS_OK)
    return luaL_error(L, "%s", error.message);
  return 0;
}
static void raw_abort(lua_State *L, raw_owner *owner, int index) {
  int top = lua_gettop(L);
  owner->busy = 1;
  allocator_pin(L, owner, index, 1);
  (void)vectis_zlib_raw_close(owner->receiver, NULL);
  owner->receiver = NULL;
  owner->busy = 0;
  allocator_pin(L, owner, index, 0);
  allocator_detach(L, owner, index);
  vectis_buffer_span_close(owner->back_input);
  owner->back_input = NULL;
  lua_settop(L, top);
}

static int create(lua_State *L) {
  int kind = (int)lua_tointeger(L, lua_upvalueindex(1)), version_index;
  vectis_zlib_raw_options options;
  vectis_error error;
  raw_owner *owner;
  allocator_context *context;
  int context_index, allocator_index;
  vectis_zlib_raw_options_init(&options);
  options.compress = (kind & 1) != 0;
  options.extended = (kind & 2) != 0;
  if (options.compress) {
    options.level =
        lua_isnoneornil(L, 1) ? Z_DEFAULT_COMPRESSION : integer(L, 1);
    if (options.extended) {
      options.method = integer(L, 2);
      options.window_bits = integer(L, 3);
      options.memory_level = integer(L, 4);
      options.strategy = integer(L, 5);
    }
    version_index = options.extended ? 6 : 2;
  } else {
    if (options.extended)
      options.window_bits = integer(L, 1);
    version_index = options.extended ? 2 : 1;
  }
  if (kind & 4) {
    options.version = cstring(L, version_index);
    options.stream_size = integer(L, version_index + 1);
  }
  allocator_index = version_index + ((kind & 4) ? 2 : 0);
  context = allocator_create(L, allocator_index);
  context_index = context ? lua_gettop(L) : 0;
  owner = new_owner(L, options.compress);
  owner->allocator = context;
  if (context) {
    allocator_attach(L, owner, -1, context_index);
    if (context->custom) {
      options.alloc = allocator_alloc;
      options.free = allocator_free;
    }
    options.opaque = context;
  }
  owner->busy = 1;
  {
    vectis_status status =
        vectis_zlib_raw_new(&options, &owner->receiver, &error);
    owner->busy = 0;
    if (status == VECTIS_OK && context) {
      ++context->owners;
      owner->allocator_attached = 1;
    }
    if (context && context->failed)
      allocator_raise(L, owner);
    if (status != VECTIS_OK)
      return error_result(L, &error);
  }
  return 1;
}
static int back_create(lua_State *L) {
  vectis_zlib_raw_options options;
  vectis_error error;
  vectis_status status;
  raw_owner *owner;
  vectis_buffer *buffer = NULL;
  int bits = integer(L, 1), context_index;
  allocator_context *context;
  vectis_zlib_raw_options_init(&options);
  options.compress = 0;
  options.window_bits = bits;
  if (lua_toboolean(L, lua_upvalueindex(1))) {
    options.version = cstring(L, 3);
    options.stream_size = integer(L, 4);
  }
  context = allocator_create(L, lua_toboolean(L, lua_upvalueindex(1)) ? 5 : 3);
  context_index = context ? lua_gettop(L) : 0;
  owner = new_owner(L, 0);
  owner->allocator = context;
  if (context) {
    allocator_attach(L, owner, -1, context_index);
    if (context->custom) {
      options.alloc = allocator_alloc;
      options.free = allocator_free;
    }
    options.opaque = context;
  }
  owner->back_mode = 1;
  status =
      vectis_lua_buffer_acquire_span(L, 2, 0, &owner->temporary_window, &error);
  if (status == VECTIS_OK)
    status = vectis_buffer_new(VECTIS_ZLIB_CHUNK_BYTES, &buffer, &error);
  if (status == VECTIS_OK)
    status = vectis_buffer_borrow(buffer, 0, VECTIS_ZLIB_CHUNK_BYTES, 0,
                                  &owner->back_input, &error);
  vectis_buffer_close(buffer);
  owner->busy = 1;
  if (status == VECTIS_OK)
    status = vectis_zlib_raw_back_new(&options, owner->temporary_window,
                                      &owner->receiver, &error);
  owner->busy = 0;
  if (status == VECTIS_OK && context) {
    ++context->owners;
    owner->allocator_attached = 1;
  }
  vectis_buffer_span_close(owner->temporary_window);
  owner->temporary_window = NULL;
  if (status != VECTIS_OK) {
    vectis_buffer_span_close(owner->back_input);
    owner->back_input = NULL;
    allocator_raise(L, owner);
    return error_result(L, &error);
  }
  return 1;
}
typedef struct back_frame {
  lua_State *L;
  raw_owner *owner;
  int failed;
  int has_error;
  int input_called;
  unsigned count;
  unsigned char *data;
  size_t size;
} back_frame;
static void back_owned_value(lua_State *L, int key) {
  lua_getiuservalue(L, 1, 1);
  lua_rawgeti(L, -1, key);
  lua_remove(L, -2);
}
static int back_read_frame(lua_State *L) {
  back_frame *frame = (back_frame *)lua_touserdata(L, 2);
  const char *data;
  size_t size;
  back_owned_value(L, 1);
  lua_pushinteger(L, VECTIS_ZLIB_CHUNK_BYTES);
  back_owned_value(L, 2);
  lua_call(L, 2, 2);
  if (!lua_isnil(L, -1))
    return lua_error(L);
  if (lua_isnil(L, -2))
    return 0;
  luaL_checktype(L, -2, LUA_TSTRING);
  data = lua_tolstring(L, -2, &size);
  if (!size || size > VECTIS_ZLIB_CHUNK_BYTES)
    return luaL_error(L, "back reader must return 1..65536 bytes or nil");
  memcpy(frame->data, data, size);
  frame->count = (unsigned)size;
  return 0;
}
static int back_write_frame(lua_State *L) {
  back_frame *frame = (back_frame *)lua_touserdata(L, 2);
  back_owned_value(L, 3);
  lua_pushlstring(L, (const char *)frame->data, frame->size);
  back_owned_value(L, 4);
  lua_call(L, 2, 2);
  if (!lua_isnil(L, -1))
    return lua_error(L);
  if (!lua_isnil(L, -2) && !(lua_isboolean(L, -2) && lua_toboolean(L, -2)) &&
      !(lua_isinteger(L, -2) && lua_tointeger(L, -2) == 0))
    return luaL_error(L, "back writer rejected output");
  return 0;
}
static int back_protect(back_frame *frame, lua_CFunction function) {
  lua_State *L = frame->L;
  int top = lua_gettop(L), status;
  if (frame->failed)
    return 0;
  if (!lua_checkstack(L, 10)) {
    frame->failed = 1;
    return 0;
  }
  lua_pushcfunction(L, function);
  lua_pushvalue(L, 1);
  lua_pushlightuserdata(L, frame);
  status = lua_pcall(L, 2, 0, 0);
  if (status != LUA_OK) {
    lua_getiuservalue(L, 1, 1);
    lua_pushvalue(L, -2);
    lua_rawseti(L, -2, 5);
    lua_pop(L, 1);
    frame->failed = 1;
    frame->has_error = 1;
  }
  lua_settop(L, top);
  return status == LUA_OK;
}
static unsigned back_lua_input(void *user, z_const unsigned char **out) {
  back_frame *frame = (back_frame *)user;
  void *data;
  size_t size;
  vectis_error error;
  frame->input_called = 1;
  frame->count = 0;
  if (vectis_buffer_span_data_mutable(frame->owner->back_input, &data, &size,
                                      &error) != VECTIS_OK) {
    frame->failed = 1;
    *out = NULL;
    return 0;
  }
  frame->data = (unsigned char *)data;
  if (!back_protect(frame, back_read_frame)) {
    *out = NULL;
    return 0;
  }
  memcpy(out, &data, sizeof(data));
  return frame->count;
}
static int back_lua_output(void *user, unsigned char *data, unsigned size) {
  back_frame *frame = (back_frame *)user;
  frame->data = data;
  frame->size = size;
  return back_protect(frame, back_write_frame) ? 0 : 1;
}
static void back_capture(lua_State *L, int argument, int key,
                         const char *name) {
  int owner = 1, callback, user;
  argument = lua_absindex(L, argument);
  if (lua_isfunction(L, argument)) {
    lua_pushvalue(L, argument);
    lua_pushnil(L);
  } else {
    luaL_checktype(L, argument, LUA_TTABLE);
    raw_field(L, argument, name);
    luaL_checktype(L, -1, LUA_TFUNCTION);
    raw_field(L, argument, "userdata");
  }
  callback = lua_absindex(L, -2);
  user = lua_absindex(L, -1);
  lua_getiuservalue(L, owner, 1);
  lua_pushvalue(L, callback);
  lua_rawseti(L, -2, key);
  lua_pushvalue(L, user);
  lua_rawseti(L, -2, key + 1);
  lua_pop(L, 3);
}
static int back_driver(lua_State *L) {
  raw_owner *owner = userdata(L, 1);
  back_frame frame;
  vectis_zlib_back_result result;
  vectis_error error;
  vectis_status status;
  z_streamp stream;
  void *base;
  size_t capacity, offset = 0;
  memset(&frame, 0, sizeof(frame));
  frame.L = L;
  frame.owner = owner;
  owner->back_advanced = 1;
  status = vectis_zlib_raw_back(owner->receiver, back_lua_input, &frame,
                                back_lua_output, &frame, &result, &error);
  if (status != VECTIS_OK)
    return error_result(L, &error);
  stream = native(L, owner);
  if (frame.input_called) {
    if (stream->next_in) {
      if (vectis_buffer_span_data_mutable(owner->back_input, &base, &capacity,
                                          &error) != VECTIS_OK)
        return error_result(L, &error);
      if ((uintptr_t)stream->next_in < (uintptr_t)base ||
          (uintptr_t)stream->next_in - (uintptr_t)base > capacity)
        return luaL_error(L, "back native input left bounded storage");
      offset = (size_t)((uintptr_t)stream->next_in - (uintptr_t)base);
      status = vectis_zlib_raw_bind(owner->receiver, 1, owner->back_input,
                                    offset, stream->avail_in, &error);
    } else
      status = vectis_zlib_raw_bind(owner->receiver, 1, NULL, 0, 0, &error);
    if (status != VECTIS_OK)
      return error_result(L, &error);
  }
  if (frame.failed) {
    if (!frame.has_error)
      return luaL_error(L, "back callback failed");
    back_owned_value(L, 5);
    return lua_error(L);
  }
  allocator_raise(L, owner);
  lua_createtable(L, 0, 6);
  lua_pushinteger(L, result.zlib_status);
  lua_setfield(L, -2, "zlib_status");
  unsigned_value(L, result.supplied);
  lua_setfield(L, -2, "supplied");
  unsigned_value(L, result.consumed);
  lua_setfield(L, -2, "consumed");
  unsigned_value(L, result.output);
  lua_setfield(L, -2, "output_bytes");
  lua_pushboolean(L, result.input_failed);
  lua_setfield(L, -2, "input_failed");
  unsigned_value(L, stream->avail_in);
  lua_setfield(L, -2, "unused_length");
  push_io(L, owner, 1);
  lua_setfield(L, -2, "unused");
  if (stream->avail_in <= VECTIS_ZLIB_CHUNK_BYTES) {
    lua_pushlstring(L, stream->next_in ? (const char *)stream->next_in : "",
                    stream->avail_in);
    lua_setfield(L, -2, "unused_materialized");
  }
  return 1;
}
static int back_full_driver(lua_State *L) {
  back_capture(L, 2, 1, "read");
  back_capture(L, 3, 3, "write");
  lua_getiuservalue(L, 1, 1);
  lua_pushboolean(L, 0);
  lua_rawseti(L, -2, 5);
  lua_pop(L, 1);
  return back_driver(L);
}
static int back_call(lua_State *L) {
  raw_owner *owner = live(L);
  int status, index;
  if (!owner->back_mode)
    return luaL_error(L, "back-inflate state required");
  lua_settop(L, 3);
  owner->back_advanced = 0;
  owner->busy = 1;
  lua_pushcfunction(L, back_full_driver);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  status = lua_pcall(L, 3, 2, 0);
  owner->busy = 0;
  lua_getiuservalue(L, 1, 1);
  for (index = 1; index <= 5; ++index) {
    lua_pushnil(L);
    lua_rawseti(L, -2, index);
  }
  lua_pop(L, 1);
  if (status != LUA_OK) {
    if (owner->back_advanced) {
      raw_abort(L, owner, 1);
    }
    return lua_error(L);
  }
  return 2;
}

static int process_driver(lua_State *L) {
  raw_owner *owner = userdata(L, 1);
  const char *input = NULL;
  size_t size = 0, capacity = (size_t)lua_tointeger(L, 3);
  unsigned char output[VECTIS_ZLIB_CHUNK_BYTES];
  vectis_zlib_raw_step step;
  vectis_error error;
  if (!lua_isnil(L, 2))
    input = lua_tolstring(L, 2, &size);
  if (owner->receiver->process(owner->receiver, input, size, output, capacity,
                               (int)lua_tointeger(L, 4), &step,
                               &error) != VECTIS_OK)
    return error_result(L, &error);
  allocator_raise(L, owner);
  lua_createtable(L, 0, 4);
  lua_pushinteger(L, step.zlib_status);
  lua_setfield(L, -2, "zlib_status");
  lua_pushinteger(L, (lua_Integer)step.consumed);
  lua_setfield(L, -2, "consumed");
  lua_pushinteger(L, (lua_Integer)step.produced);
  lua_setfield(L, -2, "produced");
  lua_pushlstring(L, (const char *)output, step.produced);
  lua_setfield(L, -2, "output");
  return 1;
}
static int process_materialized(lua_State *L) {
  raw_owner *owner = live(L);
  size_t size;
  int capacity, flush, status;
  if (!lua_isnoneornil(L, 2)) {
    luaL_checktype(L, 2, LUA_TSTRING);
    lua_tolstring(L, 2, &size);
    if (size > VECTIS_ZLIB_CHUNK_BYTES)
      return luaL_error(L, "raw zlib chunk exceeds 65536 bytes");
  }
  capacity =
      lua_isnoneornil(L, 3) ? (int)VECTIS_ZLIB_CHUNK_BYTES : integer(L, 3);
  if (capacity < 0 || (unsigned)capacity > VECTIS_ZLIB_CHUNK_BYTES)
    return luaL_error(L, "raw output capacity must be 0..65536");
  flush = lua_isnoneornil(L, 4) ? Z_NO_FLUSH : integer(L, 4);
  lua_settop(L, 4);
  lua_pushinteger(L, capacity);
  lua_replace(L, 3);
  lua_pushinteger(L, flush);
  lua_replace(L, 4);
  owner->busy = 1;
  lua_pushcfunction(L, process_driver);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 4);
  status = lua_pcall(L, 4, 2, 0);
  owner->busy = 0;
  if (status != LUA_OK) {
    raw_abort(L, owner, 1);
    return lua_error(L);
  }
  return 2;
}
static int capability(lua_State *L) {
  (void)native(L, live(L));
  lua_settop(L, 1);
  return 1;
}
static int fields_driver(lua_State *L) {
  raw_owner *owner = userdata(L, 1);
  z_streamp stream = native(L, owner);
  lua_createtable(L, 0, 7);
  unsigned_value(L, stream->total_in);
  lua_setfield(L, -2, "total_in");
  unsigned_value(L, stream->total_out);
  lua_setfield(L, -2, "total_out");
  unsigned_value(L, stream->adler);
  lua_setfield(L, -2, "adler");
  lua_pushinteger(L, stream->data_type);
  lua_setfield(L, -2, "data_type");
  lua_pushinteger(L, stream->avail_in);
  lua_setfield(L, -2, "avail_in");
  lua_pushinteger(L, stream->avail_out);
  lua_setfield(L, -2, "avail_out");
  if (stream->msg) {
    lua_pushstring(L, stream->msg);
    lua_setfield(L, -2, "msg");
  }
  if (owner->allocator && owner->allocator->custom) {
    allocator_value(L, owner->allocator, 1);
    lua_setfield(L, -2, "zalloc");
    allocator_value(L, owner->allocator, 2);
    lua_setfield(L, -2, "zfree");
  } else {
    lua_pushcfunction(L, default_alloc);
    lua_setfield(L, -2, "zalloc");
    lua_pushcfunction(L, default_free);
    lua_setfield(L, -2, "zfree");
  }
  if (owner->allocator) {
    allocator_value(L, owner->allocator, 3);
    lua_setfield(L, -2, "opaque");
  }
  push_io(L, owner, 1);
  lua_setfield(L, -2, "next_in");
  push_io(L, owner, 0);
  lua_setfield(L, -2, "next_out");
  return 1;
}
static int bind_io(lua_State *L) {
  int input = lua_toboolean(L, lua_upvalueindex(1));
  raw_owner *owner = live(L);
  vectis_buffer_span *span = NULL;
  vectis_error error;
  size_t offset = lua_isnoneornil(L, 3) ? 0 : size_argument(L, 3), capacity = 0,
         size;
  int has_capacity = !lua_isnoneornil(L, 4);
  vectis_status status;
  if (has_capacity)
    capacity = size_argument(L, 4);
  if (!lua_isnoneornil(L, 2)) {
    status = vectis_lua_buffer_acquire_span(L, 2, input, &span, &error);
    if (status != VECTIS_OK)
      return error_result(L, &error);
    status = vectis_buffer_span_size(span, &size, &error);
    if (status != VECTIS_OK || offset > size) {
      vectis_buffer_span_close(span);
      return luaL_error(L, "IO offset exceeds storage");
    }
    if (!has_capacity)
      capacity = size - offset;
  }
  status = vectis_zlib_raw_bind(owner->receiver, input, span, offset, capacity,
                                &error);
  vectis_buffer_span_close(span);
  if (status != VECTIS_OK)
    return error_result(L, &error);
  lua_settop(L, 1);
  return 1;
}
static int availability(lua_State *L) {
  raw_owner *owner = live(L);
  vectis_error error;
  int input = lua_toboolean(L, lua_upvalueindex(1));
  size_t size = size_argument(L, 2);
  if (vectis_zlib_raw_availability(owner->receiver, input, size, &error) !=
      VECTIS_OK)
    return error_result(L, &error);
  lua_settop(L, 1);
  return 1;
}
static int bound_driver(lua_State *L) {
  raw_owner *owner = userdata(L, 1);
  vectis_zlib_raw_step result;
  vectis_error error;
  int params = lua_toboolean(L, 4);
  vectis_status status;
  status =
      params ? vectis_zlib_raw_params(owner->receiver, (int)lua_tointeger(L, 2),
                                      (int)lua_tointeger(L, 3), &result, &error)
             : vectis_zlib_raw_run(owner->receiver, (int)lua_tointeger(L, 2),
                                   &result, &error);
  if (status != VECTIS_OK)
    return error_result(L, &error);
  allocator_raise(L, owner);
  lua_createtable(L, 0, 3);
  lua_pushinteger(L, result.zlib_status);
  lua_setfield(L, -2, "zlib_status");
  unsigned_value(L, result.consumed);
  lua_setfield(L, -2, "consumed");
  unsigned_value(L, result.produced);
  lua_setfield(L, -2, "produced");
  return 1;
}
static int bound_call(lua_State *L, int params) {
  raw_owner *owner = live(L);
  int first = params ? integer(L, 2)
                     : (lua_isnoneornil(L, 2) ? Z_NO_FLUSH : integer(L, 2));
  int second = params ? integer(L, 3) : 0, status;
  lua_settop(L, 1);
  lua_pushcfunction(L, bound_driver);
  lua_pushvalue(L, 1);
  lua_pushinteger(L, first);
  lua_pushinteger(L, second);
  lua_pushboolean(L, params);
  owner->busy = 1;
  status = lua_pcall(L, 4, 2, 0);
  owner->busy = 0;
  if (status != LUA_OK) {
    raw_abort(L, owner, 1);
    return lua_error(L);
  }
  return 2;
}
static int run_bound(lua_State *L) { return bound_call(L, 0); }
static int params_bound(lua_State *L) { return bound_call(L, 1); }
static int bound_size(lua_State *L) {
  raw_owner *owner = live(L);
  uint64_t output;
  vectis_error error;
  size_t input = size_argument(L, 2);
  if (vectis_zlib_raw_bound(owner->receiver, (uint64_t)input,
                            lua_toboolean(L, lua_upvalueindex(1)), &output,
                            &error) != VECTIS_OK)
    return error_result(L, &error);
  unsigned_value(L, output);
  return 1;
}
static int fields(lua_State *L) {
  raw_owner *owner = live(L);
  int status;
  owner->busy = 1;
  lua_pushcfunction(L, fields_driver);
  lua_pushvalue(L, 1);
  status = lua_pcall(L, 1, 1, 0);
  owner->busy = 0;
  if (status != LUA_OK)
    return lua_error(L);
  return 1;
}
static int copy_owner(lua_State *L) {
  raw_owner *source = live(L), *copy = new_owner(L, source->compress);
  vectis_error error;
  source = live(L);
  copy->allocator = source->allocator;
  if (copy->allocator) {
    lua_getiuservalue(L, 1, 1);
    lua_rawgeti(L, -1, ALLOCATOR_OWNER_KEY);
    lua_remove(L, -2);
    allocator_attach(L, copy, -2, -1);
    lua_pop(L, 1);
  }
  source->busy = 1;
  if (source->receiver->copy(source->receiver, &copy->receiver, &error) !=
      VECTIS_OK) {
    source->busy = 0;
    allocator_raise(L, source);
    return error_result(L, &error);
  }
  source->busy = 0;
  if (copy->allocator) {
    ++copy->allocator->owners;
    copy->allocator_attached = 1;
  }
  allocator_raise(L, source);
  return 1;
}
static int end_owner(lua_State *L) {
  raw_owner *owner = live(L);
  vectis_error error;
  int code, has_error;
  allocator_pin(L, owner, 1, 1);
  owner->busy = 1;
  if (owner->receiver->end(owner->receiver, &code, &error) != VECTIS_OK) {
    owner->busy = 0;
    allocator_pin(L, owner, 1, 0);
    return error_result(L, &error);
  }
  owner->busy = 0;
  if (owner->back_mode) {
    vectis_buffer_span_close(owner->back_input);
    owner->back_input = NULL;
  }
  allocator_pin(L, owner, 1, 0);
  has_error = allocator_take_error(L, owner->allocator);
  allocator_detach(L, owner, 1);
  if (has_error)
    return lua_error(L);
  lua_pushinteger(L, code);
  return 1;
}
static int attach_header(lua_State *L) {
  raw_owner *owner = live(L);
  header_owner *header = NULL;
  vectis_error error;
  int code;
  int compress = lua_toboolean(L, lua_upvalueindex(1));
  if (owner->compress != compress) {
    lua_pushinteger(L, Z_STREAM_ERROR);
    return 1;
  }
  if (!lua_isnoneornil(L, 2)) {
    header = (header_owner *)luaL_checkudata(L, 2, HEADER);
    if (!header->receiver)
      return luaL_error(L, "gzip header is closed");
  }
  if (vectis_zlib_raw_header(owner->receiver, header ? header->receiver : NULL,
                             &code, &error) != VECTIS_OK)
    return error_result(L, &error);
  lua_pushinteger(L, code);
  return 1;
}
static int typed_copy(lua_State *L) {
  raw_owner *owner = live(L);
  if (owner->compress != lua_toboolean(L, lua_upvalueindex(1))) {
    vectis_error error;
    memset(&error, 0, sizeof(error));
    error.code = VECTIS_ERR_INVALID;
    error.dependency_code = Z_STREAM_ERROR;
    strcpy(error.message, "raw zlib copy kind mismatch");
    return error_result(L, &error);
  }
  return copy_owner(L);
}
static int typed_end(lua_State *L) {
  if (live(L)->back_mode) {
    lua_pushinteger(L, Z_STREAM_ERROR);
    return 1;
  }
  if (live(L)->compress != lua_toboolean(L, lua_upvalueindex(1))) {
    lua_pushinteger(L, Z_STREAM_ERROR);
    return 1;
  }
  return end_owner(L);
}
static int back_end(lua_State *L) {
  if (!live(L)->back_mode) {
    lua_pushinteger(L, Z_STREAM_ERROR);
    return 1;
  }
  return end_owner(L);
}
static int typed_process(lua_State *L) {
  if (live(L)->compress != lua_toboolean(L, lua_upvalueindex(1))) {
    vectis_error error;
    memset(&error, 0, sizeof(error));
    error.code = VECTIS_ERR_INVALID;
    error.dependency_code = Z_STREAM_ERROR;
    strcpy(error.message, "raw zlib processing kind mismatch");
    return error_result(L, &error);
  }
  return process_materialized(L);
}

enum operation {
  D_RESET,
  I_RESET,
  D_KEEP,
  I_KEEP,
  I_RESET2,
  D_SET_DICT,
  I_SET_DICT,
  D_GET_DICT,
  I_GET_DICT,
  D_TUNE,
  D_PENDING,
  D_USED,
  D_PRIME,
  I_PRIME,
  I_SYNC,
  I_SYNCPOINT,
  I_MARK,
  I_VALIDATE,
  I_UNDERMINE,
  I_CODES
};
static int control_driver(lua_State *L) {
  int op = (int)lua_tointeger(L, lua_upvalueindex(1)), code = Z_STREAM_ERROR;
  raw_owner *owner = userdata(L, 1);
  z_streamp stream = native(L, owner);
  unsigned char dictionary[32768];
  uInt length = 0;
  unsigned pending = 0;
  int bits = 0;
  size_t size;
  const char *data;
  int wants_compress = op == D_RESET || op == D_KEEP || op == D_SET_DICT ||
                       op == D_GET_DICT || op == D_TUNE || op == D_PENDING ||
                       op == D_USED || op == D_PRIME;
  if (owner->compress != wants_compress) {
    lua_pushinteger(L, Z_STREAM_ERROR);
    return 1;
  }
  if (owner->back_mode) {
    lua_pushinteger(L, Z_STREAM_ERROR);
    return 1;
  }
  switch (op) {
  case D_RESET: {
    vectis_error error;
    if (vectis_zlib_raw_reset(owner->receiver, 0, 0, &code, &error) !=
        VECTIS_OK)
      return error_result(L, &error);
  } break;
  case I_RESET: {
    vectis_error error;
    if (vectis_zlib_raw_reset(owner->receiver, 0, 0, &code, &error) !=
        VECTIS_OK)
      return error_result(L, &error);
  } break;
  case D_KEEP: {
    vectis_error error;
    if (vectis_zlib_raw_reset(owner->receiver, 1, 0, &code, &error) !=
        VECTIS_OK)
      return error_result(L, &error);
  } break;
  case I_KEEP: {
    vectis_error error;
    if (vectis_zlib_raw_reset(owner->receiver, 1, 0, &code, &error) !=
        VECTIS_OK)
      return error_result(L, &error);
  } break;
  case I_RESET2: {
    vectis_error error;
    if (vectis_zlib_raw_reset(owner->receiver, 2, integer(L, 2), &code,
                              &error) != VECTIS_OK)
      return error_result(L, &error);
  } break;
  case D_SET_DICT:
  case I_SET_DICT:
    luaL_checktype(L, 2, LUA_TSTRING);
    data = lua_tolstring(L, 2, &size);
    if (size > UINT_MAX)
      return luaL_error(L, "dictionary length exceeds uInt");
    code = op == D_SET_DICT
               ? deflateSetDictionary(stream, (const Bytef *)data, (uInt)size)
               : inflateSetDictionary(stream, (const Bytef *)data, (uInt)size);
    break;
  case D_GET_DICT:
  case I_GET_DICT:
    code = op == D_GET_DICT ? deflateGetDictionary(stream, dictionary, &length)
                            : inflateGetDictionary(stream, dictionary, &length);
    lua_pushinteger(L, code);
    if (code == Z_OK)
      lua_pushlstring(L, (const char *)dictionary, length);
    else
      lua_pushnil(L);
    return 2;
  case D_TUNE:
    code = deflateTune(stream, integer(L, 2), integer(L, 3), integer(L, 4),
                       integer(L, 5));
    break;
  case D_PENDING:
    code = deflatePending(stream, &pending, &bits);
    lua_pushinteger(L, code);
    lua_pushinteger(L, pending);
    lua_pushinteger(L, bits);
    return 3;
  case D_USED:
    code = deflateUsed(stream, &bits);
    lua_pushinteger(L, code);
    lua_pushinteger(L, bits);
    return 2;
  case D_PRIME:
    code = deflatePrime(stream, integer(L, 2), integer(L, 3));
    break;
  case I_PRIME:
    code = inflatePrime(stream, integer(L, 2), integer(L, 3));
    break;
  case I_SYNC:
    code = inflateSync(stream);
    break;
  case I_SYNCPOINT:
    code = inflateSyncPoint(stream);
    break;
  case I_MARK:
    lua_pushinteger(L, (lua_Integer)inflateMark(stream));
    return 1;
  case I_VALIDATE:
    code = inflateValidate(stream, integer(L, 2));
    break;
  case I_UNDERMINE:
    code = inflateUndermine(stream, integer(L, 2));
    break;
  case I_CODES:
    unsigned_value(L, inflateCodesUsed(stream));
    return 1;
  }
  lua_pushinteger(L, code);
  return 1;
}
static int control(lua_State *L) {
  raw_owner *owner = live(L);
  int count = lua_gettop(L), status, index, results;
  owner->busy = 1;
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_pushcclosure(L, control_driver, 1);
  for (index = 1; index <= count; ++index)
    lua_pushvalue(L, index);
  status = lua_pcall(L, count, LUA_MULTRET, 0);
  owner->busy = 0;
  if (status != LUA_OK)
    return lua_error(L);
  results = lua_gettop(L) - count;
  allocator_raise(L, owner);
  return results;
}
typedef struct buffer_operation {
  vectis_buffer_span *input, *output;
  int operation, size_width, checksum;
} buffer_operation;
static int buffer_operation_driver(lua_State *L) {
  static const unsigned char empty_input = 0;
  buffer_operation *frame = (buffer_operation *)lua_touserdata(L, 1);
  vectis_error error;
  vectis_status status;
  vectis_zlib_raw_step result;
  const void *bytes = NULL;
  size_t size = 0, length;
  uLong seed, checksum;
  int level = Z_DEFAULT_COMPRESSION;
  if (frame->checksum) {
    seed = (uLong)unsigned_argument(L, 2, ULONG_MAX);
    if (!lua_isnoneornil(L, 3)) {
      if (vectis_lua_buffer_acquire_span(L, 3, 1, &frame->input, &error) !=
              VECTIS_OK ||
          vectis_buffer_span_data(frame->input, &bytes, &size, &error) !=
              VECTIS_OK)
        return luaL_error(L, "%s", error.message);
      /* A real zero-capacity resource is empty input, not native NULL init. */
      if (!bytes)
        bytes = &empty_input;
    }
    length = lua_isnoneornil(L, 4) ? size : size_argument(L, 4);
    if (length > size)
      return luaL_error(L, "checksum length exceeds typed storage");
    status = vectis_zlib_raw_checksum(frame->operation, frame->size_width, seed,
                                      bytes, length, &checksum, &error);
    if (status != VECTIS_OK)
      return luaL_error(L, "%s", error.message);
    unsigned_value(L, checksum);
    return 1;
  }
  if (frame->operation == 1)
    level = integer(L, 4);
  if (vectis_lua_buffer_acquire_span(L, 2, 0, &frame->output, &error) !=
      VECTIS_OK)
    return luaL_error(L, "%s", error.message);
  if (!lua_isnoneornil(L, 3) &&
      vectis_lua_buffer_acquire_span(L, 3, 1, &frame->input, &error) !=
          VECTIS_OK)
    return luaL_error(L, "%s", error.message);
  status = vectis_zlib_buffer_transform(frame->operation, frame->size_width,
                                        frame->output, frame->input, level,
                                        &result, &error);
  if (status != VECTIS_OK)
    return luaL_error(L, "%s", error.message);
  lua_pushinteger(L, result.zlib_status);
  unsigned_value(L, result.produced);
  if (frame->operation == 3) {
    unsigned_value(L, result.consumed);
    return 3;
  }
  return 2;
}
static int buffer_operation_call(lua_State *L) {
  buffer_operation frame;
  int count = lua_gettop(L), index, status, results;
  memset(&frame, 0, sizeof(frame));
  if (count > INT_MAX - 3 || !lua_checkstack(L, count + 3))
    return luaL_error(L, "cannot reserve buffer operation argument stack");
  frame.operation = (int)lua_tointeger(L, lua_upvalueindex(1));
  frame.size_width = (int)lua_tointeger(L, lua_upvalueindex(2));
  frame.checksum = lua_toboolean(L, lua_upvalueindex(3));
  lua_pushcfunction(L, buffer_operation_driver);
  lua_pushlightuserdata(L, &frame);
  for (index = 1; index <= count; ++index)
    lua_pushvalue(L, index);
  status = lua_pcall(L, count + 1, LUA_MULTRET, 0);
  vectis_buffer_span_close(frame.input);
  vectis_buffer_span_close(frame.output);
  if (status != LUA_OK)
    return lua_error(L);
  results = lua_gettop(L) - count;
  return results;
}
static int buffer_bound(lua_State *L) {
  int size_width = lua_toboolean(L, lua_upvalueindex(1));
  uint64_t input = unsigned_argument(L, 1, size_width ? SIZE_MAX : ULONG_MAX),
           output;
  vectis_error error;
  if (vectis_zlib_buffer_bound(input, size_width, &output, &error) != VECTIS_OK)
    return luaL_error(L, "%s", error.message);
  unsigned_value(L, output);
  return 1;
}
static int native_combine(lua_State *L) {
  int kind = (int)lua_tointeger(L, lua_upvalueindex(1));
  int large = (int)lua_tointeger(L, lua_upvalueindex(2));
  int mode = (int)lua_tointeger(L, lua_upvalueindex(3));
  vectis_error error;
  vectis_status status;
  uLong first, second, out;
  uint64_t length;
  if (mode == 1) {
    length = unsigned_argument(L, 1, (uint64_t)INT64_MAX);
    status = vectis_zlib_raw_crc_gen(large, length, &out, &error);
  } else {
    first = (uLong)unsigned_argument(L, 1, ULONG_MAX);
    second = (uLong)unsigned_argument(L, 2, ULONG_MAX);
    if (mode == 2)
      status = vectis_zlib_raw_crc_op(first, second,
                                      (uLong)unsigned_argument(L, 3, ULONG_MAX),
                                      &out, &error);
    else {
      length = unsigned_argument(L, 3, (uint64_t)INT64_MAX);
      status = vectis_zlib_raw_checksum_combine(kind, large, first, second,
                                                length, &out, &error);
    }
  }
  if (status != VECTIS_OK)
    return luaL_error(L, "%s", error.message);
  unsigned_value(L, out);
  return 1;
}
typedef struct crc_table_owner {
  vectis_zlib_crc_table *receiver;
} crc_table_owner;
static int table_close(lua_State *L) {
  crc_table_owner *owner = (crc_table_owner *)luaL_checkudata(L, 1, CRC_TABLE);
  vectis_zlib_crc_table_close(owner->receiver);
  owner->receiver = NULL;
  return 0;
}
static int table_method(lua_State *L) {
  crc_table_owner *owner = (crc_table_owner *)luaL_checkudata(L, 1, CRC_TABLE);
  vectis_error error;
  z_crc_t value;
  const z_crc_t *native_values;
  size_t size;
  int method = (int)lua_tointeger(L, lua_upvalueindex(1));
  if (!owner->receiver)
    return luaL_error(L, "CRC table is closed");
  if (method == 0) {
    if (owner->receiver->size(owner->receiver, &size, &error) != VECTIS_OK)
      return luaL_error(L, "%s", error.message);
    unsigned_value(L, size);
  } else if (method == 1) {
    if (owner->receiver->entry(owner->receiver, size_argument(L, 2), &value,
                               &error) != VECTIS_OK)
      return luaL_error(L, "%s", error.message);
    unsigned_value(L, value);
  } else {
    if (owner->receiver->native(owner->receiver, &native_values, &error) !=
        VECTIS_OK)
      return luaL_error(L, "%s", error.message);
    lua_pushvalue(L, 1);
  }
  return 1;
}
static int table_new(lua_State *L) {
  vectis_error error;
  crc_table_owner *owner =
      (crc_table_owner *)lua_newuserdatauv(L, sizeof(*owner), 0);
  owner->receiver = NULL;
  luaL_setmetatable(L, CRC_TABLE);
  if (vectis_zlib_crc_table_new(&owner->receiver, &error) != VECTIS_OK)
    return luaL_error(L, "%s", error.message);
  return 1;
}
static void buffer_operations_register(lua_State *L, int module) {
  static const char *const transforms[] = {
      "compress",   "compress2",   "uncompress",   "uncompress2",
      "compress_z", "compress2_z", "uncompress_z", "uncompress2_z"};
  static const char *const checksums[] = {"adler32", "crc32", "adler32_z",
                                          "crc32_z"};
  static const char *const combines[] = {
      "adler32_combine", "crc32_combine",     "adler32_combine64",
      "crc32_combine64", "crc32_combine_gen", "crc32_combine_gen64",
      "crc32_combine_op"};
  static const char *const methods[] = {"size", "entry", "native"};
  int i;
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, buffer_bound, 1);
  lua_setfield(L, module, "compressBound");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, buffer_bound, 1);
  lua_setfield(L, module, "compressBound_z");
  for (i = 0; i < 8; ++i) {
    lua_pushinteger(L, i % 4);
    lua_pushinteger(L, i / 4);
    lua_pushboolean(L, 0);
    lua_pushcclosure(L, buffer_operation_call, 3);
    lua_setfield(L, module, transforms[i]);
  }
  for (i = 0; i < 4; ++i) {
    lua_pushinteger(L, i % 2);
    lua_pushinteger(L, i / 2);
    lua_pushboolean(L, 1);
    lua_pushcclosure(L, buffer_operation_call, 3);
    lua_setfield(L, module, checksums[i]);
  }
  for (i = 0; i < 7; ++i) {
    lua_pushinteger(L, i < 4 ? i % 2 : VECTIS_ZLIB_CRC32);
    lua_pushinteger(L, i < 4 ? i / 2 : i == 5);
    lua_pushinteger(L, i < 4 ? 0 : i == 6 ? 2 : 1);
    lua_pushcclosure(L, native_combine, 3);
    lua_setfield(L, module, combines[i]);
  }
  luaL_newmetatable(L, CRC_TABLE);
  lua_pushcfunction(L, table_close);
  lua_setfield(L, -2, "__gc");
  lua_pushcfunction(L, table_close);
  lua_setfield(L, -2, "__close");
  lua_newtable(L);
  lua_pushcfunction(L, table_close);
  lua_setfield(L, -2, "close");
  for (i = 0; i < 3; ++i) {
    lua_pushinteger(L, i);
    lua_pushcclosure(L, table_method, 1);
    lua_setfield(L, -2, methods[i]);
  }
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
  lua_pushcfunction(L, table_new);
  lua_setfield(L, module, "get_crc_table");
}
void vectis_lua_zlib_raw_register(lua_State *L) {
  static const char *const constructors[] = {
      "deflateInit",  "inflateInit",  "deflateInit2",  "inflateInit2",
      "deflateInit_", "inflateInit_", "deflateInit2_", "inflateInit2_"};
  static const int kinds[] = {1, 0, 3, 2, 5, 4, 7, 6};
  static const char *const controls[] = {"deflateReset",
                                         "inflateReset",
                                         "deflateResetKeep",
                                         "inflateResetKeep",
                                         "inflateReset2",
                                         "deflateSetDictionary",
                                         "inflateSetDictionary",
                                         "deflateGetDictionary_materialized",
                                         "inflateGetDictionary_materialized",
                                         "deflateTune",
                                         "deflatePending",
                                         "deflateUsed",
                                         "deflatePrime",
                                         "inflatePrime",
                                         "inflateSync",
                                         "inflateSyncPoint",
                                         "inflateMark",
                                         "inflateValidate",
                                         "inflateUndermine",
                                         "inflateCodesUsed"};
  int module = lua_absindex(L, -1), i;
  buffer_operations_register(L, module);
  luaL_newmetatable(L, ALLOCATOR);
  lua_pushcfunction(L, allocator_gc);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, ALLOCATOR_MAP);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, ALLOCATOR_MAP);
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, ALLOCATOR_PINS);
  } else
    lua_pop(L, 1);

  luaL_newmetatable(L, HEADER);
  lua_pushcfunction(L, header_close);
  lua_setfield(L, -2, "__gc");
  lua_pushcfunction(L, header_close);
  lua_setfield(L, -2, "__close");
  lua_newtable(L);
  lua_pushcfunction(L, header_close);
  lua_setfield(L, -2, "close");
  lua_pushcfunction(L, header_fields);
  lua_setfield(L, -2, "fields");
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
  lua_pushcfunction(L, header_create);
  lua_setfield(L, module, "gzip_header");
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, back_create, 1);
  lua_setfield(L, module, "inflateBackInit");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, back_create, 1);
  lua_setfield(L, module, "inflateBackInit_");
  lua_pushcfunction(L, back_call);
  lua_setfield(L, module, "inflateBack");
  lua_pushcfunction(L, back_end);
  lua_setfield(L, module, "inflateBackEnd");
  luaL_requiref(L, "vectis.buffer.core", luaopen_vectis_buffer_core, 0);
  lua_getfield(L, -1, "new");
  lua_setfield(L, LUA_REGISTRYINDEX, ALLOCATOR_BUFFER_FACTORY);
  lua_pop(L, 1);
  lua_pushinteger(L, (lua_Integer)sizeof(z_stream));
  lua_setfield(L, module, "Z_STREAM_SIZE");
  unsigned_value(L, (uint64_t)SIZE_MAX);
  lua_setfield(L, module, "Z_SIZE_MAX");
  unsigned_value(L, (uint64_t)ULONG_MAX);
  lua_setfield(L, module, "Z_ULONG_MAX");
  luaL_newmetatable(L, RAW);
  lua_pushcfunction(L, close_owner);
  lua_setfield(L, -2, "__gc");
  lua_pushcfunction(L, close_owner);
  lua_setfield(L, -2, "__close");
  lua_newtable(L);
  lua_pushcfunction(L, back_call);
  lua_setfield(L, -2, "back");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, attach_header, 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, module, "deflateSetHeader");
  lua_setfield(L, -2, "deflateSetHeader");
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, attach_header, 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, module, "inflateGetHeader");
  lua_setfield(L, -2, "inflateGetHeader");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, bind_io, 1);
  lua_setfield(L, -2, "set_input");
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, bind_io, 1);
  lua_setfield(L, -2, "set_output");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, availability, 1);
  lua_setfield(L, -2, "set_avail_in");
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, availability, 1);
  lua_setfield(L, -2, "set_avail_out");
  lua_pushcfunction(L, run_bound);
  lua_setfield(L, -2, "run");
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, bound_size, 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, module, "deflateBound");
  lua_setfield(L, -2, "deflateBound");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, bound_size, 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, module, "deflateBound_z");
  lua_setfield(L, -2, "deflateBound_z");
  lua_pushcfunction(L, params_bound);
  lua_pushvalue(L, -1);
  lua_setfield(L, module, "deflateParams");
  lua_setfield(L, -2, "deflateParams");
  lua_pushcfunction(L, close_owner);
  lua_setfield(L, -2, "close");
  lua_pushcfunction(L, end_owner);
  lua_setfield(L, -2, "end_native");
  lua_pushcfunction(L, copy_owner);
  lua_setfield(L, -2, "copy");
  lua_pushcfunction(L, process_materialized);
  lua_setfield(L, -2, "process_materialized");
  lua_pushcfunction(L, capability);
  lua_setfield(L, -2, "native");
  lua_pushcfunction(L, fields);
  lua_setfield(L, -2, "fields");
  for (i = 0; i < (int)(sizeof(controls) / sizeof(*controls)); ++i) {
    lua_pushinteger(L, i);
    lua_pushcclosure(L, control, 1);
    lua_pushvalue(L, -1);
    lua_setfield(L, module, controls[i]);
    lua_setfield(L, -2, controls[i]);
  }
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
  for (i = 0; i < 8; ++i) {
    lua_pushinteger(L, kinds[i]);
    lua_pushcclosure(L, create, 1);
    lua_setfield(L, module, constructors[i]);
  }
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, typed_copy, 1);
  lua_setfield(L, module, "deflateCopy");
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, typed_copy, 1);
  lua_setfield(L, module, "inflateCopy");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, typed_end, 1);
  lua_setfield(L, module, "deflateEnd");
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, typed_end, 1);
  lua_setfield(L, module, "inflateEnd");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, typed_process, 1);
  lua_setfield(L, module, "deflate_chunk_materialized");
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, typed_process, 1);
  lua_setfield(L, module, "inflate_chunk_materialized");
}
