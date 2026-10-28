#include "vectis_lua_xml_objects.h"
#include <assert.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
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
int main(int argc, char **argv) {
  size_t i;
  int completed = 0, failures = 0, status;
  fault f;
  lua_State *lua;
  assert(argc == 2);
  for (i = 0u; i < 1024u; ++i) {
    f.remaining = i;
    f.armed = f.fired = 0;
    lua = lua_newstate(allocate, &f, 0u);
    assert(lua != NULL);
    luaL_openlibs(lua);
    luaL_requiref(lua, "vectis.xml.objects.core",
                  luaopen_vectis_xml_objects_core, 0);
    lua_pop(lua, 1);
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
  printf("XML Lua allocation failures: %d; clean execution completed\n",
         failures);
  return 0;
}
