local audio = require("audio")

assert(type(audio.capture.open_default) == "function")
assert(type(audio.playback.open_default) == "function")

local tone = {}
for i = 1, 320 do
  tone[i] = math.sin(((i - 1) * 440 * 2.0 * math.pi) / 16000) * 0.05
end
assert(#tone == 320)

if os.getenv("VECTIS_LUA_AUDIO_DEVICE_EXAMPLE") == "1" then
  local backend = tonumber(os.getenv("VECTIS_LUA_AUDIO_DEVICE_BACKEND") or "") or
      audio.DEVICE_BACKEND_AUTO

  local capture_events = 0
  local capture = assert(audio.capture.open_default({
    backend = backend,
    buffer_ms = 200,
    period_ms = 20,
    state = function(event)
      capture_events = capture_events + 1
      assert(type(event.state) == "number")
      assert(type(event.frame_count) == "number")
      return 0
    end,
  }))
  assert(capture:start() == true)
  capture:wait_ready(100)
  local captured = assert(capture:read_f32_mono_16k(160))
  assert(type(captured) == "table")
  capture:stop()
  assert(capture:close() == true)

  local playback = assert(audio.playback.open_default({
    backend = backend,
    buffer_ms = 200,
    period_ms = 20,
  }))
  assert(playback:start() == true)
  assert(playback:write_f32_mono_16k(tone) == #tone)
  playback:drain()
  playback:stop()
  assert(playback:close() == true)
end

print("lua audio devices example ok")
