#ifndef VECTIS_LUA_LOCKDC_H
#define VECTIS_LUA_LOCKDC_H
#include <lua.h>
#include <vectis/vectis.h>
int vectis_lua_lockdc_open(lua_State *, vectis_app *);
int luaopen_vectis_lockd_core(lua_State *);
#endif
