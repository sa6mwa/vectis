#include "vectis_cli.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>
#include <vectis/vectis.h>

static void vectis_cli_usage(FILE *stream) {
  fputs("usage: vectis [--version] [--help] script.lua [args...]\n", stream);
}

static int vectis_lua_status_string(lua_State *lua) {
  lua_Integer status;
  const char *name;

  status = luaL_checkinteger(lua, 1);
  name = vectis_status_string((vectis_status)status);
  if (name == NULL) {
    lua_pushnil(lua);
  } else {
    lua_pushstring(lua, name);
  }
  return 1;
}

static int luaopen_vectis(lua_State *lua) {
  lua_newtable(lua);
  lua_pushliteral(lua, "0.0.0");
  lua_setfield(lua, -2, "version");
  lua_pushinteger(lua, VECTIS_OK);
  lua_setfield(lua, -2, "OK");
  lua_pushinteger(lua, VECTIS_ERR_INVALID);
  lua_setfield(lua, -2, "ERR_INVALID");
  lua_pushcfunction(lua, vectis_lua_status_string);
  lua_setfield(lua, -2, "status_string");
  return 1;
}

static void vectis_lua_preload(lua_State *lua) {
  lua_getglobal(lua, "package");
  lua_getfield(lua, -1, "preload");
  lua_pushcfunction(lua, luaopen_vectis);
  lua_setfield(lua, -2, "vectis");
  lua_pop(lua, 2);
}

static void vectis_lua_set_arg(lua_State *lua, int argc, char **argv, int script_index) {
  int i;

  lua_newtable(lua);
  lua_pushstring(lua, argv[script_index]);
  lua_rawseti(lua, -2, 0);
  for (i = script_index + 1; i < argc; ++i) {
    lua_pushstring(lua, argv[i]);
    lua_rawseti(lua, -2, i - script_index);
  }
  lua_setglobal(lua, "arg");
}

static int vectis_lua_run_script(int argc, char **argv, int script_index) {
  lua_State *lua;
  int status;

  lua = luaL_newstate();
  if (lua == NULL) {
    fputs("vectis: failed to allocate Lua state\n", stderr);
    return 70;
  }
  luaL_openlibs(lua);
  vectis_lua_preload(lua);
  vectis_lua_set_arg(lua, argc, argv, script_index);

  status = luaL_loadfile(lua, argv[script_index]);
  if (status == LUA_OK) {
    status = lua_pcall(lua, 0, LUA_MULTRET, 0);
  }
  if (status != LUA_OK) {
    fprintf(stderr, "vectis: %s\n", lua_tostring(lua, -1));
    lua_close(lua);
    return 1;
  }
  lua_close(lua);
  return 0;
}

int vectis_cli_main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--help") == 0) {
    vectis_cli_usage(stdout);
    return 0;
  }
  if (argc > 1 && strcmp(argv[1], "--version") == 0) {
    puts("vectis 0.0.0");
    return 0;
  }

  if (argc > 1) {
    return vectis_lua_run_script(argc, argv, 1);
  }
  vectis_cli_usage(stderr);
  return 64;
}
