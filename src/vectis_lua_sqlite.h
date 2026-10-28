#ifndef VECTIS_LUA_SQLITE_H
#define VECTIS_LUA_SQLITE_H
#include <lua.h>
#include <vectis/sql.h>
int luaopen_vectis_sqlite_core(lua_State *);
int vectis_lua_sqlite_declare(lua_State *, vectis_app *, void *,
                              void (*)(void *), void (*)(void *));
#endif
