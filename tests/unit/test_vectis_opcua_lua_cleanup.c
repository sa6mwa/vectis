#include <assert.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>

static size_t allocations;
static void *tracked_malloc(size_t size) {
  void *p = malloc(size);
  if (p != NULL)
    ++allocations;
  return p;
}
static void *tracked_calloc(size_t count, size_t size) {
  void *p = calloc(count, size);
  if (p != NULL)
    ++allocations;
  return p;
}
static void tracked_free(void *p) {
  if (p != NULL) {
    assert(allocations != 0u);
    --allocations;
  }
  free(p);
}

/* Isolate array helper ownership across Lua nonlocal error unwinding. */
#define malloc tracked_malloc
#define calloc tracked_calloc
#define free tracked_free
#include "../../src/vectis_opcua_lua.c"
#undef malloc
#undef calloc
#undef free

static int arrays(lua_State *lua) {
  size_t count;
  int *integers;
  unsigned long *dimensions;
  cpkt_opcua_value *values;
  lua_settop(lua, 4);
  integers = vectis_opcua_lua_int_array_field(lua, 1, "first", &count);
  assert(count == 2u && integers[0] == 1 && integers[1] == 2);
  (void)vectis_opcua_lua_int_array_field(lua, 1, "second", &count);
  dimensions =
      vectis_opcua_lua_ulong_array_from_lua(lua, 2, &count, "dimensions");
  assert(count == 2u && dimensions[0] == 1u && dimensions[1] == 2u);
  values = vectis_opcua_lua_value_array_from_lua(lua, 3, &count, "inputs");
  assert(count == 2u && values[0].integer_value == 1);
  if (lua_toboolean(lua, 4))
    return luaL_error(lua, "later option rejected");
  lua_pushboolean(lua, 1);
  return 1;
}

/* Exercise every client callback path with the real Lua receiver shell. */
static int invoke_callback(lua_State *lua) {
  vectis_opcua_lua_client *client = vectis_opcua_lua_check_client(lua, 1);
  int kind = (int)luaL_checkinteger(lua, 3);
  luaL_checktype(lua, 2, LUA_TFUNCTION);
  if (kind <= 5) {
    vectis_opcua_lua_async_callback *callback =
        vectis_opcua_lua_async_callback_new(lua, client, 2);
    vectis_opcua_lua_client_keep_async_callback(client, callback);
    switch (kind) {
    case 1:
      vectis_opcua_lua_async_value_cb(1u, CPKT_OPCUA_ERR_UPSTREAM, NULL, 0u,
                                      callback);
      break;
    case 2:
      vectis_opcua_lua_async_status_cb(1u, CPKT_OPCUA_OK, 0u, callback);
      break;
    case 3:
      vectis_opcua_lua_async_browse_cb(1u, CPKT_OPCUA_OK, 0u, callback);
      break;
    case 4:
      vectis_opcua_lua_async_call_cb(1u, CPKT_OPCUA_OK, NULL, 0u, 0u, callback);
      break;
    case 5:
      vectis_opcua_lua_async_node_cb(1u, CPKT_OPCUA_ERR_UPSTREAM, NULL, 0u,
                                     callback);
      break;
    }
  } else {
    vectis_opcua_lua_monitor_callback *callback =
        vectis_opcua_lua_monitor_callback_new(lua, client, 2, 1u);
    cpkt_opcua_value value;
    cpkt_opcua_event event;
    cpkt_opcua_value_integer(&value, 7L);
    memset(&event, 0, sizeof(event));
    vectis_opcua_lua_client_keep_monitor_callback(client, callback);
    if (kind == 6)
      vectis_opcua_lua_data_change_cb(1u, 1u, &value, 0u, callback);
    else if (kind == 7)
      vectis_opcua_lua_event_cb(1u, 1u, &event, 0u, callback);
    else
      vectis_opcua_lua_event_fields_cb(1u, 1u, NULL, 0u, 0u, callback);
  }
  return 0;
}

int main(void) {
  lua_State *lua = luaL_newstate();
  int i;
  assert(lua != NULL);
  luaL_openlibs(lua);
  assert(luaopen_opcua(lua) == 1);
  lua_setglobal(lua, "opcua");
  lua_pushcfunction(lua, invoke_callback);
  lua_setglobal(lua, "invoke_callback");
  lua_pushcfunction(lua, arrays);
  lua_setglobal(lua, "arrays");
  for (i = 0; i < 30; ++i) {
    int rc = luaL_dostring(
        lua,
        "assert(not pcall(arrays, {first={1,2},second={1,{}}}, {1,2}, {1,2}))\n"
        "assert(not pcall(arrays, {first={1,2}}, {1,{}}, {1,2}))\n"
        "assert(not pcall(arrays, {first={1,2}}, {1,-1}, {1,2}))\n"
        "assert(not pcall(arrays, {first={1,2}}, {1,2}, {1,{}}))\n"
        "assert(not pcall(arrays, {first={1,2}}, {1,2}, {1,2}, true))\n"
        "assert(arrays({first={1,2},second={}}, {1,2}, {1,2}))\n");
    if (rc != LUA_OK)
      fprintf(stderr, "%s\n", lua_tostring(lua, -1));
    assert(rc == LUA_OK);
    /* No collection required: __close cleans allocations immediately. */
    assert(allocations == 0u);
  }
  assert(luaL_dostring(lua, "local co=coroutine.create(function()\n"
                            "  arrays({first={1,2}}, {1,{}}, {1,2})\n"
                            "end)\n"
                            "assert(not coroutine.resume(co)); co=nil; "
                            "collectgarbage('collect')\n") == LUA_OK);
  assert(allocations == 0u);
  {
    int rc = luaL_dostring(
        lua,
        "for kind=1,8 do\n"
        "  local client=assert(opcua.client()); local calls=0\n"
        "  invoke_callback(client,function()\n"
        "    calls=calls+1; collectgarbage('collect')\n"
        "    for _,name in "
        "ipairs({'close','disconnect','connect','iterate','__gc'}) do\n"
        "      local fn=name=='__gc' and getmetatable(client).__gc or "
        "client[name]\n"
        "      local ok,err=pcall(fn,client)\n"
        "      assert(not ok and tostring(err):find('opcua client is "
        "busy',1,true))\n"
        "    end\n"
        "  end,kind)\n"
        "  assert(calls==1); assert(client:close()); assert(client:close())\n"
        "end\n");
    if (rc != LUA_OK)
      fprintf(stderr, "%s\n", lua_tostring(lua, -1));
    assert(rc == LUA_OK);
    assert(allocations == 0u);
  }
  lua_close(lua);
  assert(allocations == 0u);
  return 0;
}
