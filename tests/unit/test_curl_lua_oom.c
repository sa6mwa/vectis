#include "vectis_lua_curl.h"
#include <assert.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
static int curl_close_fault;
static int curl_test_fclose(FILE *file) {
  int result = fclose(file);
  if (curl_close_fault) {
    curl_close_fault = 0;
    return EOF;
  }
  return result;
}
#define fclose curl_test_fclose
#include "../../src/vectis_lua_curl.c"
#undef fclose
static int curl_arm_close_fault(lua_State *L) {
  (void)L;
  curl_close_fault = 1;
  return 0;
}
static int curl_finalize_children(lua_State *L) {
  int i;
  luaL_checkudata(L, 1, CURL_MULTI);
  lua_getiuservalue(L, 1, 1);
  for (i = 1; i <= (int)VECTIS_CURL_MULTI_LIMIT; ++i) {
    lua_rawgeti(L, -1, i);
    if (!lua_isnil(L, -1)) {
      lua_pushcfunction(L, curl_lua_request_gc);
      lua_pushvalue(L, -2);
      lua_call(L, 1, 0);
    }
    lua_pop(L, 1);
  }
  return 0;
}
typedef struct fault {
  size_t remaining;
  int armed, fired;
} fault;
static void *allocate(void *context, void *pointer, size_t previous,
                      size_t size) {
  fault *f = (fault *)context;
  if (size == 0u) {
    free(pointer);
    return NULL;
  }
  if (f->armed && (pointer == NULL || size > previous)) {
    if (f->remaining == 0u) {
      f->fired = 1;
      return NULL;
    }
    --f->remaining;
  }
  return realloc(pointer, size);
}
static void completion_recovery(const char *source, const char *script,
                                const char *output) {
  size_t i;
  int completed = 0, failures = 0, status;
  fault f;
  lua_State *lua;
  for (i = 0u; i < 1024u; ++i) {
    f.remaining = i;
    f.armed = f.fired = 0;
    lua = lua_newstate(allocate, &f, 0u);
    assert(lua != NULL);
    luaL_openlibs(lua);
    luaL_requiref(lua, "vectis.curl.handles.core",
                  luaopen_vectis_curl_handles_core, 0);
    lua_pop(lua, 1);
    lua_pushstring(lua, source);
    lua_setglobal(lua, "CURL_OOM_SOURCE");
    lua_pushstring(lua, output);
    lua_setglobal(lua, "CURL_OOM_OUTPUT");
    lua_pushcfunction(lua, curl_arm_close_fault);
    lua_setglobal(lua, "curl_test_fail_close");
    lua_pushcfunction(lua, curl_finalize_children);
    lua_setglobal(lua, "curl_test_finalize_children");
    assert(luaL_loadfile(lua, script) == LUA_OK &&
           lua_pcall(lua, 0, 2, 0) == LUA_OK);
    lua_pushvalue(lua, 1);
    f.armed = 1;
    status = lua_pcall(lua, 0, 1, 0);
    f.armed = 0;
    if (f.fired) {
      assert(status != LUA_OK);
      ++failures;
    } else {
      assert(status == LUA_OK);
      completed = 1;
    }
    lua_pushvalue(lua, 2);
    lua_pushboolean(lua, status == LUA_OK);
    lua_pushvalue(lua, 3);
    status = lua_pcall(lua, 2, 0, 0);
    if (status != LUA_OK)
      fprintf(stderr, "%s\n", lua_tostring(lua, -1));
    assert(status == LUA_OK && !curl_close_fault);
    lua_close(lua);
    if (completed)
      break;
  }
  assert(failures > 0 && completed);
  printf("Curl completion-copy failures: %d; every queued result recovered\n",
         failures);
}
int main(int argc, char **argv) {
  size_t i;
  int completed = 0, failures = 0, status;
  fault f;
  lua_State *lua;
  assert(argc == 4);
  for (i = 0u; i < 1024u; ++i) {
    f.remaining = i;
    f.armed = f.fired = 0;
    lua = lua_newstate(allocate, &f, 0u);
    assert(lua != NULL);
    luaL_openlibs(lua);
    luaL_requiref(lua, "vectis.curl.handles.core",
                  luaopen_vectis_curl_handles_core, 0);
    lua_pop(lua, 1);
    lua_pushstring(lua, argv[1]);
    lua_setglobal(lua, "CURL_OOM_SOURCE");
    assert(luaL_loadfile(lua, argv[1]) == LUA_OK);
    f.armed = 1;
    status = lua_pcall(lua, 0, 1, 0);
    f.armed = 0;
    if (f.fired) {
      ++failures;
      assert(status != LUA_OK);
    } else {
      if (status != LUA_OK)
        fprintf(stderr, "%s\n", lua_tostring(lua, -1));
      assert(status == LUA_OK && lua_toboolean(lua, -1));
      completed = 1;
    }
    lua_close(lua);
    if (completed)
      break;
  }
  assert(failures > 0 && completed);
  printf("Curl Lua allocation failures: %d; clean execution completed\n",
         failures);
  completion_recovery(argv[1], argv[2], argv[3]);
  return 0;
}
