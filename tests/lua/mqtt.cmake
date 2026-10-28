set(script "${WORK_DIR}/vectis-mqtt-smoke.lua")

file(WRITE "${script}" [[
local vectis = require("vectis")
local mqtt = require("vectis.mqtt")

assert(vectis.mqtt == mqtt)
assert(type(mqtt.publish) == "function")
assert(type(mqtt.normalize) == "function")
assert(type(mqtt.error_for) == "function")

local missing_url_ok, missing_url_err = pcall(function()
  mqtt.publish({message = "hello"})
end)
assert(missing_url_ok == false)
assert(tostring(missing_url_err):find("url", 1, true))

local missing_body_ok, missing_body_err = pcall(function()
  mqtt.publish({url = "mqtt://127.0.0.1:1/vectis"})
end)
assert(missing_body_ok == false)
assert(tostring(missing_body_err):find("body", 1, true))

for _, name in ipairs({"body_path", "upload_path"}) do
  for _, include_body in ipairs({false, true}) do
    local opts = {url = "mqtt://127.0.0.1:1/vectis", [name] = "/missing/file"}
    if include_body then opts.body = "hello" end
    local ok, err = pcall(mqtt.publish, opts)
    assert(not ok and tostring(err):find("does not support", 1, true))
  end
end
local invalid_ok, invalid_err = pcall(mqtt.publish, {
  url = "mqtt://127.0.0.1:1/vectis", body = false,
})
assert(not invalid_ok and tostring(invalid_err):find("string body", 1, true))
local curl = require("curl")
local perform = curl.perform
curl.perform = function(opts)
  assert(opts.method == "POST" and opts.upload == false)
  assert(opts.protocols == "mqtt" and opts.body == "hello")
  return {ok = true}
end
local opts = {url = "mqtt://127.0.0.1:1/vectis", message = "hello",
              method = "GET", upload = true}
assert(mqtt.publish(opts).ok)
assert(opts.method == "GET" and opts.upload and opts.body == nil)
curl.perform = perform

local refused = mqtt.publish({
  url = "mqtt://127.0.0.1:1/vectis",
  message = "hello mqtt\n",
  timeout_ms = 200,
  connect_timeout_ms = 50,
  no_signal = true,
  retry = false,
})
assert(refused.ok == false)
assert(refused.transport_ok == false)
assert(type(refused.error) == "table")
assert(refused.error.kind == "transport")
assert(type(refused.error.message) == "string")
assert(type(refused.error_message) == "string")

local normalized = mqtt.normalize({ok = true, status = 0})
assert(normalized.ok == true)
assert(normalized.transport_ok == true)
assert(normalized.error == nil)
]])

execute_process(COMMAND "${VECTIS_BIN}" "${script}"
                RESULT_VARIABLE mqtt_result
                OUTPUT_VARIABLE mqtt_stdout
                ERROR_VARIABLE mqtt_stderr)
if(NOT mqtt_result EQUAL 0)
  message(FATAL_ERROR "vectis MQTT Lua smoke failed: ${mqtt_stdout}${mqtt_stderr}")
endif()

execute_process(COMMAND "${MQTT_WIRE}" "${VECTIS_BIN}" "${SOURCE_DIR}/tests/lua/mqtt_wire.lua"
                RESULT_VARIABLE wire_result
                OUTPUT_VARIABLE wire_stdout ERROR_VARIABLE wire_stderr)
if(NOT wire_result EQUAL 0)
  message(FATAL_ERROR "MQTT wire regression failed: ${wire_stdout}${wire_stderr}")
endif()
