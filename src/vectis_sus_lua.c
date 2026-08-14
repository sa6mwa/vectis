#include <cpkt/sus.h>
#include <lauxlib.h>
#include <lua.h>
#include <stddef.h>
#include <string.h>

#define VECTIS_SUS_MODEL "sus.model"

typedef struct vectis_sus_model_lua {
  cpkt_sus *sus;
} vectis_sus_model_lua;

typedef struct vectis_sus_cache_status_lua {
  lua_State *lua;
  int ref;
} vectis_sus_cache_status_lua;

int luaopen_sus(lua_State *lua);

static void vectis_sus_lua_set_string(lua_State *lua, const char *key,
                                      const char *value) {
  lua_pushstring(lua, value != NULL ? value : "");
  lua_setfield(lua, -2, key);
}

static int vectis_sus_lua_push_error(lua_State *lua, cpkt_sus_result result,
                                     const char *context) {
  const char *result_string;

  result_string = cpkt_sus_result_string(result);
  lua_pushnil(lua);
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)result);
  lua_setfield(lua, -2, "result");
  vectis_sus_lua_set_string(lua, "result_string", result_string);
  if (context != NULL && context[0] != '\0') {
    lua_pushfstring(lua, "%s: %s", context,
                    result_string != NULL ? result_string : "sus error");
  } else {
    lua_pushstring(lua, result_string != NULL ? result_string : "sus error");
  }
  lua_setfield(lua, -2, "message");
  return 2;
}

static int vectis_sus_lua_table_bool(lua_State *lua, int index,
                                     const char *field, int fallback) {
  int value;

  if (index < 0) {
    index = lua_gettop(lua) + index + 1;
  }
  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return fallback;
  }
  value = lua_toboolean(lua, -1) ? 1 : 0;
  lua_pop(lua, 1);
  return value;
}

static const char *vectis_sus_lua_table_string(lua_State *lua, int index,
                                               const char *field) {
  const char *value;

  if (index < 0) {
    index = lua_gettop(lua) + index + 1;
  }
  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return NULL;
  }
  value = luaL_checkstring(lua, -1);
  lua_pop(lua, 1);
  return value;
}

static int vectis_sus_lua_table_function_ref(lua_State *lua, int index,
                                             const char *field) {
  int ref;

  if (index < 0) {
    index = lua_gettop(lua) + index + 1;
  }
  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return LUA_NOREF;
  }
  luaL_checktype(lua, -1, LUA_TFUNCTION);
  ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  return ref;
}

static int vectis_sus_lua_cache_status_sink(
    const cpkt_sus_cache_status_event *event, void *user) {
  vectis_sus_cache_status_lua *status;
  lua_State *lua;
  int top;
  int failed;

  status = (vectis_sus_cache_status_lua *)user;
  lua = status->lua;
  top = lua_gettop(lua);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, status->ref);
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)event->phase);
  lua_setfield(lua, -2, "phase");
  vectis_sus_lua_set_string(lua, "model", event->model);
  vectis_sus_lua_set_string(lua, "cache_path", event->cache_path);
  vectis_sus_lua_set_string(lua, "source_url", event->source_url);
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    lua_settop(lua, top);
    return -1;
  }
  failed = 0;
  if (lua_type(lua, -1) == LUA_TNUMBER) {
    failed = lua_tointeger(lua, -1) == 0 ? 0 : -1;
  } else if (lua_type(lua, -1) == LUA_TBOOLEAN) {
    failed = lua_toboolean(lua, -1) ? 0 : -1;
  }
  lua_settop(lua, top);
  return failed;
}

