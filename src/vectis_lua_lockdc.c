#include "vectis_lua_lockdc.h"
#include <lauxlib.h>
#include <lc/lc.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <vectis/storage.h>
#include <vectis/supervisor.h>

typedef struct open_scope {
  vectis_app *app;
  const char **endpoints;
  size_t count;
  int consumed, muted;
} open_scope;
static pthread_key_t open_key;
static pthread_once_t open_once = PTHREAD_ONCE_INIT;
static int open_key_status;
static void open_key_init(void) {
  open_key_status = pthread_key_create(&open_key, NULL);
}
int vectis_lua_lockdc_client_open(const lc_client_config *, lc_client **,
                                  lc_error *);
/* Only the imported lockdc Lua object redirects lc_client_open. Every other
 * caller, and every unscoped direct lockdc.open, keeps the public SDK behavior.
 */
int vectis_lua_lockdc_client_open(const lc_client_config *config,
                                  lc_client **out, lc_error *error) {
  open_scope *scope;
  lc_client_config effective;
  vectis_error failure;
  (void)pthread_once(&open_once, open_key_init);
  scope = open_key_status == 0 ? pthread_getspecific(open_key) : NULL;
  if (scope == NULL || scope->consumed)
    return lc_client_open(config, out, error);
  scope->consumed = 1;
  effective = *config;
  effective.endpoints = scope->endpoints;
  effective.endpoint_count = scope->count;
  if (vectis_app_configure_workflow_client(scope->app, &effective, &failure) !=
      VECTIS_OK) {
    if (out != NULL)
      *out = NULL;
    return LC_ERR_INVALID;
  }
  if (scope->muted)
    effective.logger = NULL;
  return lc_client_open(&effective, out, error);
}
static const char *string_at(lua_State *lua, int index) {
  size_t size;
  const char *value;
  luaL_checktype(lua, index, LUA_TSTRING);
  value = lua_tolstring(lua, index, &size);
  if (memchr(value, 0, size) != NULL)
    luaL_error(lua, "endpoint strings cannot contain NUL");
  return value;
}
int vectis_lua_lockdc_open(lua_State *lua, vectis_app *app) {
  open_scope scope;
  const char *endpoint_storage[64];
  void *previous;
  size_t i;
  int options, endpoints, retained, clone, result, base;
  memset(&scope, 0, sizeof(scope));
  scope.app = app;
  luaL_checktype(lua, 2, LUA_TTABLE);
  lua_getfield(lua, 2, "logger");
  if (!lua_isnil(lua, -1))
    return luaL_error(lua, "workflow clients inherit the app logger; "
                           "standalone logger ownership is unsupported");
  lua_pop(lua, 1);
  lua_getfield(lua, 2, "logger_disabled");
  if (!lua_isnil(lua, -1))
    luaL_checktype(lua, -1, LUA_TBOOLEAN);
  scope.muted = lua_toboolean(lua, -1);
  lua_pop(lua, 1);
  /* Load before setting TLS: loader side effects cannot inherit this scope. */
  lua_getglobal(lua, "require");
  lua_pushliteral(lua, "lockdc");
  lua_call(lua, 1, 1);
  lua_getfield(lua, -1, "open");
  luaL_checktype(lua, -1, LUA_TFUNCTION);
  lua_remove(lua, -2);
  base = lua_gettop(lua) - 1;
  lua_newtable(lua);
  options = lua_gettop(lua);
  lua_pushnil(lua);
  while (lua_next(lua, 2)) {
    lua_pushvalue(lua, -2);
    lua_pushvalue(lua, -2);
    lua_rawset(lua, options);
    lua_pop(lua, 1);
  }
  lua_getfield(lua, 2, "endpoints");
  endpoints = lua_gettop(lua);
  if (!lua_isnil(lua, endpoints))
    luaL_checktype(lua, endpoints, LUA_TTABLE);
  scope.count = lua_isnil(lua, endpoints) ? 0u : lua_rawlen(lua, endpoints);
  if (scope.count > 64u)
    return luaL_error(lua, "at most 64 endpoints supported");
  lua_newtable(lua);
  retained = lua_gettop(lua);
  lua_newtable(lua);
  clone = lua_gettop(lua);
  for (i = 0; i < scope.count; ++i) {
    const char *value, *query;
    lua_rawgeti(lua, endpoints, (lua_Integer)i + 1);
    value = string_at(lua, -1);
    query = strncmp(value, "pouch://", 8u) == 0 ? strchr(value, '?') : NULL;
    if (query != NULL)
      lua_pushlstring(lua, value, (size_t)(query - value));
    else
      lua_pushvalue(lua, -1);
    lua_rawseti(lua, clone, (lua_Integer)i + 1);
    lua_pushvalue(lua, -1);
    lua_rawseti(lua, retained, (lua_Integer)i + 1);
    lua_pop(lua, 1);
  }
  lua_setfield(lua, options, "endpoints");
  (void)pthread_once(&open_once, open_key_init);
  if (open_key_status != 0)
    return luaL_error(lua, "lockdc open scope initialization failed");
  scope.endpoints = endpoint_storage;
  for (i = 0; i < scope.count; ++i) {
    lua_rawgeti(lua, retained, (lua_Integer)i + 1);
    scope.endpoints[i] = lua_tostring(lua, -1);
    lua_pop(lua, 1);
  }
  lua_remove(lua, endpoints);
  /* Pin immutable originals below the protected call. Source callbacks may
   * mutate the caller's endpoint array and collect its former strings. */
  lua_insert(lua, base + 1);
  ++base;
  previous = pthread_getspecific(open_key);
  if (pthread_setspecific(open_key, &scope) != 0) {
    return luaL_error(lua, "lockdc open scope failed");
  }
  result = lua_pcall(lua, 1, LUA_MULTRET, 0);
  (void)pthread_setspecific(open_key, previous);
  if (result != LUA_OK)
    return lua_error(lua); /* preserve table/string error */
  return lua_gettop(lua) - base;
}
static int push_error(lua_State *lua, int code, lc_error *error) {
  lua_pushnil(lua);
  lua_newtable(lua);
  lua_pushinteger(lua, code);
  lua_setfield(lua, -2, "code");
  lua_pushstring(lua, error->message != NULL ? error->message
                                             : "Pouch endpoint error");
  lua_setfield(lua, -2, "message");
  lc_error_cleanup(error);
  return 2;
}
static int has_option(lua_State *lua) {
  lc_error error;
  int present, code;
  const char *endpoint = string_at(lua, 1), *name = string_at(lua, 2);
  lc_error_init(&error);
  code = lc_pouch_endpoint_has_option(endpoint, name, &present, &error);
  if (code != LC_OK)
    return push_error(lua, code, &error);
  lc_error_cleanup(&error);
  lua_pushboolean(lua, present);
  return 1;
}
static int build_endpoint(lua_State *lua) {
  lc_pouch_endpoint_option options[64];
  lc_error error;
  size_t i, count;
  char *endpoint = NULL;
  const char *root = string_at(lua, 1);
  int code, pinned;
  if (!lua_isnoneornil(lua, 2))
    luaL_checktype(lua, 2, LUA_TTABLE);
  count = lua_isnoneornil(lua, 2) ? 0u : lua_rawlen(lua, 2);
  if (count > 64u)
    return luaL_error(lua, "at most 64 endpoint options supported");
  lua_newtable(lua);
  pinned = lua_gettop(lua);
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 2, (lua_Integer)i + 1);
    luaL_checktype(lua, -1, LUA_TTABLE);
    lua_getfield(lua, -1, "name");
    options[i].name = string_at(lua, -1);
    lua_pushvalue(lua, -1);
    lua_rawseti(lua, pinned, (lua_Integer)(i * 2u + 1u));
    lua_getfield(lua, -2, "value");
    options[i].value = lua_isnil(lua, -1) ? NULL : string_at(lua, -1);
    lua_pushvalue(lua, -1);
    lua_rawseti(lua, pinned, (lua_Integer)(i * 2u + 2u));
    lua_pop(lua, 3);
  }
  lc_error_init(&error);
  code = lc_pouch_endpoint_build(root, options, count, &endpoint, &error);
  if (code != LC_OK) {
    lc_pouch_endpoint_free(endpoint);
    return push_error(lua, code, &error);
  }
  lua_pushstring(lua, endpoint);
  lc_pouch_endpoint_free(endpoint);
  lc_error_cleanup(&error);
  return 1;
}
static int wait_blocking(lua_State *lua) {
  lc_xa_wait wait;
  lua_Integer value;
  vectis_error error;
  luaL_checktype(lua, 1, LUA_TTABLE);
  lua_getfield(lua, 1, "fd");
  if (!lua_isinteger(lua, -1))
    return luaL_error(lua, "XA fd must be an integer");
  value = lua_tointeger(lua, -1);
  if (value < INT_MIN || value > INT_MAX)
    return luaL_error(lua, "XA fd outside int range");
  wait.fd = (int)value;
  lua_pop(lua, 1);
  lua_getfield(lua, 1, "events");
  if (!lua_isinteger(lua, -1))
    return luaL_error(lua, "XA events must be an integer");
  value = lua_tointeger(lua, -1);
  if (value < 0 || value > 3)
    return luaL_error(lua, "invalid XA events");
  wait.events = (unsigned)value;
  lua_pop(lua, 1);
  lua_getfield(lua, 1, "deadline_ms");
  if (!lua_isinteger(lua, -1))
    return luaL_error(lua, "XA deadline must be an integer");
  wait.deadline_ms = (int64_t)lua_tointeger(lua, -1);
  lua_pop(lua, 1);
  if (vectis_storage_wait_blocking(&wait, &error) != VECTIS_OK)
    return luaL_error(lua, "%s", error.message);
  lua_pushboolean(lua, 1);
  return 1;
}
int luaopen_vectis_lockd_core(lua_State *lua) {
  static const luaL_Reg methods[] = {{"pouch_endpoint_build", build_endpoint},
                                     {"pouch_endpoint_has_option", has_option},
                                     {"wait_blocking", wait_blocking},
                                     {NULL, NULL}};
  luaL_newlib(lua, methods);
  return 1;
}
