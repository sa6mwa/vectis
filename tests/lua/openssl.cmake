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
assert(type(openssl.sha256_materialized) == "function")
assert(type(openssl.sha256_materialized_hex) == "function")
assert(type(openssl.hmac_sha256_materialized) == "function")
assert(type(openssl.hmac_sha256_materialized_hex) == "function")
assert(type(openssl.digest_materialized) == "function")
assert(type(openssl.digest_materialized_hex) == "function")
assert(type(openssl.hmac_materialized) == "function")
assert(type(openssl.hmac_materialized_hex) == "function")
assert(type(openssl.hex_encode) == "function")
assert(type(openssl.hex_decode) == "function")
assert(type(openssl.base64_encode) == "function")
assert(type(openssl.base64_decode) == "function")
assert(type(openssl.sign) == "function")
assert(type(openssl.sign_hex) == "function")
assert(type(openssl.verify) == "function")
assert(type(openssl.random_bytes) == "function")
assert(type(openssl.random_hex) == "function")

local abc_digest = openssl.sha256_materialized("abc")
assert(#abc_digest == 32)
assert(openssl.sha256_materialized_hex("abc") ==
       "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
assert(openssl.hmac_sha256_materialized_hex("key", "The quick brown fox jumps over the lazy dog") ==
       "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8")
assert(#openssl.hmac_sha256_materialized("key", "data") == 32)
assert(openssl.digest_materialized_hex("sha256", "abc") ==
       "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
assert(openssl.digest_materialized_hex("SHA1", "abc") ==
       "a9993e364706816aba3e25717850c26c9cd0d89d")
assert(#openssl.digest_materialized("sha1", "abc") == 20)
assert(openssl.hmac_materialized_hex("sha256", "key", "The quick brown fox jumps over the lazy dog") ==
       "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8")
assert(openssl.hmac_materialized_hex("sha1", "key", "The quick brown fox jumps over the lazy dog") ==
       "de7c9b85b8b78aa6bc8a7a36f70a90701c9db4d9")
assert(#openssl.hmac_materialized("sha1", "key", "data") == 20)
assert(openssl.hex_encode("abc\0xyz") == "6162630078797a")
assert(openssl.hex_decode("6162630078797a") == "abc\0xyz")
assert(openssl.base64_encode("hello") == "aGVsbG8=")
assert(openssl.base64_decode("aGVsbG8=") == "hello")

local byte_limit = 1024 * 1024
local boundary = ("x"):rep(byte_limit)
assert(openssl.base64_decode(openssl.base64_encode(boundary)) == boundary)
local signature_hex = ("00"):rep(byte_limit)
for attempt = 1, 3 do
  local ok, err = pcall(openssl.base64_decode, ("A"):rep(1398104))
  assert(not ok and tostring(err):find("base64 output must be at most", 1, true))
  for _, algorithm in ipairs({"sha256", "unsupported-digest"}) do
    ok, err = pcall(openssl.verify, {
      data = "data", signature_hex = signature_hex,
      public_key_pem = "not a key", algorithm = algorithm,
    })
    assert(not ok)
    if algorithm == "sha256" then
      assert(tostring(err):find("failed to parse openssl public key", 1, true))
    else
      assert(tostring(err):find("unsupported openssl digest", 1, true))
    end
  end
end
collectgarbage("collect")

local bad_digest_ok, bad_digest_err = pcall(function()
  openssl.digest_materialized_hex("no-such-digest", "abc")
end)
assert(bad_digest_ok == false)
assert(tostring(bad_digest_err):find("unsupported", 1, true))

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
