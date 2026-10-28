#include "vectis_lua_buffer.h"
#include <lauxlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vectis/buffer.h>
#define BUFFER "vectis.buffer.owner"
#define SPAN "vectis.buffer.span.owner"
#define WRITE_LOCK "vectis.buffer.write_lock"
typedef struct lock_owner {
  vectis_buffer_lock *lock;
} lock_owner;
typedef struct buffer_owner {
  vectis_buffer *buffer;
  vectis_buffer_span *span;
  int transfer_pending;
} buffer_owner;
static size_t size_arg(lua_State *L, int index) {
  size_t result, n, i;
  const char *text;
  unsigned digit;
  lua_Integer number;
  if (lua_isinteger(L, index)) {
    number = lua_tointeger(L, index);
    if (number < 0 || (uint64_t)number > (uint64_t)SIZE_MAX)
      luaL_error(L, "size outside native size_t domain");
    return (size_t)number;
  }
  luaL_checktype(L, index, LUA_TSTRING);
  text = lua_tolstring(L, index, &n);
  result = 0;
  if (!n)
    luaL_error(L, "size must be an unsigned decimal integer");
  for (i = 0; i < n; ++i) {
    if (text[i] < '0' || text[i] > '9')
      luaL_error(L, "size must be an unsigned decimal integer");
    digit = (unsigned)(text[i] - '0');
    if (result > (SIZE_MAX - digit) / 10u)
      luaL_error(L, "size outside native size_t domain");
    result = result * 10u + digit;
  }
  return result;
}
static void push_size(lua_State *L, size_t value) {
  char text[3 * sizeof(size_t) + 1];
  size_t position;
  if (value <= (size_t)LUA_MAXINTEGER)
    lua_pushinteger(L, (lua_Integer)value);
  else {
    position = sizeof(text);
    text[--position] = '\0';
    do {
      text[--position] = (char)('0' + value % 10u);
      value /= 10u;
    } while (value);
    lua_pushstring(L, text + position);
  }
}
static int error_result(lua_State *L, const vectis_error *error) {
  lua_pushnil(L);
  lua_createtable(L, 0, 4);
  lua_pushinteger(L, error->code);
  lua_setfield(L, -2, "status");
  lua_pushinteger(L, error->source);
  lua_setfield(L, -2, "source");
  lua_pushstring(L, vectis_status_string(error->code));
  lua_setfield(L, -2, "status_string");
  lua_pushstring(L, error->message);
  lua_setfield(L, -2, "message");
  return 2;
}
static int close_owner(lua_State *L) {
  buffer_owner *owner = (buffer_owner *)luaL_testudata(L, 1, BUFFER);
  if (!owner)
    owner = (buffer_owner *)luaL_testudata(L, 1, SPAN);
  if (owner) {
    if (owner->transfer_pending)
      return luaL_error(L, "buffer ownership transfer is pending");
    vectis_buffer_close(owner->buffer);
    vectis_buffer_span_close(owner->span);
    owner->buffer = NULL;
    owner->span = NULL;
  }
  return 0;
}
static buffer_owner *check_owner(lua_State *L) {
  buffer_owner *owner = (buffer_owner *)luaL_testudata(L, 1, BUFFER);
  if (!owner)
    owner = (buffer_owner *)luaL_checkudata(L, 1, SPAN);
  if (!owner->buffer && !owner->span)
    luaL_error(L, "byte resource is closed");
  if (owner->transfer_pending)
    luaL_error(L, "buffer ownership transfer is pending");
  return owner;
}
static buffer_owner *new_owner(lua_State *L, int span) {
  buffer_owner *owner = (buffer_owner *)lua_newuserdatauv(L, sizeof(*owner), 0);
  memset(owner, 0, sizeof(*owner));
  luaL_setmetatable(L, span ? SPAN : BUFFER);
  return owner;
}
static int create(lua_State *L) {
  buffer_owner *owner;
  const char *input = NULL;
  size_t size;
  vectis_error error;
  vectis_status status;
  if (lua_toboolean(L, lua_upvalueindex(1))) {
    luaL_checktype(L, 1, LUA_TSTRING);
    input = lua_tolstring(L, 1, &size);
  } else
    size = size_arg(L, 1);
  owner = new_owner(L, 0);
  status = vectis_buffer_new(size, &owner->buffer, &error);
  if (status == VECTIS_OK && input)
    status = vectis_buffer_write(owner->buffer, 0, input, size, &error);
  if (status != VECTIS_OK) {
    vectis_buffer_close(owner->buffer);
    owner->buffer = NULL;
    return error_result(L, &error);
  }
  return 1;
}
static int resource_size(lua_State *L) {
  buffer_owner *owner = check_owner(L);
  size_t size;
  vectis_error error;
  vectis_status status =
      owner->buffer ? vectis_buffer_size(owner->buffer, &size, &error)
                    : vectis_buffer_span_size(owner->span, &size, &error);
  if (status != VECTIS_OK)
    return luaL_error(L, "%s", error.message);
  push_size(L, size);
  return 1;
}
static int read_bytes(lua_State *L) {
  buffer_owner *owner = check_owner(L);
  size_t offset = size_arg(L, 2), size = size_arg(L, 3);
  unsigned char bytes[VECTIS_BUFFER_CHUNK_BYTES];
  vectis_error error;
  vectis_status status;
  if (size > sizeof(bytes))
    return luaL_error(L, "byte copy exceeds 65536 bytes");
  status =
      owner->buffer
          ? vectis_buffer_read(owner->buffer, offset, bytes, size, &error)
          : vectis_buffer_span_read(owner->span, offset, bytes, size, &error);
  if (status != VECTIS_OK)
    return luaL_error(L, "%s", error.message);
  lua_pushlstring(L, (const char *)bytes, size);
  return 1;
}
static int write_bytes(lua_State *L) {
  buffer_owner *owner = check_owner(L);
  size_t offset = size_arg(L, 2), size;
  const char *bytes;
  vectis_error error;
  vectis_status status;
  luaL_checktype(L, 3, LUA_TSTRING);
  bytes = lua_tolstring(L, 3, &size);
  if (size > VECTIS_BUFFER_CHUNK_BYTES)
    return luaL_error(L, "byte copy exceeds 65536 bytes");
  status =
      owner->buffer
          ? vectis_buffer_write(owner->buffer, offset, bytes, size, &error)
          : vectis_buffer_span_write(owner->span, offset, bytes, size, &error);
  if (status != VECTIS_OK)
    return luaL_error(L, "%s", error.message);
  lua_settop(L, 1);
  return 1;
}
static int resize(lua_State *L) {
  buffer_owner *owner = (buffer_owner *)luaL_checkudata(L, 1, BUFFER);
  vectis_error error;
  size_t size = size_arg(L, 2);
  vectis_status status;
  if (!owner->buffer)
    return luaL_error(L, "byte resource is closed");
  if (owner->transfer_pending)
    return luaL_error(L, "buffer ownership transfer is pending");
  status = vectis_buffer_resize(owner->buffer, size, &error);
  if (status == VECTIS_ERR_NOMEM)
    return error_result(L, &error);
  if (status != VECTIS_OK)
    return luaL_error(L, "%s", error.message);
  lua_settop(L, 1);
  return 1;
}
static int borrow(lua_State *L) {
  buffer_owner *owner = check_owner(L), *view;
  size_t offset = size_arg(L, 2), size = size_arg(L, 3);
  int readonly = 1;
  vectis_error error;
  vectis_status status;
  if (!owner->buffer && !owner->span)
    return luaL_error(L, "byte resource is closed");
  if (!lua_isnoneornil(L, 4)) {
    luaL_checktype(L, 4, LUA_TBOOLEAN);
    readonly = lua_toboolean(L, 4);
  }
  view = new_owner(L, 1);
  /* Creating userdata can run another finalizer; recheck before acquisition. */
  if (!owner->buffer && !owner->span)
    return luaL_error(L, "byte resource is closed");
  status = owner->buffer
               ? vectis_buffer_borrow(owner->buffer, offset, size, readonly,
                                      &view->span, &error)
               : vectis_buffer_span_borrow(owner->span, offset, size, readonly,
                                           &view->span, &error);
  if (status == VECTIS_ERR_NOMEM)
    return error_result(L, &error);
  if (status != VECTIS_OK)
    return luaL_error(L, "%s", error.message);
  return 1;
}
static int data_capability(lua_State *L) {
  buffer_owner *owner = check_owner(L);
  const void *bytes;
  void *mutable_bytes;
  size_t size;
  vectis_error error;
  vectis_status status;
  if (!owner->span && !owner->buffer)
    return luaL_error(L, "byte resource is closed");
  if (owner->buffer)
    status = lua_toboolean(L, lua_upvalueindex(1))
                 ? vectis_buffer_data_mutable(owner->buffer, &mutable_bytes,
                                              &size, &error)
                 : vectis_buffer_data(owner->buffer, &bytes, &size, &error);
  else
    status = lua_toboolean(L, lua_upvalueindex(1))
                 ? vectis_buffer_span_data_mutable(owner->span, &mutable_bytes,
                                                   &size, &error)
                 : vectis_buffer_span_data(owner->span, &bytes, &size, &error);
  if (status != VECTIS_OK)
    return luaL_error(L, "%s", error.message);
  lua_settop(L, 1);
  push_size(L, size);
  return 2;
}
static int close_lock(lua_State *L) {
  lock_owner *owner = (lock_owner *)luaL_testudata(L, 1, WRITE_LOCK);
  if (owner) {
    vectis_buffer_lock_close(owner->lock);
    owner->lock = NULL;
  }
  return 0;
}
static int lock_writes(lua_State *L) {
  lock_owner *owner;
  vectis_buffer_span *span = NULL;
  vectis_error error;
  vectis_status status;
  (void)check_owner(L);
  owner = (lock_owner *)lua_newuserdatauv(L, sizeof(*owner), 0);
  owner->lock = NULL;
  luaL_setmetatable(L, WRITE_LOCK);
  status = vectis_lua_buffer_acquire_span(L, 1, 1, &span, &error);
  if (status == VECTIS_OK)
    status = vectis_buffer_span_lock_writes(span, &owner->lock, &error);
  vectis_buffer_span_close(span);
  if (status != VECTIS_OK)
    return error_result(L, &error);
  return 1;
}
static void register_type(lua_State *L, const char *name, int span) {
  luaL_newmetatable(L, name);
  lua_pushcfunction(L, close_owner);
  lua_setfield(L, -2, "__gc");
  lua_pushcfunction(L, close_owner);
  lua_setfield(L, -2, "__close");
  lua_newtable(L);
  lua_pushcfunction(L, close_owner);
  lua_setfield(L, -2, "close");
  lua_pushcfunction(L, resource_size);
  lua_setfield(L, -2, "size");
  lua_pushcfunction(L, read_bytes);
  lua_setfield(L, -2, "read_materialized");
  lua_pushcfunction(L, write_bytes);
  lua_setfield(L, -2, "write");
  lua_pushcfunction(L, borrow);
  lua_setfield(L, -2, "borrow");
  lua_pushcfunction(L, lock_writes);
  lua_setfield(L, -2, "lock_writes");
  {
    lua_pushboolean(L, 0);
    lua_pushcclosure(L, data_capability, 1);
    lua_setfield(L, -2, "data");
    lua_pushboolean(L, 1);
    lua_pushcclosure(L, data_capability, 1);
    lua_setfield(L, -2, "data_mutable");
  }
  if (!span) {
    lua_pushcfunction(L, resize);
    lua_setfield(L, -2, "resize");
  }
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
}
int luaopen_vectis_buffer_core(lua_State *L) {
  luaL_newmetatable(L, WRITE_LOCK);
  lua_pushcfunction(L, close_lock);
  lua_setfield(L, -2, "__gc");
  lua_pushcfunction(L, close_lock);
  lua_setfield(L, -2, "__close");
  lua_newtable(L);
  lua_pushcfunction(L, close_lock);
  lua_setfield(L, -2, "close");
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
  register_type(L, BUFFER, 0);
  register_type(L, SPAN, 1);
  lua_newtable(L);
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, create, 1);
  lua_setfield(L, -2, "new");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, create, 1);
  lua_setfield(L, -2, "from_materialized");
  lua_pushinteger(L, VECTIS_BUFFER_CHUNK_BYTES);
  lua_setfield(L, -2, "CHUNK_BYTES");
  return 1;
}
vectis_status vectis_lua_buffer_acquire_span(lua_State *L, int index,
                                             int readonly,
                                             vectis_buffer_span **out,
                                             vectis_error *error) {
  buffer_owner *owner = (buffer_owner *)luaL_testudata(L, index, BUFFER);
  size_t size;
  vectis_status status;
  if (out)
    *out = NULL;
  if (!owner)
    owner = (buffer_owner *)luaL_testudata(L, index, SPAN);
  if (!owner || owner->transfer_pending || (!owner->buffer && !owner->span)) {
    if (error) {
      memset(error, 0, sizeof(*error));
      error->code = VECTIS_ERR_STATE;
      error->source = VECTIS_ERROR_SOURCE_VECTIS;
      strcpy(error->message, "expected a live byte buffer or span");
    }
    return VECTIS_ERR_STATE;
  }
  status = owner->buffer ? vectis_buffer_size(owner->buffer, &size, error)
                         : vectis_buffer_span_size(owner->span, &size, error);
  if (status != VECTIS_OK)
    return status;
  return owner->buffer ? vectis_buffer_borrow(owner->buffer, 0u, size, readonly,
                                              out, error)
                       : vectis_buffer_span_borrow(owner->span, 0u, size,
                                                   readonly, out, error);
}

