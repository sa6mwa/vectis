#include "vectis_lua_png.h"
#include <lauxlib.h>
#include <limits.h>
#include <string.h>
#include <vectis/png.h>

#define PNG_OWNER "vectis.png.owner"
#define PNG_MATERIALIZED "vectis.png.materialized"
typedef struct png_lua_owner {
  vectis_png *receiver;
  lua_State *lua;
  int busy, violation, failed, callback, writing;
  void *read_data;
  const void *write_data;
  size_t size, read_size;
} png_lua_owner;
typedef struct png_lua_materialized {
  vectis_png_materialized image;
  vectis_mutable_bytes encoded;
} png_lua_materialized;
static int png_lua_error(lua_State *L, const vectis_error *error,
                         const char *operation) {
  lua_pushnil(L);
  lua_createtable(L, 0, 8);
  lua_pushinteger(L, error->code);
  lua_setfield(L, -2, "status");
  lua_pushstring(L, vectis_status_string(error->code));
  lua_setfield(L, -2, "status_string");
  lua_pushinteger(L, error->source);
  lua_setfield(L, -2, "source_code");
  lua_pushstring(L, vectis_error_source_string(error->source));
  lua_setfield(L, -2, "source");
  lua_pushinteger(L, error->dependency_code);
  lua_setfield(L, -2, "dependency_code");
  lua_pushstring(L, error->message);
  lua_setfield(L, -2, "message");
  lua_pushstring(L, operation);
  lua_setfield(L, -2, "operation");
  return 2;
}
static const char *png_lua_string(lua_State *L, int index) {
  size_t size;
  const char *value;
  luaL_checktype(L, index, LUA_TSTRING);
  value = lua_tolstring(L, index, &size);
  if (memchr(value, 0, size) != NULL)
    luaL_error(L, "PNG string contains NUL");
  return value;
}
static void png_lua_fields(lua_State *L, int index, const char *const *fields) {
  const char *key;
  int i;
  index = lua_absindex(L, index);
  if (lua_isnoneornil(L, index))
    return;
  luaL_checktype(L, index, LUA_TTABLE);
  lua_pushnil(L);
  while (lua_next(L, index)) {
    key = png_lua_string(L, -2);
    for (i = 0; fields[i] != NULL && strcmp(key, fields[i]) != 0; ++i) {
    }
    if (fields[i] == NULL)
      luaL_error(L, "unknown PNG option '%s'", key);
    lua_pop(L, 1);
  }
}
static void png_lua_field(lua_State *L, int index, const char *name) {
  if (lua_isnoneornil(L, index)) {
    lua_pushnil(L);
    return;
  }
  index = lua_absindex(L, index);
  lua_pushstring(L, name);
  lua_rawget(L, index);
}
static size_t png_lua_size(lua_State *L, int index, size_t fallback) {
  lua_Integer value;
  if (lua_isnil(L, index))
    return fallback;
  value = luaL_checkinteger(L, index);
  if (value <= 0 || (lua_Unsigned)value > (size_t)-1)
    luaL_error(L, "PNG size must be positive and fit size_t");
  return (size_t)value;
}
static unsigned int png_lua_dimension(lua_State *L, int index,
                                      unsigned int fallback, int allow_zero) {
  lua_Integer value;
  if (lua_isnil(L, index))
    return fallback;
  value = luaL_checkinteger(L, index);
  if (value < (allow_zero ? 0 : 1) || (lua_Unsigned)value > UINT_MAX)
    luaL_error(L, "PNG dimension is out of range");
  return (unsigned int)value;
}
static void png_lua_limits(lua_State *L, int index, int materialized,
                           vectis_png_limits *limits) {
  static const char *const row_fields[] = {"max_width", "max_height",
                                           "max_row_bytes", NULL};
  static const char *const image_fields[] = {
      "max_width", "max_height", "max_row_bytes", "max_bytes", NULL};
  png_lua_fields(L, index, materialized ? image_fields : row_fields);
  vectis_png_limits_init(limits);
  png_lua_field(L, index, "max_width");
  limits->max_width = png_lua_dimension(L, -1, limits->max_width, 0);
  lua_pop(L, 1);
  png_lua_field(L, index, "max_height");
  limits->max_height = png_lua_dimension(L, -1, limits->max_height, 0);
  lua_pop(L, 1);
  png_lua_field(L, index, "max_row_bytes");
  limits->max_row_bytes = png_lua_size(L, -1, limits->max_row_bytes);
  lua_pop(L, 1);
  if (materialized) {
    png_lua_field(L, index, "max_bytes");
    limits->max_bytes = png_lua_size(L, -1, limits->max_bytes);
    lua_pop(L, 1);
  }
}
/* A private preallocated snapshot pins metadata strings across subsequent Lua
 * allocations/finalizers; no borrowed table value remains unrooted. */
