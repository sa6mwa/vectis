local ssl = require("openssl")
local digest = require("vectis.digest")
assert(require("vectis").digest == digest)
assert(digest.new == ssl.digest_stream_new)
local expected = "e9a099c75ef837c28bc91683bee127e463fa0ee10c11fd816f8d2d428c0d610e"
assert(ssl.DIGEST_MAX_BYTES == 64 and ssl.DIGEST_CHUNK_BYTES == 65536)
do
  local context <close> = ssl.digest_stream_new("sha256")
  assert(context:update("a"):update("bc") == context)
  local digest, count = context:final_hex()
  assert(digest == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
  assert(count.high == 0 and count.low == 3)
  assert(context:final_hex() == digest)
  assert(not pcall(context.update, context, "x"))
  context:close()
  context:close()
  assert(not pcall(context.final, context))
end
assert(ssl.sha256 == nil and ssl.digest == nil and ssl.sha256_hex == nil)
assert(ssl.sha256_materialized_hex(string.rep("\0", 2097153)) == expected)
local function zero_reader()
  local remaining = 2097153
  return function(cap)
    assert(cap == 65536)
    if remaining == 0 then return nil end
    local count = math.min(remaining, cap)
    remaining = remaining - count
    return string.rep("\0", count)
  end
end
assert(ssl.sha256_stream_hex(zero_reader()) == expected)
assert(ssl.digest_stream_hex("sha256", zero_reader()) == expected)
assert(digest.stream_hex("sha256", zero_reader()) == expected)
assert(#ssl.sha256_stream(zero_reader()) == 32)
assert(ssl.sha256_stream_hex(function() return nil end) ==
  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
local offset = 0
assert(ssl.sha256_stream_hex(function()
  offset = offset + 1
  if offset > 3 then return nil end
  collectgarbage("collect")
  return ("abc"):sub(offset, offset)
end) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
for _, value in ipairs({"", true, 3, string.rep("x", 65537)}) do
  assert(not pcall(ssl.sha256_stream, function() return value end))
end
local failure = {message = "reader failure"}
local ok, err = pcall(ssl.sha256_stream, function() error(failure, 0) end)
assert(not ok and err == failure)
ok, err = pcall(ssl.sha256_stream, function() return nil, failure end)
assert(not ok and err == failure)
assert(not pcall(ssl.digest_stream, "invalid-algorithm", zero_reader()))
assert(not pcall(ssl.sha256_file_hex, "missing-digest-input"))
assert(not pcall(ssl.sha256_file_hex, "filename\0suffix"))
local root = assert(os.getenv("VECTIS_UPGRADE_TEST_ROOT"))
local path = root .. "/digest-input.bin"
local f = assert(io.open(path, "wb"))
local read = zero_reader()
while true do
  local chunk = read(65536)
  if not chunk then break end
  assert(f:write(chunk))
end
assert(f:close())
assert(ssl.sha256_file_hex(path) == expected)
assert(ssl.digest_file_hex("sha256", path) == expected)
assert(#ssl.sha256_file(path) == 32)
local hmac_expected = ssl.hmac_materialized_hex("sha256", "k\0ey", string.rep("\0", 2097153))
assert(ssl.hmac_stream_hex("sha256", "k\0ey", zero_reader()) == hmac_expected)
assert(ssl.hmac_sha256_stream_hex("k\0ey", zero_reader()) == hmac_expected)
assert(ssl.hmac_file_hex("sha256", "k\0ey", path) == hmac_expected)
assert(ssl.hmac_sha256_file_hex("k\0ey", path) == hmac_expected)
assert(#ssl.hmac_stream("sha256", "key", zero_reader()) == 32)
assert(#ssl.hmac_sha256_stream("key", zero_reader()) == 32)
assert(#ssl.hmac_file("sha256", "key", path) == 32)
assert(#ssl.hmac_sha256_file("key", path) == 32)
assert(os.remove(path))
assert(ssl.sha256_materialized_hex("abc") ==
  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
do
  local message = "The quick brown fox jumps over the lazy dog"
  local expected_hmac = "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"
  local context <close> = digest.hmac_new("sha256", "key")
  context:update(message:sub(1, 7)):update(message:sub(8))
  local value, count = context:final_hex()
  assert(value == expected_hmac and count.high == 0 and count.low == #message)
  assert(context:final_hex() == value)
  assert(not pcall(context.update, context, "x"))
  assert(ssl.hmac_sha256_materialized_hex("key", message) == expected_hmac)
  assert(ssl.hmac_materialized_hex("sha256", "key", message) == expected_hmac)
  assert(digest.hmac_materialized_hex("sha256", "key", message) == expected_hmac)
  context:close(); context:close()
  assert(not pcall(context.final, context))
end
assert(ssl.hmac == nil and ssl.hmac_sha256 == nil)
assert(ssl.hmac_stream_hex("sha512", "", function() return nil end) ==
       ssl.hmac_materialized_hex("sha512", "", ""))
for _, value in ipairs({"", true, 3, string.rep("x", 65537)}) do
  assert(not pcall(ssl.hmac_stream, "sha256", "key", function() return value end))
end
for _, read in ipairs({function() error(failure, 0) end,
                       function() return nil, failure end}) do
  local passed, raised = pcall(ssl.hmac_stream, "sha256", "key", read)
  assert(not passed and raised == failure)
end
assert(not pcall(ssl.hmac_stream_new, "sha256\0suffix", "key"))
assert(not pcall(ssl.hmac_stream_new, "shake128", "key"))
assert(not pcall(ssl.hmac_file, "sha256", "key", "missing-hmac-input"))
assert(not pcall(ssl.hmac_file, "sha256", "key", "filename\0suffix"))
print("upgrade seam behavior ok: bounded digest streaming")
