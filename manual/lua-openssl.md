# OpenSSL Lua Facade

`require("openssl")` exposes a narrow dependency-native OpenSSL facade for
stable crypto primitives that are useful from Lua application code. Certificate
workflows remain under `vectis.cert`.

## API

- `openssl.version()` returns the linked OpenSSL version string.
- `openssl.sha256_materialized(data)` returns a 32-byte binary digest.
- `openssl.sha256_materialized_hex(data)` returns the SHA-256 digest as lowercase hex.
- `openssl.hmac_sha256_materialized(key, data)` returns a 32-byte binary HMAC digest.
- `openssl.hmac_sha256_materialized_hex(key, data)` returns the HMAC digest as lowercase
  hex.
- `openssl.digest_materialized(algorithm, data)` returns a binary digest for an OpenSSL EVP
  digest name such as `sha1`, `sha256`, or `sha512`.
- `openssl.digest_materialized_hex(algorithm, data)` returns the digest as lowercase hex.
- `openssl.hmac_materialized(algorithm, key, data)` returns a binary HMAC for an OpenSSL EVP
  digest name.
- `openssl.hmac_materialized_hex(algorithm, key, data)` returns the HMAC as lowercase hex.
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

All whole-string digest and HMAC operations are explicitly named materialized.
They accept an already-created Lua string and have no 1 MiB payload ceiling;
they do not read files or consume a producer incrementally. HMAC keys are binary
strings passed with a native size_t length. Encoding, random-byte, PEM and signing helpers
have separate 1 MiB allocation/input limits.

True streaming digest operations:

- `openssl.sha256_stream(reader)` / `sha256_stream_hex(reader)` hash successive
  bounded chunks. `reader(capacity)` receives `65536`; return a nonempty string
  no larger than that, or `nil` at EOF. `nil, err` and callback exceptions fail
  the operation and preserve the original error object.
- `openssl.digest_stream(algorithm, reader)` / `digest_stream_hex(...)` select
  another fixed-output EVP digest.
- `openssl.sha256_file(path)` / `sha256_file_hex(path)` and
  `openssl.digest_file(algorithm, path)` / `digest_file_hex(...)` perform binary
  file reads in bounded chunks. They detect IO failure and do not hash a
  concurrently modified file as an atomic snapshot.
- `openssl.digest_stream_new(algorithm)` creates an incremental owned receiver.
  `ctx:update(chunk)` consumes up to 65536 bytes and returns the receiver;
  `ctx:final()` / `ctx:final_hex()` return the digest and an exact byte count as
  `{high=unsigned_32_bits, low=unsigned_32_bits}`. Final is idempotent. Updates
  after final are rejected. `ctx:close()`, `__close` and `__gc` release ownership.
- `openssl.hmac_stream_new(algorithm, key)` creates the same receiver for HMAC.
  Empty and binary keys are valid. `hmac_stream[_hex](algorithm, key, reader)`
  and `hmac_file[_hex](algorithm, key, path)` share the digest reader/file contract.
  `hmac_sha256_stream[_hex](key, reader)` and
  `hmac_sha256_file[_hex](key, path)` select SHA-256. Provider key state is owned
  until close; the input payload is never accumulated by these operations.

These operations are synchronous. Reader/file paths never collect the whole
input, use a staging file or impose a cumulative 1 MiB cap. Checked uint64 byte
accounting rejects overflow; native algorithm semantics still apply. The final
digest is necessarily materialized (32 bytes for SHA-256). XOF algorithms require
their separate length-aware public operations rather than this fixed-output
helper. [`vectis.digest`](lua-digest.md) binds the same libvectis C receiver.

```lua
local openssl = require("openssl")

assert(openssl.sha256_materialized_hex("abc") ==
  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")

local nonce = openssl.random_hex(16)
local sha1 = openssl.digest_materialized_hex("sha1", "abc")
local mac = openssl.hmac_materialized_hex("sha256", "key", "message")
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

This facade is intentionally small. The current lower-level surface is limited
to concrete workflows already needed by Vectis Lua applications: digest and
HMAC helpers, encoding helpers, PEM-backed signing and verification,
key/certificate-backed verification, reusable keys, materialized AES-GCM and
CSPRNG bytes. Certificate generation,
CSR generation, CA material, bundle validation, and certificate inspection stay
under `vectis.cert`.

Exhaustive BIO/provider/X.509 object mirroring is outside the current useful
facade scope. These operations use public EVP/PEM APIs without adding another
TLS stack or provider manager.

## Owned signing keys

`openssl.load_key(opts)` accepts exactly one of `private_key_pem`,
`private_key_path`, `public_key_pem`, `public_key_path`, `certificate_pem` or
`certificate_path`, plus optional `passphrase`. PEM/file input is materialized
and limited to 16 MiB; files must be seekable. Passphrase bytes are limited to
1023 and copied by a native callback that never prompts or enters Lua.

```lua
local openssl = require('openssl')
local key <close> = assert(openssl.load_key({private_key_path='signing-key.pem'}))
local public <close> = assert(openssl.load_key({certificate_path='signing-cert.pem'}))
local signature = assert(key:sign('report'))
assert(public:verify('report',signature))
```

`key:sign(data,{algorithm='sha256'})` returns binary signature bytes.
`key:verify(data,signature,opts)` returns a boolean for a completed check,
or `nil,error` for native failure. Digest options retain existing `algorithm`
and `digest` naming/precedence and SHA-256 default. Unsupported key/digest
combinations report errors. Inputs/signatures have a 16 MiB ceiling. Key objects
reuse decoded key state with a fresh operation context per call.

`key:info()` returns `type`, `bits`, `max_signature_bytes`, `private_key`.
Certificate loading extracts its public key; it does not validate a trust chain.
Use `vectis.cert` for certificate workflows. Key generation is not added here.
Keys are confined to their creating process/thread. Lua close is idempotent;
`__close`/`__gc` release native ownership, and active operations reject close.
Errors include status/source, operation and native code/detail. Unknown option
names and invalid argument types raise before native execution where possible.

## Materialized AES-GCM

`openssl.aes_gcm_encrypt_materialized({key,nonce,data,aad=...})` returns
`{ciphertext,tag}`. `aes_gcm_decrypt_materialized({key,nonce,ciphertext,tag,aad=...})`
returns authenticated bytes or `nil,error`. Keys must be 16/24/32 bytes, nonce
12 bytes and tag 16 bytes. Supply a distinct nonce for each encryption with a
key; there is no implicit nonce generation or persistence.

```lua
local key = openssl.random_bytes(32)
local nonce = openssl.random_bytes(12)
local encrypted = assert(openssl.aes_gcm_encrypt_materialized({
  key=key,nonce=nonce,data='report',aad='application record',
}))
local plaintext = assert(openssl.aes_gcm_decrypt_materialized({
  key=key,nonce=nonce,ciphertext=encrypted.ciphertext,tag=encrypted.tag,
  aad='application record',
}))
assert(plaintext=='report')
```

Data/ciphertext and AAD each have a 16 MiB ceiling. Empty data/AAD and binary
strings are supported. Complete output is deliberately materialized. No
unauthenticated plaintext is returned; authentication failure frees internal
plaintext and returns an error. Lua allocation failures also release owned
native result buffers before unwinding. The C API is in `vectis/crypto.h`.
