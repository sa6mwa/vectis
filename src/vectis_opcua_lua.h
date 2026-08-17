#ifndef VECTIS_OPCUA_LUA_H
#define VECTIS_OPCUA_LUA_H

#include <cpkt/opcua.h>
#include <lua.h>

int vectis_opcua_lua_borrow_server(lua_State *lua, int index,
                                   cpkt_opcua_server **out,
                                   int *has_lua_callbacks_out);

#endif
