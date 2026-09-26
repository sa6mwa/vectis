#include <assert.h>
#include <lualib.h>
#include <stdio.h>
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

static int new_callback_encoder(lua_State *lua) {
  vectis_audio_encoder_lua *handle;

  luaL_checktype(lua, 1, LUA_TFUNCTION);
  handle = vectis_audio_lua_new_encoder(lua, NULL, 1u);
  lua_pushvalue(lua, 1);
  handle->write_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  return 1;
}

static int invoke_encoder_callback(lua_State *lua) {
  vectis_audio_encoder_lua *handle;

  handle = vectis_audio_lua_check_encoder(lua, 1);
  lua_pushinteger(lua, (lua_Integer)vectis_audio_lua_write_cb(handle, "x", 1u));
  return 1;
}

static void run_lua(lua_State *lua, const char *script) {
  int result;

  result = luaL_dostring(lua, script);
  if (result != LUA_OK) {
    fprintf(stderr, "%s\n", lua_tostring(lua, -1));
  }
  assert(result == LUA_OK);
}

int main(void) {
  lua_State *lua = luaL_newstate();
  cpkt_audio_capture capture;
  vectis_audio_capture_lua *handle;
  lua_Integer capacity;
  int i;
  assert(lua != NULL);
  luaL_openlibs(lua);
  assert(luaopen_audio(lua) == 1);
  lua_setglobal(lua, "audio");
  lua_pushcfunction(lua, new_callback_encoder);
  lua_setglobal(lua, "new_callback_encoder");
  lua_pushcfunction(lua, invoke_encoder_callback);
  lua_setglobal(lua, "invoke_encoder_callback");
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
  run_lua(
      lua,
      "weak = setmetatable({}, {__mode='v'})\n"
      "local co = coroutine.create(function()\n"
      "  weak.thread = coroutine.running()\n"
      "  encoder = new_callback_encoder(function(bytes) return #bytes end)\n"
      "end)\n"
      "assert(coroutine.resume(co)); co = nil; collectgarbage('collect')\n"
      "assert(weak.thread ~= nil)\n"
      "assert(invoke_encoder_callback(encoder) == 1)\n"
      "assert(encoder:close()); encoder = nil\n"
      "collectgarbage('collect'); collectgarbage('collect')\n"
      "assert(weak.thread == nil)\n");
  run_lua(lua,
          "local frames={}; for i=1,320 do frames[i]=0.1 end\n"
          "for kind=1,4 do\n"
          "  local weak=setmetatable({}, {__mode='v'})\n"
          "  local retained, calls\n"
          "  calls=0\n"
          "  local ptt=assert(audio.ptt.open({min_segment_ms=1,\n"
          "    segment=function(segment)\n"
          "      calls=calls+1; weak.segment=segment\n"
          "      if kind~=1 then retained=segment end\n"
          "      segment=nil; collectgarbage('collect')\n"
          "      assert(weak.segment~=nil)\n"
          "      if kind==3 then error('segment failed') end\n"
          "      return kind~=4\n"
          "    end}))\n"
          "  assert(ptt:press()); assert(ptt:push_f32_mono_16k(frames))\n"
          "  local released,err=ptt:release(); assert(calls==1)\n"
          "  if kind<3 then assert(released)\n"
          "  else assert(released==nil and type(err)=='table') end\n"
          "  if retained then\n"
          "    local ok,message=pcall(retained.info, retained)\n"
          "    assert(not ok and message:find('valid only during callback'))\n"
          "  else\n"
          "    collectgarbage('collect'); assert(weak.segment==nil)\n"
          "  end\n"
          "  assert(ptt:close())\n"
          "end\n");
  run_lua(lua,
          "local encoder; local attempts=0\n"
          "local function check_busy()\n"
          "  if not encoder then return end\n"
          "  attempts=attempts+1\n"
          "  local ok,message=pcall(encoder.close,encoder)\n"
          "  assert(not ok and message:find('encoder is busy'))\n"
          "  ok,message=pcall(encoder.write_f32,encoder,{0.1})\n"
          "  assert(not ok and message:find('encoder is busy'))\n"
          "end\n"
          "encoder=assert(audio.encoder.open_writer({format='wav',\n"
          "  write=function(chunk) check_busy(); return #chunk end,\n"
          "  seek=function() check_busy(); return true end}))\n"
          "assert(encoder:write_f32({0.1,0.2,0.3})==3)\n"
          "assert(attempts>0); assert(encoder:write_f32({0.4})==1)\n"
          "local before=attempts\n"
          "assert(encoder:close()); assert(attempts>before)\n"
          "assert(encoder:close())\n"
          "encoder=nil\n"
          "local once=false\n"
          "encoder=assert(audio.encoder.open_writer({format='wav',\n"
          "  write=function(chunk)\n"
          "    if encoder and not once then once=true; encoder:close() end\n"
          "    return #chunk\n"
          "  end, seek=function() return true end}))\n"
          "local closed,err=encoder:close()\n"
          "assert(closed==nil and type(err)=='table'); assert(once)\n"
          "assert(encoder:close())\n");
  assert(allocations == 0u);
  lua_close(lua);
  return 0;
}