static void vectis_sus_lua_push_model_entry(
    lua_State *lua, const cpkt_sus_model_entry *entry) {
  lua_newtable(lua);
  vectis_sus_lua_set_string(lua, "name", entry->name);
  vectis_sus_lua_set_string(lua, "provider", entry->provider);
  vectis_sus_lua_set_string(lua, "source_url", entry->source_url);
  vectis_sus_lua_set_string(lua, "filename", entry->filename);
  vectis_sus_lua_set_string(lua, "sha256", entry->sha256);
  lua_pushinteger(lua, (lua_Integer)entry->size_bytes);
  lua_setfield(lua, -2, "size_bytes");
  vectis_sus_lua_set_string(lua, "license", entry->license);
  vectis_sus_lua_set_string(lua, "quantization", entry->quantization);
  lua_pushboolean(lua, entry->is_default != 0);
  lua_setfield(lua, -2, "is_default");
}

static vectis_sus_model_lua *vectis_sus_lua_new_model(lua_State *lua,
                                                      cpkt_sus *sus) {
  vectis_sus_model_lua *handle;

  handle = (vectis_sus_model_lua *)lua_newuserdatauv(lua, sizeof(*handle), 0);
  handle->sus = sus;
  luaL_getmetatable(lua, VECTIS_SUS_MODEL);
  lua_setmetatable(lua, -2);
  return handle;
}

static vectis_sus_model_lua *vectis_sus_lua_check_model(lua_State *lua,
                                                        int index) {
  return (vectis_sus_model_lua *)luaL_checkudata(lua, index, VECTIS_SUS_MODEL);
}

static int vectis_sus_lua_model_close(lua_State *lua) {
  vectis_sus_model_lua *handle;

  handle = vectis_sus_lua_check_model(lua, 1);
  if (handle->sus != NULL) {
    handle->sus->destroy(handle->sus);
    handle->sus = NULL;
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_sus_lua_model_info(lua_State *lua) {
  vectis_sus_model_lua *handle;
  cpkt_sus_info info;
  cpkt_sus_result result;

  handle = vectis_sus_lua_check_model(lua, 1);
  if (handle->sus == NULL) {
    return luaL_error(lua, "sus model is closed");
  }
  memset(&info, 0, sizeof(info));
  result = handle->sus->info(handle->sus, &info);
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result, "sus model info");
  }
  lua_newtable(lua);
  vectis_sus_lua_set_string(lua, "backend_version", info.backend_version);
  vectis_sus_lua_set_string(lua, "backend_system_info",
                            info.backend_system_info);
  lua_pushboolean(lua, info.cpu_only != 0);
  lua_setfield(lua, -2, "cpu_only");
  return 1;
}

static int vectis_sus_lua_model_reset_spacing(lua_State *lua) {
  vectis_sus_model_lua *handle;
  cpkt_sus_result result;

  handle = vectis_sus_lua_check_model(lua, 1);
  if (handle->sus == NULL) {
    return luaL_error(lua, "sus model is closed");
  }
  result = handle->sus->reset_transcript_spacing(handle->sus);
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result,
                                    "sus reset_transcript_spacing");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_sus_lua_result_string(lua_State *lua) {
  lua_pushstring(
      lua, cpkt_sus_result_string((cpkt_sus_result)luaL_checkinteger(lua, 1)));
  return 1;
}

static int vectis_sus_lua_backend_version(lua_State *lua) {
  lua_pushstring(lua, cpkt_sus_backend_version());
  return 1;
}

static int vectis_sus_lua_backend_system_info(lua_State *lua) {
  lua_pushstring(lua, cpkt_sus_backend_system_info());
  return 1;
}

static int vectis_sus_lua_backend_capabilities(lua_State *lua) {
  lua_pushstring(lua, cpkt_sus_backend_capabilities());
  return 1;
}

static int vectis_sus_lua_facade_version(lua_State *lua) {
  lua_pushstring(lua, cpkt_sus_facade_version());
  return 1;
}

static int vectis_sus_lua_catalog_count(lua_State *lua) {
  lua_pushinteger(lua, (lua_Integer)cpkt_sus_model_catalog_count());
  return 1;
}

