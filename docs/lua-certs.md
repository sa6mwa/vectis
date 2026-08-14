# Vectis Lua Certificates

`vectis.cert` exposes Vectis-owned OpenSSL-backed certificate workflows. It is
the default Lua DX for certificate material; lower-level OpenSSL access remains
a separate raw facade under `require("openssl")`.

## Generation

- `vectis.cert.generate_private_key(opts)` writes a standalone PEM private key.
- `vectis.cert.generate_csr(opts)` writes a PEM certificate signing request.
- `vectis.cert.generate_bundle(opts)` writes a certificate/private-key bundle,
  split cert/key files, or both.

```lua
local vectis = require("vectis")

assert(vectis.cert.generate_private_key({
  output_key_path = "server.key",
  key_bits = 2048,
}) == true)

assert(vectis.cert.generate_csr({
  subject = {
    common_name = "server.local",
    organization = "Example",
  },
  dns_names = "server.local",
  ip_addresses = "127.0.0.1",
  private_key_path = "server.key",
  output_csr_path = "server.csr",
}) == true)

assert(vectis.cert.generate_bundle({
  common_name = "localhost",
  dns_names = "localhost",
  ip_addresses = "127.0.0.1",
  output_bundle_path = "server.pem",
  key_bits = 2048,
  valid_days = 30,
}) == true)
```

`subject` may be supplied as a nested table. `generate_bundle` also accepts
top-level `common_name` for the common case.

## Validation And Inspection

- `vectis.cert.validate_bundle(path_or_opts)`
- `vectis.cert.validate_pair(opts)`
- `vectis.cert.inspect_bundle(path_or_opts)`

Validation helpers return `true` on success. On failure they return `nil,
error`, where `error` is a structured Vectis status table.
