set(script "${WORK_DIR}/vectis-openssl-smoke.lua")

file(WRITE "${script}" [[
local openssl = require("openssl")

assert(type(openssl.version) == "function")
assert(type(openssl.version()) == "string")
assert(openssl.version():lower():find("openssl", 1, true))
assert(type(openssl.sha256) == "function")
assert(type(openssl.sha256_hex) == "function")
assert(type(openssl.hmac_sha256) == "function")
assert(type(openssl.hmac_sha256_hex) == "function")
assert(type(openssl.random_bytes) == "function")
assert(type(openssl.random_hex) == "function")

local abc_digest = openssl.sha256("abc")
assert(#abc_digest == 32)
assert(openssl.sha256_hex("abc") ==
       "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
assert(openssl.hmac_sha256_hex("key", "The quick brown fox jumps over the lazy dog") ==
       "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8")
assert(#openssl.hmac_sha256("key", "data") == 32)

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

execute_process(COMMAND "${VECTIS_BIN}" "${script}"
                RESULT_VARIABLE openssl_result
                OUTPUT_VARIABLE openssl_stdout
                ERROR_VARIABLE openssl_stderr)
if(NOT openssl_result EQUAL 0)
  message(FATAL_ERROR "vectis OpenSSL Lua smoke failed: ${openssl_stdout}${openssl_stderr}")
endif()
