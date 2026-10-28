#include "vectis_lua_pg_connection.h"
#include <assert.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vectis/pg.h>
/* Model a finalizer mutating parameters at scratch allocation, before parse. */
static int mutate_parameters;
static void *pg_userdata_with_finalizer(lua_State *L, size_t size, int values) {
  void *result = lua_newuserdatauv(L, size, values);
  if (mutate_parameters &&
      size == VECTIS_SQL_MAX_PARAMETERS * sizeof(vectis_pg_parameter)) {
    mutate_parameters = 0;
    lua_getglobal(L, "fixture_mutate");
    lua_call(L, 0, 0);
  }
  return result;
}
#define lua_newuserdatauv pg_userdata_with_finalizer
#include "../../src/vectis_lua_pg_connection.c"
#undef lua_newuserdatauv

typedef struct pg_fault {
  size_t remaining;
  int armed, fired;
} pg_fault;
static void *pg_allocate(void *context, void *pointer, size_t previous,
                         size_t size) {
  pg_fault *fault = (pg_fault *)context;
  if (size == 0u) {
    free(pointer);
    return NULL;
  }
  if (fault->armed && (pointer == NULL || size > previous)) {
    if (fault->remaining == 0u) {
      fault->fired = 1;
      return NULL;
    }
    --fault->remaining;
  }
  return realloc(pointer, size);
}
static void option(lua_State *L, const char *name, const char *value) {
  lua_pushstring(L, value);
  lua_setfield(L, -2, name);
}
int main(int argc, char **argv) {
  const char *port = getenv("VECTIS_PG_PORT"),
             *path = getenv("VECTIS_PG_PASSWORD_FILE");
  char password[256];
  FILE *file;
  size_t i;
  int failures = 0, completed = 0, status;
  pg_fault fault;
  lua_State *L;
  assert(argc == 2 && port != NULL && path != NULL);
  file = fopen(path, "rb");
  assert(file != NULL && fgets(password, sizeof(password), file) != NULL);
  assert(fclose(file) == 0);
  password[strcspn(password, "\r\n")] = '\0';
  for (i = 0u; i < 2048u; ++i) {
    fault.remaining = i;
    fault.armed = fault.fired = 0;
    L = lua_newstate(pg_allocate, &fault, 0u);
    assert(L != NULL);
    luaL_openlibs(L);
    luaL_requiref(L, "vectis.pg.connection.core",
                  luaopen_vectis_pg_connection_core, 0);
    lua_pop(L, 1);
    lua_newtable(L);
    lua_newtable(L);
    option(L, "host", "127.0.0.1");
    option(L, "port", port);
    option(L, "user", "vectis");
    option(L, "dbname", "vectis");
    option(L, "password", password);
    option(L, "sslmode", "disable");
    lua_setfield(L, -2, "parameters");
    lua_setglobal(L, "PG_TEST_OPTIONS");
    assert(luaL_loadfile(L, argv[1]) == LUA_OK);
    fault.armed = 1;
    status = lua_pcall(L, 0, 1, 0);
    fault.armed = 0;
    if (fault.fired) {
      ++failures;
      assert(status != LUA_OK);
    } else {
      if (status != LUA_OK)
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
      assert(status == LUA_OK && lua_toboolean(L, -1));
      completed = 1;
    }
    lua_close(L);
    if (completed)
      break;
  }
  L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  luaL_requiref(L, "vectis.pg.connection.core",
                luaopen_vectis_pg_connection_core, 0);
  lua_pop(L, 1);
  lua_newtable(L);
  lua_newtable(L);
  option(L, "host", "127.0.0.1");
  option(L, "port", port);
  option(L, "user", "vectis");
  option(L, "dbname", "vectis");
  option(L, "password", password);
  option(L, "sslmode", "disable");
  lua_setfield(L, -2, "parameters");
  lua_setglobal(L, "PG_TEST_OPTIONS");
  assert(luaL_dostring(
             L, "pg=require('vectis.pg.connection.core'); "
                "db=assert(pg.connect(PG_TEST_OPTIONS)); "
                "params={1}; local fields={}; for i=1,256 do "
                "fields[i]='$'..i..'::int' end; "
                "sql='SELECT '..table.concat(fields,','); "
                "fixture_mutate=function() for i=2,256 do params[i]=i end; "
                "collectgarbage('collect') end; "
                "return function() local q <close> = "
                "assert(db:query(sql,params)); return q:next()[256] end") ==
         LUA_OK);
  mutate_parameters = 1;
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  assert(mutate_parameters == 0 && strcmp(lua_tostring(L, -1), "256") == 0);
  lua_close(L);
  memset(password, 0, sizeof(password));
  assert(failures > 0 && completed);
  printf("PostgreSQL Lua allocation failures: %d; clean execution completed\n",
         failures);
  return 0;
}
