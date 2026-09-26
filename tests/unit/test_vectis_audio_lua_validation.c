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
  run_lua(
      lua,
      "local frames={}; for i=1,320 do frames[i]=0.1 end\n"
      "for _,kind in ipairs({'ptt','vox'}) do\n"
      "  local handle, segments, states\n"
      "  segments,states=0,0\n"
      "  local function check_busy()\n"
      "    if not handle then return end\n"
      "    local methods={'close','flush','push_f32_mono_16k','__gc'}\n"
      "    if kind=='ptt' then methods[#methods+1]='press'; "
      "methods[#methods+1]='release' end\n"
      "    for _,method in ipairs(methods) do\n"
      "      local fn=method=='__gc' and getmetatable(handle).__gc or "
      "handle[method]\n"
      "      local ok,err=pcall(fn,handle,{0.1})\n"
      "      assert(not ok and tostring(err):find('audio '..kind..' is "
      "busy',1,true))\n"
      "    end\n"
      "  end\n"
      "  handle=assert(audio[kind].open({min_segment_ms=1,threshold=0.05,\n"
      "    prebuffer_ms=0,release_silence_ms=1,\n"
      "    segment=function(segment)\n"
      "      segments=segments+1; check_busy(); collectgarbage('collect')\n"
      "      local info=assert(segment:info()); assert(info.frame_count==320)\n"
      "      local got,count=segment:read_f32_mono_16k(1)\n"
      "      assert(count==1 and #got==1 and math.abs(got[1]-0.1)<0.001)\n"
      "      return true\n"
      "    end,state=function() states=states+1; check_busy(); return true "
      "end}))\n"
      "  for attempt=1,2 do\n"
      "    if kind=='ptt' then assert(handle:press()) end\n"
      "    assert(handle:push_f32_mono_16k(frames))\n"
      "    if kind=='ptt' then assert(handle:release()) else "
      "assert(handle:flush()) end\n"
      "  end\n"
      "  assert(segments==2 and states>0); assert(handle:close()); "
      "assert(handle:close())\n"
      "end\n"
      "local ptt\n"
      "ptt=assert(audio.ptt.open({min_segment_ms=1,segment=function() "
      "ptt:close() end}))\n"
      "assert(ptt:press()); assert(ptt:push_f32_mono_16k(frames))\n"
      "local released,err=ptt:release()\n"
      "assert(released==nil and type(err)=='table'); assert(ptt:close())\n"
      "local payload=string.rep('\\0',32000)\n"
      "local wav='RIFF'..string.pack('<I4',36+#payload)..'WAVEfmt '..\n"
      "  string.pack('<I4I2I2I4I4I2I2',16,1,1,16000,32000,2,16)..\n"
      "  'data'..string.pack('<I4',#payload)..payload\n"
      "local function source(opts)\n"
      "  local pos, decoder, calls=1,nil,0\n"
      "  local function check_busy()\n"
      "    if not decoder then return end\n"
      "    calls=calls+1\n"
      "    for _,method in ipairs({'close','info','read_f32_mono_16k','__gc'}) "
      "do\n"
      "      local fn=method=='__gc' and getmetatable(decoder).__gc or "
      "decoder[method]\n"
      "      local ok,err=pcall(fn,decoder,1)\n"
      "      assert(not ok and tostring(err):find('audio decoder is "
      "busy',1,true))\n"
      "    end\n"
      "  end\n"
      "  decoder=audio.decoder.open_reader({encoding='wav',read=function(n)\n"
      "    check_busy()\n"
      "    if opts.fail then error(opts.fail,0) end\n"
      "    local chunk=wav:sub(pos,pos+n-1); pos=pos+#chunk; return chunk\n"
      "  end,seek=function(offset,origin)\n"
      "    check_busy()\n"
      "    if opts.seek_fail then error(opts.seek_fail,0) end\n"
      "    if origin==audio.SEEK_SET then pos=offset+1\n"
      "    elseif origin==audio.SEEK_CUR then pos=pos+offset\n"
      "    else pos=#wav+offset+1 end\n"
      "    return true\n"
      "  end})\n"
      "  return decoder,function() return calls end\n"
      "end\n"
      "local marker={message='upstream reader failed'}\n"
      "for attempt=1,3 do\n"
      "  local ok,err=pcall(source,{fail=marker})\n"
      "  assert(not ok and err==marker)\n"
      "  ok,err=pcall(source,{seek_fail=marker})\n"
      "  assert(not ok and err==marker)\n"
      "  local opts={}\n"
      "  local decoder=assert(source(opts))\n"
      "  opts.fail=marker\n"
      "  ok,err=pcall(decoder.read_f32_mono_16k,decoder,16000)\n"
      "  assert(not ok and err==marker)\n"
      "  opts.fail=nil\n"
      "  ok,err=pcall(decoder.read_f32_mono_16k,decoder,1)\n"
      "  assert(not ok and err==marker)\n"
      "  ok,err=pcall(decoder.info,decoder)\n"
      "  assert(not ok and err==marker)\n"
      "  assert(decoder:close()); assert(decoder:close()); decoder=nil\n"
      "  collectgarbage('collect')\n"
      "end\n"
      "local decoder,calls=source({})\n"
      "assert(decoder)\n"
      "local total=0\n"
      "while true do\n"
      "  local got,count,result=decoder:read_f32_mono_16k(1024)\n"
      "  assert(got and #got==count)\n"
      "  total=total+count\n"
      "  if result==audio.AT_END then break end\n"
      "  assert(result==audio.OK)\n"
      "end\n"
      "assert(total==16000 and calls()>0)\n"
      "assert(decoder:close()); assert(decoder:close())\n");
  assert(allocations == 0u);
  lua_close(lua);
  return 0;
}