static int vectis_sus_lua_catalog_entry(lua_State *lua) {
  cpkt_sus_model_entry entry;
  cpkt_sus_result result;
  lua_Integer index;

  index = luaL_checkinteger(lua, 1);
  if (index < 1) {
    return luaL_error(lua, "sus catalog index is 1-based");
  }
  memset(&entry, 0, sizeof(entry));
  result = cpkt_sus_model_catalog_entry((unsigned long)(index - 1), &entry);
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result, "sus catalog entry");
  }
  vectis_sus_lua_push_model_entry(lua, &entry);
  return 1;
}

static int vectis_sus_lua_catalog_default(lua_State *lua) {
  cpkt_sus_model_entry entry;
  cpkt_sus_result result;

  memset(&entry, 0, sizeof(entry));
  result = cpkt_sus_model_catalog_default(&entry);
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result, "sus catalog default");
  }
  vectis_sus_lua_push_model_entry(lua, &entry);
  return 1;
}

static int vectis_sus_lua_catalog_find(lua_State *lua) {
  cpkt_sus_model_entry entry;
  cpkt_sus_result result;
  const char *name;

  name = NULL;
  if (!lua_isnoneornil(lua, 1)) {
    name = luaL_checkstring(lua, 1);
  }
  memset(&entry, 0, sizeof(entry));
  result = cpkt_sus_model_catalog_find(name, &entry);
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result, "sus catalog find");
  }
  vectis_sus_lua_push_model_entry(lua, &entry);
  return 1;
}

static int vectis_sus_lua_open_path(lua_State *lua) {
  cpkt_sus_config config;
  cpkt_sus *sus;
  cpkt_sus_result result;

  luaL_checktype(lua, 1, LUA_TTABLE);
  cpkt_sus_config_default(&config);
  config.model_path = vectis_sus_lua_table_string(lua, 1, "model_path");
  if (config.model_path == NULL) {
    config.model_path = vectis_sus_lua_table_string(lua, 1, "path");
  }
  config.cpu_only = vectis_sus_lua_table_bool(lua, 1, "cpu_only", 0);
  config.preserve_initial_space_after_first_transcriber =
      vectis_sus_lua_table_bool(
          lua, 1, "preserve_initial_space_after_first_transcriber", 0);
  sus = NULL;
  result = cpkt_sus_open_path(&sus, &config);
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result, "sus open_path");
  }
  vectis_sus_lua_new_model(lua, sus);
  return 1;
}

static int vectis_sus_lua_open_cached(lua_State *lua) {
  cpkt_sus_cache_config config;
  cpkt_sus *sus;
  cpkt_sus_result result;
  vectis_sus_cache_status_lua status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  cpkt_sus_cache_config_default(&config);
  config.model = vectis_sus_lua_table_string(lua, 1, "model");
  config.cache_dir = vectis_sus_lua_table_string(lua, 1, "cache_dir");
  config.sha256 = vectis_sus_lua_table_string(lua, 1, "sha256");
  config.source_url = vectis_sus_lua_table_string(lua, 1, "source_url");
  config.insecure_no_checksum =
      vectis_sus_lua_table_bool(lua, 1, "insecure_no_checksum", 0);
  config.offline = vectis_sus_lua_table_bool(lua, 1, "offline", 0);
  config.cpu_only = vectis_sus_lua_table_bool(lua, 1, "cpu_only", 0);
  config.preserve_initial_space_after_first_transcriber =
      vectis_sus_lua_table_bool(
          lua, 1, "preserve_initial_space_after_first_transcriber", 0);
  status.lua = lua;
  status.ref = vectis_sus_lua_table_function_ref(lua, 1, "status");
  if (status.ref != LUA_NOREF) {
    config.status_sink = vectis_sus_lua_cache_status_sink;
    config.status_user = &status;
  }
  sus = NULL;
  result = cpkt_sus_open_cached(&sus, &config);
  if (status.ref != LUA_NOREF) {
    luaL_unref(lua, LUA_REGISTRYINDEX, status.ref);
  }
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result, "sus open_cached");
  }
  vectis_sus_lua_new_model(lua, sus);
  return 1;
}

