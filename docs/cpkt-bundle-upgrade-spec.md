# Independent cpkt bundle and lifecycle migration

Status: original migration contract, reviewed 2026-10-27.
The upstream facade policies and LQL pin in its historical input table are
superseded by `upstream-lua-facades.md`, `useful-dependency-lua-spec.md` and the
user-authorized `liblql-0.4.0-upgrade.md`. No withdrawn provider adaptation is
retained or reintroduced. Current lockdc/cpkt pins and verification supersede
this historical input table in [the 0.24.0 upgrade](liblockdc-0.24.0-upgrade.md).

Original review: This supersedes the
monolithic c.pkt.systems dependency layout and the previous hosted Darwin gate.
Implementation and verification are not yet complete. Preserve the existing
uncommitted facade/streaming work on the named `init` development branch.
The liblockdc 0.23.0 stateful-renewal fix is verified. cpkt 0.6.0 supplies the
previously missing trace observer; its integration and remaining final gates
are recorded in the current [surface audit](lua-upgrade-seam-audit.md).

## Selected published inputs

GitHub release metadata was checked for every directly pinned dependency:

| Dependency | Current pin | Selected release |
| --- | --- | --- |
| c.pkt.systems | 0.11.0 | Replaced by cpkt 0.6.0, cpktdb 0.3.1 and cpktmisc 0.2.1 |
| liblockdc | 0.18.0 | 0.23.0, core ABI 6, plus its separate PostgreSQL provider ABI 0 |
| libmdf | 0.14.0 | 0.16.0, ABI 6; review intermediate 0.15 behavior too |
| softline | 0.9.0 | 0.13.0, ABI 2; inspect intermediate changes |
| lonejson | 0.45.0 | Latest remains 0.45.0 |
| libpslog | 0.12.0 | Latest remains 0.12.0; retain verified local owner repair |
| liblql | 0.3.0 | Latest remains 0.3.0; retain public-surface adaptation |
| cai | 0.6.0 | Latest remains 0.6.0; libcai_app/Smith migration deferred |
| libpid0 | 0.5.0 | Latest remains 0.5.0 |

Every selected archive, including Lua sources and the PostgreSQL provider, gets
an exact URL, target and published SHA-256 pin. Preserve the seven declared
Linux/Darwin targets; prepare/debug the native target first. The SDK component
versions come from validated provider manifests, not copied monolithic defaults.
Core 0.6.0 retains the ABI required by the optional providers' 0.5.1 baseline;
their exact producer pins remain provenance, not consumer byte equality.

### cpkt 0.6.0 trace integration contract

Use the public C89 `cpkt_lua_runtime_set_trace_callback` between host calls.
The callback prints bounded source identifiers and positive one-based lines
to stderr, never enters a Lua state and never retains borrowed event fields.
Control bytes become `?`; identifiers are limited to 160 bytes with an ellipsis,
retaining the filename suffix for long file paths. Runtime ownership and
instruction-limit enforcement remain in cpkt. No raw hook installer or private
runtime layout is retained. This is embedding policy, not a Lua-callable hook.

`vectis -x script.lua [args]` preserves script arguments, stdout and error status.
`packed-app --vectis -x [args]` traces its embedded script through the same
observer. Plain packed `-x` remains an application argument. Trace events
include coroutines, close handlers and Lua finalizers; native C execution and
stripped chunks have no line events. An unrestricted runtime remains
unrestricted; tracing does not invent a new instruction-budget CLI option.

Prove normal/packed output and arguments, default-off behavior, error status,
coroutine and finalizer events, and bounded control-byte source names. A small
public C89 runtime test must also prove removal/replacement, guarded callback
reentry and terminal budgets with tracing enabled/disabled, including protected
calls and coroutine yields. No delay-based probes or full-value collection.

## Acquisition, discovery and ownership

Keep every SDK installation in its own prefix beneath the checkout-owned
`.cache/deps/<selection>/`. Never merge cpkt, cpktdb or cpktmisc payloads. Keep
lockdc core and its PostgreSQL provider separate too. Use standard CMake prefix
and find-root lists and the packages' imported targets. Preserve public cpkt
header names. Lua source archives have separate owned source directories.

Installed archives keep the same independent prefixes under `deps/<name>`.
Only Vectis-owned libraries receive relative loader paths to those peers;
provider payloads, notices and validation inventories remain unchanged.
ELF uses transitive relative RPATH and Darwin uses loader-relative LC_RPATH,
so moving the complete SDK does not require rewriting dependency bytes.

Use the declared shared `CPKT_DEPENDENCY_CACHE`, otherwise
`${XDG_CACHE_HOME:-$HOME/.cache}/cpkt/deps`; toolchains use the corresponding
`cpkt/toolchains`. Verified archive hits make no acquisition network requests.
Use per-digest locks, bounded HTTPS retries, verified temporary downloads and
atomic publication. Changed pins invalidate only their owned extraction; do
not erase unrelated dependency prefixes on every preparation. Never consume
another checkout's build, source or extracted cache state.

