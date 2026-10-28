# Libvectis digests

`vectis.digest` binds the public C89 receiver in `vectis/digest.h`, also available
through the OpenSSL helpers. Whole-value APIs are named materialized.

`digest.new(algorithm)` returns a caller-owned incremental receiver with update,
final/final_hex and close. `__close` and `__gc` clean up retained resources.
`final` returns the digest plus the exact consumed count as unsigned high/low
32-bit words. The receiver rejects updates after final or operational failure.

`digest.materialized(algorithm, data)` / `materialized_hex` take a whole string.
`digest.stream(algorithm, reader)` / `stream_hex` consume bounded chunks without
collecting the input. `digest.file(algorithm, path)` / `file_hex` read a binary
file directly; they do not create a staging file. Stream/file helpers return a
finite digest, not a copy of the source.

Readers return 1 through `digest.CHUNK_BYTES` (65536) bytes per call, nil at EOF,
or nil/error on failure. Exceptions preserve their original Lua object. All
methods are synchronous. Byte accounting is checked through UINT64_MAX;
overflow fails rather than wrapping. File operations obey OS and algorithm
semantics and hash observed bytes without promising an atomic file snapshot.
`digest.MAX_DIGEST_BYTES` is 64, the fixed-output digest capacity, not an input
size limit.

`digest.hmac_new(algorithm, key)` constructs the same incremental receiver for
HMAC. The key is a length-bearing binary string; empty keys and embedded NUL
are supported. `hmac_materialized[_hex](algorithm, key, data)` operates on a
whole payload. `hmac_stream[_hex](algorithm, key, reader)` and
`hmac_file[_hex](algorithm, key, path)` process bounded chunks with the same
EOF, cleanup, exact-count and failure contracts as digests. They use the public
C `vectis_hmac_new`, `vectis_hmac_materialized`, `vectis_hmac_stream` and
`vectis_hmac_file` operations. The provider retains key state only until close.

```lua
local digest = require("vectis.digest")
local context <close> = digest.new("sha256")
context:update("a"):update("bc")
local hex, count = context:final_hex()
assert(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
assert(count.high == 0 and count.low == 3)
```
