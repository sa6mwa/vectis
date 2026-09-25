#include "vectis_cli_proxy_lua.h"

#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct vectis_lua_proxy_view {
  const vectis_proxy_inbound *in;
  vectis_proxy_outbound *out;
  vectis_proxy_response *response;
  int active;
} vectis_lua_proxy_view;

enum vectis_lua_proxy_edit {
  VECTIS_LUA_PROXY_TARGET = 1,
  VECTIS_LUA_PROXY_METHOD,
  VECTIS_LUA_PROXY_PATH,
  VECTIS_LUA_PROXY_QUERY,
  VECTIS_LUA_PROXY_HOST,
  VECTIS_LUA_PROXY_ADD_HEADER,
  VECTIS_LUA_PROXY_SET_HEADER,
  VECTIS_LUA_PROXY_REMOVE_HEADER
};

enum vectis_lua_proxy_response_edit {
  VECTIS_LUA_PROXY_RESPONSE_STATUS = 1,
  VECTIS_LUA_PROXY_RESPONSE_ADD_HEADER,
  VECTIS_LUA_PROXY_RESPONSE_SET_HEADER,
  VECTIS_LUA_PROXY_RESPONSE_REMOVE_HEADER
};

static void vectis_lua_proxy_error(vectis_error *error, vectis_status status,
                                   const char *message) {
  if (error == NULL)
    return;
  vectis_error_clear(error);
  error->code = status;
  error->source = VECTIS_ERROR_SOURCE_VECTIS;
  (void)snprintf(error->message, sizeof(error->message), "%s", message);
}

vectis_lua_proxy_route *vectis_lua_proxy_route_new(lua_State *lua,
                                                   int rewrite_index,
                                                   int response_index) {
  vectis_lua_proxy_route *route;

  if (lua == NULL || (lua_type(lua, rewrite_index) != LUA_TFUNCTION &&
                      lua_type(lua, response_index) != LUA_TFUNCTION))
    return NULL;
  route = (vectis_lua_proxy_route *)calloc(1u, sizeof(*route));
  if (route == NULL)
    return NULL;
  route->lua = lua;
  route->rewrite_ref = LUA_NOREF;
  route->response_ref = LUA_NOREF;
  if (lua_type(lua, rewrite_index) == LUA_TFUNCTION) {
    lua_pushvalue(lua, rewrite_index);
    route->rewrite_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  }
  if (lua_type(lua, response_index) == LUA_TFUNCTION) {
    lua_pushvalue(lua, response_index);
    route->response_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  }
  return route;
}

void vectis_lua_proxy_route_free(vectis_lua_proxy_route *route) {
  if (route == NULL)
    return;
  if (route->lua != NULL && route->rewrite_ref != LUA_NOREF)
    luaL_unref(route->lua, LUA_REGISTRYINDEX, route->rewrite_ref);
  if (route->lua != NULL && route->response_ref != LUA_NOREF)
    luaL_unref(route->lua, LUA_REGISTRYINDEX, route->response_ref);
  free(route);
}

static vectis_lua_proxy_view *vectis_lua_proxy_view_checked(lua_State *lua) {
  vectis_lua_proxy_view *view;

  view = (vectis_lua_proxy_view *)lua_touserdata(lua, lua_upvalueindex(1));
  if (view == NULL || !view->active)
    luaL_error(lua, "proxy view has expired");
  return view;
}

static int vectis_lua_proxy_param(lua_State *lua) {
  vectis_lua_proxy_view *view;
  const char *name;
  const char *value;
  int index;

  view = vectis_lua_proxy_view_checked(lua);
  index = lua_istable(lua, 1) ? 2 : 1;
  name = luaL_checkstring(lua, index);
  value = vectis_proxy_inbound_path_param(view->in, name);
  if (value != NULL)
    lua_pushstring(lua, value);
  else
    lua_pushnil(lua);
  return 1;
}

static vectis_http_method vectis_lua_proxy_method(lua_State *lua, int index) {
  const char *name;
  vectis_http_method method;

  name = luaL_checkstring(lua, index);
  for (method = VECTIS_HTTP_GET; method <= VECTIS_HTTP_MOVE;
       method = (vectis_http_method)(method + 1)) {
    if (strcmp(name, vectis_http_method_string(method)) == 0)
      return method;
  }
  return VECTIS_HTTP_ANY;
}

static const char *vectis_lua_proxy_string(lua_State *lua, int index) {
  if (lua_type(lua, index) != LUA_TSTRING)
    luaL_error(lua, "proxy value must be a string");
  return lua_tostring(lua, index);
}

