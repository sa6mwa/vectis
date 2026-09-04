set(bundle_path "${WORK_DIR}/lua-cert-bundle.pem")
set(cert_path "${WORK_DIR}/lua-cert.pem")
set(key_path "${WORK_DIR}/lua-key.pem")
set(csr_key_path "${WORK_DIR}/lua-csr-key.pem")
set(csr_path "${WORK_DIR}/lua-cert.csr")
set(malformed_path "${WORK_DIR}/lua-malformed-cert.pem")
set(pouch_root "${WORK_DIR}/lua-cert-pouch")
set(lockd_endpoint "pouch://${pouch_root}?single_writer=false")
set(script "${WORK_DIR}/lua-certs-smoke.lua")

if(NOT DEFINED VECTIS_PORT_HELPER)
  message(FATAL_ERROR "VECTIS_PORT_HELPER is required")
endif()
execute_process(COMMAND "${VECTIS_PORT_HELPER}"
                RESULT_VARIABLE port_result
                OUTPUT_VARIABLE picked_port
                ERROR_VARIABLE port_stderr
                OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT port_result EQUAL 0)
  message(FATAL_ERROR "test port allocation failed: ${port_stderr}")
endif()
if(NOT picked_port MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "test port helper returned invalid port: ${picked_port}")
endif()

file(REMOVE "${bundle_path}" "${cert_path}" "${key_path}" "${csr_key_path}"
            "${csr_path}" "${malformed_path}")
file(REMOVE_RECURSE "${pouch_root}")
file(WRITE "${malformed_path}" "not a certificate\n")

execute_process(
  COMMAND "${VECTIS_BIN}" -a users --lockd-endpoint "${lockd_endpoint}"
          --add "cert-user" --password "cert-password"
  RESULT_VARIABLE user_add_result
  OUTPUT_VARIABLE user_add_output
  ERROR_VARIABLE user_add_error)
if(NOT user_add_result EQUAL 0)
  message(FATAL_ERROR "certs user provisioning failed: ${user_add_error}")
endif()
execute_process(
  COMMAND "${VECTIS_BIN}" -a users --lockd-endpoint "${lockd_endpoint}"
          --webdav-key "cert-user" --password "cert-password"
  RESULT_VARIABLE webdav_key_result
  OUTPUT_VARIABLE webdav_key_output
  ERROR_VARIABLE webdav_key_error)
if(NOT webdav_key_result EQUAL 0)
  message(FATAL_ERROR "certs WebDAV key provisioning failed: ${webdav_key_error}")
endif()
string(REGEX MATCH "client_id=([^\n]+)" webdav_client_id_match
       "${webdav_key_output}")
if(NOT webdav_client_id_match)
  message(FATAL_ERROR "certs WebDAV key did not include client_id")
endif()
set(webdav_client_id "${CMAKE_MATCH_1}")
string(REGEX MATCH "client_secret=([^\n]+)" webdav_client_secret_match
       "${webdav_key_output}")
if(NOT webdav_client_secret_match)
  message(FATAL_ERROR "certs WebDAV key did not include client_secret")
endif()
set(webdav_client_secret "${CMAKE_MATCH_1}")

