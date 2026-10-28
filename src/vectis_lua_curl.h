#ifndef VECTIS_LUA_CURL_H
#define VECTIS_LUA_CURL_H
#include <lua.h>
void vectis_lua_curl_register(lua_State *);
int luaopen_vectis_curl_handles_core(lua_State *);
#endif
