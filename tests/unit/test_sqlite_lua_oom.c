#include "vectis_lua_sqlite.h"
#include <assert.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Model a finalizer running at an allocating userdata boundary. No provider
 * call is active here. Mutations use the real Lua state and real SQL engine. */
static int mutate_userdata;
static size_t mutate_size;
static void *userdata_with_finalizer(lua_State *L, size_t size, int values) {
  void *result = lua_newuserdatauv(L, size, values);
  if (mutate_userdata && (mutate_size == 0u || size == mutate_size)) {
    mutate_userdata = 0;
    lua_getglobal(L, "fixture_mutate");
    lua_call(L, 0, 0);
  }
  return result;
}
#define lua_newuserdatauv userdata_with_finalizer
#include "../../src/vectis_lua_sqlite.c"
#undef lua_newuserdatauv

static void mutable_arguments(void) {
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  luaL_requiref(L, "vectis.sqlite.core", luaopen_vectis_sqlite_core, 0);
  lua_pop(L, 1);
  assert(luaL_loadstring(
             L,
             "s=require('vectis.sqlite.core'); "
             "opts={mode='readwrite_'..'create'}; "
             "fixture_mutate=function() "
             "opts.mode='invalid';collectgarbage('collect') end; "
             "return function() return s.open(':memory:',opts) end") == LUA_OK);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  mutate_userdata = 1;
  assert(lua_pcall(L, 0, 2, 0) == LUA_OK);
  assert(lua_isnil(L, -2) && lua_istable(L, -1));
  lua_settop(L, 0);
  assert(
      luaL_loadstring(
          L,
          "db=assert(s.open(':memory:')); params={string.rep('before',1000)}; "
          "fixture_mutate=function() "
          "params[1]=string.rep('after',400);collectgarbage('collect') end; "
          "return function() local q <close> = assert(db:query('select "
          "?',params)); return q:next()[1] end") == LUA_OK);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  mutate_userdata = 1;
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  assert(lua_rawlen(L, -1) == 2000u &&
         memcmp(lua_tostring(L, -1), "after", 5u) == 0);
  lua_settop(L, 0);
  assert(
      luaL_loadstring(
          L,
          "fixture_mutate=function() db:close();collectgarbage('collect') end; "
          "return function() return db:query('select 1',{}) end") == LUA_OK);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  mutate_userdata = 1;
  assert(lua_pcall(L, 0, 2, 0) == LUA_OK);
  assert(lua_isnil(L, -2) && lua_istable(L, -1));
  lua_settop(L, 0);
  assert(luaL_dostring(
             L, "db=assert(s.open(':memory:')); params={1}; local fields={}; "
                "for i=1,256 do fields[i]='?' end; sql='SELECT "
                "'..table.concat(fields,','); "
                "fixture_mutate=function() for i=2,256 do params[i]=i end; "
                "collectgarbage('collect') end; "
                "return function() local q <close> = "
                "assert(db:query(sql,params)); return q:next()[256] end") ==
         LUA_OK);
  mutate_size = VECTIS_SQL_MAX_PARAMETERS * sizeof(vectis_sql_value);
  mutate_userdata = 1;
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  assert(mutate_userdata == 0 && lua_tointeger(L, -1) == 256);
  mutate_size = 0u;
  lua_close(L);
}

typedef struct allocation_fault {
  size_t remaining;
  int armed;
  int fired;
} allocation_fault;
static void *allocate(void *context, void *pointer, size_t old_size,
                      size_t size) {
  allocation_fault *fault = (allocation_fault *)context;
  if (size == 0u) {
    free(pointer);
    return NULL;
  }
  if (fault->armed && (pointer == NULL || size > old_size)) {
    if (fault->remaining == 0u) {
      fault->fired = 1;
      return NULL;
    }
    --fault->remaining;
  }
  return realloc(pointer, size);
}
int main(int argc, char **argv) {
  size_t index;
  int fired = 0, succeeded = 0;
  assert(argc == 2);
  for (index = 0; index < 768u; ++index) {
    allocation_fault fault;
    lua_State *L;
    int status;
    fault.armed = fault.fired = 0;
    fault.remaining = index;
    L = lua_newstate(allocate, &fault, 0u);
    assert(L != NULL);
    luaL_openlibs(L);
    luaL_requiref(L, "vectis.sqlite.core", luaopen_vectis_sqlite_core, 0);
    lua_pop(L, 1);
    assert(luaL_loadfile(L, argv[1]) == LUA_OK);
    fault.armed = 1;
    status = lua_pcall(L, 0, 1, 0);
    fault.armed = 0;
    if (fault.fired) {
      ++fired;
      assert(status != LUA_OK);
    } else {
      if (status != LUA_OK)
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
      assert(status == LUA_OK && lua_toboolean(L, -1));
      ++succeeded;
    }
    lua_close(L);
  }
  printf("SQLite Lua allocation failures: %d, completed runs: %d\n", fired,
         succeeded);
  fflush(stdout);
  assert(fired > 0 && succeeded > 0);
  mutable_arguments();
  return 0;
}
