set(bundle_path "${WORK_DIR}/lua-cert-bundle.pem")
set(cert_path "${WORK_DIR}/lua-cert.pem")
set(key_path "${WORK_DIR}/lua-key.pem")
set(csr_key_path "${WORK_DIR}/lua-csr-key.pem")
set(csr_path "${WORK_DIR}/lua-cert.csr")
set(malformed_path "${WORK_DIR}/lua-malformed-cert.pem")
set(auth_path "${WORK_DIR}/lua-cert-auth.json")
set(script "${WORK_DIR}/lua-certs-smoke.lua")

file(REMOVE "${bundle_path}" "${cert_path}" "${key_path}" "${csr_key_path}"
            "${csr_path}" "${malformed_path}" "${auth_path}"
            "${auth_path}.lock")
file(WRITE "${malformed_path}" "not a certificate\n")

file(WRITE "${script}" [[
local vectis = require("vectis")
local curl = require("curl")

local bundle_path = assert(arg[1])
local cert_path = assert(arg[2])
local key_path = assert(arg[3])
local csr_key_path = assert(arg[4])
local csr_path = assert(arg[5])
local malformed_path = assert(arg[6])
local auth_path = assert(arg[7])

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

assert(vectis.auth.store_init({credentials_path = auth_path}) == true)
assert(vectis.auth.user_add({
  credentials_path = auth_path,
  username = "cert-user",
  password = "cert-password",
}).username == "cert-user")
local webdav_key = assert(vectis.auth.webdav_key({
  credentials_path = auth_path,
  username = "cert-user",
  password = "cert-password",
}))
local basic_auth = "Basic " .. base64(webdav_key.client_id .. ":" .. webdav_key.client_secret)
local server = assert(vectis.server.new({
  bind = "127.0.0.1",
  port = 28383,
  tls = {
    mode = "manual",
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
    credentials_path = auth_path,
    realm = "certs",
    purpose = "webdav",
  },
  body = '{"ok":true,"tls":"split"}\n',
}) == true)
assert(server:start() == true)
local response
for _ = 1, 20 do
  response = curl.perform({
    url = "https://localhost:28383/probe",
    headers = {Authorization = basic_auth},
    protocols = "https",
    timeout_ms = 2000,
    connect_timeout_ms = 1000,
    verify_peer = false,
    verify_host = false,
    no_signal = true,
  })
  if response.ok then break end
  os.execute("sleep 0.1")
end
assert(response.ok == true, response.error)
assert(response.status == 200)
assert(response.body == '{"ok":true,"tls":"split"}\n')
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
                        "${csr_path}" "${malformed_path}" "${auth_path}"
                RESULT_VARIABLE certs_result
                OUTPUT_VARIABLE certs_stdout
                ERROR_VARIABLE certs_stderr)
if(NOT certs_result EQUAL 0)
  message(FATAL_ERROR "vectis Lua cert smoke failed: ${certs_stdout}${certs_stderr}")
endif()
