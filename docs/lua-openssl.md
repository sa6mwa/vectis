# OpenSSL Lua Facade

`require("openssl")` exposes a narrow raw OpenSSL facade for stable crypto
primitives that are useful from Lua application code. Certificate workflows
remain under `vectis.cert`.

## API

- `openssl.version()` returns the linked OpenSSL version string.
- `openssl.sha256(data)` returns a 32-byte binary digest.
- `openssl.sha256_hex(data)` returns the SHA-256 digest as lowercase hex.
- `openssl.hmac_sha256(key, data)` returns a 32-byte binary HMAC digest.
- `openssl.hmac_sha256_hex(key, data)` returns the HMAC digest as lowercase
  hex.
- `openssl.digest(algorithm, data)` returns a binary digest for an OpenSSL EVP
  digest name such as `sha1`, `sha256`, or `sha512`.
- `openssl.digest_hex(algorithm, data)` returns the digest as lowercase hex.
- `openssl.hmac(algorithm, key, data)` returns a binary HMAC for an OpenSSL EVP
  digest name.
- `openssl.hmac_hex(algorithm, key, data)` returns the HMAC as lowercase hex.
- `openssl.hex_encode(data)` returns lowercase hexadecimal.
- `openssl.hex_decode(hex)` decodes lowercase or uppercase hexadecimal.
- `openssl.base64_encode(data)` returns padded Base64.
- `openssl.base64_decode(data)` decodes padded Base64.
- `openssl.sign(opts)` signs `opts.data` with `opts.private_key_pem` or
  `opts.private_key_path` and returns a binary signature. `opts.algorithm` or
  `opts.digest` defaults to `sha256`.
- `openssl.sign_hex(opts)` returns the signature as lowercase hex.
- `openssl.verify(opts)` verifies `opts.signature` or `opts.signature_hex`
  against `opts.data` using `opts.public_key_pem`, `opts.public_key_path`,
  `opts.certificate_pem`, or `opts.certificate_path`. It returns a boolean.
- `openssl.random_bytes(size)` returns CSPRNG bytes from `RAND_bytes`.
- `openssl.random_hex(size)` returns CSPRNG bytes encoded as lowercase hex.

Allocation-backed helpers accept payloads from `0` to `1048576` bytes.

```lua
local openssl = require("openssl")

assert(openssl.sha256_hex("abc") ==
  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")

local nonce = openssl.random_hex(16)
local sha1 = openssl.digest_hex("sha1", "abc")
local mac = openssl.hmac_hex("sha256", "key", "message")
local signature = openssl.sign({
  private_key_path = "client-key.pem",
  data = "payload",
})
assert(openssl.verify({
  certificate_path = "client-cert.pem",
  data = "payload",
  signature = signature,
}) == true)
```

This facade is intentionally small. Additions should be made when they expose a
stable OpenSSL-backed primitive or unblock a concrete Vectis workflow without
pulling Lua into OpenSSL object lifetime management.
