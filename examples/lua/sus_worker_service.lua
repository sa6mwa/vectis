local vectis = require("vectis")
local sus_worker = require("vectis.sus_worker")

local frames = {}
for i = 1, 160 do
  frames[i] = 0.0
end

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
  app_name = "lua-sus-worker-example",
  service_failure_policy = "fail_closed",
}))

local callback_service, callback_err = server:sus_worker_service({
  request_mailbox = requests,
  callback = function()
    error("SUS worker services must not call Lua callbacks")
  end,
})
assert(callback_service == nil)
assert(callback_err.message:find("does not accept Lua callbacks", 1, true))

local service_config = {
  name = "lua-sus-worker",
  request_mailbox = requests,
  reply_broker = broker,
  poll_timeout_ms = 10,
  max_frames = 1024,
  max_text_bytes = 8192,
}

local model_path = os.getenv("VECTIS_LUA_SUS_WORKER_MODEL_PATH")
if model_path ~= nil and model_path ~= "" then
  service_config.model_path = model_path
end

local cached_model = os.getenv("VECTIS_LUA_SUS_WORKER_CACHED_MODEL")
if cached_model ~= nil and cached_model ~= "" then
  service_config.cached_model = cached_model
  service_config.cache_dir = os.getenv("VECTIS_LUA_SUS_WORKER_CACHE_DIR")
  service_config.sha256 = os.getenv("VECTIS_LUA_SUS_WORKER_SHA256")
  service_config.source_url = os.getenv("VECTIS_LUA_SUS_WORKER_SOURCE_URL")
  service_config.offline = os.getenv("VECTIS_LUA_SUS_WORKER_OFFLINE") == "1"
  service_config.insecure_no_checksum =
      os.getenv("VECTIS_LUA_SUS_WORKER_INSECURE_NO_CHECKSUM") == "1"
end

local event = assert(sus_worker.transcribe_pcm_request({
  frames = frames,
  language = "en",
  max_text_bytes = 8192,
}))
assert(event.kind == sus_worker.TRANSCRIBE_PCM_KIND)
assert(event.expects_reply == true)

if service_config.model_path == nil and service_config.cached_model == nil then
  local started, start_err = server:start()
  assert(started == true, start_err and start_err.message or tostring(start_err))

  local states, states_err = server:sus_worker_service_states()
  if states == nil then
    error(states_err and states_err.message or tostring(states_err))
  end
  assert(#states == 0)

  local stopped, stop_err = server:stop()
  assert(stopped == true, stop_err and stop_err.message or tostring(stop_err))
  server:close()
  broker:close()
  requests:close()

  print("lua SUS worker service example ok")
  return
end

assert(server:sus_worker_service(service_config) == true)

local started, start_err = server:start()
assert(started == true, start_err and start_err.message or tostring(start_err))

local reply, request_err = broker:request(event, { timeout_ms = 3000 })
if reply == nil then
  error(request_err and request_err.message or tostring(request_err))
end

local decoded, decode_err = sus_worker.decode_reply(reply)
if decoded == nil then
  error(decode_err and decode_err.message or tostring(decode_err))
end

assert(decoded.ok == true, decoded.message or "SUS transcription failed")
assert(decoded.operation == "transcribe_pcm")
assert(type(decoded.text) == "string")

local states, states_err = server:sus_worker_service_states()
if states == nil then
  error(states_err and states_err.message or tostring(states_err))
end
assert(#states == 1)
assert(states[1].name == "lua-sus-worker")
assert(states[1].started == true)

local stopped, stop_err = server:stop()
assert(stopped == true, stop_err and stop_err.message or tostring(stop_err))
server:close()
broker:close()
requests:close()

print("lua SUS worker service example ok")
