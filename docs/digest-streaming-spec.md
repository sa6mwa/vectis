# Materialized and streaming digests

Implement a public C89 `vectis_digest` receiver in libvectis, using the SDK's
`cpkt/openssl.h` import surface. Lua binds this C implementation. Existing
SHA-256/EVP digest and HMAC string helpers are explicitly named `*_materialized`
with `_hex` following that qualifier for hexadecimal results. Migrate callers
and remove the ambiguous names in this pre-1.0 Lua API; do not retain aliases.
The existing digest payload helpers have no 1 MiB input limit. They still require
a whole Lua string. Encoding, random-byte, PEM and signing limits are separate.

The receiver owns an EVP context, has explicit self method fields, a private
implementation and eight reserved slots. It supports update, final and close,
with corresponding public free functions. Final returns a bounded binary result
and an exact uint64_t consumed-byte count, is idempotent, and prohibits later
updates. Errors latch failure; closing frees the receiver and tolerates NULL.
Contexts are caller-serialized and create no threads. Reject unknown and
variable-output digest algorithms rather than invent a result length.

Public C materialized, reader-stream and file conveniences return the same
fixed result record and vectis_error. Reader callbacks distinguish failure from
EOF and cannot exceed the supplied capacity. Read files in binary mode, process
short reads, detect read and close failure, and never return a successful partial
digest. Hash observed bytes; file hashing does not promise a snapshot against
concurrent mutation. Caller-selected paths follow ordinary filesystem semantics.

Lua exposes `sha256_materialized[_hex]`, `digest_materialized[_hex]`, and the
matching explicit materialized HMAC names. Add `sha256_stream[_hex](reader)` and
`digest_stream[_hex](algorithm, reader)`, and corresponding `*_file[_hex]`
functions taking a path. A reader receives capacity 65536 and returns a string
no larger than that, or nil at EOF. Empty strings fail to prevent a no-progress
loop; nil with an error is failure. Callback exceptions retain their original
Lua object and native resources are closed before rethrowing. Install a Lua GC
cleanup owner before native acquisition; close the native context before
allocating the final Lua result. Never longjmp through a provider callback.

`openssl.digest_stream_new(algorithm)` exposes the public incremental receiver
with update, final/final_hex and idempotent close, plus __close/__gc ownership.
Updates consume caller-provided chunks no larger than 65536 bytes. Final also
returns the exact consumed count as unsigned 32-bit high/low words, retaining the
full uint64 domain. Persistent receivers retain their finalized result for retry
after a Lua allocation error; convenience calls free their context before result
allocation. No provider context leaks into Lua as an unchecked native pointer.

Streaming uses one bounded input chunk and the provider's digest state. No whole
input string, concatenation, staging file or cumulative input-size cap is used.
Support total byte counts through UINT64_MAX; check addition before updating and
fail on overflow. This is finite 64-bit capacity, not literally infinite input.
Readers/files and materialized hashing are synchronous, explicitly documented.
The finite digest output is necessarily materialized (32 bytes for SHA-256).

HMAC uses the same public receiver update/final/close contract, constructed by
`vectis_hmac_new(algorithm, key, key_size, out, error)`. The public
`vectis_hmac_materialized`, `vectis_hmac_stream` and `vectis_hmac_file`
conveniences accept an explicit length-bearing binary key. Construct the HMAC
provider through EVP_MAC using only `cpkt/openssl.h`; no legacy integer key
length narrowing. A key is supplied at construction, not accumulated from the
payload reader. Empty and binary keys are valid. Native context ownership
includes provider key storage, which is released on close.

Lua adds `hmac_stream_new(algorithm, key)`,
`hmac_stream[_hex](algorithm, key, reader)` and
`hmac_file[_hex](algorithm, key, path)`, plus SHA-256 conveniences named
`hmac_sha256_stream[_hex]` and `hmac_sha256_file[_hex]`. The existing explicit
materialized HMAC names retain their signatures but bind the same C engine.
Receiver finalization and count accounting are identical to digest receivers.
Reader failures preserve identity and release native state. No streaming HMAC
operation assembles the payload. The fixed-output convenience rejects XOFs;
the separate complete OpenSSL API inventory still owns other MAC algorithms.

Prove known vectors, binary NUL, empty input, >1 MiB materialized and streaming
input, fragmented reads, reader errors/empty/oversize chunks, invalid algorithms,
file failure/cleanup, latched failure and repeated final. Exercise the 64-bit
overflow boundary by setting private test state, without multi-exabyte fixtures
or production fault switches. Add normal/packed Lua checks, C/C++ header and
static/shared installed-consumer checks, focused Memcheck, and update manuals,
coverage and integration evidence. Keep new probes deterministic and fast.
