#ifndef VECTIS_LUA_BUFFER_H
#define VECTIS_LUA_BUFFER_H
#include <lua.h>
#include <vectis/buffer.h>
int luaopen_vectis_buffer_core(lua_State *lua);
/* Vectis-owned binding bridge. Only this module knows its userdata layout.
 * Provider adapters receive independent public C leases, not Lua pointers. */
vectis_status vectis_lua_buffer_acquire_span(lua_State *, int, int,
                                             vectis_buffer_span **,
                                             vectis_error *);
vectis_status vectis_lua_buffer_owned_info(lua_State *, int, size_t, void **,
                                           size_t *, vectis_error *);
vectis_status vectis_lua_buffer_take(lua_State *, int, size_t, vectis_buffer **,
                                     vectis_error *);
void *vectis_lua_buffer_push_scope(lua_State *);
vectis_status vectis_lua_buffer_scope_init(void *, vectis_buffer *, size_t, int,
                                           vectis_error *);
void vectis_lua_buffer_scope_close(void *);
void vectis_lua_buffer_scope_adopt_span(void *, vectis_buffer_span *);
vectis_status vectis_lua_buffer_prepare_take(lua_State *, int, size_t, void **,
                                             vectis_buffer **, vectis_error *);
vectis_buffer *vectis_lua_buffer_finish_take(void *, int);
vectis_status vectis_lua_buffer_close_resource(lua_State *, int,
                                               vectis_error *);
#endif
