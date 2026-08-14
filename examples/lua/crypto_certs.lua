local openssl = require("openssl")
local vectis = require("vectis")

local function join_path(dir, name)
  if dir:sub(-1) == "/" then
    return dir .. name
  end
  return dir .. "/" .. name
end

local work_dir = os.getenv("VECTIS_LUA_CRYPTO_EXAMPLE_DIR")
local cert_path
local key_path
local bundle_path
if work_dir ~= nil and work_dir ~= "" then
  cert_path = join_path(work_dir, "vectis-lua-example-cert.pem")
  key_path = join_path(work_dir, "vectis-lua-example-key.pem")
  bundle_path = join_path(work_dir, "vectis-lua-example-bundle.pem")
else
  cert_path = os.tmpname()
  key_path = os.tmpname()
  bundle_path = os.tmpname()
end

os.remove(cert_path)
os.remove(key_path)
os.remove(bundle_path)

assert(openssl.sha256_hex("abc") ==
  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
assert(openssl.hex_decode(openssl.hex_encode("payload")) == "payload")
assert(openssl.base64_decode(openssl.base64_encode("payload")) == "payload")
assert(#openssl.random_hex(16) == 32)
assert(#openssl.hmac_hex("sha256", "key", "message") == 64)

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
assert(vectis.cert.validate_pair({
  certificate_path = cert_path,
  private_key_path = key_path,
  ca_bundle_path = bundle_path,
}) == true)
local inspected = assert(vectis.cert.inspect_bundle(bundle_path))
assert(inspected.subject.common_name == "localhost")
assert(inspected.public_key_type == "rsa")
assert(inspected.public_key_bits == 2048)

local signature = assert(openssl.sign({
  private_key_path = key_path,
  data = "signed payload",
}))
assert(openssl.verify({
  certificate_path = cert_path,
  data = "signed payload",
  signature = signature,
}) == true)
assert(openssl.verify({
  certificate_path = cert_path,
  data = "mutated payload",
  signature = signature,
}) == false)

os.remove(cert_path)
os.remove(key_path)
os.remove(bundle_path)

print("lua crypto certs example ok")
