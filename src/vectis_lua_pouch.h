#ifndef VECTIS_LUA_POUCH_H
#define VECTIS_LUA_POUCH_H
#include <lc/lc.h>
#include <lua.h>
/* Selected strings are rooted in the caller's pins table until C capture. */
void vectis_lua_pouch_parse(lua_State *, int, lc_pouch_settings *, int pins);
#endif
