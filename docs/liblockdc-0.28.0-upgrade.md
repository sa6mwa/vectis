# liblockdc 0.28.0 upgrade

## Specification and review

Upgrade Vectis baseline `6b1e263` from liblockdc 0.27.0 to the released
[0.28.0 artifacts](https://github.com/c89systems/liblockdc/releases/tag/v0.28.0),
including all seven core SDKs, seven PostgreSQL-provider SDKs and unchanged
upstream Lua API. Align its required LoneJSON 0.46.0 and pslog 0.13.0 C/Lua
SDKs on all seven targets; keep cpkt 0.6.1 and liblql 0.4.1. Preserve unrelated
staged `LICENSE` changes and all sibling repositories. No release/tag/push.

Public C interfaces/layouts remain unchanged. Lockdc core ABI remains 9,
PostgreSQL provider ABI 0, and LoneJSON ABI 26. CMake and pkg-config consumer
requirements must select the same dependency closure as the executable. Public
LoneJSON header changes are attribution only; pslog clarifies failed-constructor
output ownership without adding API. Its Lua fixes protect callback cycles,
protected allocation/error paths and owner lifetimes; consume them unchanged.
LoneJSON fixes unoptimized serializer stack exhaustion with bounded/heap scratch,
while retaining its public and Lua interop contract.

Pouch push and `append_staged` now encode bounded chunks directly into canonical
segments and immutable offset indexes. Commit publishes the entry atomically
without copying a complete staged body. Large reads, compaction, compression
conversion and recovery consume bounded chunks; reads use up to 64 KiB heap
scratch. Public method names, source/writer ownership, lease fencing, retention,
append IDs, uncertain publication, durable group sync and exact counts remain.
Vectis's existing app-owned collection/writer adapters already forward these
operations; no new facade method or local provider patch is required.

Existing contiguous records remain readable. Push/staged writes can publish new
chunk/index record kinds that older clients cannot read. Upgrade every client
sharing a root before using those paths and do not downgrade afterward. No
Vectis migration engine, format-version bump, implicit replay or storage fallback
is introduced. Keep production roots untouched during verification.

Implement all pin/build/consumer floors, version assertions, manual/example,
matrix/audit and regression changes before broad testing. Verify direct/app-owned
large push/staged/edit/read, publication visibility/order, retry/failure/cleanup,
reopen and compression conversion, upstream logger callback ownership, actual
selected-header/embedded-source provenance, default musl worker stacks,
normal/packed execution, services/XA wake, installed static/shared consumers,
source-rock and embedded manuals. Use existing broader regressions and independent
released SDKs; build operational lanes serially. Upstream private crash/allocator
fault tests are evidence to inspect, not hooks to import into Vectis.

Specification review: complete. No new application or architecture choice is
needed; the upstream storage compatibility boundary is explicit.

## Changes and verification

Updated 31 release-asset pins: seven core SDKs, seven PostgreSQL-provider SDKs,
one lockdc Lua source archive, and seven SDKs plus one Lua source archive for
each of LoneJSON and pslog. Acquisition verified their SHA-256 digests. Exact
build requirements and installed CMake/pkg-config minimums select this closure.
Version assertions, manuals, the storage writer example and current coverage
documents now describe the released behavior. Public headers and Lua sources
were compared with the previous pinned artifacts; no Vectis production C/Lua
adapter change or dependency-facade extension was needed.

Added observable collection regressions for direct/app-owned receivers with
plain/encrypted storage: an incremental 131,083-byte encoded push, another
publication during input, pending-entry invisibility, commit-order positions,
bounded read callbacks and an incremental digest of the complete output. The
existing larger staged fixture now uses 128 KiB segment targets to exercise
rollover. Existing error, lease fencing, edits, head validation, cleanup and
reopen assertions remain. Added an embedded pslog callback/owner/logger cycle
collection regression.

Verification completed on 2026-11-06, with operational commands serialized:

- GNU Debug focused checks: **7/7 passed** in 19.12 seconds. Full Debug CTest:
  **286/286 passed** in 521.32 seconds, including real local PostgreSQL/XA,
  supervisor lifecycle/wake paths, normal/packed execution, and actual compiled
  header and embedded upstream-source provenance. No production implementation
  changes followed this run.
- All seven Release targets built the executable, static/shared libraries and
  C89 public-header checks: x86_64/aarch64/armhf GNU and musl, plus arm64 Darwin.
  These used the acquired released SDKs. Darwin verification is compile/link
  only; this does not claim a new full application deployment on every target.
- x86_64 musl Release focused runtime checks: **6/6 passed** in 21.71 seconds.
  Native default-pthread-stack checks also passed on aarch64 and armhf musl
  under QEMU. Observed default stacks were 133,656, 133,640 and 132,048 bytes
  respectively. The worker exercised released collection callbacks, encrypted
  reads/edits and nested native digest/Lua callbacks without increasing stacks.
- After the final test-only small-segment refinement, the collection fixture
  passed again in Debug, GNU Release and musl Release, including packed runs.
  Strict Valgrind on this exact final fixture reported **53,714 allocations and
  frees, zero live blocks and zero errors**, with no suppressions. Separate
  worker and upstream-facade runs likewise had zero live blocks/errors
  (117,777 and 24,115 allocations/frees). All leak kinds were fatal.
- A scratch encrypted root seeded by the preserved **0.27.0 executable** opened
  with 0.28.0, preserving collection identity, count policy, old ETag and read
  continuation. New chunked publication, reopen, zlib-to-none conversion and
  inherited compression mode preserved payload digests and append receipts.
  No production root was used.
- A fresh GNU Release SDK archive was relocated. Installed static and shared
  consumer checks passed C89/C++98/pkg-config closure and runtime contracts for
  **36 C examples in each mode**. Matching core/provider ABI metadata was
  inspected. The archive is a development artifact, not a new Vectis release.
- Embedded dependency-facade manuals matched their source bytes. Lua source-rock
  staging/smoke checks passed; their stub-based packaging checks are distinct
  from the native behavior runs above. Repository formatting and whitespace
  checks passed.

Scratch inputs, comparisons, legacy fixtures, artifacts and command logs are
under ignored `build/lockdc-0.28.0-review/`. Representative commands include
`cmake --preset debug`, `cmake --build --preset debug --parallel 2`,
`ctest --test-dir build/debug --output-on-failure -j1`, the seven concrete
`<target>-release` presets, `scripts/verify_installed_sdk.sh <prefix> static`
and `shared`, `scripts/stage_lua_rock_sources.sh`, and
`scripts/check_format.sh`. Strict memory runs used
`valgrind --error-exitcode=99 --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all`.

Review found no missing Vectis seam or actionable integration defect. Upstream
private power-loss, allocator-fault and maintenance-injection suites were not
rerun downstream. These results do not establish a whole-process RSS bound or
authorize older clients to consume the new chunk records. No release, tag or
push was performed; the unrelated staged `LICENSE` change remains excluded.
