#include "vectis_lua_pdf.h"
#include <assert.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct pdf_allocation_fault {
  size_t remaining;
  int armed;
  int fired;
} pdf_allocation_fault;
static void *pdf_allocate(void *context, void *pointer, size_t previous,
                          size_t size) {
  pdf_allocation_fault *fault = (pdf_allocation_fault *)context;
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
int main(int argc, char **argv) {
  size_t index;
  unsigned char png[4096];
  char path[4096];
  size_t png_size;
  FILE *file;
  int length;
  int failures = 0, completed = 0;
  assert(argc == 3);
  length = snprintf(path, sizeof(path), "%s/pixels.png", argv[2]);
  assert(length >= 0 && (size_t)length < sizeof(path));
  file = fopen(path, "rb");
  assert(file != NULL);
  png_size = fread(png, 1u, sizeof(png), file);
  assert(png_size != 0u && feof(file));
  assert(fclose(file) == 0);
  for (index = 0u; index < 1024u; ++index) {
    pdf_allocation_fault fault;
    lua_State *L;
    int status;
    fault.remaining = index;
    fault.armed = fault.fired = 0;
    L = lua_newstate(pdf_allocate, &fault, 0u);
    assert(L != NULL);
    luaL_openlibs(L);
    luaL_requiref(L, "vectis.pdf.core", luaopen_vectis_pdf_core, 0);
    lua_pop(L, 1);
    lua_pushstring(L, argv[2]);
    lua_setglobal(L, "PDF_TEST_FIXTURES");
    lua_pushlstring(L, (const char *)png, png_size);
    lua_setglobal(L, "PDF_TEST_PNG");
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
  assert(failures > 0 && completed);
  printf("PDF Lua allocation failures: %d; clean execution completed\n",
         failures);
  return 0;
}
