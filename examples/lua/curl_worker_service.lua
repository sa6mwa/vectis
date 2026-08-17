local vectis = require("vectis")
local curl_worker = require("vectis.curl_worker")

local bind = os.getenv("VECTIS_LUA_CURL_WORKER_EXAMPLE_BIND") or "127.0.0.1"
local port = tonumber(os.getenv("VECTIS_LUA_CURL_WORKER_EXAMPLE_PORT") or "28589")
local base_url = "http://" .. bind .. ":" .. tostring(port)

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

local server = assert(vectis.server.new({
  app_name = "lua-curl-worker-example",
  bind = bind,
  port = port,
  supervision_policy = "supervised",
}))

assert(server:json({
  path = "/hello",
  body = '{"message":"hello from vectis"}\n',
  cache_control = "no-store",
}) == true)

assert(server:curl_worker_service({
  name = "lua-curl-worker",
  request_mailbox = requests,
  reply_broker = broker,
  poll_timeout_ms = 10,
  http = {
    timeout_ms = 2000,
    connect_timeout_ms = 1000,
    retry_max_attempts = 1,
    follow_redirects = true,
  },
}) == true)

local started, start_err = server:start()
assert(started == true, start_err and start_err.message or tostring(start_err))

local response
for _ = 1, 20 do
  local event = curl_worker.http_request({
    method = "GET",
    url = base_url .. "/hello",
    headers = {
      Accept = "application/json",
    },
    max_response_body_bytes = 4096,
  })
  local reply, request_err = broker:request(event, { timeout_ms = 3000 })
  if reply == nil then
    error(request_err and request_err.message or tostring(request_err))
  end
  local decoded, decode_err = curl_worker.decode_http_response(reply)
  if decoded == nil then
    error(decode_err and decode_err.message or tostring(decode_err))
  end
  response = decoded
  if response.ok and response.status == 200 then
    break
  end
  vectis.sleep_ms(50)
end

assert(response ~= nil, "curl worker response missing")
assert(response.ok == true, response.message or "curl worker transfer failed")
assert(response.status == 200)
assert(response.content_type == "application/json")
assert(response.body:find('"message":"hello from vectis"', 1, true))

local states, states_err = server:curl_worker_service_states()
if states == nil then
  error(states_err and states_err.message or tostring(states_err))
end
assert(#states == 1)
assert(states[1].name == "lua-curl-worker")
assert(states[1].started == true)

local stopped, stop_err = server:stop()
assert(stopped == true, stop_err and stop_err.message or tostring(stop_err))
server:close()
broker:close()
requests:close()

print("lua curl worker service example ok")
