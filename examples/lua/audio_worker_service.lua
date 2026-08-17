local vectis = require("vectis")
local audio_worker = require("vectis.audio_worker")

local output_path = os.getenv("VECTIS_LUA_AUDIO_WORKER_EXAMPLE_PATH") or "/tmp/vectis-audio-worker-example.wav"
local frames = {}
for i = 1, 160 do
  frames[i] = ((i - 1) % 32 - 16) / 32
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
  poll_timeout_ms = 10,
  max_frames = 1024,
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
requests:close()
os.remove(output_path)

print("lua audio worker service example ok")
