#ifndef VECTIS_LUA_SUPERVISOR_H
#define VECTIS_LUA_SUPERVISOR_H
#include <lua.h>
#include <vectis/supervisor.h>
int vectis_lua_supervisor_declare(lua_State *, vectis_app *);
int vectis_lua_supervisor_pump(lua_State *, vectis_app *);
void vectis_lua_supervisor_restore(lua_State *, int app_index);
void vectis_lua_supervisor_invalidate(lua_State *, int app_index);
#endif
