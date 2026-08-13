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
- `openssl.random_bytes(size)` returns CSPRNG bytes from `RAND_bytes`.
- `openssl.random_hex(size)` returns CSPRNG bytes encoded as lowercase hex.

`random_bytes` and `random_hex` accept sizes from `0` to `1048576` bytes.

```lua
local openssl = require("openssl")

assert(openssl.sha256_hex("abc") ==
  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")

local nonce = openssl.random_hex(16)
```

This facade is intentionally small. Additions should be made when they expose a
stable OpenSSL-backed primitive or unblock a concrete Vectis workflow without
pulling Lua into OpenSSL object lifetime management.
