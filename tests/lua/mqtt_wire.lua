local mqtt = require("vectis.mqtt")
local url = "mqtt://127.0.0.1:" .. arg[1] .. "/vectis/wire"
local cases = {
  {body = "body"}, {payload = "payload"}, {message = "message"},
  {body = "binary\0tail"}, {body = ""},
  {body = ("x"):rep(16384), method = "GET", upload = true},
}
for index, opts in ipairs(cases) do
  opts.url = url
  opts.timeout_ms = 2000
  opts.connect_timeout_ms = 1000
  opts.retry = false
  opts.proxy = ""
  local result
  if index == 3 then
    opts.url = nil
    result = mqtt.publish(url, opts)
  else
    result = mqtt.publish(opts)
  end
  assert(result.ok, result.error and result.error.message)
  assert(result.transport_ok)
end
print("MQTT publications verified")
