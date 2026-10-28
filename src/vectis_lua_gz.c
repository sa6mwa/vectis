#include "vectis_lua_gz.h"
#include <lauxlib.h>
#include <limits.h>
#include <string.h>
#include <vectis/gz.h>
#define OWNER "vectis.gz.owner"
typedef struct gz_owner {
  vectis_gz *g;
  int advanced;
  unsigned char buffer[VECTIS_GZ_CHUNK_BYTES];
} gz_owner;
static const char *const names[] = {
    "read",      "write",   "fread",   "fwrite", "gets",   "puts",  "buffer",
    "setparams", "getc",    "getc_",   "putc",   "ungetc", "flush", "rewind",
    "seek",      "tell",    "offset",  "eof",    "direct", "error", "clearerr",
    "close",     "close_r", "close_w", "gzopen", "gzdopen"};
static lua_Integer integer(lua_State *L, int i, lua_Integer lo,
                           lua_Integer hi) {
  lua_Integer v;
  if (!lua_isinteger(L, i))
    luaL_error(L, "expected exact integer");
  v = lua_tointeger(L, i);
  if (v < lo || v > hi)
    luaL_error(L, "integer outside allowed domain");
  return v;
}
static const char *bytes(lua_State *L, int i, size_t *n, int cstring) {
  const char *s;
  luaL_checktype(L, i, LUA_TSTRING);
  s = lua_tolstring(L, i, n);
  if (cstring && memchr(s, 0, *n))
    luaL_error(L, "embedded NUL in path or mode");
  return s;
}
static void push_error(lua_State *L, const vectis_gz_error *e) {
  lua_createtable(L, 0, 3);
  lua_pushinteger(L, e->code);
  lua_setfield(L, -2, "code");
  lua_pushinteger(L, e->system_errno);
  lua_setfield(L, -2, "errno");
  lua_pushstring(L, e->message);
  lua_setfield(L, -2, "message");
}
static int gc(lua_State *L) {
  gz_owner *o;
  o = (gz_owner *)luaL_testudata(L, 1, OWNER);
  if (o) {
    vectis_gz_destroy(o->g);
    o->g = NULL;
  }
  return 0;
}
static int driver(lua_State *L) {
  gz_owner *o;
  vectis_gz_error e;
  int op;
  int result;
  int fd;
  size_t n;
  size_t size;
  size_t count;
  size_t items;
  int64_t before;
  int64_t after;
  const char *s;
  const char *mode;
  unsigned char *buffer = NULL;
  op = (int)lua_tointeger(L, lua_upvalueindex(1));
  o = (gz_owner *)luaL_checkudata(L, lua_upvalueindex(2), OWNER);
  buffer = o->buffer;
  memset(&e, 0, sizeof(e));
  result = 0;
  n = 0;
  items = 0;
  if (op >= 24) {
    fd = -1;
    s = NULL;
    if (op == 24)
      s = bytes(L, 1, &n, 1);
    else
      fd = (int)integer(L, 1, 0, INT_MAX);
    mode = bytes(L, 2, &n, 1);
    o->advanced = 1;
    if ((op == 24 ? vectis_gz_open(s, mode, &o->g, &e)
                  : vectis_gz_dopen(fd, mode, &o->g, &e)) != VECTIS_OK) {
      lua_pushnil(L);
      push_error(L, &e);
      return 2;
    }
    lua_pushvalue(L, lua_upvalueindex(2));
    return 1;
  }
  if (!o->g)
    return luaL_error(L, "gzip receiver is closed");
  switch (op) {
  case 0:
  case 4:
    n = (size_t)integer(L, 2, op == 4 ? 1 : 0, VECTIS_GZ_CHUNK_BYTES);
    o->advanced = 1;
    result = op == 0 ? vectis_gz_read(o->g, buffer, n, &e)
                     : vectis_gz_gets(o->g, (char *)buffer, n, &e);
    if (op == 4 && result > 0)
      n = strlen((char *)buffer);
    else if (op == 0 && result >= 0)
      n = (size_t)result;
    if (result < 0 ||
        (result == 0 && (op == 4 || n != 0 || lua_tointeger(L, 2) != 0)))
      lua_pushnil(L);
    else
      lua_pushlstring(L, (char *)buffer, n);
    if (e.code) {
      push_error(L, &e);
      return 2;
    }
    return 1;
  case 1:
  case 5:
    s = bytes(L, 2, &n, 0);
    if (n > VECTIS_GZ_CHUNK_BYTES)
      return luaL_error(L, "chunk exceeds 65536 bytes");
    o->advanced = 1;
    result =
        op == 1 ? vectis_gz_write(o->g, s, n, &e) : vectis_gz_puts(o->g, s, &e);
    break;
  case 2:
  case 3:
    if (op == 3)
      s = bytes(L, 2, &n, 0);
    else
      s = NULL;
    size = (size_t)integer(L, op == 2 ? 2 : 3, 0, VECTIS_GZ_CHUNK_BYTES);
    count = (size_t)integer(L, op == 2 ? 3 : 4, 0, VECTIS_GZ_CHUNK_BYTES);
    if (size && count > VECTIS_GZ_CHUNK_BYTES / size)
      return luaL_error(L, "element product exceeds 65536 bytes");
    if (op == 3 && (n > VECTIS_GZ_CHUNK_BYTES || size * count > n))
      return luaL_error(L, "invalid fwrite bytes");
    before = vectis_gz_tell(o->g, &e);
    if (before < 0) {
      lua_pushnil(L);
      push_error(L, &e);
      return 2;
    }
    o->advanced = 1;
    items = op == 2 ? vectis_gz_fread(o->g, buffer, size, count, &e)
                    : vectis_gz_fwrite(o->g, s, size, count, &e);
    if (op == 2) {
      vectis_gz_error position_error;
      after = vectis_gz_tell(o->g, &position_error);
      if (after < before || (uint64_t)(after - before) > size * count)
        return luaL_error(L, "invalid native fread position");
      lua_pushlstring(L, (char *)buffer, (size_t)(after - before));
      lua_pushinteger(L, (lua_Integer)items);
      if (e.code) {
        push_error(L, &e);
        return 3;
      }
      return 2;
    }
    lua_pushinteger(L, (lua_Integer)items);
    if (e.code) {
      push_error(L, &e);
      return 2;
    }
    return 1;
  case 6:
    result = (int)integer(L, 2, 1, VECTIS_GZ_CHUNK_BYTES);
    o->advanced = 1;
    result = vectis_gz_buffer(o->g, (unsigned)result, &e);
    break;
  case 7:
    result = (int)integer(L, 2, -1, 9);
    fd = (int)integer(L, 3, 0, 4);
    o->advanced = 1;
    result = vectis_gz_setparams(o->g, result, fd, &e);
    break;
  case 8:
    o->advanced = 1;
    result = vectis_gz_getc(o->g, &e);
    break;
  case 9:
    o->advanced = 1;
    result = vectis_gz_getc_(o->g, &e);
    break;
  case 10:
  case 11:
  case 12:
    result = (int)integer(L, 2,
                          op == 11   ? -1
                          : op == 12 ? 0
                                     : INT_MIN,
                          op == 12 ? 6 : INT_MAX);
    o->advanced = 1;
    result = op == 10   ? vectis_gz_putc(o->g, result, &e)
             : op == 11 ? vectis_gz_ungetc(o->g, result, &e)
                        : vectis_gz_flush(o->g, result, &e);
    break;
  case 13:
    o->advanced = 1;
    result = vectis_gz_rewind(o->g, &e);
    break;
  case 14:
  case 15:
  case 16:
    before = 0;
    fd = 0;
    if (op == 14) {
      before = (int64_t)integer(L, 2, LUA_MININTEGER, LUA_MAXINTEGER);
      fd = (int)integer(L, 3, 0, 2);
    }
    o->advanced = 1;
    after = op == 14   ? vectis_gz_seek(o->g, before, fd, &e)
            : op == 15 ? vectis_gz_tell(o->g, &e)
                       : vectis_gz_offset(o->g, &e);
    lua_pushinteger(L, (lua_Integer)after);
    if (e.code) {
      push_error(L, &e);
      return 2;
    }
    return 1;
  case 17:
    result = vectis_gz_eof(o->g, &e);
    break;
  case 18:
    o->advanced = 1;
    result = vectis_gz_direct(o->g, &e);
    break;
  case 19:
    vectis_gz_get_error(o->g, &e);
    lua_pushinteger(L, e.code);
    lua_pushstring(L, e.message);
    lua_pushinteger(L, e.system_errno);
    return 3;
  case 20:
    o->advanced = 1;
    result = vectis_gz_clearerr(o->g, &e);
    break;
  default:
    o->advanced = 1;
    result = vectis_gz_close(o->g, op - 21, &e);
    break;
  }
  lua_pushinteger(L, result);
  if (e.code) {
    push_error(L, &e);
    return 2;
  }
  return 1;
}
static int call(lua_State *L) {
  gz_owner *o;
  int op;
  int argc;
  int status;
  op = (int)lua_tointeger(L, lua_upvalueindex(1));
  argc = lua_gettop(L);
  if (op >= 24) {
    o = (gz_owner *)lua_newuserdatauv(L, sizeof(*o), 0);
    memset(o, 0, sizeof(*o));
    luaL_setmetatable(L, OWNER);
  } else {
    o = (gz_owner *)luaL_checkudata(L, 1, OWNER);
    lua_pushvalue(L, 1);
  }
  o->advanced = 0;
  lua_pushinteger(L, op);
  lua_pushvalue(L, -2);
  lua_pushcclosure(L, driver, 2);
  lua_insert(L, 1);
  lua_pop(L, 1);
  status = lua_pcall(L, argc, LUA_MULTRET, 0);
  if (status != LUA_OK) {
    if (o->advanced) {
      vectis_gz_destroy(o->g);
      o->g = NULL;
    }
    return lua_error(L);
  }
  return lua_gettop(L);
}
static void method(lua_State *L, const char *name, int op) {
  lua_pushinteger(L, op);
  lua_pushcclosure(L, call, 1);
  lua_setfield(L, -2, name);
}
void vectis_lua_gz_register(lua_State *L) {
  int i;
  luaL_newmetatable(L, OWNER);
  lua_pushcfunction(L, gc);
  lua_setfield(L, -2, "__gc");
  method(L, "__close", 21);
  lua_newtable(L);
  for (i = 0; i < 24; ++i)
    method(L, names[i], i);
  method(L, "seek64", 14);
  method(L, "tell64", 15);
  method(L, "offset64", 16);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
  method(L, "gzopen", 24);
  method(L, "gzopen64", 24);
  method(L, "gzdopen", 25);
}
