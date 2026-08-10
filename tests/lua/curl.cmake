set(body_file "${WORK_DIR}/curl-body.txt")
set(json_file "${WORK_DIR}/curl-response.json")
set(script "${WORK_DIR}/curl-smoke.lua")

file(WRITE "${body_file}" "plain curl body\n")
file(WRITE "${json_file}" "{\"ok\":true,\"message\":\"streamed\"}\n")
file(WRITE "${script}" [[
local curl = require("curl")
local lonejson = require("lonejson")

local body_url = "file://" .. assert(arg[1])
local json_url = "file://" .. assert(arg[2])

assert(type(curl) == "table")
assert(type(curl.version()) == "string")
assert(curl.version():find("libcurl", 1, true))
assert(type(curl.perform) == "function")
assert(type(curl.json) == "function")
assert(type(curl.stream_json) == "function")

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

local smtp_ok, smtp_err = pcall(curl.perform, {
  url = "smtp://127.0.0.1:9",
  smtp = {mail_from = "from@example.test", rcpt = {"to@example.test"}},
})
assert(smtp_ok == false)
assert(tostring(smtp_err):find("curl body is required for SMTP upload", 1, true))
]])

execute_process(COMMAND "${VECTIS_BIN}" "${script}" "${body_file}" "${json_file}"
                RESULT_VARIABLE curl_result
                OUTPUT_VARIABLE curl_stdout
                ERROR_VARIABLE curl_stderr)
if(NOT curl_result EQUAL 0)
  message(FATAL_ERROR "vectis curl Lua smoke failed: ${curl_stdout}${curl_stderr}")
endif()
