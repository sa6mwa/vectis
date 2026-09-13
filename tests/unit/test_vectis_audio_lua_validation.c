#include <assert.h>
#include <stdlib.h>

static size_t allocations;
static void *frame_malloc(size_t size) {
  void *pointer = malloc(size);
  if (pointer != NULL)
    ++allocations;
  return pointer;
}
static void frame_free(void *pointer) {
  if (pointer != NULL) {
    assert(allocations != 0u);
    --allocations;
  }
  free(pointer);
}

/* Exercise the private adapter with a capture receiver, without a microphone.
 * Count its native frame allocations across Lua's nonlocal error unwinding. */
#define malloc frame_malloc
#define free frame_free
#include "../../src/vectis_audio_lua.c"
#undef malloc
#undef free

static int capture_reads;
static cpkt_audio_result capture_read(cpkt_audio_capture *capture,
                                      float *frames, size_t capacity,
                                      size_t *read_count) {
  (void)capture;
  (void)frames;
  (void)capacity;
  ++capture_reads;
  *read_count = 0u;
  return CPKT_AUDIO_OK;
}

static int read_array(lua_State *lua) {
  size_t count;
  float *frames = vectis_audio_lua_frame_array(lua, 1, &count);
  frame_free(frames);
  lua_pushinteger(lua, (lua_Integer)count);
  return 1;
}

int main(void) {
  lua_State *lua = luaL_newstate();
  cpkt_audio_capture capture;
  vectis_audio_capture_lua *handle;
  lua_Integer capacity;
  int i;
  assert(lua != NULL);
  assert(luaopen_audio(lua) == 1);
  lua_pop(lua, 1);
  memset(&capture, 0, sizeof(capture));
  capture.read_f32_mono_16k = capture_read;
  handle = vectis_audio_lua_new_capture(lua, &capture);
  capacity = (lua_Integer)(((size_t)-1) / sizeof(float)) + 1;
  for (i = 0; i < 3; ++i) {
    lua_pushcfunction(lua, vectis_audio_lua_capture_read);
    lua_pushvalue(lua, 1);
    lua_pushinteger(lua, i == 0 ? -1 : capacity + i - 1);
    assert(lua_pcall(lua, 2, 1, 0) != LUA_OK);
    lua_pop(lua, 1);
    assert(capture_reads == 0 && allocations == 0u);
  }
  lua_pushcfunction(lua, vectis_audio_lua_capture_read);
  lua_pushvalue(lua, 1);
  lua_pushinteger(lua, 0);
  assert(lua_pcall(lua, 2, 2, 0) == LUA_OK);
  lua_pop(lua, 2);
  assert(capture_reads == 1 && allocations == 0u);
  handle->capture = NULL;
  lua_pop(lua, 1);
  for (i = 0; i < 30; ++i) {
    int j;
    lua_pushcfunction(lua, read_array);
    lua_createtable(lua, 160, 0);
    for (j = 1; j <= 160; ++j) {
      if (j == (i % 2 ? 160 : 1))
        lua_newtable(lua);
      else
        lua_pushnumber(lua, 0.0);
      lua_rawseti(lua, -2, j);
    }
    assert(lua_pcall(lua, 1, 1, 0) != LUA_OK);
    lua_pop(lua, 1);
    assert(allocations == 0u);
  }
  lua_close(lua);
  return 0;
}