static void png_lua_image(lua_State *L, int index, int materialized,
                          vectis_png_info *image, vectis_png_text *text) {
  static const char *const fields[] = {"width",     "height",     "format",
                                       "text",      "physical",   "pixels",
                                       "bit_depth", "color_type", "interlaced",
                                       "row_bytes", NULL};
  static const char *const physical_fields[] = {"x", "y", "unit", NULL};
  int snapshot, text_index, physical;
  const char *format, *unit;
  index = lua_absindex(L, index);
  luaL_checktype(L, index, LUA_TTABLE);
  png_lua_fields(L, index, fields);
  memset(image, 0, sizeof(*image));
  png_lua_field(L, index, "width");
  image->width = png_lua_dimension(L, -1, 0u, 0);
  lua_pop(L, 1);
  png_lua_field(L, index, "height");
  image->height = png_lua_dimension(L, -1, 0u, 0);
  lua_pop(L, 1);
  png_lua_field(L, index, "format");
  format = lua_isnil(L, -1) ? "rgba8" : png_lua_string(L, -1);
  image->format = strcmp(format, "rgba8") == 0   ? VECTIS_PNG_RGBA8
                  : strcmp(format, "rgb8") == 0  ? VECTIS_PNG_RGB8
                  : strcmp(format, "gray8") == 0 ? VECTIS_PNG_GRAY8
                                                 : 0;
  lua_pop(L, 1);
  lua_createtable(L, 32, 0);
  snapshot = lua_gettop(L);
  png_lua_field(L, index, "text");
  text_index = lua_gettop(L);
  if (!lua_isnil(L, text_index)) {
    luaL_checktype(L, text_index, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, text_index)) {
      if (image->text_count == 16u)
        luaL_error(L, "PNG text entry limit exceeded");
      text[image->text_count].key = png_lua_string(L, -2);
      text[image->text_count].value = png_lua_string(L, -1);
      lua_pushvalue(L, -2);
      lua_rawseti(L, snapshot, (lua_Integer)image->text_count * 2 + 1);
      lua_pushvalue(L, -1);
      lua_rawseti(L, snapshot, (lua_Integer)image->text_count * 2 + 2);
      ++image->text_count;
      lua_pop(L, 1);
    }
  }
  image->text = text;
  png_lua_field(L, index, "physical");
  physical = lua_gettop(L);
  if (!lua_isnil(L, physical)) {
    png_lua_fields(L, physical, physical_fields);
    image->physical_present = 1;
    png_lua_field(L, physical, "x");
    image->physical_x = png_lua_dimension(L, -1, 0u, 1);
    lua_pop(L, 1);
    png_lua_field(L, physical, "y");
    image->physical_y = png_lua_dimension(L, -1, 0u, 1);
    lua_pop(L, 1);
    png_lua_field(L, physical, "unit");
    unit = lua_isnil(L, -1) ? "unknown" : png_lua_string(L, -1);
    if (strcmp(unit, "unknown") != 0 && strcmp(unit, "meter") != 0)
      luaL_error(L, "PNG physical unit must be meter or unknown");
    image->physical_meter = strcmp(unit, "meter") == 0;
    lua_pop(L, 1);
  }
  if (!materialized) {
    png_lua_field(L, index, "pixels");
    if (!lua_isnil(L, -1))
      luaL_error(L, "row writer image must not contain materialized pixels");
    lua_pop(L, 1);
  }
}
static png_lua_owner *png_lua_owner_check(lua_State *L, int open) {
  png_lua_owner *owner = (png_lua_owner *)luaL_checkudata(L, 1, PNG_OWNER);
  if (owner->busy) {
    owner->violation = 1;
    luaL_error(L, "cannot reenter or close active PNG receiver");
  }
  if (open && owner->receiver == NULL)
    luaL_error(L, "PNG receiver is closed");
  return owner;
}
static int png_lua_gc(lua_State *L) {
  png_lua_owner *owner = (png_lua_owner *)luaL_testudata(L, 1, PNG_OWNER);
  if (owner != NULL && owner->receiver != NULL && !owner->busy) {
    vectis_png_close(owner->receiver, NULL);
    owner->receiver = NULL;
  }
  return 0;
}
static int png_lua_close(lua_State *L) {
  png_lua_owner *owner = png_lua_owner_check(L, 0);
  vectis_error error;
  vectis_status status = VECTIS_OK;
  if (owner->receiver != NULL) {
    status = vectis_png_close(owner->receiver, &error);
    owner->receiver = NULL;
  }
  if (status != VECTIS_OK)
    return png_lua_error(L, &error, "close");
  lua_pushboolean(L, 1);
  return 1;
}
static int png_lua_callback(lua_State *L) {
  png_lua_owner *owner = (png_lua_owner *)lua_touserdata(L, 1);
  const char *bytes;
  size_t size;
  lua_pushvalue(L, 2);
  if (owner->writing)
    lua_pushlstring(L, (const char *)owner->write_data, owner->size);
  else
    lua_pushinteger(L, (lua_Integer)owner->size);
  lua_call(L, 1, 2);
  if (owner->writing) {
    if (lua_isboolean(L, -2) && lua_toboolean(L, -2))
      return 0;
  } else {
    if (lua_type(L, -2) == LUA_TSTRING) {
      bytes = lua_tolstring(L, -2, &size);
      if (size == 0u || size > owner->size)
        return luaL_error(L, "PNG read callback must return 1..capacity bytes");
      memcpy(owner->read_data, bytes, size);
      owner->read_size = size;
      return 0;
    }
    if (lua_isnil(L, -2) && lua_isnil(L, -1)) {
      owner->read_size = 0u;
      return 0;
    }
  }
  if (lua_isnil(L, -1))
    return luaL_error(L, "PNG callback must return data/true or nil,error");
  owner->failed = 2;
  return 1;
}
static vectis_status png_lua_call_user(png_lua_owner *owner,
                                       vectis_error *error) {
  lua_State *L = owner->lua;
  lua_pushcfunction(L, png_lua_callback);
  lua_pushlightuserdata(L, owner);
  lua_pushvalue(L, owner->callback);
  if (lua_pcall(L, 2, LUA_MULTRET, 0) != LUA_OK)
    owner->failed = 1;
  if (owner->failed || owner->violation) {
    memset(error, 0, sizeof(*error));
    error->code = VECTIS_ERR_STATE;
    error->source = VECTIS_ERROR_SOURCE_VECTIS;
    strcpy(error->message, "PNG callback failed or attempted receiver reentry");
    return VECTIS_ERR_STATE;
  }
  return VECTIS_OK;
}
static vectis_status png_lua_read(void *context, void *data, size_t capacity,
                                  size_t *size, vectis_error *error) {
  png_lua_owner *owner = (png_lua_owner *)context;
  vectis_status status;
  owner->read_data = data;
  owner->size = capacity;
  owner->read_size = 0u;
  status = png_lua_call_user(owner, error);
  *size = owner->read_size;
  return status;
}
static vectis_status png_lua_write(void *context, const void *data, size_t size,
                                   vectis_error *error) {
  png_lua_owner *owner = (png_lua_owner *)context;
  owner->write_data = data;
  owner->size = size;
  return png_lua_call_user(owner, error);
}
static int png_lua_callback_failure(lua_State *L, png_lua_owner *owner) {
  if (owner->failed == 1)
    return lua_error(L);
  lua_pushnil(L);
  lua_insert(L, -2);
  return 2;
}
static int png_lua_new(lua_State *L, int writing) {
  png_lua_owner *owner;
  vectis_png_limits limits;
  vectis_png_info image;
  vectis_png_text text[16];
  vectis_error error;
  vectis_status status;
  const char *path = NULL;
  int result;
  lua_settop(L, writing ? 3 : 2);
  if (!lua_checkstack(L, 64))
    return luaL_error(L, "cannot reserve PNG constructor stack");
  owner = (png_lua_owner *)lua_newuserdatauv(L, sizeof(*owner), 1);
  memset(owner, 0, sizeof(*owner));
  result = lua_gettop(L);
  luaL_getmetatable(L, PNG_OWNER);
  lua_setmetatable(L, result);
  if (lua_type(L, 1) == LUA_TSTRING)
    path = png_lua_string(L, 1);
  else {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, result, 1);
  }
  png_lua_limits(L, writing ? 3 : 2, 0, &limits);
  if (writing)
    png_lua_image(L, 2, 0, &image, text);
  owner->busy = 1;
  owner->lua = L;
  owner->callback = 1;
  owner->writing = writing;
  if (writing)
    status = path != NULL
                 ? vectis_png_writer_file(path, &image, &limits,
                                          &owner->receiver, &error)
                 : vectis_png_writer(png_lua_write, owner, &image, &limits,
                                     &owner->receiver, &error);
  else
    status = path != NULL ? vectis_png_reader_file(path, &limits,
                                                   &owner->receiver, &error)
                          : vectis_png_reader(png_lua_read, owner, &limits,
                                              &owner->receiver, &error);
  owner->busy = 0;
  owner->lua = NULL;
  if (owner->failed)
    return png_lua_callback_failure(L, owner);
  if (status != VECTIS_OK)
    return png_lua_error(L, &error, writing ? "writer" : "reader");
  lua_pushvalue(L, result);
  return 1;
}
static int png_lua_reader(lua_State *L) { return png_lua_new(L, 0); }
static int png_lua_writer(lua_State *L) { return png_lua_new(L, 1); }
static void png_lua_push_info(lua_State *L, const vectis_png_info *info) {
  size_t i;
  lua_createtable(L, 0, 10);
  lua_pushinteger(L, info->width);
  lua_setfield(L, -2, "width");
  lua_pushinteger(L, info->height);
  lua_setfield(L, -2, "height");
  lua_pushinteger(L, info->row_bytes);
  lua_setfield(L, -2, "row_bytes");
  lua_pushinteger(L, info->bit_depth);
  lua_setfield(L, -2, "bit_depth");
  lua_pushinteger(L, info->color_type);
  lua_setfield(L, -2, "color_type");
  lua_pushboolean(L, info->interlaced);
  lua_setfield(L, -2, "interlaced");
  lua_pushstring(L, info->format == VECTIS_PNG_RGBA8  ? "rgba8"
                    : info->format == VECTIS_PNG_RGB8 ? "rgb8"
                                                      : "gray8");
  lua_setfield(L, -2, "format");
  lua_createtable(L, 0, (int)info->text_count);
  for (i = 0u; i < info->text_count; ++i) {
    lua_pushstring(L, info->text[i].value);
    lua_setfield(L, -2, info->text[i].key);
  }
  lua_setfield(L, -2, "text");
  if (info->physical_present) {
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, info->physical_x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, info->physical_y);
    lua_setfield(L, -2, "y");
    lua_pushstring(L, info->physical_meter ? "meter" : "unknown");
    lua_setfield(L, -2, "unit");
    lua_setfield(L, -2, "physical");
  }
}
static int png_lua_dispatch(lua_State *L) {
  png_lua_owner *owner = (png_lua_owner *)lua_touserdata(L, 1);
  int operation = (int)lua_tointeger(L, 2);
  const char *input;
  const void *row;
  size_t size;
  vectis_error error;
  vectis_status status;
  vectis_png_info info;
  owner->lua = L;
  owner->callback = 4;
  if (operation == 1) {
    status = vectis_png_next_row(owner->receiver, &row, &size, &error);
    if (owner->failed)
      return png_lua_callback_failure(L, owner);
    if (status != VECTIS_OK)
      return png_lua_error(L, &error, "next_row");
    if (row == NULL) {
      lua_pushnil(L);
      lua_pushnil(L);
      return 2;
    }
    lua_pushlstring(L, (const char *)row, size);
    return 1;
  }
  if (operation == 2) {
    input = lua_tolstring(L, 3, &size);
    status = vectis_png_write_row(owner->receiver, input, size, &error);
  } else if (operation == 3)
    status = vectis_png_finish(owner->receiver, &error);
  else
    status = vectis_png_get_info(owner->receiver, &info, &error);
  if (owner->failed)
    return png_lua_callback_failure(L, owner);
  if (status != VECTIS_OK)
    return png_lua_error(L, &error,
                         operation == 2   ? "write_row"
                         : operation == 3 ? "finish"
                                          : "info");
  if (operation == 4) {
    png_lua_push_info(L, &info);
    return 1;
  }
  lua_pushboolean(L, 1);
  return 1;
}
static int png_lua_operate(lua_State *L, int operation) {
  png_lua_owner *owner;
  int status, base;
  if (operation == 2)
    luaL_checktype(L, 2, LUA_TSTRING);
  lua_settop(L, 2);
  base = lua_gettop(L);
  if (!lua_checkstack(L, 16))
    return luaL_error(L, "cannot reserve PNG operation stack");
  owner = png_lua_owner_check(L, 1);
  lua_pushcfunction(L, png_lua_dispatch);
  lua_pushvalue(L, 1);
  lua_pushinteger(L, operation);
  lua_pushvalue(L, 2);
  lua_getiuservalue(L, 1, 1);
  owner->busy = 1;
  owner->failed = owner->violation = 0;
  status = lua_pcall(L, 4, LUA_MULTRET, 0);
  owner->busy = 0;
  owner->lua = NULL;
  if (status != LUA_OK) {
    vectis_png_close(owner->receiver, NULL);
    owner->receiver = NULL;
    return lua_error(L);
  }
  return lua_gettop(L) - base;
}
static int png_lua_next_row(lua_State *L) { return png_lua_operate(L, 1); }
static int png_lua_write_row(lua_State *L) { return png_lua_operate(L, 2); }
static int png_lua_finish(lua_State *L) { return png_lua_operate(L, 3); }
static int png_lua_info(lua_State *L) { return png_lua_operate(L, 4); }
static int png_lua_materialized_gc(lua_State *L) {
  png_lua_materialized *owner = (png_lua_materialized *)lua_touserdata(L, 1);
  vectis_png_materialized_cleanup(&owner->image);
  vectis_mutable_bytes_cleanup(&owner->encoded);
  return 0;
}
static int png_lua_materialized_call(lua_State *L, int writing) {
  png_lua_materialized *owner;
  vectis_png_limits limits;
  vectis_png_info image;
  vectis_png_text text[16];
  vectis_error error;
  const char *pixels;
  size_t size;
  vectis_status status;
  lua_settop(L, 2);
  if (!lua_checkstack(L, 64))
    return luaL_error(L, "cannot reserve materialized PNG stack");
  owner = (png_lua_materialized *)lua_newuserdatauv(L, sizeof(*owner), 0);
  memset(owner, 0, sizeof(*owner));
  luaL_getmetatable(L, PNG_MATERIALIZED);
  lua_setmetatable(L, -2);
  lua_toclose(L, -1);
  png_lua_limits(L, 2, 1, &limits);
  if (writing) {
    png_lua_image(L, 1, 1, &image, text);
    png_lua_field(L, 1, "pixels");
    luaL_checktype(L, -1, LUA_TSTRING);
    pixels = lua_tolstring(L, -1, &size);
    status = vectis_png_encode_materialized(&image, pixels, size, &limits,
                                            &owner->encoded, &error);
  } else {
    luaL_checktype(L, 1, LUA_TSTRING);
    pixels = lua_tolstring(L, 1, &size);
    status = vectis_png_decode_materialized(pixels, size, &limits,
                                            &owner->image, &error);
  }
  if (status != VECTIS_OK)
    return png_lua_error(
        L, &error, writing ? "encode_materialized" : "decode_materialized");
  if (writing) {
    lua_pushlstring(L, (const char *)owner->encoded.data, owner->encoded.size);
    return 1;
  }
  png_lua_push_info(L, &owner->image.info);
  lua_pushlstring(L, (const char *)owner->image.pixels.data,
                  owner->image.pixels.size);
  lua_setfield(L, -2, "pixels");
  return 1;
}
static int png_lua_encode(lua_State *L) {
  return png_lua_materialized_call(L, 1);
}
static int png_lua_decode(lua_State *L) {
  return png_lua_materialized_call(L, 0);
}
int luaopen_vectis_png_core(lua_State *L) {
  static const luaL_Reg methods[] = {
      {"info", png_lua_info},           {"next_row", png_lua_next_row},
      {"write_row", png_lua_write_row}, {"finish", png_lua_finish},
      {"close", png_lua_close},         {NULL, NULL}};
  static const luaL_Reg functions[] = {{"reader", png_lua_reader},
                                       {"writer", png_lua_writer},
                                       {"encode_materialized", png_lua_encode},
                                       {"decode_materialized", png_lua_decode},
                                       {NULL, NULL}};
  if (luaL_newmetatable(L, PNG_OWNER)) {
    lua_pushcfunction(L, png_lua_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, png_lua_close);
    lua_setfield(L, -2, "__close");
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__index");
  }
  lua_pop(L, 1);
  if (luaL_newmetatable(L, PNG_MATERIALIZED)) {
    lua_pushcfunction(L, png_lua_materialized_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, png_lua_materialized_gc);
    lua_setfield(L, -2, "__close");
  }
  lua_pop(L, 1);
  luaL_newlib(L, functions);
  return 1;
}
