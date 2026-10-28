#include "vectis_lua_compression.h"
#include <lauxlib.h>
#include <limits.h>
#include <string.h>
#include <vectis/compression.h>

#define COMPRESSION_MT "vectis.compression.owner"
typedef struct lua_compression {
  vectis_compression *receiver;
  int busy, violation;
} lua_compression;
typedef struct compression_delivery {
  lua_State *lua;
  lua_compression *owner;
  const void *data;
  size_t size;
  int sink;
  int failed; /* 1: exception, 2: returned failure */
} compression_delivery;
static int compression_push_error(lua_State *lua, const vectis_error *error,
                                  const char *operation) {
  lua_pushnil(lua);
  lua_createtable(lua, 0, 5);
  lua_pushinteger(lua, error->code);
  lua_setfield(lua, -2, "status");
  lua_pushstring(lua, vectis_status_string(error->code));
  lua_setfield(lua, -2, "status_string");
  lua_pushinteger(lua, error->source);
  lua_setfield(lua, -2, "source_code");
  lua_pushstring(lua, vectis_error_source_string(error->source));
  lua_setfield(lua, -2, "source");
  lua_pushinteger(lua, error->dependency_code);
  lua_setfield(lua, -2, "dependency_code");
  lua_pushstring(lua, error->message);
  lua_setfield(lua, -2, "message");
  lua_pushstring(lua, operation);
  lua_setfield(lua, -2, "operation");
  return 2;
}
static lua_compression *compression_owner(lua_State *lua, int open) {
  lua_compression *owner =
      (lua_compression *)luaL_checkudata(lua, 1, COMPRESSION_MT);
  if (owner->busy) {
    owner->violation = 1;
    luaL_error(lua, "cannot reenter or close active compression receiver");
  }
  if (open && owner->receiver == NULL)
    luaL_error(lua, "compression receiver is closed");
  return owner;
}
static int compression_gc(lua_State *lua) {
  lua_compression *owner =
      (lua_compression *)luaL_testudata(lua, 1, COMPRESSION_MT);
  if (owner != NULL && owner->receiver != NULL && !owner->busy) {
    if (vectis_compression_close(owner->receiver, NULL) == VECTIS_OK)
      owner->receiver = NULL;
  }
  return 0;
}
static int compression_close(lua_State *lua) {
  lua_compression *owner = compression_owner(lua, 0);
  vectis_error error;
  if (owner->receiver != NULL) {
    if (vectis_compression_close(owner->receiver, &error) != VECTIS_OK)
      return compression_push_error(lua, &error, "close");
    owner->receiver = NULL;
  }
  lua_pushboolean(lua, 1);
  return 1;
}
static const char *compression_string(lua_State *lua, int index) {
  size_t size;
  const char *value;
  luaL_checktype(lua, index, LUA_TSTRING);
  value = lua_tolstring(lua, index, &size);
  if (strlen(value) != size)
    luaL_argerror(lua, index, "NUL byte is not allowed");
  return value;
}
static int compression_new(lua_State *lua) {
  lua_compression *owner;
  vectis_compression_config config;
  vectis_error error;
  const char *algorithm, *direction, *key;
  lua_Integer value;
  /* Install ownership before acquisition and before reading mutable options. */
  lua_settop(lua, 3);
  owner = (lua_compression *)lua_newuserdatauv(lua, sizeof(*owner), 0);
  memset(owner, 0, sizeof(*owner));
  luaL_getmetatable(lua, COMPRESSION_MT);
  lua_setmetatable(lua, -2);
  algorithm = compression_string(lua, 1);
  direction = compression_string(lua, 2);
  vectis_compression_config_init(&config);
  if (!lua_isnil(lua, 3)) {
    luaL_checktype(lua, 3, LUA_TTABLE);
    lua_pushnil(lua);
    while (lua_next(lua, 3) != 0) {
      key = compression_string(lua, -2);
      value = luaL_checkinteger(lua, -1);
      if (strcmp(key, "level") == 0) {
        if (strcmp(direction, "compress") != 0)
          return luaL_error(lua, "level applies only to compression");
        if (value < INT_MIN || value > INT_MAX)
          return luaL_error(lua, "level is out of range");
        config.level = (int)value;
        config.level_set = 1;
      } else if (strcmp(key, "max_window_log") == 0) {
        if (strcmp(algorithm, "zstd") != 0 ||
            strcmp(direction, "decompress") != 0)
          return luaL_error(
              lua, "max_window_log applies only to Zstandard decompression");
        if (value < 10 || value > 31)
          return luaL_error(lua, "max_window_log must be 10..31");
        config.max_window_log = (unsigned int)value;
      } else if (strcmp(key, "memory_limit") == 0) {
        if (strcmp(algorithm, "xz") != 0 ||
            strcmp(direction, "decompress") != 0)
          return luaL_error(lua,
                            "memory_limit applies only to XZ decompression");
        if (value <= 0 || (lua_Unsigned)value > (size_t)-1)
          return luaL_error(lua,
                            "memory_limit must fit size_t and be positive");
        config.memory_limit = (size_t)value;
      } else
        return luaL_error(lua, "unknown compression option: %s", key);
      lua_pop(lua, 1);
    }
  }
  if (vectis_compression_new(algorithm, direction, &config, &owner->receiver,
                             &error) != VECTIS_OK)
    return compression_push_error(lua, &error, "new");
  return 1;
}
/* Allocation and arbitrary Lua run wholly inside this protected frame. */
static int compression_deliver(lua_State *lua) {
  compression_delivery *delivery =
      (compression_delivery *)lua_touserdata(lua, 1);
  lua_pushvalue(lua, 2);
  lua_pushlstring(lua, (const char *)delivery->data, delivery->size);
  lua_call(lua, 1, 2);
  if (!lua_isboolean(lua, -2) || !lua_toboolean(lua, -2)) {
    if (lua_isnil(lua, -1)) {
      lua_pop(lua, 1);
      lua_pushliteral(lua, "compression sink must return true or nil,error");
    }
    delivery->failed = 2;
    return 1;
  }
  return 0;
}
static vectis_status compression_sink(void *context, const void *data,
                                      size_t size, vectis_error *error) {
  compression_delivery *delivery = (compression_delivery *)context;
  lua_State *lua = delivery->lua;
  delivery->data = data;
  delivery->size = size;
  lua_pushcfunction(lua, compression_deliver);
  lua_pushlightuserdata(lua, delivery);
  lua_pushvalue(lua, delivery->sink);
  if (lua_pcall(lua, 2, LUA_MULTRET, 0) != LUA_OK)
    delivery->failed = 1;
  if (delivery->failed || delivery->owner->violation) {
    memset(error, 0, sizeof(*error));
    error->code = VECTIS_ERR_STATE;
    error->source = VECTIS_ERROR_SOURCE_VECTIS;
    strcpy(error->message,
           "compression sink failed or attempted receiver reentry");
    return VECTIS_ERR_STATE;
  }
  return VECTIS_OK;
}
static int compression_operate(lua_State *lua, int finish) {
  lua_compression *owner;
  compression_delivery delivery;
  const char *input = NULL;
  size_t size = 0u;
  vectis_error error;
  vectis_status status;
  luaL_checktype(lua, finish ? 2 : 3, LUA_TFUNCTION);
  if (!finish) {
    luaL_checktype(lua, 2, LUA_TSTRING);
    input = lua_tolstring(lua, 2, &size);
  }
  lua_settop(lua, finish ? 2 : 3);
  /* Reserve stack before borrowing native state; allocation can run finalizers.
   */
  if (!lua_checkstack(lua, 12))
    return luaL_error(lua, "cannot reserve compression callback stack");
  owner = compression_owner(lua, 1);
  memset(&delivery, 0, sizeof(delivery));
  delivery.lua = lua;
  delivery.owner = owner;
  delivery.sink = finish ? 2 : 3;
  owner->busy = 1;
  owner->violation = 0;
  status =
      finish ? vectis_compression_finish(owner->receiver, compression_sink,
                                         &delivery, &error)
             : vectis_compression_process(owner->receiver, input, size,
                                          compression_sink, &delivery, &error);
  owner->busy = 0;
  if (delivery.failed == 1)
    return lua_error(lua);
  if (delivery.failed == 2) {
    lua_pushnil(lua);
    lua_insert(lua, -2);
    return 2;
  }
  if (status != VECTIS_OK)
    return compression_push_error(lua, &error, finish ? "finish" : "process");
  lua_pushboolean(lua, 1);
  return 1;
}
static int compression_process(lua_State *lua) {
  return compression_operate(lua, 0);
}
static int compression_finish(lua_State *lua) {
  return compression_operate(lua, 1);
}
int luaopen_vectis_compression_core(lua_State *lua) {
  static const luaL_Reg methods[] = {{"process", compression_process},
                                     {"finish", compression_finish},
                                     {"close", compression_close},
                                     {NULL, NULL}};
  if (luaL_newmetatable(lua, COMPRESSION_MT)) {
    lua_pushcfunction(lua, compression_gc);
    lua_setfield(lua, -2, "__gc");
    lua_pushcfunction(lua, compression_close);
    lua_setfield(lua, -2, "__close");
    lua_newtable(lua);
    luaL_setfuncs(lua, methods, 0);
    lua_setfield(lua, -2, "__index");
  }
  lua_pop(lua, 1);
  lua_newtable(lua);
  lua_pushcfunction(lua, compression_new);
  lua_setfield(lua, -2, "new");
  return 1;
}
