set(script "${WORK_DIR}/vectis-openssl-smoke.lua")
set(private_key "${WORK_DIR}/vectis-openssl-signing-key.pem")
set(certificate "${WORK_DIR}/vectis-openssl-signing-cert.pem")

file(WRITE "${script}" [[
local openssl = require("openssl")
local vectis = require("vectis")

local function read_file(path)
  local fp = assert(io.open(path, "rb"))
  local body = fp:read("*a")
  fp:close()
  return body
end

assert(type(openssl.version) == "function")
assert(type(openssl.version()) == "string")
assert(openssl.version():lower():find("openssl", 1, true))
assert(type(openssl.sha256) == "function")
assert(type(openssl.sha256_hex) == "function")
assert(type(openssl.hmac_sha256) == "function")
assert(type(openssl.hmac_sha256_hex) == "function")
assert(type(openssl.digest) == "function")
assert(type(openssl.digest_hex) == "function")
assert(type(openssl.hmac) == "function")
assert(type(openssl.hmac_hex) == "function")
assert(type(openssl.hex_encode) == "function")
assert(type(openssl.hex_decode) == "function")
assert(type(openssl.base64_encode) == "function")
assert(type(openssl.base64_decode) == "function")
assert(type(openssl.sign) == "function")
assert(type(openssl.sign_hex) == "function")
assert(type(openssl.verify) == "function")
assert(type(openssl.random_bytes) == "function")
assert(type(openssl.random_hex) == "function")

local abc_digest = openssl.sha256("abc")
assert(#abc_digest == 32)
assert(openssl.sha256_hex("abc") ==
       "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
assert(openssl.hmac_sha256_hex("key", "The quick brown fox jumps over the lazy dog") ==
       "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8")
assert(#openssl.hmac_sha256("key", "data") == 32)
assert(openssl.digest_hex("sha256", "abc") ==
       "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
assert(openssl.digest_hex("SHA1", "abc") ==
       "a9993e364706816aba3e25717850c26c9cd0d89d")
assert(#openssl.digest("sha1", "abc") == 20)
assert(openssl.hmac_hex("sha256", "key", "The quick brown fox jumps over the lazy dog") ==
       "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8")
assert(openssl.hmac_hex("sha1", "key", "The quick brown fox jumps over the lazy dog") ==
       "de7c9b85b8b78aa6bc8a7a36f70a90701c9db4d9")
assert(#openssl.hmac("sha1", "key", "data") == 20)
assert(openssl.hex_encode("abc\0xyz") == "6162630078797a")
assert(openssl.hex_decode("6162630078797a") == "abc\0xyz")
assert(openssl.base64_encode("hello") == "aGVsbG8=")
assert(openssl.base64_decode("aGVsbG8=") == "hello")

local bad_digest_ok, bad_digest_err = pcall(function()
  openssl.digest_hex("no-such-digest", "abc")
end)
assert(bad_digest_ok == false)
assert(tostring(bad_digest_err):find("unsupported openssl digest", 1, true))

local private_key_path = assert(arg[1])
local certificate_path = assert(arg[2])
assert(vectis.cert.generate_bundle({
  common_name = "lua-openssl-signing.local",
  output_cert_path = certificate_path,
  output_key_path = private_key_path,
  key_bits = 2048,
  valid_days = 1,
}) == true)
local payload = "signed payload"
local signature = assert(openssl.sign({
  private_key_path = private_key_path,
  data = payload,
}))
assert(#signature > 64)
assert(openssl.verify({
  certificate_path = certificate_path,
  data = payload,
  signature = signature,
}) == true)
assert(openssl.verify({
  certificate_path = certificate_path,
  data = payload .. "!",
  signature = signature,
}) == false)
local signature_hex = assert(openssl.sign_hex({
  private_key_pem = read_file(private_key_path),
  data = payload,
  algorithm = "sha256",
}))
assert(signature_hex:match("^[0-9a-f]+$"))
assert(openssl.verify({
  certificate_pem = read_file(certificate_path),
  data = payload,
  signature_hex = signature_hex,
  digest = "sha256",
}) == true)

local empty = openssl.random_bytes(0)
assert(empty == "")
local random_a = openssl.random_bytes(32)
local random_b = openssl.random_bytes(32)
assert(#random_a == 32)
assert(#random_b == 32)
assert(random_a ~= random_b)
local random_hex = openssl.random_hex(16)
assert(#random_hex == 32)
assert(random_hex:match("^[0-9a-f]+$"))

local bad_size_ok, bad_size_err = pcall(function()
  openssl.random_bytes(-1)
end)
assert(bad_size_ok == false)
assert(tostring(bad_size_err):find("between 0", 1, true))
]])

execute_process(COMMAND "${VECTIS_BIN}" "${script}" "${private_key}" "${certificate}"
                RESULT_VARIABLE openssl_result
                OUTPUT_VARIABLE openssl_stdout
                ERROR_VARIABLE openssl_stderr)
if(NOT openssl_result EQUAL 0)
  message(FATAL_ERROR "vectis OpenSSL Lua smoke failed: ${openssl_stdout}${openssl_stderr}")
endif()
