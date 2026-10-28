#ifndef VECTIS_LUA_DIGEST_H
#define VECTIS_LUA_DIGEST_H
#include <lua.h>
void vectis_lua_digest_register(lua_State *lua);
int luaopen_vectis_digest_core(lua_State *lua);
#endif
