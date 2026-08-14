set(script "${WORK_DIR}/facade-contracts.lua")

file(WRITE "${script}" [[
local vectis = require("vectis")
local dsv = require("vectis.dsv")
local http = require("vectis.http")
local lonejson = require("lonejson")
local mqtt = require("vectis.mqtt")
local smtp = require("vectis.smtp")
local webdav = require("vectis.webdav")
local lockd = require("vectis.lockd")
local xml = require("vectis.xml")

local function assert_status_error(err, status, message_fragment)
  assert(type(err) == "table")
  assert(err.status == status)
  assert(err.status_string == vectis.status_string(status))
  assert(type(err.message) == "string")
  assert(type(err.source) == "string")
  assert(type(err.source_code) == "number")
  assert(err.source == vectis.error_source_string(err.source_code))
  if message_fragment ~= nil then
    assert(err.message:find(message_fragment, 1, true), err.message)
  end
end

assert(vectis.status_string(vectis.ERR_INVALID) == "invalid")
assert(vectis.status_string(vectis.ERR_STATE) == "state")
assert(vectis.status_string(vectis.ERR_TIMEOUT) == "timeout")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_NONE) == "none")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_VECTIS) == "vectis")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_LOCKDC) == "lockdc")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_LONEJSON) == "lonejson")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_LIBSSH2) == "libssh2")

local stopped, stopped_err = dsv.each({
  data = "id\nalpha\n",
  on_row = function()
    return false
  end,
})
assert(stopped == nil)
assert_status_error(stopped_err, vectis.ERR_STATE, "callback stopped")
assert(stopped_err.source == "vectis")
assert(stopped_err.source_code == vectis.ERROR_SOURCE_VECTIS)

local bad_each_ok, bad_each_err = pcall(function()
  dsv.each({ data = "id\nalpha\n" })
end)
assert(bad_each_ok == false)
assert(tostring(bad_each_err):find("on_row callback", 1, true))

local missing_bundle, missing_bundle_err = lockd.config({
  client_bundle = "embedded",
})
assert(missing_bundle == nil)
assert_status_error(missing_bundle_err, vectis.ERR_STATE,
                    "no embedded lockd bundle")
assert(missing_bundle_err.source == "vectis")

local xml_schema = lonejson.schema("contract", {
  lonejson.field("id", lonejson.string({required = true})),
})
local bad_xml, bad_xml_err = xml.parse({
  schema = xml_schema,
  xml = "<wrong id=\"x\"/>",
  root_element = "contract",
})
assert(bad_xml == nil)
assert_status_error(bad_xml_err, vectis.ERR_INVALID, "root element")
assert(bad_xml_err.source == "vectis")
assert(bad_xml_err.source_code == vectis.ERROR_SOURCE_VECTIS)

local invalid_http = http.normalize(nil)
assert(invalid_http.ok == false)
assert(invalid_http.transport_ok == false)
assert(invalid_http.error.kind == "invalid_result")
assert(invalid_http.error.message == "HTTP result is invalid")

local transport_http = http.normalize({
  ok = false,
  error = "connection refused",
  code = 7,
  code_name = "CURLE_COULDNT_CONNECT",
  attempts = 2,
})
assert(transport_http.ok == false)
assert(transport_http.transport_ok == false)
assert(transport_http.error.kind == "transport")
assert(transport_http.error.code == 7)
assert(transport_http.error.attempts == 2)
assert(transport_http.error_message == "connection refused")

local status_http = http.normalize({
  ok = true,
  status = 404,
  body = "missing",
  attempts = 1,
})
assert(status_http.ok == false)
assert(status_http.transport_ok == true)
assert(status_http.error.kind == "http_status")
assert(status_http.error.status == 404)
assert(status_http.error.body == "missing")

local mqtt_transport = mqtt.normalize({
  ok = false,
  error = "mqtt failed",
  attempts = 3,
})
assert(mqtt_transport.ok == false)
assert(mqtt_transport.transport_ok == false)
assert(mqtt_transport.error.kind == "transport")
assert(mqtt_transport.error.attempts == 3)

local smtp_transport = smtp.normalize({
  ok = false,
  error = "smtp failed",
  attempts = 4,
})
assert(smtp_transport.ok == false)
assert(smtp_transport.transport_ok == false)
assert(smtp_transport.error.kind == "transport")
assert(smtp_transport.error.attempts == 4)

assert(webdav.normalize({
  ok = true,
  status = 409,
  body = "conflict",
}).error.kind == "http_status")

local bad_webdav_ok, bad_webdav_err = pcall(function()
  webdav.copy({ url = "https://example.test/source" })
end)
assert(bad_webdav_ok == false)
assert(tostring(bad_webdav_err):find("destination", 1, true))

print("vectis-lua-facade-contracts-ok")
]])

execute_process(COMMAND "${VECTIS_BIN}" "${script}"
                RESULT_VARIABLE facade_contracts_result
                OUTPUT_VARIABLE facade_contracts_stdout
                ERROR_VARIABLE facade_contracts_stderr)
if(NOT facade_contracts_result EQUAL 0)
  message(FATAL_ERROR
    "vectis Lua facade contracts failed: ${facade_contracts_stdout}${facade_contracts_stderr}")
endif()
if(NOT facade_contracts_stdout MATCHES "vectis-lua-facade-contracts-ok")
  message(FATAL_ERROR
    "vectis Lua facade contracts did not report success: ${facade_contracts_stdout}")
endif()
