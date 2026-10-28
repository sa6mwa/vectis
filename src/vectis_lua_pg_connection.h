#ifndef VECTIS_LUA_PG_CONNECTION_H
#define VECTIS_LUA_PG_CONNECTION_H
#include <lua.h>
#include <vectis/sql.h>
int luaopen_vectis_pg_connection_core(lua_State *);
int vectis_lua_pg_declare(lua_State *, vectis_app *, void *, void (*)(void *),
                          void (*)(void *));
#endif
