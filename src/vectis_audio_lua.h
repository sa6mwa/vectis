#ifndef VECTIS_AUDIO_LUA_H
#define VECTIS_AUDIO_LUA_H

#include <cpkt/audio.h>
#include <lua.h>

int vectis_audio_lua_borrow_decoder(lua_State *lua, int index,
                                    cpkt_audio_decoder **out);
int vectis_audio_lua_borrow_vox_segment(lua_State *lua, int index,
                                        cpkt_audio_vox_segment **out);

#endif
