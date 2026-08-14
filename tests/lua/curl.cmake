set(body_file "${WORK_DIR}/curl-body.txt")
set(json_file "${WORK_DIR}/curl-response.json")
set(script "${WORK_DIR}/curl-smoke.lua")

file(WRITE "${body_file}" "plain curl body\n")
file(WRITE "${json_file}" "{\"ok\":true,\"message\":\"streamed\"}\n")
file(WRITE "${script}" [[
local curl = require("curl")
local lonejson = require("lonejson")
local smtp = require("vectis.smtp")

local body_url = "file://" .. assert(arg[1])
local json_url = "file://" .. assert(arg[2])

assert(type(curl) == "table")
assert(type(curl.version()) == "string")
assert(curl.version():find("libcurl", 1, true))
assert(type(curl.perform) == "function")
assert(type(curl.json) == "function")
assert(type(curl.stream_json) == "function")
assert(type(smtp.send) == "function")

local plain = curl.perform({
  url = body_url,
  protocols = "file",
  timeout_ms = 1000,
  no_signal = true,
})
assert(plain.ok == true, plain.error)
assert(plain.body == "plain curl body\n")

local decoded = curl.json({
  url = json_url,
  protocols = "file",
})
assert(decoded.ok == true, decoded.error)
assert(decoded.json.ok == true)
assert(decoded.response_json.message == "streamed")

local response_schema = lonejson.schema("curl-response", {
  lonejson.field("ok", lonejson.boolean()),
  lonejson.field("message", lonejson.string()),
})
local streamed = curl.stream_json({
  url = json_url,
  protocols = "file",
  response = {schema = response_schema},
})
assert(streamed.ok == true, streamed.error)
assert(streamed.json.ok == true)
assert(streamed.response_json.message == "streamed")

local retry_transport = curl.perform({
  url = "http://127.0.0.1:1/",
  protocols = "http",
  timeout_ms = 200,
  connect_timeout_ms = 50,
  no_signal = true,
  retry = {
    max_attempts = 2,
    initial_delay_ms = 0,
    max_delay_ms = 0,
    conditions = {"transport"},
  },
})
assert(retry_transport.ok == false)
assert(retry_transport.attempts == 2)
assert(type(retry_transport.error) == "string")

local no_retry_transport = curl.perform({
  url = "http://127.0.0.1:1/",
  protocols = "http",
  timeout_ms = 200,
  connect_timeout_ms = 50,
  no_signal = true,
  retry = false,
})
assert(no_retry_transport.ok == false)
assert(no_retry_transport.attempts == 1)

local stream_retry_ok, stream_retry_err = pcall(curl.stream_json, {
  url = json_url,
  protocols = "file",
  response = {schema = response_schema},
  retry = {max_attempts = 2},
})
assert(stream_retry_ok == false)
assert(tostring(stream_retry_err):find(
    "curl streaming responses cannot be retried safely", 1, true))

local smtp_ok, smtp_err = pcall(curl.perform, {
  url = "smtp://127.0.0.1:9",
  smtp = {mail_from = "from@example.test", rcpt = {"to@example.test"}},
})
assert(smtp_ok == false)
assert(tostring(smtp_err):find("body, body_path, or upload_path", 1, true))

local smtp_missing_body_ok, smtp_missing_body_err = pcall(smtp.send, {
  url = "smtp://127.0.0.1:9",
  mail_from = "from@example.test",
  rcpt = {"to@example.test"},
})
assert(smtp_missing_body_ok == false)
assert(tostring(smtp_missing_body_err):find("body, message, body_path, or upload_path", 1, true))

local smtp_file = smtp.send({
  url = "smtp://127.0.0.1:9",
  mail_from = "from@example.test",
  to = "to@example.test",
  body_path = assert(arg[1]),
  timeout_ms = 200,
  connect_timeout_ms = 50,
  retry = false,
})
assert(smtp_file.ok == false)
assert(smtp_file.transport_ok == false)
assert(type(smtp_file.error) == "table")
assert(type(smtp_file.error.message) == "string")
]])

execute_process(COMMAND "${VECTIS_BIN}" "${script}" "${body_file}" "${json_file}"
                RESULT_VARIABLE curl_result
                OUTPUT_VARIABLE curl_stdout
                ERROR_VARIABLE curl_stderr)
if(NOT curl_result EQUAL 0)
  message(FATAL_ERROR "vectis curl Lua smoke failed: ${curl_stdout}${curl_stderr}")
endif()
