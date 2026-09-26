#ifndef VECTIS_AUDIO_LUA_H
#define VECTIS_AUDIO_LUA_H

#include <cpkt/audio.h>
#include <lua.h>

/* Acquire exclusive use until release_decoder; no Lua calls that can raise
 * may occur before release. Unavailable, busy or failed handles return zero. */
int vectis_audio_lua_borrow_decoder(lua_State *lua, int index,
                                    cpkt_audio_decoder **out);
void vectis_audio_lua_release_decoder(lua_State *lua, int index);
/* Push the original callback error, if any, without raising. Consumers must
 * release native resources and the borrow before propagating with lua_error. */
int vectis_audio_lua_push_decoder_error(lua_State *lua, int index);
int vectis_audio_lua_borrow_vox_segment(lua_State *lua, int index,
                                        cpkt_audio_vox_segment **out);

#endif