static vectis_status owned_error(vectis_error *error, const char *message) {
  if (error) {
    memset(error, 0, sizeof(*error));
    error->code = VECTIS_ERR_STATE;
    error->source = VECTIS_ERROR_SOURCE_VECTIS;
    strncpy(error->message, message, sizeof(error->message) - 1u);
  }
  return VECTIS_ERR_STATE;
}
vectis_status vectis_lua_buffer_owned_info(lua_State *L, int index,
                                           size_t required, void **data,
                                           size_t *size, vectis_error *error) {
  buffer_owner *owner = (buffer_owner *)luaL_testudata(L, index, BUFFER);
  vectis_status status;
  if (data)
    *data = NULL;
  if (size)
    *size = 0u;
  if (!owner || !owner->buffer || owner->transfer_pending)
    return owned_error(error, "expected an owned live byte buffer");
  status = vectis_buffer_data_mutable(owner->buffer, data, size, error);
  if (status != VECTIS_OK)
    return status;
  if (*size < required)
    return owned_error(error, "allocation buffer is smaller than requested");
  return VECTIS_OK;
}
vectis_status vectis_lua_buffer_take(lua_State *L, int index, size_t required,
                                     vectis_buffer **out, vectis_error *error) {
  buffer_owner *owner;
  void *data;
  size_t size;
  vectis_status status;
  if (out)
    *out = NULL;
  if (!out)
    return owned_error(error, "buffer transfer output is required");
  status =
      vectis_lua_buffer_owned_info(L, index, required, &data, &size, error);
  if (status != VECTIS_OK)
    return status;
  owner = (buffer_owner *)luaL_testudata(L, index, BUFFER);
  *out = owner->buffer;
  owner->buffer = NULL;
  return VECTIS_OK;
}
void *vectis_lua_buffer_push_scope(lua_State *L) { return new_owner(L, 1); }
vectis_status vectis_lua_buffer_scope_init(void *token, vectis_buffer *buffer,
                                           size_t size, int readonly,
                                           vectis_error *error) {
  buffer_owner *owner = (buffer_owner *)token;
  return vectis_buffer_borrow(buffer, 0u, size, readonly, &owner->span, error);
}
void vectis_lua_buffer_scope_close(void *token) {
  buffer_owner *owner = (buffer_owner *)token;
  if (owner) {
    vectis_buffer_span_close(owner->span);
    owner->span = NULL;
  }
}
void vectis_lua_buffer_scope_adopt_span(void *token, vectis_buffer_span *span) {
  buffer_owner *owner = (buffer_owner *)token;
  vectis_buffer_span_close(owner->span);
  owner->span = span;
}

