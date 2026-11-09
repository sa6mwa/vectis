# cpkt 0.7.0 upgrade

## Reviewed specification

Upgrade baseline `75194ad` from cpkt 0.6.1 to the seven released
[0.7.0 SDKs](https://github.com/c89systems/cpkt/releases/tag/v0.7.0), revision
`380332d4638701c0632a0818a7b8863baffc3026`. Source SHA-256:
`4d77a38fec34e2acb53dcd297578628dc57e7b0ed6d8cb37d5dbe49cfdac15f5`.
Retain every other dependency pin and the unrelated staged `LICENSE` change.
No release, tag, push or sibling-repository modification.

Compare released headers, package manifests, source recipes and policy changes.
Existing native component versions/source digests and ABI majors are unchanged;
the only existing public header change documents Lua's native-stack guard.
Lua remains 5.5.1, with a new downstream implementation patch for cached OS stack
bounds, protected overflow/error-handler recovery and serialized state migration.
Consume the released VM and authoritative dependency Lua facades unchanged.

The new bundled zlib-ng 2.3.3 has an upstream C89 facade (ABI 0), without Lua.
Add `zlib-ng` to Vectis's existing C/Lua compression receiver, `stream` and
explicit `materialized` helpers. Its wire format is one zlib-wrapped DEFLATE
stream, interoperable with existing zlib. Omitted compression level selects the
native default; explicit levels -1 through 9 are accepted. Preserve bounded
chunks, process/thread confinement, synchronous callbacks, failure latching,
reentry rejection, idempotent finish, cleanup and strict trailing/truncation
checks. Add static/shared and pkg-config closure for `cpkt::zlibng` and retain
direct SDK C access. Do not add 82 Lua methods, another gzip/file API or a codec
replacement policy; existing zlib/gzip interfaces remain available. Reconsider
extra native controls only for a concrete application need.

Exercise one-byte input, empty streams, invalid levels/data, checksum failures,
both directions of zlib interoperability, source/sink exceptions, OOM, nested
producer-to-consumer delivery before finish, close/reentry, fork and thread
boundaries, normal/packed execution and installed consumers. Exercise the new
codec and public large CRC calls on a default musl worker. Native CRC heap-fault
injection stays upstream; downstream tests use public APIs.

Apply the lifecycle's glibc Debug 128 KiB worker defaults through a private
constructor linked only into test executables and their child applications.
Disable glibc's stack cache in test environments; preserve main-thread limits,
explicit stack requests, nonzero guards and build-tool defaults. Verify actual
default/nested worker sizes, real guard faults and exec children. No installed
library, Release executable or consumer acquires this test policy. Add focused
Lua overflow, recursive error-handler and coroutine/migration recovery tests on
default workers using public runtime entry points, plus the existing combined
callback worker. Do not change VM limits or patch providers locally.

These SDK archives store epoch mtimes. On replacement, mark newly extracted
payload files as current build inputs so native CMake/Ninja sees changed static
libraries and headers. Verified preparation hits must preserve their timestamps
and avoid requests/re-extraction. Test this with the existing acquisition fixture.

Implement the entire surface before broad checks. Then run focused Debug tests,
the standard suite with constrained workers, musl runtime/stack and strict memory
checks, seven-target SDK/product compilation as applicable, actual header/source
provenance, embedded manuals, Lua source-rock packaging, and relocated installed
static/shared consumers. Distinguish cross compile/link from execution. Review
the final implementation and fix findings with focused Debug evidence.

Specification review: complete. Useful Lua coverage fits the existing owned
compression abstraction; no additional product decision is pending.

## Implementation and verification

The first broad run exposed an existing upload/DSV integration defect: the
pull-reader worker entered an already-active owner Lua state. The new native
stack guard rejects that unsupported crossing. Required seam repair: use the
existing native push-upload callbacks on the request thread, retaining per-request
Lua values with registry ownership. Share one persistent DSV record state machine
between pull parsing and incremental push parsing; keep schema coercion and row
callbacks on the owner thread. Preserve auth, bounded delivery, cleanup/error
semantics and source responses. No new public API, whole-document buffering,
provider patch or worker-to-Lua dispatcher is required.

All seven core pins select the published SHA-256 digests. Build requirements,
installed CMake/pkg-config closure and README now select 0.7.0. Existing component
versions and source digests, including curl, OpenSSL, libssh2, libxml2, OpenLDAP
and zlib, were compared with 0.6.1 and remain unchanged. The released source
comparison identifies the Lua stack patch and new zlib-ng recipe/facade/heap-CRC
patch as the functional changes. No existing provider facade was modified.

The owned compression receiver now initializes, incrementally drives and ends
the released zlib-ng stream. Provider state and chunk workspace remain on the
heap; borrowed input/output pointers are cleared before invoking sinks. The
existing Lua binding and convenience helpers forward the algorithm unchanged.
Shared linkage includes `libcpkt_zlibng.so.0` and `libz-ng.so.2`; Vectis's public
receiver layout and shared ABI remain unchanged. The private push parser and
response-move helper do not become public SDK APIs.

Verification completed on 2026-11-09, with operational commands serialized:

| Check | Evidence |
| --- | --- |
| GNU Debug standard suite | **295/295 passed**, 503.16 seconds, with constrained default workers. Includes PostgreSQL/XA services, normal/packed applications, callback ownership, lifecycle, runtime closure and editor/toolchain checks. |
| Codec regressions | All four algorithms pass incremental pipelines, one-byte input, empty/truncated/trailing data, failed sinks, reentry, thread/fork confinement and cleanup. New zlib-ng tests cover levels -1/0/1/9, invalid levels, damaged checksums and interoperability in both directions with zlib. Normal/packed execution also passes on the final GNU and musl products. |
| Lua/native stack | Default/nested/exec Debug workers report **131,072 bytes**, explicit 256 KiB requests remain intact and guard access faults in a child. Lua recursion, recursive error handlers, string callbacks, coroutines, serialized migration and recovery pass on default workers. Release products and installed libraries contain no Debug stack constructor. |
| Upload/DSV seam | Owner-confined codec calls prove upload, DSV and response-source callbacks share the request thread. A partial HTTP request emits its first DSV row before the remaining body is sent. One-byte parser tests cover CRLF, comments, doubled/distinct escapes, multiline quotes, headerless rows, field/width/type failures, EOF and latched errors. Auth headers, source conversion/consumption faults and exactly-once upload cleanup pass in normal/packed execution. |
| Seven SDKs | New facade C89 static/C++98 shared consumers, the owned compression implementation and Lua stack probe compile/link for all seven SDKs. All six Linux targets pass **4/4 runtime probes each**; ARM uses QEMU. Darwin is compile/link only. These are SDK/component checks, not seven full Vectis builds. Lua creation/free and migration use default workers, including ARMhf musl. |
| GNU/musl products | Executable, static/shared libraries and public-header checks build with their pinned toolchains. Musl's focused product selection passes **11/11**, 4.70 seconds. Its combined Lua/native/large-CRC worker reports **133,640 bytes** with NULL pthread attributes; no stack was enlarged. |
| Strict memory probes | Seven standalone probes finish with **zero live blocks and zero errors**, without suppressions, with all leak kinds fatal: native codec, Lua stack, codec OOM, combined worker, SDK/DSV, response-source ownership and actual Lua compression. Codec OOM exercises **77 deterministic allocation failures** before clean execution; its 37,425 allocations/frees balance. The combined worker balances 143,199 allocations/frees; Lua compression balances 25,899. |
| HTTP memory scope | The actual parent/worker source-lifetime scenario passes the existing invalid-access and definite/indirect/possible-leak gate in all three process logs. The owner exits with zero live blocks. Forked processes retain reachable exit state (16,147 and 136,571 bytes); this is not an all-leak-kinds-zero result for those processes. |
| Installed GNU SDK | Fresh archive relocated to a new prefix. Static/shared C89/C++98 and pkg-config consumers execute the four-codec workflow. Both modes build **36 packaged C examples** and pass runtime-closure checks; shared relative dependency loading and private-symbol rejection also pass. |
| Provenance and inputs | Actual compile/link commands use the selected SDK prefixes. Reviewed Lua-runtime/zlib-ng headers match acquired headers byte-for-byte; the final executable contains the new native-stack TLS symbol and zlib-ng implementation. Epoch-mtime replacement invalidates actual headers/libraries, while verified cache hits preserve timestamps. The acquisition regression passes. |
| Embedded docs/rock | Updated README and Lua/app/compression manuals match embedded bytes and SHA-256 digests in both final GNU/musl executables. Lua source-rock staging/validation/smoke passes; its native stubs establish packaging, separately from real native execution above. |
| Formatting | `scripts/check_format.sh`, Bash syntax validation and `git diff --check` pass. |

After the full Debug run, optimized compilation requested explicit initialization
of the shared pull parser's byte variable; the four affected Debug checks passed
again. Musl exposed disabled assertions in the existing custom codec OOM target;
it now uses `-UNDEBUG`, with focused Debug and musl evidence. Strict SDK Memcheck
found one retained 18-byte libxml last-error diagnostic from a negative XML test;
the fixture now resets that diagnostic at program exit. The repaired SDK test
passes Debug, musl and strict Memcheck (29,374 allocations/frees, zero live bytes).
These focused corrections did not trigger repeated full suites or cross matrices.

The initial direct-I/O TLS fixture also had a timing-dependent WANT_READ
assertion: receiving upgrade headers did not establish that the server had
attempted an empty read. A pipe readiness signal now establishes that condition
before payload transmission. The focused test and subsequent full suite pass;
no timing sleep or relaxed assertion replaced the requirement.

Final semantic review traced new codec ownership/errors, native stack policy,
fork/worker placement, auth response propagation, incremental row delivery,
response-source lifetime, embedded docs, packaging and consumer closure. No
further actionable integration defect or required upstream handoff remains.
Existing upstream Lua facades are unchanged. Native CRC allocation-fault/ISA
internals remain upstream coverage; public CRC calls were exercised downstream.
Darwin runtime behavior and full ARM Vectis applications were not executed here.
Additional native zlib-ng controls remain C-only until a concrete Lua workflow
needs them. SSH execution and LZO remain deferred.

Reproduction uses `scripts/deps.sh`, the Debug and x86_64 GNU/musl presets,
`ctest --test-dir <build> -j1 --output-on-failure`,
`scripts/verify_installed_sdk.sh <relocated-prefix> static|shared`, and
`scripts/stage_lua_rock_sources.sh`. Strict Valgrind options are
`--error-exitcode=99 --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all`.
The scratch SDK and memory probes use native CMake/CTest. Sources, comparisons,
commands, artifacts and logs are under ignored `build/cpkt-0.7.0-review/`.
The task commit on `init` is relative to baseline `75194ad`; the unrelated staged
`LICENSE` attribution edit remains excluded. No release, tag, push or sibling
repository modification was performed.
