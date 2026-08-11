set(bundle_path "${WORK_DIR}/lua-cert-bundle.pem")
set(cert_path "${WORK_DIR}/lua-cert.pem")
set(key_path "${WORK_DIR}/lua-key.pem")
set(malformed_path "${WORK_DIR}/lua-malformed-cert.pem")
set(script "${WORK_DIR}/lua-certs-smoke.lua")

file(REMOVE "${bundle_path}" "${cert_path}" "${key_path}" "${malformed_path}")
file(WRITE "${malformed_path}" "not a certificate\n")

file(WRITE "${script}" [[
local vectis = require("vectis")

local bundle_path = assert(arg[1])
local cert_path = assert(arg[2])
local key_path = assert(arg[3])
local malformed_path = assert(arg[4])

assert(type(vectis.cert.generate_bundle) == "function")
assert(type(vectis.cert.validate_bundle) == "function")
assert(type(vectis.cert.validate_pair) == "function")

assert(vectis.cert.generate_bundle({
  common_name = "lua-cert.local",
  ip_addresses = "127.0.0.1",
  output_bundle_path = bundle_path,
  output_cert_path = cert_path,
  output_key_path = key_path,
  key_bits = 2048,
  valid_days = 1,
}) == true)

assert(vectis.cert.validate_bundle(bundle_path) == true)
assert(vectis.cert.validate_bundle({bundle_path = bundle_path}) == true)
assert(vectis.cert.validate_pair({
  certificate_path = cert_path,
  private_key_path = key_path,
  ca_bundle_path = bundle_path,
}) == true)

local malformed, malformed_error =
    vectis.cert.validate_bundle({path = malformed_path})
assert(malformed == nil)
assert(type(malformed_error) == "table")
assert(malformed_error.status == vectis.ERR_INVALID)
assert(malformed_error.status_string == vectis.status_string(vectis.ERR_INVALID))
assert(malformed_error.message:find("parse certificate", 1, true))
]])

execute_process(COMMAND "${VECTIS_BIN}" "${script}" "${bundle_path}"
                        "${cert_path}" "${key_path}" "${malformed_path}"
                RESULT_VARIABLE certs_result
                OUTPUT_VARIABLE certs_stdout
                ERROR_VARIABLE certs_stderr)
if(NOT certs_result EQUAL 0)
  message(FATAL_ERROR "vectis Lua cert smoke failed: ${certs_stdout}${certs_stderr}")
endif()
