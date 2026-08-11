set(json_file "${WORK_DIR}/vectis-http-response.json")
set(download_source "${WORK_DIR}/vectis-http-download-source.txt")
set(download_target "${WORK_DIR}/vectis-http-download-target.txt")
set(upload_source "${WORK_DIR}/vectis-http-upload-source.txt")
set(upload_target "${WORK_DIR}/vectis-http-upload-target.txt")
set(script "${WORK_DIR}/vectis-http-smoke.lua")

file(WRITE "${json_file}" "{\"ok\":true,\"message\":\"vectis-http\"}\n")
file(WRITE "${download_source}" "downloaded through curl file sink\n")
file(WRITE "${upload_source}" "uploaded through curl file source\n")
file(REMOVE "${download_target}" "${upload_target}")

file(WRITE "${script}" [[
local vectis = require("vectis")
local lonejson = require("lonejson")

local json_url = "file://" .. assert(arg[1])
local download_url = "file://" .. assert(arg[2])
local download_path = assert(arg[3])
local upload_path = assert(arg[4])
local upload_url = "file://" .. assert(arg[5])

assert(type(vectis.http) == "table")
assert(type(vectis.http.request) == "function")
assert(type(vectis.http.request_json) == "function")
assert(type(vectis.http.get_json) == "function")
assert(type(vectis.http.post_json) == "function")
assert(type(vectis.http.download) == "function")
assert(type(vectis.http.upload) == "function")
assert(type(vectis.http.sftp_download) == "function")
assert(type(vectis.http.sftp_upload) == "function")
assert(require("vectis.http") == vectis.http)

local decoded = vectis.http.get_json({
  url = json_url,
  protocols = "file",
})
assert(decoded.ok == true, decoded.error and decoded.error.message)
assert(decoded.transport_ok == true)
assert(decoded.json.message == "vectis-http")
assert(decoded.error == nil)

local failed = vectis.http.request({
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
assert(failed.ok == false)
assert(failed.transport_ok == false)
assert(failed.attempts == 2)
assert(failed.error.kind == "transport")
assert(failed.error.attempts == 2)
assert(type(failed.error.message) == "string")
assert(type(failed.error_message) == "string")

local downloaded = vectis.http.download({
  url = download_url,
  protocols = "file",
  download_path = download_path,
})
assert(downloaded.ok == true, downloaded.error and downloaded.error.message)
assert(downloaded.body == "")
do
  local fp = assert(io.open(download_path, "rb"))
  local body = fp:read("*a")
  fp:close()
  assert(body == "downloaded through curl file sink\n")
end

local uploaded = vectis.http.upload({
  url = upload_url,
  protocols = "file",
  upload_path = upload_path,
})
assert(uploaded.ok == true, uploaded.error and uploaded.error.message)
do
  local fp = assert(io.open(arg[5], "rb"))
  local body = fp:read("*a")
  fp:close()
  assert(body == "uploaded through curl file source\n")
end

local response_schema = lonejson.schema("vectis-http-response", {
  lonejson.field("ok", lonejson.boolean()),
  lonejson.field("message", lonejson.string()),
})
local streamed = vectis.http.stream_json({
  url = json_url,
  protocols = "file",
  response = {schema = response_schema},
})
assert(streamed.ok == true, streamed.error and streamed.error.message)
assert(streamed.json.ok == true)
assert(streamed.response_json.message == "vectis-http")
]])

execute_process(COMMAND "${VECTIS_BIN}" "${script}" "${json_file}"
                        "${download_source}" "${download_target}"
                        "${upload_source}" "${upload_target}"
                RESULT_VARIABLE http_result
                OUTPUT_VARIABLE http_stdout
                ERROR_VARIABLE http_stderr)
if(NOT http_result EQUAL 0)
  message(FATAL_ERROR "vectis HTTP Lua smoke failed: ${http_stdout}${http_stderr}")
endif()
