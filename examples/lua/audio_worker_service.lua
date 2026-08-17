local vectis = require("vectis")
local audio_worker = require("vectis.audio_worker")

local output_path = os.getenv("VECTIS_LUA_AUDIO_WORKER_EXAMPLE_PATH") or "/tmp/vectis-audio-worker-example.wav"
local frames = {}
for i = 1, 160 do
  frames[i] = ((i - 1) % 32 - 16) / 32
end
local vox_frames = {}
for i = 1, 4800 do
  vox_frames[i] = i <= 1600 and 0.4 or 0.0
end

os.remove(output_path)

local requests = vectis.mailbox.new({
  capacity = 8,
  max_payload_bytes = 262144,
})
local broker = vectis.mailbox.broker({
  requests = requests,
  max_pending = 4,
  reply_mailbox = {
    capacity = 4,
    max_payload_bytes = 262144,
  },
})
local events = vectis.mailbox.new({
  capacity = 16,
  max_payload_bytes = 262144,
})

local server = assert(vectis.server.new({
  app_name = "lua-audio-worker-example",
  service_failure_policy = "fail_closed",
}))

local callback_service, callback_err = server:audio_worker_service({
  request_mailbox = requests,
  callback = function()
    error("audio worker services must not call Lua callbacks")
  end,
})
assert(callback_service == nil)
assert(callback_err.message:find("does not accept Lua callbacks", 1, true))

assert(server:audio_worker_service({
  name = "lua-audio-worker",
  request_mailbox = requests,
  reply_broker = broker,
  event_mailbox = events,
  poll_timeout_ms = 10,
  max_frames = 1024,
  max_segment_frames = 8192,
}) == true)

local started, start_err = server:start()
assert(started == true, start_err and start_err.message or tostring(start_err))

local encode_event = assert(audio_worker.encode_file_request({
  path = output_path,
  format = "wav",
  sample_rate = 16000,
  channels = 1,
  frames = frames,
}))
assert(encode_event.kind == audio_worker.ENCODE_KIND)
assert(encode_event.expects_reply == true)

local encode_reply, encode_request_err = broker:request(encode_event, { timeout_ms = 3000 })
if encode_reply == nil then
  error(encode_request_err and encode_request_err.message or tostring(encode_request_err))
end
local encoded, encode_decode_err = audio_worker.decode_reply(encode_reply)
if encoded == nil then
  error(encode_decode_err and encode_decode_err.message or tostring(encode_decode_err))
end
assert(encoded.ok == true, encoded.message or "audio encode failed")
assert(encoded.operation == "encode")

local decode_event = assert(audio_worker.decode_file_request({
  path = output_path,
  encoding = "wav",
  max_frames = 1024,
}))
assert(decode_event.kind == audio_worker.DECODE_KIND)
assert(decode_event.expects_reply == true)

local decode_reply, decode_request_err = broker:request(decode_event, { timeout_ms = 3000 })
if decode_reply == nil then
  error(decode_request_err and decode_request_err.message or tostring(decode_request_err))
end
local decoded, decode_decode_err = audio_worker.decode_reply(decode_reply)
if decoded == nil then
  error(decode_decode_err and decode_decode_err.message or tostring(decode_decode_err))
end
assert(decoded.ok == true, decoded.message or "audio decode failed")
assert(decoded.operation == "decode")
assert(decoded.sample_rate == 16000)
assert(decoded.channels == 1)
assert(decoded.frame_count > 0)
assert(type(decoded.frames) == "table")
assert(#decoded.frames == decoded.frame_count)

local vox_event = assert(audio_worker.vox_request({
  frames = vox_frames,
  threshold = 0.05,
  release_silence_ms = 10,
  prebuffer_ms = 0,
  min_segment_ms = 1,
  max_segment_frames = 8192,
}))
assert(vox_event.kind == audio_worker.VOX_KIND)
assert(vox_event.expects_reply == true)

local vox_reply, vox_request_err = broker:request(vox_event, { timeout_ms = 3000 })
if vox_reply == nil then
  error(vox_request_err and vox_request_err.message or tostring(vox_request_err))
end
local vox_done, vox_decode_err = audio_worker.decode_reply(vox_reply)
if vox_done == nil then
  error(vox_decode_err and vox_decode_err.message or tostring(vox_decode_err))
end
assert(vox_done.ok == true, vox_done.message or "audio VOX failed")
assert(vox_done.operation == "vox")

local saw_state = false
local saw_segment = false
for _ = 1, 8 do
  if saw_state and saw_segment then
    break
  end
  local observed = events:next(100)
  if observed ~= nil then
    if observed.kind == audio_worker.VOX_STATE_KIND then
      local state = assert(audio_worker.decode_vox_state(observed))
      assert(state.state == 1 or state.state == 2 or state.state == 3)
      saw_state = true
    elseif observed.kind == audio_worker.VOX_SEGMENT_KIND then
      local segment = assert(audio_worker.decode_vox_segment(observed))
      assert(segment.frame_count > 0)
      assert(type(segment.frames) == "table")
      saw_segment = true
    end
  end
end
assert(saw_state)
assert(saw_segment)

local states, states_err = server:audio_worker_service_states()
if states == nil then
  error(states_err and states_err.message or tostring(states_err))
end
assert(#states == 1)
assert(states[1].name == "lua-audio-worker")
assert(states[1].started == true)

local stopped, stop_err = server:stop()
assert(stopped == true, stop_err and stop_err.message or tostring(stop_err))
server:close()
broker:close()
events:close()
requests:close()
os.remove(output_path)

print("lua audio worker service example ok")
