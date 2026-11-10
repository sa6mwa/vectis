# cpktmisc 0.4.0 upgrade

## Reviewed specification

Upgrade the seven cpktmisc SDK pins from 0.2.1 at Vectis `91e1d76` to
[0.4.0](https://github.com/c89systems/cpktmisc/releases/tag/v0.4.0), upstream
commit `c7446deb8f9b66f9fda6dbb0951a73ff7eae4d6e`. Source SHA-256:
`a7ac3928921208b1eb95c0be769c90b5f618bbb9a76140b74833eae8430a0917`.
Retain core 0.7.0, database 0.4.0 and all other dependency pins. Preserve the
unrelated staged LICENSE attribution. No release, push or sibling edits.

Compare released source, public headers, manifests, package targets, recipes,
documentation and tests against 0.2.1. Retained bundled component versions and
source digests are unchanged. LZO and its targets are removed; libarchive's
static/shared closure no longer links it. Archive therefore becomes feasible,
but remains a separate planned binding: this upgrade preserves Vectis's shipped
OPC UA, audio/SUS, PDF/PNG and compression workflows. No archive/extraction or
generic FFI/YAML application workflow is introduced by this version upgrade.
Correct active planning documents so they no longer claim an upstream blocker.
LZO remains excluded; no external lzop fallback is exposed.

Retained facade signatures and shared ABI majors are unchanged. Audio documents
heap allocation failure for MP3 workspace; existing Lua error conversion must
preserve ERR_ALLOC. Heap-backed SUS hashing, MP3 decoding, PDF CID maps, LZ4
contexts and OPC UA workspaces, iterative diagnostic copy/free, YAML traversal
and GGML graph construction improve default-worker stack use. URL rewind-buffer
compaction is amortized; decoding remains incremental. FFI admission limits
(32 args, 8 aggregate levels, 1024-byte values, 4096 visits, 16 KiB marshalling,
64 KiB native headroom) affect SDK C callers only: Vectis has no FFI binding.
Do not bypass them or create an unused facade. Authoritative upstream Lua
facades are consumed unchanged; no Vectis C ABI change is needed.

Require the selected misc version in build and installed CMake discovery.
Add default-worker regressions for native LZ4 and deep OPC UA diagnostics, and
exercise TrueType UTF-8 CID fonts through the existing Lua PDF receiver on the
combined callback worker. Keep buffers on heap and worker attributes default.
Finish implementation before broad testing. Verify focused Debug behavior,
the standard suite, musl default workers, strict memory cleanup, actual header
and link provenance, normal/packed applications, embedded manuals, source rocks
and relocated static/shared consumers. Cross-SDK checks distinguish Linux
runtime from Darwin compile/link evidence; live hardware/model tests remain
explicitly conditional. Record exact executed checks and limitations below.

Specification review: sufficiently specified for the existing product surface.
Archive implementation is now unblocked, not delivered by this upgrade.

## Implementation and evidence

All seven pins now use the published 0.4.0 SDK digests. Build discovery checks
`CpktMisc_VERSION` exactly; installed discovery requires at least 0.4.0. The
bundle has no ConfigVersion file, so this checks its validated manifest value.
pkg-config anchors the minimum through the bundle-versioned `cpkt-lz4 >= 0.4.0`
package; audio, PDF and OPC UA package versions name their unchanged native
components instead. Existing imported targets retain separate core/misc prefixes.

Reviewed released 0.2.1 source SHA-256
`a6d2736a03808b0490b07e05534bf22bd831c7c069ecaac6048a5cfb61a4dc36`
and SDK headers/configs against 0.4.0. Audio's header changes document allocation
failure; FFI adds its admission constants/contracts; `cpkt/lzo.h` disappears.
Other retained facade headers are unchanged. Source/recipe/patch review includes
SUS digest buffers, audio workspace and compaction, LibHaru CID allocation, LZ4
heap defaults, iterative libyaml and GGML traversal, and OPC UA diagnostic,
batch, encoding and PubSub allocation changes. Existing audio/SUS bindings map
allocation failures to `VECTIS_ERR_NOMEM` with native diagnostics. Existing
Vectis-owned receivers/convenience workflows need no new methods or ownership
changes. No authoritative upstream Lua facade is modified.

Verification on 2026-11-10, with operations serialized:

| Check | Result and scope |
| --- | --- |
| Focused Debug | 9/9 passed, 18.16 seconds. Main executable, combined worker and new misc worker were freshly built; the subsequent full build/suite supplies fresh evidence for all remaining helpers. |
| Standard checks | Fresh complete GNU Debug build and **296/296 tests passed**, 526.03 seconds. Includes real PostgreSQL/XA, OPC UA client/server and reentry, managed lifecycle/fork/wake, normal/packed facades, callback failures and Lua allocation-failure cleanup. Formatting and `git diff --check` pass. |
| Default workers | Native LZ4 default/high-compression/frame round trips, undersized decode failure and independent copy/free of an 8,192-node diagnostic chain pass. Debug measures **131,072 bytes with a 4,096-byte guard**. Lua loads a UTF-8 TrueType CID font and draws text through the existing PDF receiver on the combined worker. No stack is enlarged. |
| Seven SDKs | C89 static and C++98 shared header/codec consumers plus C11 native static/shared workers compile/link for all seven targets. Linux passes **26 runtime cases**, ARM under QEMU. GNU workers use the private 128 KiB test constructor; musl workers use native default attributes. Darwin is compile/link only. Two x86_64 cases additionally construct/revisit/clear an 8,192-node GGML graph without a model. These are SDK probes, not seven full Vectis builds. |
| GNU/musl products | Executable, static/shared libraries and public-header checks build against the pinned SDKs. Musl passes **7/7**, 2.67 seconds: normal/packed PDF/PNG/compression, audio/SUS smoke, native misc/combined workers and C++ headers. Its misc worker measures **134,456 bytes with an 8,192-byte guard**; combined worker **133,624 bytes**, both with NULL pthread attributes. |
| Strict memory | **Nine unchanged-environment probes pass with zero live blocks and zero errors**, without suppressions and with every leak kind fatal: native misc worker (32,783 allocations/frees), combined Lua/native worker (153,530), PDF Lua OOM (22,678), PNG Lua OOM (31,454), compression Lua OOM (37,425), audio binding cleanup (2,045), SUS binding cleanup (4,286), static SDK worker (32,783) and shared SDK worker (32,783). SDK archives use dynamic pinned libc so Memcheck observes malloc. |
| GGML memory qualification | The extra graph probe's default C++ runtime retains its 73,728-byte emergency exception pool; strict Memcheck therefore fails that default run. With only this scratch probe's `GLIBCXX_TUNABLES=glibcxx.eh_pool.obj_count=0`, graph ownership balances **7 allocations/frees, zero live bytes/errors**. This conditional result does not replace or relabel the default failure. See the explanation below. |
| Installed SDK | Fresh GNU development archive relocated to a new prefix. Static/shared C89/C++98 and pkg-config consumers execute successfully, including existing useful facade workflows. Each mode builds **36 packaged examples** and passes runtime-closure checks; this does not claim execution of all 36 workflows. Shared loading/private-symbol checks pass. An explicit old-misc fixture is rejected with `Vectis requires cpktmisc >= 0.4.0; selected 0.2.1`. |
| Payload provenance | Actual compile/link commands use the selected misc prefix and provider archives/DSOs. Acquired audio/FFI headers match reviewed release bytes; SHA-256 respectively `5a68c63c83913d7874e95ce7e23d194dcb2eb8bc4d4e7f6442fa9a4ed4ab909b` and `ee35ac2e6137cb7e0c183a85d35a28666fea6bdde4c8fa0af649d2566c2af5fc`. All seven extracted SDKs report 0.4.0, have no LZO payload and declare no LZO archive link dependency. Relocated packaging also contains no LZO payload. |
| Embedded docs/rock | Final GNU and musl `--action docs` output matches exact README/audio manual bytes and SHA-256 digests. Lua source archive, rockspec, source rock and packaging smoke pass. Native stub packaging evidence is separate from actual native execution above. |

The GGML retention originates in `_GLOBAL__sub_I_eh_alloc.cc`, including with
the static C++ runtime. GCC documents the process-start emergency pool and its
[per-process disabling option](https://gcc.gnu.org/onlinedocs/libstdc++/manual/using_exceptions.html).
Only the scratch memory probe sets that option. Product configuration and the
nine default-environment memory probes are unchanged. No allocator suppression,
provider cleanup patch or shipping runtime policy was introduced.

Scratch fixture corrections used the actual native `png.h`, an independently
compiled Debug stack constructor (avoiding inherited duplicate feature defines),
the published OPC UA target name and explicit cross-toolchain package roots.
The GGML C-link driver still showed the same static runtime pool, then the
documented probe-only tunable isolated graph allocations. Passed platform and
product suites were not repeated for these fixture-only changes.

Final semantic review traced every changed used contract through native binding,
Lua receivers, convenience/app ownership, default worker and fork placement,
error/logging paths, bounded streaming, normal/packed execution, documentation,
source rocks and static/shared closure. No actionable Vectis integration defect
or required provider handoff remains for the existing product surface. No ABI
version bump is needed. Archive remains separately planned and now unblocked;
generic FFI/YAML bindings still lack a current application use case. Reconsider
them when a concrete configuration/plugin workflow requires those libraries.

Limits: loaded-model transcription, real audio hardware, live PubSub/MQTT and
HTTP MP3/long-forward-seek stress were not run here. Existing conditional model
tests reported skip; offline callback/file/URL-WAV behavior and model-free GGML
graph traversal are distinct evidence. Upstream private allocation-fault and
full codec suites were not rerun. No native Darwin or full ARM Vectis execution
is claimed, nor an arbitrary model/callback stack bound. This is an upgrade
review, not release approval.

Reproduction uses the Debug/GNU/musl presets, `scripts/deps.sh`, serialized
CTest, `scripts/verify_installed_sdk.sh <prefix> static|shared`, source-rock
staging and `scripts/check_format.sh`. Strict Memcheck options are
`--error-exitcode=99 --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all`.
Sources, SDK comparisons, CMake/CTest probes, actual commands, logs and
development artifacts are under ignored `build/cpktmisc-0.4.0-review/`.
The task commit on `init` is relative to `91e1d76`; the staged LICENSE edit is
excluded. No release, tag, push or out-of-repository edit was performed.