file(WRITE "${script}" [[
local vectis = require("vectis")
local curl = require("curl")

local bundle_path = assert(arg[1])
local cert_path = assert(arg[2])
local key_path = assert(arg[3])
local csr_key_path = assert(arg[4])
local csr_path = assert(arg[5])
local malformed_path = assert(arg[6])
local lockd_endpoint = assert(arg[7])
local client_id = assert(arg[8])
local client_secret = assert(arg[9])
local port = assert(tonumber(arg[10]))

local b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local function base64(data)
  local out = {}
  for i = 1, #data, 3 do
    local a = data:byte(i) or 0
    local b = data:byte(i + 1) or 0
    local c = data:byte(i + 2) or 0
    local n = a * 65536 + b * 256 + c
    local pad = (#data - i == 0) and 2 or ((#data - i == 1) and 1 or 0)
    out[#out + 1] = b64chars:sub(math.floor(n / 262144) % 64 + 1, math.floor(n / 262144) % 64 + 1)
    out[#out + 1] = b64chars:sub(math.floor(n / 4096) % 64 + 1, math.floor(n / 4096) % 64 + 1)
    out[#out + 1] = pad >= 2 and "=" or b64chars:sub(math.floor(n / 64) % 64 + 1, math.floor(n / 64) % 64 + 1)
    out[#out + 1] = pad >= 1 and "=" or b64chars:sub(n % 64 + 1, n % 64 + 1)
  end
  return table.concat(out)
end

assert(type(vectis.cert.generate_bundle) == "function")
assert(type(vectis.cert.generate_private_key) == "function")
assert(type(vectis.cert.generate_csr) == "function")
assert(type(vectis.cert.inspect_bundle) == "function")
assert(type(vectis.cert.validate_bundle) == "function")
assert(type(vectis.cert.validate_pair) == "function")

local function read_file(path)
  local fp = assert(io.open(path, "rb"))
  local body = fp:read("*a")
  fp:close()
  return body
end

assert(vectis.cert.generate_bundle({
  common_name = "localhost",
  dns_names = "localhost",
  ip_addresses = "127.0.0.1",
  output_bundle_path = bundle_path,
  output_cert_path = cert_path,
  output_key_path = key_path,
  key_bits = 2048,
  valid_days = 1,
}) == true)

assert(vectis.cert.validate_bundle(bundle_path) == true)
assert(vectis.cert.validate_bundle({bundle_path = bundle_path}) == true)
local inspected = assert(vectis.cert.inspect_bundle(bundle_path))
assert(inspected.path == bundle_path)
assert(inspected.version == 3)
assert(type(inspected.serial_hex) == "string")
assert(#inspected.serial_hex > 0)
assert(type(inspected.not_before) == "string")
assert(type(inspected.not_after) == "string")
assert(inspected.is_ca == false)
assert(inspected.public_key_type == "rsa")
assert(inspected.public_key_bits == 2048)
assert(inspected.subject.common_name == "localhost")
assert(inspected.issuer.common_name == "localhost")
assert(inspected.subject_alt_names.dns_names[1] == "localhost")
assert(inspected.subject_alt_names.ip_addresses[1] == "127.0.0.1")
local inspected_table = assert(vectis.cert.inspect_bundle({bundle_path = bundle_path}))
assert(inspected_table.subject.common_name == "localhost")
assert(vectis.cert.validate_pair({
  certificate_path = cert_path,
  private_key_path = key_path,
  ca_bundle_path = bundle_path,
}) == true)

assert(vectis.cert.generate_private_key({
  output_key_path = csr_key_path,
  key_bits = 2048,
}) == true)
assert(read_file(csr_key_path):find("BEGIN PRIVATE KEY", 1, true))
assert(vectis.cert.generate_csr({
  subject = {
    common_name = "csr.localhost",
    organization = "Vectis",
    country = "SE",
  },
  dns_names = "csr.localhost",
  ip_addresses = "127.0.0.1",
  private_key_path = csr_key_path,
  output_csr_path = csr_path,
}) == true)
local csr_body = read_file(csr_path)
assert(csr_body:find("BEGIN CERTIFICATE REQUEST", 1, true))
assert(csr_body:find("END CERTIFICATE REQUEST", 1, true))

local basic_auth = "Basic " .. base64(client_id .. ":" .. client_secret)
local server = assert(vectis.app.new({
  bind = "127.0.0.1",
  port = port,
  lockd = { endpoints = {lockd_endpoint} },
  hsts_max_age_seconds = 31536000,
  tls = {
    mode = "manual",
    version = "1.2",
    cipher_list = "ECDHE-RSA-AES128-GCM-SHA256",
    domain = "localhost",
    cert_path = cert_path,
    key_path = key_path,
    ca_path = bundle_path,
  },
}))
assert(server:auth_json({
  path = "/probe",
  auth = {
    kind = "native",
    state_key = "auth/v1/store",
    realm = "certs",
    purpose = "webdav",
  },
  body = '{"ok":true,"tls":"split"}\n',
}) == true)
assert(server:start() == true)
local response
for _ = 1, 20 do
  response = curl.perform({
    url = "https://localhost:" .. tostring(port) .. "/probe",
    headers = {Authorization = basic_auth},
    protocols = "https",
    timeout_ms = 2000,
    connect_timeout_ms = 1000,
    verify_peer = false,
    verify_host = false,
    no_signal = true,
  })
  if response.ok then break end
  assert(vectis.sleep_ms(100) == true)
end
assert(response.ok == true, response.error)
assert(response.status == 200)
assert(response.body == '{"ok":true,"tls":"split"}\n')
assert(response.headers:lower():find(
  "strict-transport-security: max-age=31536000; includesubdomains", 1, true))
assert(server:stop() == true)
server:close()

local malformed, malformed_error =
    vectis.cert.validate_bundle({path = malformed_path})
assert(malformed == nil)
assert(type(malformed_error) == "table")
assert(malformed_error.status == vectis.ERR_INVALID)
assert(malformed_error.status_string == vectis.status_string(vectis.ERR_INVALID))
assert(malformed_error.message:find("parse certificate", 1, true))
local malformed_inspect, malformed_inspect_error =
    vectis.cert.inspect_bundle({path = malformed_path})
assert(malformed_inspect == nil)
assert(type(malformed_inspect_error) == "table")
assert(malformed_inspect_error.status == vectis.ERR_INVALID)
assert(malformed_inspect_error.message:find("parse certificate", 1, true))
]])

execute_process(COMMAND "${VECTIS_BIN}" "${script}" "${bundle_path}"
                        "${cert_path}" "${key_path}" "${csr_key_path}"
                        "${csr_path}" "${malformed_path}" "${lockd_endpoint}"
                        "${webdav_client_id}" "${webdav_client_secret}"
                        "${picked_port}"
                RESULT_VARIABLE certs_result
                OUTPUT_VARIABLE certs_stdout
                ERROR_VARIABLE certs_stderr)
if(NOT certs_result EQUAL 0)
  message(FATAL_ERROR "vectis Lua cert smoke failed: ${certs_stdout}${certs_stderr}")
endif()
