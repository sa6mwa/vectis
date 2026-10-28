#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "vectis_lua_gz.h"
#include <assert.h>
#include <fcntl.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE 1
#endif
#include <vectis/gz.h>
#include <zlib.h>
static int oom_active;
static void *allocate(void *ud, void *ptr, size_t old, size_t size) {
  (void)ud;
  (void)old;
  if (!size) {
    free(ptr);
    return NULL;
  }
  if (oom_active && size >= 1024)
    return NULL;
  return realloc(ptr, size);
}
static int arm(lua_State *L) {
  (void)L;
  oom_active = 1;
  return 0;
}
static void oom_test(void) {
  lua_State *L;
  FILE *f;
  int i;
  int fd;
  int kept;
  int status;
  f = fopen("gz-oom-plain", "wb");
  assert(f);
  for (i = 0; i < 2048; ++i)
    assert(fputc(i % 251, f) != EOF);
  assert(fclose(f) == 0);
  fd = open("gz-oom-plain", O_RDONLY);
  assert(fd >= 0);
  kept = dup(fd);
  assert(kept >= 0);
  L = lua_newstate(allocate, NULL, 0);
  assert(L);
  luaL_openlibs(L);
  lua_newtable(L);
  vectis_lua_gz_register(L);
  lua_setglobal(L, "zlib");
  lua_pushinteger(L, fd);
  lua_setglobal(L, "fd");
  lua_pushcfunction(L, arm);
  lua_setglobal(L, "arm");
  assert(luaL_dostring(
             L, "g=assert(zlib.gzdopen(fd,'rb')); assert(g:direct()==1)") ==
         LUA_OK);
  assert(luaL_loadstring(L, "arm(); return g:read(1024)") == LUA_OK);
  status = lua_pcall(L, 0, 1, 0);
  oom_active = 0;
  assert(status == LUA_ERRMEM);
  assert(lseek(kept, 0, SEEK_CUR) > 0);
  assert(fcntl(fd, F_GETFD) == -1);
  lua_pop(L, 1);
  assert(luaL_dostring(L, "assert(not pcall(g.read,g,1))") == LUA_OK);
  lua_close(L);
  close(kept);
  assert(remove("gz-oom-plain") == 0);
}
/* Pure public-provider reproducer: bypasses the receiver entirely. */
static int native_tell_probe(void) {
  gzFile file;
  file = gzopen("gz-native-tell.gz", "wb");
  assert(file);
  assert(gzwrite(file, "x", 1) == 1);
  assert(gztell(file) == 1);
  assert(gzclose(file) == Z_OK);
  assert(remove("gz-native-tell.gz") == 0);
  return 0;
}
int main(int argc, char **argv) {
  vectis_gz *g;
  vectis_gz_error e;
  char b[16];
  int fd;
  int saved_errno;
  const char *path = "gz-unit-fixture.gz";
  if (argc == 2 && strcmp(argv[1], "--native-tell") == 0)
    return native_tell_probe();
  assert(vectis_gz_open(path, "wb", &g, &e) == VECTIS_OK);
  assert(g->buffer(g, 32, &e) == 0);
  assert(g->write(g, "a\0b", 3, &e) == 3);
  assert(g->fwrite(g, "cd", 1, 2, &e) == 2);
  assert(g->tell(g, &e) == 5);
  assert(g->close(g, 1, &e) == -2);
  assert(g->close(g, 2, &e) == 0);
  assert(g->close(g, 0, &e) == 0);
  assert(g->read(g, b, 1, &e) == -1);
  g->destroy(g);
  fd = open(path, O_RDONLY);
  assert(fd >= 0);
  assert(vectis_gz_dopen(fd, "bad", &g, &e) != VECTIS_OK);
  assert(fcntl(fd, F_GETFD) >= 0);
  assert(vectis_gz_dopen(fd, "rb", &g, &e) == VECTIS_OK);
  assert(g->fread(g, b, 2, 3, &e) == 2);
  assert(g->tell(g, &e) == 5);
  assert(memcmp(b, "a\0bcd", 5) == 0);
  assert(g->rewind(g, &e) == 0);
  assert(g->seek(g, INT64_MAX, 2, &e) == -1);
  assert(g->read(g, b, 65537, &e) == -1);
  assert(g->fread(g, b, (size_t)-1, 2, &e) == 0 && e.code != 0);
  assert(g->read(g, b, 16, &e) == 5 && e.code == 0);
  assert(g->read(g, b, 1, &e) == 0 && e.code == 0);
  assert(g->eof(g, &e) == 1);
  assert(g->clearerr(g, &e) == 0 && g->eof(g, &e) == 0);
  assert(g->close(g, 1, &e) == 0);
  assert(fcntl(fd, F_GETFD) == -1);
  g->destroy(g);
  fd = open(path, O_RDONLY);
  assert(fd >= 0);
  assert(close(fd) == 0);
  assert(vectis_gz_dopen(fd, "rb", &g, &e) == VECTIS_OK);
  assert(g->read(g, b, 1, &e) == -1 && e.code != 0);
  saved_errno = e.system_errno;
  assert(saved_errno != 0 && g->error(g, &e) == -1 &&
         e.system_errno == saved_errno);
  assert(g->close(g, 1, &e) != 0);
  g->destroy(g);
#ifdef __linux__
  assert(vectis_gz_open("/dev/full", "wb", &g, &e) == VECTIS_OK);
  assert(g->write(g, "x", 1, &e) == 1);
  assert(g->close(g, 2, &e) == -1 && e.system_errno != 0);
  assert(g->close(g, 0, &e) == -1);
  g->destroy(g);
#endif
  assert(remove(path) == 0);
  oom_test();
  return 0;
}
