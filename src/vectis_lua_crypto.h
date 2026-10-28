#ifndef VECTIS_LUA_CRYPTO_H
#define VECTIS_LUA_CRYPTO_H
#include <lua.h>
void vectis_lua_crypto_register(lua_State *);
int luaopen_vectis_crypto_core(lua_State *);
#endif
