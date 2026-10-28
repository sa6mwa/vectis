# liblql 0.4.0 upgrade

## Specification and pre-implementation review

Authorized by the user on 2026-10-28 alongside the useful dependency facade goal.
Upgrade only liblql's seven SDK archives and released Lua source archive, from
0.3.0 to 0.4.0, using the published CHECKSUMS. Consume the upstream facade without
patches, renamed methods, convenience mirrors or new selector APIs.

Baseline: Vectis `9fb3265` plus the in-progress independent SDK layout and facade
work. Upstream release: `203ef044c5b83a628a32649d454e6444a9daf3a4`,
<https://github.com/sa6mwa/liblql/releases/tag/v0.4.0>.
Source archive SHA-256:
`fdc2add0fe6da66253924a80d49ac91b4af8cbb62848b18973283b8a2399464e`.
Exact selected SDK/source digests belong in `scripts/dependency-pins.tsv`.

The public `include/lql/lql.h` is byte-identical to 0.3.0. There is no public C
API or ABI change and no Vectis ABI change. Native scanner, selector, file and
spool workspaces move to receiver-budgeted allocation; stream buffers shrink.
The receiver's existing shared 8 MiB quota can now reject work formerly placed
on the stack. This is a failure-behavior change, not a new configurable policy.
It reduces worker-stack requirements; verify on default musl pthread stacks.
No new SDK or runtime service dependency is introduced.

`lua/lql/init.lua` remains an unchanged zero-policy alias. The released core gains
adjacent callback/lifetime headers, protected setup/result-copy and callback
cleanup, and explicit/idempotent `close` plus scoped `__close` for clients,
selectors, projections and mutations. Closing the client invalidates child
operations while retaining native owners until children are cleaned. Borrowed
cursors expire with their selector; callback values/writers expire at callback
return. Closing an active handle invalidates it immediately, fails the current
operation and defers native destruction until unwind. Ordinary nested use remains
supported. Do not replace this policy with a Vectis wrapper.

Mutations capture `file_value_open` and optional `file_value_close` once at parse
time. Each opened function-reader gets one protected cleanup attempt on success,
read failure, early stop or active close. First operation error wins. A cleanup
failure can be reported after output or inline rewrite committed because native
close has no return status; callers must not treat every error as rollback.
`nil, error` from readers is failure, not EOF. Limit overflows and option getter
failures are rejected after cleanup. True `stream_apply` stays incremental;
explicitly named spooled operations retain their materialized compatibility
contract, including the native lack of true-stream projection/mutation output.

Vectis integration remains direct: `vectis.libs.lql` is the upstream module;
there is no app-owned LQL service, thread or configuration receiver to invent.
Clients and callbacks are created on the execution thread after applicable fork
boundaries. Update the existing example and manual to use scoped close and the
actual JSON-pointer selector/projection grammar. Correct the source-rock's stale
claim that an adapted local core is required; native modules remain supplied by
the embedding Vectis executable or separately installed upstream facades.

## Verification contract

- Prove native version 0.4.0 through module and client methods, unchanged module
  identity, selected public include roots, direct released C compilation, and
  embedded Lua bytes. Manifest changes must invalidate upstream C object inputs
  even when archive source mtimes are older than existing objects.
- Normal and Linux-packed execution: explicit/scoped/exception cleanup, child
  invalidation, borrowed callback expiry, nested calls, active close, file-reader
  cleanup and first-error precedence, bounded streaming with a producer, limits,
  and structured failure. Exercise the updated bundled example.
- Inject allocation failures inside callbacks and during result adoption; run
  native memory checking to test the archived 0.3.0 leak path. Never reintroduce
  the archived mixed bugfix/API expansion patch.
- Compile/link current SDKs on all supported targets. Execute applicable Linux
  binaries, including a native public-API worker-stack probe on musl; Darwin
  compilation is not runtime proof on this Linux host.
- Verify source-rock, embedded manual, installed/relocated static and shared
  consumers and the relevant ordinary regression set. Existing checks remain
  valid for inputs unaffected by this isolated upgrade; record refreshed evidence.

Review conclusion: specification is sufficient for implementation. No C ABI,
app lifecycle, persistence, notification or new convenience-policy decision is
required. Pslog/MDF and deferred SSH/LZO/archive scope remain independent.

## Results

The host build compiles the selected released core directly. Native/client
version is 0.4.0; normal and packed behavior passes, including exception-scope
cleanup, child/cursor invalidation, active mutation close, captured file callbacks,
first-read-error precedence, nested use, callback expiry and bounded producer
limits. Default and 64 KiB worker-stack native probes pass on the host. The small-stack
probe uses the platform minimum when it exceeds 64 KiB (aarch64 GNU: 128 KiB).

The four-case allocation sweep covers 21 + 21 + 21 + 28 failure positions.
Strict Valgrind reports 82703 allocations/frees, zero bytes at exit and zero
errors, with no suppressions. This verifies the archived callback native-leak
path, not execution of arbitrary Lua cleanup under persistent allocator failure.

A remaining upstream limit was reproduced: four persistent-OOM positions leave
an opened reader without entry to its Lua closer (first: fault3, opened2/closed1).
Protected cleanup can fail before entering Lua. No local facade patch is made.
The ordinary no-fault/error/active-close controls require balanced cleanup; the
OOM test records the limitation and still requires native/Lua memory cleanup.
`LQL_REQUIRE_PAIRED_CLEANUP=1` enables strict reproduction. Draft report and fixture
are under ignored `stash/upstream-lua-handoffs-2026-10-28/lql-0.4.0/`.
This is not a release approval; downstream resource owners cannot assume their
Lua closer will execute when Lua cannot allocate a callback frame.

Final verification passed: 279/279 Vectis tests; all seven target builds; LQL and
combined facade behavior plus default/small worker-stack probes on six Linux
targets (ARM under QEMU); no-Kore execution; source-rock helper smoke; current
debug/Release embedded manuals; GNU installed and relocated static/shared CMake,
C++ and pkg-config consumers with explicit native version 0.4.0 calls. Darwin is
compile/link evidence only. Exact commands, limits and current evidence are in
`useful-dependency-lua-implementation-review.md`; logs are under
`build/useful-lua-facades/final-*` and this upgrade's scratch directory.

The user-required final sequence was a complete implementation pass followed by
verification. No further full matrix was run for the small-stack test correction.
The remaining closer-entry limit is reported, and is not silently repaired or
treated as guaranteed execution of arbitrary Lua code under persistent OOM.
