local vectis = require("vectis")
local cai_worker = require("vectis.cai_worker")

local bind = os.getenv("VECTIS_LUA_CAI_WORKER_EXAMPLE_BIND") or "127.0.0.1"
local port_env = os.getenv("VECTIS_LUA_CAI_WORKER_EXAMPLE_PORT")
local port = tonumber(port_env or "0")
local supervised = port_env ~= nil and port > 0

local requests = vectis.mailbox.new({
  capacity = 8,
  max_payload_bytes = 65536,
})
local broker = vectis.mailbox.broker({
  requests = requests,
  max_pending = 4,
  reply_mailbox = {
    capacity = 4,
    max_payload_bytes = 65536,
  },
})

local server_config = {
  app_name = "lua-cai-worker-example",
  service_failure_policy = "fail_closed",
}
if supervised then
  server_config.bind = bind
  server_config.port = port
  server_config.supervision_policy = "supervised"
end
local app = assert(vectis.app.new(server_config))

if supervised then
  assert(app:json({
    path = "/ready",
    body = '{"ok":true}\n',
    cache_control = "no-store",
  }) == true)
end

local callback_service, callback_err = app:cai_worker_service({
  request_mailbox = requests,
  callback = function()
    error("CAI worker services must not call Lua callbacks")
  end,
})
assert(callback_service == nil)
assert(callback_err.message:find("does not accept Lua callbacks", 1, true))

assert(app:cai_worker_service({
  name = "lua-cai-worker",
  request_mailbox = requests,
  reply_broker = broker,
  poll_timeout_ms = 10,
  client = {
    api_key = "unused-local-example-key",
    timeout_ms = 1000,
    logger_disabled = true,
  },
}) == true)

local started, start_err = app:start()
assert(started == true, start_err and start_err.message or tostring(start_err))

local event = assert(cai_worker.request({
  provider = "invalid-provider",
  model = "gpt-test",
  text = "hello from vectis",
  developer_instructions = "be brief",
  max_output_tokens = 4,
}))
assert(event.kind == cai_worker.REQUEST_KIND)
assert(event.expects_reply == true)

local reply, request_err = broker:request(event, { timeout_ms = 3000 })
if reply == nil then
  error(request_err and request_err.message or tostring(request_err))
end

local decoded, decode_err = cai_worker.decode_reply(reply)
if decoded == nil then
  error(decode_err and decode_err.message or tostring(decode_err))
end
assert(decoded.ok == false)
assert(decoded.status == vectis.ERR_INVALID)
assert(decoded.status_string == "invalid")
assert(decoded.source == "vectis")
assert(decoded.source_code == vectis.ERROR_SOURCE_VECTIS)
assert(decoded.message:find("provider", 1, true))

local states, states_err = app:cai_worker_service_states()
if states == nil then
  error(states_err and states_err.message or tostring(states_err))
end
assert(#states == 1)
assert(states[1].name == "lua-cai-worker")
assert(states[1].started == true)

local stopped, stop_err = app:stop()
assert(stopped == true, stop_err and stop_err.message or tostring(stop_err))
app:close()
broker:close()
requests:close()

print("lua cai worker service example ok")