static int vectis_lua_proxy_edit(lua_State *lua) {
  vectis_lua_proxy_view *view;
  vectis_status status;
  vectis_error error;
  lua_Integer target_index;
  size_t target_slot;
  int operation;
  int index;

  view = vectis_lua_proxy_view_checked(lua);
  operation = (int)lua_tointeger(lua, lua_upvalueindex(2));
  index = lua_istable(lua, 1) ? 2 : 1;
  vectis_error_clear(&error);
  switch (operation) {
  case VECTIS_LUA_PROXY_TARGET:
    target_index = luaL_checkinteger(lua, index);
    target_slot = target_index > 0 ? (size_t)(target_index - 1) : (size_t)-1;
    if (target_index > 0 && (lua_Integer)target_slot != target_index - 1)
      target_slot = (size_t)-1;
    status =
        vectis_proxy_outbound_select_target(view->out, target_slot, &error);
    break;
  case VECTIS_LUA_PROXY_METHOD:
    status = vectis_proxy_outbound_set_method(
        view->out, vectis_lua_proxy_method(lua, index), &error);
    break;
  case VECTIS_LUA_PROXY_PATH:
    status = vectis_proxy_outbound_set_path(
        view->out, vectis_lua_proxy_string(lua, index), &error);
    break;
  case VECTIS_LUA_PROXY_QUERY:
    status = vectis_proxy_outbound_set_query(
        view->out,
        lua_isnil(lua, index) ? NULL : vectis_lua_proxy_string(lua, index),
        &error);
    break;
  case VECTIS_LUA_PROXY_HOST:
    status = vectis_proxy_outbound_set_host(
        view->out,
        lua_isnil(lua, index) ? NULL : vectis_lua_proxy_string(lua, index),
        &error);
    break;
  case VECTIS_LUA_PROXY_ADD_HEADER:
    status = vectis_proxy_outbound_add_header(
        view->out, vectis_lua_proxy_string(lua, index),
        vectis_lua_proxy_string(lua, index + 1), &error);
    break;
  case VECTIS_LUA_PROXY_SET_HEADER:
    status = vectis_proxy_outbound_set_header(
        view->out, vectis_lua_proxy_string(lua, index),
        vectis_lua_proxy_string(lua, index + 1), &error);
    break;
  case VECTIS_LUA_PROXY_REMOVE_HEADER:
    status = vectis_proxy_outbound_remove_header(
        view->out, vectis_lua_proxy_string(lua, index), &error);
    break;
  default:
    return luaL_error(lua, "unknown proxy rewrite operation");
  }
  if (status != VECTIS_OK) {
    lua_pushnil(lua);
    lua_pushstring(lua, error.message);
    return 2;
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static void vectis_lua_proxy_method_field(lua_State *lua, int view_index,
                                          const char *name, int operation) {
  lua_pushvalue(lua, view_index);
  lua_pushinteger(lua, operation);
  lua_pushcclosure(lua, vectis_lua_proxy_edit, 2);
  lua_setfield(lua, -2, name);
}

static void vectis_lua_proxy_push_in(lua_State *lua, int view_index,
                                     const vectis_proxy_inbound *in) {
  const char *name;
  const char *value;
  size_t count;
  size_t i;

  lua_newtable(lua);
  lua_pushstring(lua,
                 vectis_http_method_string(vectis_proxy_inbound_method(in)));
  lua_setfield(lua, -2, "method");
  lua_pushstring(lua, vectis_proxy_inbound_path(in));
  lua_setfield(lua, -2, "path");
  if (vectis_proxy_inbound_query(in) != NULL) {
    lua_pushstring(lua, vectis_proxy_inbound_query(in));
    lua_setfield(lua, -2, "query");
  }
  lua_pushstring(lua, vectis_proxy_inbound_host(in));
  lua_setfield(lua, -2, "host");
  lua_pushboolean(lua, vectis_proxy_inbound_websocket(in));
  lua_setfield(lua, -2, "websocket");
  lua_pushvalue(lua, view_index);
  lua_pushcclosure(lua, vectis_lua_proxy_param, 1);
  lua_setfield(lua, -2, "param");
  count = vectis_proxy_inbound_header_count(in);
  lua_createtable(lua, (int)count, 0);
  for (i = 0u; i < count; ++i) {
    if (vectis_proxy_inbound_header_at(in, i, &name, &value) != VECTIS_OK)
      continue;
    lua_createtable(lua, 0, 2);
    lua_pushstring(lua, name);
    lua_setfield(lua, -2, "name");
    lua_pushstring(lua, value);
    lua_setfield(lua, -2, "value");
    lua_rawseti(lua, -2, (lua_Integer)i + 1);
  }
  lua_setfield(lua, -2, "headers");
}

static void vectis_lua_proxy_push_out(lua_State *lua, int view_index) {
  lua_createtable(lua, 0, 8);
  vectis_lua_proxy_method_field(lua, view_index, "select_target",
                                VECTIS_LUA_PROXY_TARGET);
  vectis_lua_proxy_method_field(lua, view_index, "set_method",
                                VECTIS_LUA_PROXY_METHOD);
  vectis_lua_proxy_method_field(lua, view_index, "set_path",
                                VECTIS_LUA_PROXY_PATH);
  vectis_lua_proxy_method_field(lua, view_index, "set_query",
                                VECTIS_LUA_PROXY_QUERY);
  vectis_lua_proxy_method_field(lua, view_index, "set_host",
                                VECTIS_LUA_PROXY_HOST);
  vectis_lua_proxy_method_field(lua, view_index, "add_header",
                                VECTIS_LUA_PROXY_ADD_HEADER);
  vectis_lua_proxy_method_field(lua, view_index, "set_header",
                                VECTIS_LUA_PROXY_SET_HEADER);
  vectis_lua_proxy_method_field(lua, view_index, "remove_header",
                                VECTIS_LUA_PROXY_REMOVE_HEADER);
}

static int vectis_lua_proxy_response_edit(lua_State *lua) {
  vectis_lua_proxy_view *view;
  vectis_error error;
  vectis_status status;
  lua_Integer status_number;
  int operation;
  int index;

  view = vectis_lua_proxy_view_checked(lua);
  operation = (int)lua_tointeger(lua, lua_upvalueindex(2));
  index = lua_istable(lua, 1) ? 2 : 1;
  vectis_error_clear(&error);
  switch (operation) {
  case VECTIS_LUA_PROXY_RESPONSE_STATUS:
    status_number = luaL_checkinteger(lua, index);
    status = vectis_proxy_response_set_status(
        view->response,
        status_number < 0 || status_number > 599 ? 600 : (int)status_number,
        &error);
    break;
  case VECTIS_LUA_PROXY_RESPONSE_ADD_HEADER:
    status = vectis_proxy_response_add_header(
        view->response, vectis_lua_proxy_string(lua, index),
        vectis_lua_proxy_string(lua, index + 1), &error);
    break;
  case VECTIS_LUA_PROXY_RESPONSE_SET_HEADER:
    status = vectis_proxy_response_set_header(
        view->response, vectis_lua_proxy_string(lua, index),
        vectis_lua_proxy_string(lua, index + 1), &error);
    break;
  case VECTIS_LUA_PROXY_RESPONSE_REMOVE_HEADER:
    status = vectis_proxy_response_remove_header(
        view->response, vectis_lua_proxy_string(lua, index), &error);
    break;
  default:
    return luaL_error(lua, "unknown proxy response operation");
  }
  if (status != VECTIS_OK) {
    lua_pushnil(lua);
    lua_pushstring(lua, error.message);
    return 2;
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static void vectis_lua_proxy_response_method_field(lua_State *lua,
                                                   int view_index,
                                                   const char *name,
                                                   int operation) {
  lua_pushvalue(lua, view_index);
  lua_pushinteger(lua, operation);
  lua_pushcclosure(lua, vectis_lua_proxy_response_edit, 2);
  lua_setfield(lua, -2, name);
}

static void
vectis_lua_proxy_push_response(lua_State *lua, int view_index,
                               const vectis_proxy_response *response) {
  const char *name;
  const char *value;
  size_t count;
  size_t i;

  lua_createtable(lua, 0, 5);
  lua_pushinteger(lua, vectis_proxy_response_status(response));
  lua_setfield(lua, -2, "status");
  count = vectis_proxy_response_header_count(response);
  lua_createtable(lua, (int)count, 0);
  for (i = 0u; i < count; ++i) {
    if (vectis_proxy_response_header_at(response, i, &name, &value) !=
        VECTIS_OK)
      continue;
    lua_createtable(lua, 0, 2);
    lua_pushstring(lua, name);
    lua_setfield(lua, -2, "name");
    lua_pushstring(lua, value);
    lua_setfield(lua, -2, "value");
    lua_rawseti(lua, -2, (lua_Integer)i + 1);
  }
  lua_setfield(lua, -2, "headers");
  vectis_lua_proxy_response_method_field(lua, view_index, "set_status",
                                         VECTIS_LUA_PROXY_RESPONSE_STATUS);
  vectis_lua_proxy_response_method_field(lua, view_index, "add_header",
                                         VECTIS_LUA_PROXY_RESPONSE_ADD_HEADER);
  vectis_lua_proxy_response_method_field(lua, view_index, "set_header",
                                         VECTIS_LUA_PROXY_RESPONSE_SET_HEADER);
  vectis_lua_proxy_response_method_field(
      lua, view_index, "remove_header",
      VECTIS_LUA_PROXY_RESPONSE_REMOVE_HEADER);
}

vectis_status vectis_lua_proxy_rewrite(const vectis_proxy_inbound *in,
                                       vectis_proxy_outbound *out,
                                       void *userdata, vectis_error *error) {
  vectis_lua_proxy_route *route;
  vectis_lua_proxy_view *view;
  lua_State *lua;
  const char *message;
  int base;
  int view_index;
  int in_index;
  int out_index;
  int result;

  route = (vectis_lua_proxy_route *)userdata;
  if (route == NULL || route->lua == NULL || route->rewrite_ref == LUA_NOREF) {
    vectis_lua_proxy_error(error, VECTIS_ERR_STATE,
                           "Lua proxy rewrite is not configured");
    return VECTIS_ERR_STATE;
  }
  lua = route->lua;
  base = lua_gettop(lua);
  view = (vectis_lua_proxy_view *)lua_newuserdatauv(lua, sizeof(*view), 0);
  view->in = in;
  view->out = out;
  view->response = NULL;
  view->active = 1;
  view_index = lua_absindex(lua, -1);
  vectis_lua_proxy_push_in(lua, view_index, in);
  in_index = lua_absindex(lua, -1);
  vectis_lua_proxy_push_out(lua, view_index);
  out_index = lua_absindex(lua, -1);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, route->rewrite_ref);
  lua_pushvalue(lua, in_index);
  lua_pushvalue(lua, out_index);
  result = lua_pcall(lua, 2, 1, 0);
  view->active = 0;
  view->in = NULL;
  view->out = NULL;
  if (result != LUA_OK) {
    message = lua_tostring(lua, -1);
    vectis_lua_proxy_error(error, VECTIS_ERR_STATE,
                           message != NULL ? message
                                           : "Lua proxy rewrite failed");
    lua_settop(lua, base);
    return VECTIS_ERR_STATE;
  }
  if (!(lua_isnil(lua, -1) ||
        (lua_isboolean(lua, -1) && lua_toboolean(lua, -1)))) {
    vectis_lua_proxy_error(error, VECTIS_ERR_INVALID,
                           "Lua proxy rewrite must return nil or true");
    lua_settop(lua, base);
    return VECTIS_ERR_INVALID;
  }
  lua_settop(lua, base);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_lua_proxy_modify_response(vectis_proxy_response *response,
                                               void *userdata,
                                               vectis_error *error) {
  vectis_lua_proxy_route *route;
  vectis_lua_proxy_view *view;
  lua_State *lua;
  const char *message;
  int base;
  int response_index;
  int result;

  route = (vectis_lua_proxy_route *)userdata;
  if (route == NULL || route->lua == NULL || route->response_ref == LUA_NOREF) {
    vectis_lua_proxy_error(error, VECTIS_ERR_STATE,
                           "Lua proxy response hook is not configured");
    return VECTIS_ERR_STATE;
  }
  lua = route->lua;
  base = lua_gettop(lua);
  view = (vectis_lua_proxy_view *)lua_newuserdatauv(lua, sizeof(*view), 0);
  view->in = NULL;
  view->out = NULL;
  view->response = response;
  view->active = 1;
  vectis_lua_proxy_push_response(lua, lua_absindex(lua, -1), response);
  response_index = lua_absindex(lua, -1);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, route->response_ref);
  lua_pushvalue(lua, response_index);
  result = lua_pcall(lua, 1, 1, 0);
  view->active = 0;
  view->response = NULL;
  if (result != LUA_OK) {
    message = lua_tostring(lua, -1);
    vectis_lua_proxy_error(error, VECTIS_ERR_STATE,
                           message != NULL ? message
                                           : "Lua proxy response hook failed");
    lua_settop(lua, base);
    return VECTIS_ERR_STATE;
  }
  if (!(lua_isnil(lua, -1) ||
        (lua_isboolean(lua, -1) && lua_toboolean(lua, -1)))) {
    vectis_lua_proxy_error(error, VECTIS_ERR_INVALID,
                           "Lua proxy response hook must return nil or true");
    lua_settop(lua, base);
    return VECTIS_ERR_INVALID;
  }
  lua_settop(lua, base);
  vectis_error_clear(error);
  return VECTIS_OK;
}