static void vectis_sus_lua_set_constants(lua_State *lua) {
  lua_pushinteger(lua, CPKT_SUS_OK);
  lua_setfield(lua, -2, "OK");
  lua_pushinteger(lua, CPKT_SUS_ERR_ARG);
  lua_setfield(lua, -2, "ERR_ARG");
  lua_pushinteger(lua, CPKT_SUS_ERR_ALLOC);
  lua_setfield(lua, -2, "ERR_ALLOC");
  lua_pushinteger(lua, CPKT_SUS_ERR_MODEL);
  lua_setfield(lua, -2, "ERR_MODEL");
  lua_pushinteger(lua, CPKT_SUS_ERR_UPSTREAM);
  lua_setfield(lua, -2, "ERR_UPSTREAM");
  lua_pushinteger(lua, CPKT_SUS_ERR_CALLBACK);
  lua_setfield(lua, -2, "ERR_CALLBACK");
  lua_pushinteger(lua, CPKT_SUS_ERR_LOOKUP);
  lua_setfield(lua, -2, "ERR_LOOKUP");
  lua_pushinteger(lua, CPKT_SUS_ERR_IO);
  lua_setfield(lua, -2, "ERR_IO");
  lua_pushinteger(lua, CPKT_SUS_ERR_CHECKSUM);
  lua_setfield(lua, -2, "ERR_CHECKSUM");
  lua_pushinteger(lua, CPKT_SUS_ERR_NETWORK);
  lua_setfield(lua, -2, "ERR_NETWORK");
  lua_pushinteger(lua, CPKT_SUS_ABORTED);
  lua_setfield(lua, -2, "ABORTED");
  lua_pushinteger(lua, CPKT_SUS_CACHE_STATUS_LOOKUP);
  lua_setfield(lua, -2, "CACHE_STATUS_LOOKUP");
  lua_pushinteger(lua, CPKT_SUS_CACHE_STATUS_HIT);
  lua_setfield(lua, -2, "CACHE_STATUS_HIT");
  lua_pushinteger(lua, CPKT_SUS_CACHE_STATUS_MISS);
  lua_setfield(lua, -2, "CACHE_STATUS_MISS");
}

int luaopen_sus(lua_State *lua) {
  static const luaL_Reg model_methods[] = {
      {"info", vectis_sus_lua_model_info},
      {"reset_transcript_spacing", vectis_sus_lua_model_reset_spacing},
      {"close", vectis_sus_lua_model_close},
      {"__gc", vectis_sus_lua_model_close},
      {NULL, NULL}};
  static const luaL_Reg funcs[] = {
      {"result_string", vectis_sus_lua_result_string},
      {"backend_version", vectis_sus_lua_backend_version},
      {"backend_system_info", vectis_sus_lua_backend_system_info},
      {"backend_capabilities", vectis_sus_lua_backend_capabilities},
      {"facade_version", vectis_sus_lua_facade_version},
      {"model_catalog_count", vectis_sus_lua_catalog_count},
      {"model_catalog_entry", vectis_sus_lua_catalog_entry},
      {"model_catalog_default", vectis_sus_lua_catalog_default},
      {"model_catalog_find", vectis_sus_lua_catalog_find},
      {"open_path", vectis_sus_lua_open_path},
      {"open_cached", vectis_sus_lua_open_cached},
      {NULL, NULL}};

  luaL_newmetatable(lua, VECTIS_SUS_MODEL);
  lua_pushvalue(lua, -1);
  lua_setfield(lua, -2, "__index");
  luaL_setfuncs(lua, model_methods, 0);
  lua_pop(lua, 1);

  luaL_newlib(lua, funcs);
  vectis_sus_lua_set_constants(lua);
  return 1;
}
