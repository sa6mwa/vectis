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
static void tracked_free(void *p) {
  if (p != NULL) {
    assert(allocations != 0u);
    --allocations;
  }
  free(p);
}

/* Isolate array helper ownership across Lua nonlocal error unwinding. */
#define malloc tracked_malloc
#define free tracked_free
#include "../../src/vectis_opcua_lua.c"
#undef malloc
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

int main(void) {
  lua_State *lua = luaL_newstate();
  int i;
  assert(lua != NULL);
  luaL_openlibs(lua);
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
  lua_close(lua);
  assert(allocations == 0u);
  return 0;
}
