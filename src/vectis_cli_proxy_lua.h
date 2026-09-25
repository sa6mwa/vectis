#ifndef VECTIS_CLI_PROXY_LUA_H
#define VECTIS_CLI_PROXY_LUA_H

#include <lua.h>
#include <vectis/proxy.h>

typedef struct vectis_lua_proxy_route {
  lua_State *lua;
  int rewrite_ref;
  struct vectis_lua_proxy_route *next;
} vectis_lua_proxy_route;

/* Retain a Lua rewrite function until the owning app closes. */
vectis_lua_proxy_route *vectis_lua_proxy_route_new(lua_State *lua, int index);
void vectis_lua_proxy_route_free(vectis_lua_proxy_route *route);

/* Bridge the public C callback to the owning Kore worker's Lua VM. */
vectis_status vectis_lua_proxy_rewrite(const vectis_proxy_inbound *in,
                                       vectis_proxy_outbound *out,
                                       void *userdata, vectis_error *error);

#endif
