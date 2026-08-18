include("${VECTIS_SOURCE_DIR}/tests/lua/port_retry.cmake")

set(bundle_path "${WORK_DIR}/lua-http-redirect-bundle.pem")
set(script "${WORK_DIR}/lua-http-redirect.lua")

vectis_pick_test_port(https_port)
vectis_pick_test_port(http_port)
if(https_port STREQUAL http_port)
  message(FATAL_ERROR "HTTP redirect test requires distinct TLS and HTTP ports")
endif()

file(REMOVE "${bundle_path}")
file(WRITE "${script}" [[
local curl = require("curl")
local vectis = require("vectis")

local bundle_path = assert(arg[1])
local https_port = assert(tonumber(arg[2]))
local http_port = assert(tonumber(arg[3]))

assert(vectis.cert.generate_bundle({
  common_name = "localhost",
  dns_names = "localhost",
  ip_addresses = "127.0.0.1",
  output_bundle_path = bundle_path,
  key_bits = 2048,
  valid_days = 1,
}) == true)

local server = assert(vectis.server.new({
  bind = "127.0.0.1",
  port = https_port,
  tls = {
    mode = "manual",
    domain = "localhost",
    cert_key_bundle_path = bundle_path,
    http_redirect = true,
    http_redirect_port = http_port,
  },
}))
assert(server:json({
  path = "/keep",
  body = "{\"service\":\"redirect\"}",
}) == true)
assert(server:start() == true)

local response
for _ = 1, 30 do
  response = curl.perform({
    url = "http://localhost:" .. tostring(http_port) .. "/keep?source=lua",
    protocols = "http",
    follow_redirects = false,
    timeout_ms = 2000,
    connect_timeout_ms = 500,
    no_signal = true,
  })
  if response.ok then break end
  assert(vectis.sleep_ms(100) == true)
end
assert(response.ok == true, response.error)
assert(response.status == 308)
assert(response.headers:lower():find(
  "location: https://localhost/keep?source=lua", 1, true))

assert(server:stop() == true)
server:close()
assert(os.remove(bundle_path))
print("vectis-lua-http-redirect-ok")
]])

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "VECTIS_LUA_HTTPS_PORT=${https_port}"
          "VECTIS_LUA_HTTP_PORT=${http_port}"
          "${VECTIS_BIN}" "${script}" "${bundle_path}" "${https_port}" "${http_port}"
  RESULT_VARIABLE redirect_result
  OUTPUT_VARIABLE redirect_stdout
  ERROR_VARIABLE redirect_stderr)
if(NOT redirect_result EQUAL 0)
  message(FATAL_ERROR "Lua HTTP redirect test failed: ${redirect_stdout}${redirect_stderr}")
endif()
if(NOT redirect_stdout MATCHES "vectis-lua-http-redirect-ok")
  message(FATAL_ERROR "Lua HTTP redirect test missed success marker: ${redirect_stdout}${redirect_stderr}")
endif()