Update the exact vendored lifecycle helper set as one unit. Outer Make/Bash
entrypoints provision tools; CMake/try_compile only discover prepared tools.
Reject unavailable inputs with the preparation command. Full clean removes
build/, dist/ and local .cache/ including hidden content, preserving global
verified archives/toolchains and manually supplied Apple SDKs.

## Public feature integration

Compare every upgraded public header, Lua source, examples, ownership/callback/
error/streaming behavior and transitive package metadata against the old pinned
artifacts. Rebase local MDF adaptations semantically; do not accept new hashes
or drop existing typed/streaming mappings just to make a patch apply.

Expose lockdc collections through app-owned C receiver shells and Lua convenience
facades while keeping direct lockdc access. C factories share the app's native
Pouch client. Lua has one lazy client per app storage scope, capturing the app's
Pouch/logging policy after the applicable fork boundary; all its collections
and coordinators share that client and the app lifecycle. The released public
interop only exports a Lua client view and cannot import the app's C client
into Lua. Do not reinterpret native userdata to fabricate that reverse bridge.
There are no per-collection clients, workers or maintenance loops. Preserve
immutable JSON bodies, permanent uint64 positions, append identity/status,
deletion/drop/retention and native resume/window/checkpoint semantics. Deliver
raw JSONL incrementally with separate metadata, bounded chunks and no body
reconstruction or whole-result collection. Remote collections remain native
UNSUPPORTED. Collections do not enlist in XA; do not imply they do.

Link the separate official `lockdc::postgres_static/shared` driver and preload
the released `lockdc.postgres` Lua entrypoint. Add app-owned managed XA C/Lua
integration using public coordinator/resource/transaction/operation/borrow
receivers. Preserve resource identity, pending-versus-terminal status, native
wait descriptors, owner renewal, immutable decision/recovery semantics and exact
SQL branch borrowing. Parameterized application SQL uses cpktdb's public C89
facade. Never replay SQL during recovery, hold an SQL borrow across prepare, or
materialize thread-bearing resources before the owning fork boundary. Extend
the supervisor/outbox notification path where receipts are produced.

Also integrate new state retention and mapped-save options, Softline handoff/
draft/editing behavior and MDF C89 HTML branding where Vectis owns the workflow.
Update public C ABI from the latest released baseline if required; provider ABI
changes alone do not determine Vectis's ABI. Preserve current Smith/CAI APIs and
pin pending a released libcai_app; do not build against an unreleased branch or
invent an agent runtime. Record any compatibility blocker for the retained CAI.

Mapped C state-save/update helpers take optional `const lc_update_opts *`
immediately before the error argument, matching upstream's clean cutover.
Initialize options with the public initializer; NULL preserves existing CAS and
retention defaults. Migrate all callers rather than adding parallel helpers.
The checkout and authoritative remote expose no release tags, and no released
Vectis artifact baseline exists here; the initial, unpublished SDK remains ABI 0.
This is not a rule to derive future ABI versions from development commits.

## Lifecycle cutover and evidence

Make/Bash/CMake/CTest remain the only lifecycle orchestration. Replace pipeline
Python controllers with small native declarations/tests without porting an
alternate scheduler. Python remains restricted to source/data generators and
small data fixtures. Normal native builds publish the compilation database
used by ordinary clangd discovery, including dependency/generated headers.

The proxy benchmark migration reuses the existing compiled Go
HTTP/2 fixture for bounded HTTP, SSE and WebSocket measurements and a single
owned Vectis child. Preserve direct/proxied comparisons, actual protocol and
payload assertions, latency distributions, resource sampling of that owned
process tree, and opt-in active-stream shutdown soak. No process-wide search,
pipeline scheduler, external Go module, or Python assertion driver is needed.
Go is an existing host-fixture prerequisite; CMake declares its one native
build output and source inputs. Keep HTTP/TLS smoke cases small. Lists use
comma-separated native Go flags in the migrated benchmark CLI; update examples.
Metric storage/recovery fixtures keep their Lua application work, with compiled
tests and CMake owning timing, assertions and execution. Benchmark pacing is
intentional opt-in load, separate from the zero-delay failure-test contract.

Remove hosted Darwin runs as required release gates/artifacts. Optional hosted
diagnostics use manual dispatch only and do not authorize pushes. Darwin release
build/link/Mach-O/package evidence comes from local prepared osxcross. Preserve
the local signed-default-branch/lightweight-tag release boundary; this upgrade
does not authorize a release, push, tag, workstation installation or SDK removal.

During implementation run focused deterministic debug checks: acquisition
miss/hit/corruption/request counts, independent provider discovery, ownership,
collection ordering/resume/partial failure, XA success/abort/recovery/cleanup,
normal/packed Lua, source-rock and static/shared closure as applicable. Keep
fault tests tiny and signal-driven; live PostgreSQL uses Podman Kube services.
Complete implementation before the full verification/matrix, then review and
repair using focused debug proof. Update manuals, coverage matrix, dependency
inventory, source package inputs and actual install/package metadata in the
same task. Record unavailable live/platform proof rather than claiming it.

Review: the selected releases and provider compatibility requirements were
checked against published artifacts/docs. The architecture uses ordinary native
package discovery and upstream resource ownership; no new DSL, dependency graph,
runtime plugin loader, polling substitute or hidden buffering is introduced.
