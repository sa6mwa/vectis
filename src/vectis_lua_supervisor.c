#include "vectis_lua_supervisor.h"
#include <lauxlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#define SCOPE_MT "vectis.supervisor.native"
#define WEAK_KEY "vectis.supervisor.callbacks"
typedef struct lua_scope {
  lua_State *lua;
  vectis_supervisor *scope;
} lua_scope;
static void weak_map(lua_State *lua) {
  lua_getfield(lua, LUA_REGISTRYINDEX, WEAK_KEY);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    lua_newtable(lua);
    lua_newtable(lua);
    lua_pushliteral(lua, "v");
    lua_setfield(lua, -2, "__mode");
    lua_setmetatable(lua, -2);
    lua_pushvalue(lua, -1);
    lua_setfield(lua, LUA_REGISTRYINDEX, WEAK_KEY);
  }
}
static int push_status(lua_State *lua, vectis_status status,
                       const vectis_error *error) {
  if (status == VECTIS_OK) {
    lua_pushboolean(lua, 1);
    return 1;
  }
  lua_pushnil(lua);
  if (status == VECTIS_ERR_CONFLICT)
    lua_pushliteral(lua, "full");
  else if (status == VECTIS_ERR_TIMEOUT)
    lua_pushliteral(lua, "timeout");
  else
    lua_pushstring(lua, error != NULL && error->message[0] != '\0'
                            ? error->message
                            : "closed");
  return 2;
}
static lua_scope *check(lua_State *lua) {
  lua_scope *scope = luaL_checkudata(lua, 1, SCOPE_MT);
  if (scope->scope == NULL)
    luaL_error(lua, "supervisor app is closed");
  return scope;
}
static vectis_status callback(void *context,
                              const vectis_supervisor_event *event,
                              vectis_error *error) {
  lua_scope *scope = context;
  lua_State *lua = scope->lua;
  int top = lua_gettop(lua);
  static const char *types[] = {"start",  "stop",  "message",
                                "signal", "timer", "probe"};
  weak_map(lua);
  lua_pushlightuserdata(lua, scope);
  lua_rawget(lua, -2);
  if (!lua_isuserdata(lua, -1)) {
    lua_settop(lua, top);
    return VECTIS_ERR_STATE;
  }
  lua_getiuservalue(lua, -1, 1);
  lua_getfield(lua, -1, "callback");
  lua_newtable(lua);
  lua_pushstring(lua, types[event->type]);
  lua_setfield(lua, -2, "type");
  lua_pushinteger(lua, event->kind);
  lua_setfield(lua, -2, "kind");
  lua_pushinteger(lua, event->channel_id);
  lua_setfield(lua, -2, "channel_id");
  lua_pushinteger(lua, event->correlation_id);
  lua_setfield(lua, -2, "correlation_id");
  lua_pushinteger(lua, event->flags);
  lua_setfield(lua, -2, "flags");
  lua_pushinteger(lua, event->deadline_ms);
  lua_setfield(lua, -2, "deadline_ms");
  if (event->type == VECTIS_SUPERVISOR_MESSAGE) {
    lua_pushlstring(lua, event->payload, event->payload_size);
    lua_setfield(lua, -2, "payload");
  }
  if (lua_pcall(lua, 1, 0, 0) != LUA_OK) {
    const char *message = lua_tostring(lua, -1);
    if (message == NULL && lua_istable(lua, -1)) {
      lua_pushliteral(lua, "message");
      lua_rawget(lua, -2);
      message = lua_tostring(lua, -1);
    }
    vectis_error_clear(error);
    if (error != NULL) {
      error->code = VECTIS_ERR_STATE;
      error->source = VECTIS_ERROR_SOURCE_VECTIS;
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     message != NULL ? message : "supervisor callback failed");
    }
    lua_settop(lua, top);
    return VECTIS_ERR_STATE;
  }
  lua_settop(lua, top);
  return VECTIS_OK;
}
static uint32_t kind(lua_State *lua, int index) {
  lua_Integer value = luaL_checkinteger(lua, index);
  if (value < 0 || (lua_Unsigned)value > UINT32_MAX)
    luaL_error(lua, "kind must be a uint32");
  return (uint32_t)value;
}
static int send_message(lua_State *lua) {
  lua_scope *scope = check(lua);
  vectis_supervisor_channel *channel;
  vectis_error error;
  size_t size;
  const char *payload;
  uint32_t message_kind = kind(lua, 2);
  vectis_status status;
  vectis_supervisor_event frame;
  memset(&frame, 0, sizeof(frame));
  frame.kind = message_kind;
  if (!lua_isnoneornil(lua, 4)) {
    luaL_checktype(lua, 4, LUA_TTABLE);
    lua_pushnil(lua);
    while (lua_next(lua, 4)) {
      const char *key =
          lua_type(lua, -2) == LUA_TSTRING ? lua_tostring(lua, -2) : NULL;
      if (key == NULL ||
          (strcmp(key, "correlation_id") != 0 && strcmp(key, "flags") != 0))
        return luaL_error(lua, "unsupported channel send option");
      if (strcmp(key, "correlation_id") == 0)
        frame.correlation_id = kind(lua, -1);
      else
        frame.flags = kind(lua, -1);
      lua_pop(lua, 1);
    }
  }
  luaL_checktype(lua, 3, LUA_TSTRING);
  payload = lua_tolstring(lua, 3, &size);
  vectis_error_clear(&error);
  status = vectis_supervisor_channel_get(scope->scope, &channel, &error);
  if (status == VECTIS_OK) {
    frame.payload = payload;
    frame.payload_size = size;
    status = vectis_supervisor_channel_send_frame(
        channel, frame.kind, frame.correlation_id, frame.flags, payload, size,
        &error);
  }
  return push_status(lua, status, &error);
}
static int signal_message(lua_State *lua) {
  lua_scope *scope = check(lua);
  vectis_supervisor_channel *channel;
  vectis_error error;
  uint32_t message_kind = kind(lua, 2);
  vectis_status status;
  vectis_error_clear(&error);
  status = vectis_supervisor_channel_get(scope->scope, &channel, &error);
  if (status == VECTIS_OK)
    status = vectis_supervisor_channel_signal(channel, message_kind, &error);
  return push_status(lua, status, &error);
}
static int add_timer(lua_State *lua) {
  lua_scope *scope = check(lua);
  vectis_error error;
  uint32_t timer_kind = kind(lua, 2);
  lua_Integer ms = luaL_checkinteger(lua, 3);
  vectis_status status;
  if (ms < 1 || ms > 2147483647)
    return luaL_error(lua, "timer interval out of range");
  vectis_error_clear(&error);
  status = vectis_supervisor_timer(scope->scope, timer_kind, (long)ms, &error);
  return push_status(lua, status, &error);
}
static int enable_timer(lua_State *lua) {
  lua_scope *scope = check(lua);
  vectis_error error;
  vectis_status status;
  uint32_t timer_kind = kind(lua, 2);
  luaL_checktype(lua, 3, LUA_TBOOLEAN);
  vectis_error_clear(&error);
  status = vectis_supervisor_timer_enable(scope->scope, timer_kind,
                                          lua_toboolean(lua, 3), &error);
  return push_status(lua, status, &error);
}
static int last_error_scope(lua_State *lua) {
  lua_scope *scope = check(lua);
  vectis_error error;
  if (vectis_supervisor_last_error(scope->scope, &error) != VECTIS_OK)
    return luaL_error(lua, "supervisor app is closed");
  if (error.code == VECTIS_OK) {
    lua_pushnil(lua);
    return 1;
  }
  lua_newtable(lua);
  lua_pushinteger(lua, error.code);
  lua_setfield(lua, -2, "code");
  lua_pushinteger(lua, error.code);
  lua_setfield(lua, -2, "status");
  lua_pushinteger(lua, error.source);
  lua_setfield(lua, -2, "source_code");
  lua_pushstring(lua, error.message);
  lua_setfield(lua, -2, "message");
  return 1;
}
static void lifecycle_idle(lua_State *lua) {
  lua_getiuservalue(lua, 1, 1);
  lua_getfield(lua, -1, "app");
  lua_getfield(lua, -1, "_workflow_idle");
  lua_pushvalue(lua, -2);
  lua_call(lua, 1, 0);
  lua_pop(lua, 2);
}
static int start_scope(lua_State *lua) {
  lifecycle_idle(lua);
  lua_scope *scope = check(lua);
  vectis_error error;
  vectis_status status;
  vectis_error_clear(&error);
  status = vectis_supervisor_start(scope->scope, &error);
  return push_status(lua, status, &error);
}
static int stop_scope(lua_State *lua) {
  lifecycle_idle(lua);
  lua_scope *scope = check(lua);
  vectis_error error;
  vectis_status status;
  lua_Integer requested = luaL_optinteger(lua, 2, 0);
  long ms;
  if (requested < 0 || (lua_Unsigned)requested > LONG_MAX)
    return luaL_error(lua, "timeout must be nonnegative and fit native long");
  ms = (long)requested;
  vectis_error_clear(&error);
  status = vectis_supervisor_stop(scope->scope, ms, &error);
  return push_status(lua, status, &error);
}
static int schedule_scope(lua_State *lua) {
  lua_scope *scope = check(lua);
  vectis_error error;
  vectis_status status;
  vectis_error_clear(&error);
  status = vectis_supervisor_schedule(scope->scope, &error);
  return push_status(lua, status, &error);
}
static int domain_scope(lua_State *lua) {
  lua_scope *scope = check(lua);
  vectis_supervisor_channel *channel;
  (void)vectis_supervisor_channel_get(scope->scope, &channel, NULL);
  if (vectis_supervisor_get_state(scope->scope) == VECTIS_SUPERVISOR_RUNNING ||
      vectis_supervisor_get_state(scope->scope) ==
          VECTIS_SUPERVISOR_MATERIALIZING)
    lua_pushliteral(lua, "owner");
  else if (vectis_supervisor_channel_fd(channel) < 0 &&
           vectis_supervisor_get_state(scope->scope) ==
               VECTIS_SUPERVISOR_DECLARED)
    lua_pushliteral(lua, "producer");
  else
    lua_pushliteral(lua, "declared");
  return 1;
}
static int state_scope(lua_State *lua) {
  lua_scope *scope = check(lua);
  static const char *states[] = {"declared", "materializing", "running",
                                 "stopping", "stopped",       "failed"};
  lua_pushstring(lua, states[vectis_supervisor_get_state(scope->scope)]);
  return 1;
}
int vectis_lua_supervisor_declare(lua_State *lua, vectis_app *app) {
  vectis_supervisor_config config;
  vectis_error error;
  vectis_status status;
  lua_scope *scope;
  uint32_t signals[64];
  size_t i, count;
  int result_index;
  lua_Integer bound;
  static const luaL_Reg methods[] = {{"send", send_message},
                                     {"signal", signal_message},
                                     {"timer", add_timer},
                                     {"timer_enable", enable_timer},
                                     {"last_error", last_error_scope},
                                     {"schedule", schedule_scope},
                                     {"domain", domain_scope},
                                     {"start", start_scope},
                                     {"stop", stop_scope},
                                     {"state", state_scope},
                                     {NULL, NULL}};
  luaL_checktype(lua, 2, LUA_TTABLE);
  luaL_checktype(lua, 3, LUA_TFUNCTION);
  if (!lua_pushthread(lua))
    return luaL_error(lua,
                      "supervisors must be declared on the main Lua caller");
  lua_pop(lua, 1);
  vectis_supervisor_config_init(&config);
  lua_getfield(lua, 2, "name");
  config.name = luaL_checkstring(lua, -1);
  lua_getfield(lua, 2, "capacity");
  bound = luaL_optinteger(lua, -1, 64);
  if (bound < 1 || bound > 4096)
    return luaL_error(lua, "channel capacity must be between 1 and 4096");
  config.capacity = (size_t)bound;
  lua_getfield(lua, 2, "max_frame_size");
  bound = luaL_optinteger(lua, -1, 4096);
  if (bound < 1 || bound > 1048576)
    return luaL_error(lua, "channel frame size must be between 1 and 1048576");
  config.max_frame_size = (size_t)bound;
  lua_getfield(lua, 2, "start");
  config.start_with_app = lua_isnil(lua, -1) || lua_toboolean(lua, -1);
  lua_getfield(lua, 2, "stop_on_failure");
  config.stop_on_failure = lua_toboolean(lua, -1);
  lua_getfield(lua, 2, "signal_kinds");
  count = lua_isnil(lua, -1) ? 0u : lua_rawlen(lua, -1);
  if (count > 64u)
    return luaL_error(lua, "at most 64 signal kinds are allowed");
  for (i = 0; i < count; ++i) {
    lua_rawgeti(lua, -1, (lua_Integer)i + 1);
    signals[i] = kind(lua, -1);
    lua_pop(lua, 1);
  }
  config.signal_kinds = signals;
  config.signal_kind_count = count;
  scope = lua_newuserdatauv(lua, sizeof(*scope), 1);
  scope->lua = lua;
  scope->scope = NULL;
  result_index = lua_gettop(lua);
  if (luaL_newmetatable(lua, SCOPE_MT)) {
    lua_newtable(lua);
    luaL_setfuncs(lua, methods, 0);
    lua_setfield(lua, -2, "__index");
  }
  lua_setmetatable(lua, result_index);
  lua_newtable(lua);
  lua_pushvalue(lua, 3);
  lua_setfield(lua, -2, "callback");
  lua_pushvalue(lua, 1);
  lua_setfield(lua, -2, "app");
  lua_pushthread(lua);
  lua_setfield(lua, -2, "thread");
  lua_setiuservalue(lua, result_index, 1);
  config.handler = callback;
  config.context = scope;
  vectis_error_clear(&error);
  status = vectis_supervisor_new(app, &config, &scope->scope, &error);
  if (status != VECTIS_OK)
    return push_status(lua, status, &error);
  weak_map(lua);
  lua_pushlightuserdata(lua, scope);
  lua_pushvalue(lua, result_index);
  lua_rawset(lua, -3);
  lua_pop(lua, 1);
  lua_getiuservalue(lua, 1, 1);
  lua_getfield(lua, -1, "supervisors");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    lua_newtable(lua);
    lua_pushvalue(lua, -1);
    lua_setfield(lua, -3, "supervisors");
  }
  lua_pushvalue(lua, result_index);
  lua_rawseti(lua, -2, (lua_Integer)lua_rawlen(lua, -2) + 1);
  lua_settop(lua, result_index);
  return 1;
}
int vectis_lua_supervisor_pump(lua_State *lua, vectis_app *app) {
  vectis_supervisor_host *host;
  vectis_error error;
  vectis_status status;
  lua_Integer requested = luaL_optinteger(lua, 2, 0);
  long ms;
  if (requested < 0 || (lua_Unsigned)requested > LONG_MAX)
    return luaL_error(lua, "timeout must be nonnegative and fit native long");
  ms = (long)requested;
  vectis_error_clear(&error);
  status = vectis_app_supervisor_host(app, &host, &error);
  if (status == VECTIS_OK)
    status = vectis_supervisor_host_pump(host, ms, &error);
  return push_status(lua, status, &error);
}
void vectis_lua_supervisor_invalidate(lua_State *lua, int app_index) {
  size_t i, count;
  lua_getiuservalue(lua, app_index, 1);
  lua_getfield(lua, -1, "supervisors");
  if (lua_istable(lua, -1)) {
    count = lua_rawlen(lua, -1);
    for (i = 0; i < count; ++i) {
      lua_scope *scope;
      lua_rawgeti(lua, -1, (lua_Integer)i + 1);
      scope = luaL_testudata(lua, -1, SCOPE_MT);
      if (scope != NULL)
        scope->scope = NULL;
      lua_pop(lua, 1);
    }
  }
  lua_pop(lua, 2);
}

/* Lua clears weak values before finalizing unreachable apps. Re-root callback
 * userdata from the app's owned uservalue edges during finalizer cleanup. */
void vectis_lua_supervisor_restore(lua_State *lua, int app_index) {
  size_t i, count;
  int map;
  app_index = lua_absindex(lua, app_index);
  weak_map(lua);
  map = lua_gettop(lua);
  lua_getiuservalue(lua, app_index, 1);
  lua_getfield(lua, -1, "supervisors");
  if (lua_istable(lua, -1)) {
    count = lua_rawlen(lua, -1);
    for (i = 0u; i < count; ++i) {
      lua_scope *scope;
      lua_rawgeti(lua, -1, (lua_Integer)i + 1);
      scope = luaL_testudata(lua, -1, SCOPE_MT);
      if (scope != NULL) {
        lua_pushlightuserdata(lua, scope);
        lua_pushvalue(lua, -2);
        lua_rawset(lua, map);
      }
      lua_pop(lua, 1);
    }
  }
  lua_pop(lua, 3);
}