vectis_status vectis_lua_buffer_prepare_take(lua_State *L, int index,
                                             size_t required, void **token,
                                             vectis_buffer **out,
                                             vectis_error *error) {
  buffer_owner *owner;
  void *data;
  size_t size;
  vectis_status status;
  if (token)
    *token = NULL;
  if (out)
    *out = NULL;
  if (!token || !out)
    return owned_error(error, "buffer transfer outputs are required");
  status =
      vectis_lua_buffer_owned_info(L, index, required, &data, &size, error);
  if (status != VECTIS_OK)
    return status;
  owner = (buffer_owner *)luaL_testudata(L, index, BUFFER);
  owner->transfer_pending = 1;
  *token = owner;
  *out = owner->buffer;
  return VECTIS_OK;
}
vectis_buffer *vectis_lua_buffer_finish_take(void *token, int commit) {
  buffer_owner *owner = (buffer_owner *)token;
  vectis_buffer *buffer;
  if (!owner || !owner->transfer_pending)
    return NULL;
  buffer = owner->buffer;
  owner->transfer_pending = 0;
  if (commit)
    owner->buffer = NULL;
  return commit ? buffer : NULL;
}
vectis_status vectis_lua_buffer_close_resource(lua_State *L, int index,
                                               vectis_error *error) {
  buffer_owner *owner = (buffer_owner *)luaL_testudata(L, index, BUFFER);
  if (!owner)
    owner = (buffer_owner *)luaL_testudata(L, index, SPAN);
  if (!owner || owner->transfer_pending)
    return owned_error(error,
                       "expected a byte resource without pending transfer");
  vectis_buffer_close(owner->buffer);
  owner->buffer = NULL;
  vectis_buffer_span_close(owner->span);
  owner->span = NULL;
  return VECTIS_OK;
}
