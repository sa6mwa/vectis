# Vectis Lua upgrade seam audit

## liblockdc 0.24.0 and retained LQL 0.4.0, 2026-10-29

[Upgrade specification and evidence](liblockdc-0.24.0-upgrade.md) supersede the
0.23.0 core ABI/pin status below. Core ABI is 7, PostgreSQL provider ABI 0;
cpkt is 0.6.1 and LQL remains 0.4.0. Released Lua sources are consumed unchanged.
Vectis app storage owns incremental append writers and closes them on stop;
count limits and parent-retention directives pass through to upstream. Explicit
compression changes convert synchronously during open. Verification is recorded
against this upgrade, independently of historical candidate results below.

## Useful dependency facade goal, 2026-10-28

The new [reviewed specification](useful-dependency-lua-spec.md) replaces exhaustive
parity with useful public APIs of dependencies lacking upstream Lua facades.
[Current implementation evidence](useful-dependency-lua-progress.md) distinguishes
delivered SQLite/PostgreSQL app ownership, PDF, PNG, selected compression, reusable
OpenSSL keys/AES-GCM, XML DOM/XPath/SAX and curl sessions/multi from pending
components. LZO is deferred;
the selected archive target's transitive LZO linkage is an upstream blocker.
Existing upstream Lua facades remain unchanged and await their own releases.

## Upstream facade restoration, 2026-10-28

This section supersedes the local pslog/LQL/MDF implementation claims below.
The user clarified that existing upstream Lua facades are authoritative and
must not be extended locally. The exhaustive API scope is superseded: new
dependency bindings target useful public APIs with concrete application use cases.
Vectis-owned convenience APIs and its bindings for libraries without upstream
Lua facades are a separate boundary and are preserved.

Before removal, all three adaptations were saved under ignored
`stash/upstream-lua-handoffs-2026-10-28/`, with a draft review/PR per project,
aggregate upstream-layout patches, exact released baselines, original patches,
licenses, regression fixtures, manuals, specifications and earlier bug reports.
The 117 archived files pass SHA-256 verification. Applying the three patches
with zero fuzz reproduces the last compiled local cores/modules byte-for-byte.
Optional/withdrawn API additions are distinguished from bugfix candidates; no
PR has been submitted and `withf` is not an upstream requirement.

Production CMake now selects released SDK binding sources and Lua modules
directly. Local patch staging, added provider headers and adaptation-only test
targets are removed. Vectis logging and terminal methods remain intact; terminal
string rendering uses the upstream `render` name, and the logging example uses
existing `with`. Native app/service qualification still uses the official C
interop. Zlib/buffer/digest/storage/PostgreSQL and other owned work is preserved.

See [the current boundary and exact convenience gap assessment](upstream-lua-facades.md).
No current convenience feature requires the withdrawn mirrors. The concrete
remaining concerns are the upstream allocation-failure cleanup reports; outer
convenience wrappers cannot repair abandoned native callback frames. Historical
local fault-test guarantees must not be attributed to the restored releases.

Restoration verification:

- `cmake --build build/debug --target vectis_bin -j2` passed using released
  binding source paths; MDF no longer includes Vectis bridge/header directories.
- The affected CTest selection passed **19/19 in 15.37s**, including actual
  compile-input/embedded-byte checks, normal/packed facades, Lua smoke, native
  logger handoff and app cleanup, MDF regressions, and logging/render/pipeline
  examples. The new source-export check is repeatable with an owned forced export.
- Lua-only rock smoke passed with diagnostic version 0.0.0 and its entire tree
  under `build/upstream-facade-removal/rock`; this is not a native module install
  or a release-package gate.
- Repository formatting, shell syntax and whitespace checks passed. Five
  current manuals match their actual embedded bytes. The 117-file handoff
  checksum manifest was reverified after removal.

Logs are under `build/upstream-facade-removal/`, especially `final-build.log`,
`final-tests.log`, `lua-rock.log`, `format.log`, `manual-readback.log` and
`archive-final-verification.log`. Unrelated dirty migration work is preserved;
the restoration does not claim full migration/platform/service/release approval.

Earlier component test counts and formatted-emission requirements are historical,
not current feature or release commitments.

## Active split-bundle upgrade, 2026-10-27

### Pslog formatted derivation and shared output lifetime, 2026-10-28

The previous goal turn made verified progress on owned/observed outputs. This
continuation implements `logger:withf` and `pslog.withf` through public typed
fields, not a synthetic va_list or private native layout. It maps all native
verbs and l-prefixed aliases, exact int/unsigned-int/long/unsigned-long widths,
nil/C strings, typed pointers, boolean/int and entry-errno `%m` capture. The
32-field native grammar limit and invalid-format nil result are preserved.
Argument type/arity/domain failures occur before native acquisition; derived
children retain pointer owners independently of parent cleanup. No new Vectis
wrapper is needed: `vectis.log` already returns these native receivers.

Strict C comparison found that public structured emission is not equivalent to
native variadic emission for valid long colored keys. Custom palette strings
make the difference visible. The attempted emission adapter was removed from
product registration and the independent public-C89 probe reproduced the gap
without Lua/Vectis objects. The [withdrawn requirement and reproducer](upstream-lua-facades.md)
record the exact public resolution needed; no external report has been sent.
This is not a completed emission mapping and does not stop other API work.

Review also found and reproduced premature typed-sink invalidation after root
destruction with a surviving derived logger. The adapter now counts the actual
native logger group, including deferred cleanup/adoption rollback. Sink closed
state follows last native destruction, while explicit owning close retains its
native role. Public C descriptor probes confirm that child close leaves the
shared sink open. Regression tests cover destroyed roots, last derived shells,
live borrows, collection order and exactly-once callback cleanup. Derived
finalizers resolve retained parent/configuration edges to restore weak callback
lookups even after root destruction. Structured free-function nil messages now
preserve native NULL-message behavior; a C byte comparison covers both forms.

Current focused evidence on branch `init` over
`61e84b97f3335a24bcff1b683e0637524816a9d6`:

- `cmake --build build/debug --target vectis_bin vectis_unit_pslog_owner_oom vectis_unit_pslog_withf -j2` passed.
- `ctest --test-dir build/debug --output-on-failure --no-tests=error -j1 -R 'pslog|logger_native_sink|app_log_cleanup|cli_source_integrity|preload_artifact_contract'`: **16/16, 5.79s**, including normal/packed `withf` and output behavior.
- `vectis_unit_pslog_withf` compares output byte-for-byte with native C across **32 configurations**: console/JSON, color, verbose fields, builtin/custom palettes and both nonfinite-float policies. It exercises every verb/alias, NULL strings, escaping/non-ASCII content, exact numeric boundaries, nil cloning, 32/33-field limits, long/unicode keys and static-field ownership. Emission chunks remain bounded at 32 bytes. These are derivation results, not formatted-emission evidence.
- Constructor/derivation OOM sweeps now include `withf`: seven families at sixteen deterministic allocation positions each, all observing both failure and success. Existing seven output families at 64 positions and native FILE adoption at 32 positions remain covered.
- Strict Memcheck, no suppressions, definite/indirect/possible losses as errors: ownership unit **473,748 allocations/frees**, native `withf` reference **35,123**, actual `withf` CLI **2,328**, actual expanded output CLI **3,110**. All four have zero live bytes/blocks and zero errors.
- Exact embedded-manual readback matches the current `manual/lua-pslog.md`. The standalone C89 requirement snippet matches its compiled probe source byte-for-byte.
- The updated `examples/lua/logging.lua` demonstrates `withf` through a `vectis.log` receiver; `vectis_example_lua_logging` passes. Repository formatting and whitespace checks pass. The full dirty worktree remains the coherent unfinished migration with no staged content; this adaptation depends on that base and is not an independent verified commit.

Evidence: `build/pslog-resume/withf-final-build.log`, `withf-final-tests.log`,
`withf-final-memcheck-{owner,reference,lua,outputs}.log`,
`withf-manual-readback.log` and `typed-format-gap.log`.
The coverage matrix, inventory, manuals, binding notes and component contract
track this scope. These additions use the adapted runner's native module; the
installed stock pslog rock does not expose `withf`. Source-rock availability and
consumer closure remain parent integration requirements, not inferred from CLI
proof. Retained Lua-facing interop and other public-API families
remain active. Full native/platform/service/package gates await the whole
surface. No commit/push/tag/release has run; the existing coherent migration
remains uncommitted. SSH execution, the upstream libssh2 finding and the Smith
migration keep their previously stated deferrals.

### Pslog owned/observed-output continuation, 2026-10-28

The branch remains `init` over `61e84b97f3335a24bcff1b683e0637524816a9d6`.
This extends the earlier public-value checkpoint below. The existing broader
uncommitted upgrade is preserved; no push, release or commit is claimed.

The provenance-checked binding now implements `output_from_fp`,
`output_init_file`, `output_new`, `output_destroy`, `observed_output_init` and
`observed_output_stats_get`. Typed records expose native write/close/isatty
status, exact accepted counts, captured userdata/callback fields and owned,
moved and closed state. Native FILE hooks remain native hooks. Owned FILE
adoption invalidates the original Lua file shell; borrowed FILE ownership stays
with its caller. Observed records expose actual native cumulative counters,
failure metadata and synchronous notification, with no log-line collection.

Shared heap states and collectable Lua edges implement ownership transfer,
explicit borrows, leased target views, derived logger retention and deferred
cleanup. Explicit owning close rejects live borrows. Protected scopes cover
constructor isatty, write, failure notification, close and their argument/result
allocations. First exceptions, including false values, survive later cleanup
errors. A custom Lua close callback can fail before entry under persistent OOM;
native close/free is still attempted once, with no unsafe automatic retry.
Separate native-descriptor probes prove builtin FILE rollback closes its
descriptor after successful acquisition followed by Lua adoption failure.

Review found that weak callback graph lookups can disappear before finalizers
mark retained uservalue graphs. Finalizers now restore those identities from
their owner edges inside a protected frame before native cleanup. Tests prove
exactly-once close for unreachable callback/coroutine graphs and deferred owners
sharing borrowed loggers. Cleanup preserves a preparation error while removing
temporary metadata from its Lua stack. The native interop ABI remains 1.

Vectis app seed handoff accepts native FILE/observer outputs and rejects custom
Lua hooks. `vectis.log.configure` now performs the same check when the adapted
runtime's predicate is available; the installed stock source-rock module has
no typed-output constructors and retains its existing file/path/function rules.
Direct pslog and `vectis.log.new` continue to support Lua callbacks. The manuals,
coverage matrix, inventory and component specification describe this delivered
surface. Full source-rock/platform/service gates await the complete parent
surface; module availability is not claimed as final packaging evidence.

Focused Debug evidence against the current effective sources:

- `cmake --build build/debug --target vectis_bin vectis_unit_pslog_owner_oom -j2` passed.
- `ctest --test-dir build/debug --output-on-failure --no-tests=error -j1 -R 'pslog|logger_native_sink|app_log_cleanup|cli_source_integrity|preload_artifact_contract'`: **14/14, 4.11s**, including normal and Linux packed execution.
- Existing constructor/derivation faults remain covered. Seven additional output acquisition/borrow/logger families run at 64 deterministic allocation positions each; native FILE-to-logger adoption runs at 32 positions. Every family observes failures and successes. Public native callback entry is tested with allocator failure for legacy and typed observed outputs; no Lua error escapes their C frame.
- Strict Memcheck, no suppressions, definite/indirect/possible losses as errors: expanded native unit **460,279 allocations/frees**, actual output CLI fixture **2,933 allocations/frees**; both have zero live bytes/blocks and zero errors.
- Exact embedded-manual readback matches `manual/lua-pslog.md`.
- `bash scripts/check_format.sh` and `git diff --check` pass. The full dirty
  worktree was inspected: the prior upgrade/lifecycle/native/Lua/documentation
  delta remains task-owned unfinished work, with no staged content. This output
  adaptation depends on that migration and is not an independent verified commit.

Logs: `build/pslog-resume/outputs-final-build.log`, `outputs-final-tests.log`,
`outputs-final-memcheck-unit.log`, `outputs-final-memcheck-lua.log` and
`outputs-manual-readback.log`. This is historical component evidence. The
continuation above implements `withf`; formatted emission and retained Lua-facing
interop tokens remain unfinished, as do the
other open families in the public API ledger. SSH remote execution and the
upstream libssh2 finding remain parked under the user's stated constraints.

### Recovered goal and pslog public-value continuation, 2026-10-28

The active goal was recovered from the saved conversation and the retained
build logs. The branch remains `init` over
`61e84b97f3335a24bcff1b683e0637524816a9d6`, with the earlier uncommitted overhaul
preserved. The cpkt 0.6.0 full native log records 265/265 passing checks; the
old trace blocker is resolved. The broader public-API ledger still contains
unfinished components, so that suite does not establish full surface coverage.
SSH remote execution is indefinitely parked on the future roadmap. The known
Historical status at cpkt 0.6.0: the libssh2 memory finding awaited a new release; no further
investigation is part of this continuation.

The pslog 0.12.0 local adaptation now exposes native constants and fresh defaults,
all 32 built-in palettes and all sixteen custom palette fields, the trust
predicate, all twelve typed field constructor families and their binary views,
immutable metadata/union snapshots, structured free-function/view emissions,
receiver view methods and native-output qualification. Unsigned domains use
exact integer/decimal representations; native long/int widths are checked.
Native headers and C/Lua interop ABI 1 are unchanged. Lua root exports use a
second source-hash-checked, zero-fuzz delta; the actual compiled source is the
generated adaptation and embedded source bytes use its adapted init module.

Configuration captures borrowed values before native acquisition. Protected
output frames include argument setup, chunk allocation and result validation;
native-style code/written results remain visible to the native emitter. Lua
exceptions, including false error values, propagate after native unwind.
Whole-operation guards protect field conversion and derivation as well as
emission. Weak callback lookups and collectable owner edges avoid registry-root
cycles. Preallocated cleanup nodes defer sibling native destruction discovered
by GC until unwind. Consumed field owners survive callback mutation of input
arrays/maps; derived pointer fields retain their typed resources independently
of later source-array mutation. Direct Lua emissions use the invoking coroutine,
restoring the pinned creator context for public C interop. Native-only sink
qualification continues to reject Lua callbacks for foreign-thread use.

Focused Debug verification:

- `cmake --build build/debug --target vectis_bin vectis_unit_pslog_owner_oom -j2`.
- `ctest --test-dir build/debug --output-on-failure --no-tests=error -j1 -R 'pslog|logger_native_sink|app_log_cleanup|cli_source_integrity|preload_artifact_contract'`: **13/13, 3.84s**. Includes normal/packed behavior, six constructor/derivation families at sixteen deterministic allocation positions, whole-operation/metamethod/GC guards, input mutation, defaults/palette/domain checks and app seed cleanup.
- A native C reference compares all twenty-four field forms and a NULL/zero-length message with Lua output byte-for-byte. Native subprocess references compare explicit fatal/panic and generic level-aware behavior; no delays or process scanning.
- Strict Memcheck, definite/indirect/possible losses as errors and no suppressions: the native unit frees **74,004/74,004** allocations; the actual CLI fixture frees **3,732/3,732**. Both have zero live bytes/blocks and zero errors. The native reference fixture stays in the owned build working directory.

Review repaired the generated-source selection, explicit private-header include
dependency, field-conversion reentry window, temporary receiver rooting and
consumed/prefix field ownership before this passing evidence. Startup warm-up
keeps allocation sweeps focused on acquisition/adoption rather than option-name
interning; every family observes both failures and successes. Scratch evidence
is under `build/pslog-resume/`, especially `final-build.log`, `final-tests.log`,
`memcheck-unit.log` and `memcheck-lua.log`.

This is historical component evidence, not full API or release approval. At this
checkpoint owned/observed outputs were unfinished; the continuation above
supersedes that gap. Formatted log families, retained Lua-facing interop tokens
and other families in the public-API ledger remain active.
No full cross build, release, push or commit has run for this continuation.

### cpkt 0.6.0 observer follow-up, 2026-10-28

The active core pin is now 0.6.0. This supersedes the earlier 0.5.2 trace
blocker and identities below; the records remain scoped historical evidence.
Compared `v0.5.2...v0.6.0`: one commit,
`4ae844f7d9687cff7f37ada0360c19ac786321dc`, adds the runtime observer, callback
guards, documentation and tests. The installed public-header tree differs only
in `cpkt/lua_runtime.h`; provider ABI compatibility is retained. All seven SDK
digests agree with the published checksum inventory, SHA-256
`23b191c67670e695a57f3b9d7c45521df1ebf5e0dff332d29bae395b670eaaed`.

Vectis removes its raw Lua-hook module and installs the public C89 callback
between host calls. The callback writes bounded metadata to stderr, never
enters Lua and never retains borrowed bytes. Packed `--vectis -x` uses the same
observer while preserving application arguments; plain packed `-x` remains an
application argument. The manual, coverage matrix and upgrade contract describe
the interface. The original CLI admin/trace regression, normal trace, packed
trace and public C89 observer checks pass 4/4 in 2.29s. The C89 check proves
removal/replacement, callback reentry guards, coroutine/close/finalizer events
and terminal instruction limits with and without tracing, using no delays.
The self-contained CR is marked resolved; its historical raw-hook probe remains
unchanged because that mechanism is deliberately unsupported.

The API blocker is resolved. Remaining full native/platform/package and
instrumentation gates are in progress; the older 261/262 and SDK results do not
verify these new bytes. Focused evidence:
`build/bundle-upgrade/cpkt-0.6.0-trace-focused.log`.

### liblockdc 0.23.0 follow-up, 2026-10-28

The selected core and separate PostgreSQL provider are now 0.23.0. All fifteen
target/Lua pins agree with the published checksum inventory, whose SHA-256 is
`1a6f1ae0f6f8c2fbf452d2b14904e269dfe3b447baceda627f98b6c19ecef825`.
Compared release commit `296e6d819e1961b9828cc1f4171623de431d1b96` with
0.22.0: public signatures/layouts and PostgreSQL header are unchanged, core ABI
remains 6, provider ABI 0, and native Lua bindings are byte-identical. The Lua
facade adds renewal comments. Pouch now preserves the higher committed/lease
version floor while retaining staged-version and fencing behavior.

The exact three-case report reproducer now completes read, update and
renew-update with count 1 and no error. Vectis removes the stateful restriction.
The direct public-facade regression passes eleven control/renewal/outcome cases
normal/packed in 2.20s, checking repeated renewal, writes before/after renewal,
complete/retry/dead-letter and checkpoint policy. The Vectis workflow adapter
also passes repeated-renewal deferred decisions normal/packed. Completion
retires its private sidecar; retry/dead-letter retain it. Automatic renewal
during a blocked Lua handler remains unsupported. The self-contained CR under
stash is marked resolved and retains the original historical reproduction.

Ordinary lease write/renew/write also passes through the C and normal/packed
Lua surfaces, checking that renewal preserves the committed body version.
Actual CLI Memcheck of all eleven direct cases reports 112773 balanced
allocations/frees, zero live bytes/blocks and zero errors without suppressions.
These focused 0.23.0 checks do not replace the final native/platform/package
verification of the whole candidate.

This upgrade exposed a Vectis build dependency defect: archive sources with
older mtimes replaced paths but the byte blob stayed stale while metadata
changed. The blob now depends on its generated metadata header as well as the
source paths. A 0.07s CLI check exports every actual embedded Lua source and
compares its SHA-256 with the binary's inventory. The new lockdc exported bytes
also match the acquired 0.23.0 source exactly. Generated Kore/header/input-list
files now preserve mtimes when unchanged; reconfigure plus CLI build proves
`ninja: no work to do` without sleeps or another cache framework.

### Legacy-prefix review repair, 2026-10-28

A broader compilation-command audit found test/example private include flags
that still preferred the old merged SDK over imported targets. The old payload
contained MDF 0.14 and Softline 0.9 headers. Those targets' earlier successes
cannot establish current-header compatibility. Their legacy flags are removed;
the old payload is retired after replacement SDK preparation, with a fixture
proving that unrelated generated state remains. The normal compilation database
now contains no merged `host-debug/include` path.

This exposed two stale Smith test calls to Softline's old two-argument queue
append function; they now explicitly append approved entries with the current
four-argument public contract. The cache cutover, Smith behavior, MDF allocator
and LQL OOM checks pass 6/6 in 2.59s against current headers. Standalone Kore and
kodev use separate SDK prefixes and the prepared native compiler/runtime;
clean-clone patch application/build and actual `kore -v` pass through patch
0042. Verification no longer resets the product's generated Kore checkout.
The interrupted broad suite is not a completed final gate.

### Current native continuation and trace blocker, 2026-10-28

With current headers, all native targets and formatting pass. Tests 1–71
pass, including the real PostgreSQL XA normal/packed fixture and the
20,000-update metrics recovery gate (129.14s). The installed Lua preload probe
still expected the old ambiguous hash/compression names; it now verifies the
explicit materialized names, streamed SHA-256 and the new storage/PG/workflow
module aliases, and passes in isolation. Tests 73–107 pass too, including the
two-worker XA receipt wake and stateful renewal regressions.

Test 108 exposes an upstream runtime integration gap: `vectis -x` installs a
line hook through a public module opener, but cpkt 0.5.2's strict runtime clears
or replaces that hook at host/coroutine boundaries. Its public runtime API has
no trace observer compatible with instruction-limit enforcement. A standalone
public C89 reproducer linked to the published SDK reports
`runtime_status=0 trace_lines=0`. The self-contained upstream request is
`stash/cpkt-lua-runtime-trace-observer-cr.md`. Do not bypass budget enforcement,
access private runtime state, silently remove tracing or mark the suite/release
gate complete. Independent verification may continue while this is unresolved.

Removed unused duplicate expected-version constants; actual dependency identity
continues to come from validated pins/manifests and exact package selection.
The source-archive smoke now builds inside the extracted source's own `build/`
tree, as required by the new ownership boundary. Structural archive readback
passes; its full extracted build remains pending.

### Native/package review continuation, 2026-10-28

All 262 currently registered native cases have now been exercised across scoped
runs. The retained CLI admin/trace case is the one unresolved failure: its
credential checks complete before `-x` produces no trace. All other current
cases pass after the focused preload and reconfigure-harness repairs. This is
not a passing aggregate gate. Real external AI providers, loaded models,
devices and remote services remain distinct opt-in coverage.

The reconfigure harness must preserve the host XDG cache identity when it
configures prepared native tools; product sandbox paths are inappropriate
there. Its corrected configure/remove/reconfigure/identity checks and the
actual foreign-mapping rejection/gzip tail pass 3/3 in 1.90s. Reconfiguring and
rebuilding unchanged Debug code reports `ninja: no work to do`.

Retired the three source-text service/facade audits and their CTest presence
case. Their Make targets now execute observable CTest contracts. The actual
standard lifecycle target, target-tool discovery, Darwin linker routing and
shared resolver/AFL fixtures pass. Darwin availability uses the same prepared
resolver as CMake, handles discovered Darwin-25 suffixes and invokes the
selected linker with `--ld-path`; its synthetic test no longer borrows the
workstation's MIG or expects CMake to change PATH. This is tool-route evidence,
not a complete Darwin product build.

A fresh native GNU Release build produces both libraries and the static CLI.
Exact shared exports pass. C89/C++ consumers, pkg-config and 36 installed
examples pass against the static SDK. The complete SDK was then moved to a
different prefix: shared C/C++ consumers, pkg-config, the private-symbol
negative sentinel, all 36 shared examples and actual loader mappings pass.
A minimal shared probe gives the executable only Vectis/libc runtime paths,
so broad consumer peer paths cannot mask the library's relative dependency
closure. The relocated CLI's current Lua preload/behavior probe passes too.

Shared-SDK verification initially detected the host resolver DSO mixed into
the pinned runtime: provider RUNPATHs prevent ancestor RPATH lookup for that
system peer. Local shared consumer/example probes now eagerly link the pinned
`libresolv.so.2`; their actual mapping check passes. Provider payloads and
shipped library policy are unchanged by that private test-runtime repair.
Package architecture/OS/libc metadata is now normalized even when the target
ID was explicit. Provisional native SDK bytes are not release artifacts.

Source-rock archive/spec/source-rock validation, installation and the separate
source-rock smoke pass. This checks source packaging/composition (the smoke
uses dependency doubles); real native behavior is proved separately above.
Lua build/output paths use the common physical ownership validator, including
symlinked absent destinations. The fast cache fixture proves rejection without
creating foreign Lua build/artifact state and still makes exactly seven local
archive requests. Source packages include the ownership/version helpers used
by their supplied build scripts. The complete Vectis source archive also passes
extraction, root/version and exact manifest readback with
`VECTIS_SOURCE_SMOKE_BUILD=0`; this deliberately does not claim a fresh source
build or a passing aggregate suite. Final lifecycle fixtures and formatting
pass after these helper changes, and the Debug rebuild performs no work.

Current strict Memcheck checks of the native PostgreSQL transport-fault unit,
digest unit and actual Lua GC regression each report balanced allocations,
zero live blocks/bytes and zero errors without suppressions. The eleven-case
real CLI stateful-renewal memory result above remains separate evidence.

Evidence is under ignored `build/bundle-upgrade/`: `final-native-tests*.log`,
`preload-repair.log`, `native-tail-focused.log`, `native-examples-final.log`,
`final-lifecycle.log`, `final-tool-gates.log`, `native-sdk-static.log`,
`native-sdk-shared-relocated.log`, `native-sdk-preloads.log`,
`source-rock-current.log`, `source-rock-smoke.log`,
`source-output-ownership.log`, `source-archive-structure.log`,
`final-lifecycle-fixtures.log`, `final-format.log`, `final-debug-noop.log` and
`final-memory/`. The first lifecycle log ends
at the old Darwin fixture failure; its lifecycle portion passed, and the
affected target/tool tail passes in `final-tool-gates.log` after repair.

The working tree remains based on `61e84b97f3335a24bcff1b683e0637524816a9d6` on
`init`, with the prior uncommitted facade work preserved. There is no completed
release candidate or clean semantic release gate. Full cross-platform builds,
Darwin product proof, full instrumentation/source-build gates and final
whole-candidate review remain pending the public cpkt trace-observer contract.
No commit or publication has been made; provisional SDK checks do not approve
the unfinished overhaul for release.

### Earlier 0.22.0 component evidence

The component checks below predate the 0.23.0 pin. They establish the earlier
implementation slices; final whole-candidate checks must use current pins.

Focused continuation, 2026-10-28: retained PostgreSQL commit recovery passes
normal/packed after coordinator reopening, twice without re-registering the
resource or replaying application SQL. The rootless service test preserves
the host session's Podman XDG paths; the generic process sandbox must not
redirect workstation network/runtime state. Database payloads remain owned.

The independent cache/clean fixtures prove digest repair, zero-request hits,
separate extraction, preserved file modes and rejection of borrowed checkout
paths, including symlinked absent destinations. A complete Kore patch-stack
reapplication through patch 0041 passes. A forced one-worker replacement
passes strict Memcheck in the original owner, parent and replacement child,
with zero errors/lost bytes and no suppressions; the killed child cannot have
a terminal memory summary. The two inherited static pool names are reclaimed
at their actual owners, without changing generic pool initialization contracts.

Native HTTP/MQTT/WebSocket fixtures replace their Python controllers and
preserve wire assertions. HTTP upload verifies 172 requests in 0.38s.
WebSocket logging enabled/disabled passes normal/packed in 1.19s. The optional
SSH fixture is now a data server only; Bash/Lua drive all eleven wire cases,
which pass with test-only Paramiko 4.0.0. Python remains only in data serving
and environment/source generation. Compiled proxy HTTP/TLS/H2, storage and
short recovery checks pass; the 20,000-update recovery gate and remaining full
platform/package checks are still pending. Ordinary clangd discovery for
`src/vectis_pg.c` reports zero errors with the normal native build database.
Public storage-header parsing also passes through ordinary `.clangd` database
discovery (`--tweaks=` limits this invocation to parsing/editor semantics).
Without that scope, clangd 23.1.2's check mode reports failed DefineOutline
refactoring probes for external declarations; these are not compiler errors.
Public headers have their own C89 build commands, so the editor no longer
needs a second hard-coded dependency include list.

Post-build review found a Lua-finalizer ownership gap in the new PostgreSQL
adapter/result methods. Protected guards now prevent close/reentry during
allocating calls and root exact parameter strings independently of mutable
caller tables. A real GC regression exercises adapter close during query
setup, result close during column metadata allocation and subsequent recovery
after an invalid accessor. It passes against real PostgreSQL normal/packed.
The same Lua probe runs against service-free public C89 receiver doubles with
genuine cpkt result storage in 0.05s; this supplements, not replaces, transport
and official borrow-bridge integration. Strict Memcheck reports 3657 balanced
allocations/frees, zero live bytes/blocks and zero errors without suppressions.

Focused follow-up, 2026-10-28: C/Lua mapped state writes now forward public
retention options. C and normal/packed Lua checks prove omission, clearing and
invalid-value handling. The old stateful-renewal blocker was retested through
the direct released liblockdc 0.22.0 Lua API: read/update controls complete,
but renew/update still fails deferred completion's version precondition. The
self-contained CR is `stash/liblockdc-pouch-stateful-renew-completion.md`.
That restriction was necessary then; the 0.23.0 follow-up above supersedes it.

The XA worker wake fixture now commits Pouch business state and registered
outbox work from two HTTP workers, with callbacks confined to the supervisor
owner. Eight concurrent effects pass normal/packed, using actual receipt keys
and no recovery reconciles while the recovery timer is deliberately delayed.
Separate managed XA calls have distinct global IDs; this is not an application
request-deduplication policy. Native conflicts propagate rather than replaying
application work. The ordinary outbox duplicate test remains intact.

MCP failures now use a compiled HTTP fixture and Lua JSON assertions under
CTest, retaining direct/supervised/packed behavior and an actual unfinished
oversized-body socket rejection/closure. Python no longer controls that test.
Updated resolver fixtures pass; AFL reuses common Bootlin discovery and
try_compile propagation. The lifecycle artifact fixtures now use independent
provider manifests. Source-text API/plan assertions were retired; they did
not prove behavior and cannot implement the mandatory agent semantic gate.

The embedded Kore feature guard now matches its actual compiler definition,
so parent-death cleanup is active without a process-scanning supervisor.
Forked Lua runtimes close after native teardown; optional C/Lua fork-exit
callbacks are documented and never run in the original owner. IPC padding is
initialized. Actual worker termination tests prove remaining-worker intake
after graceful exit and SIGKILL. Nonzero Kore parent exits reach stop errors.
Strict service Memcheck passed again after the callback refinements: original
CLI, Kore parent and worker each report zero errors and zero definite, indirect
or possible lost bytes, without suppressions. The fixture now requires all
three process summaries. Full lifecycle, package and platform verification
remains pending.

The current work on `init` replaces c.pkt.systems 0.11.0 with independent
cpkt 0.5.2, cpktdb 0.3.1 and cpktmisc 0.2.1 prefixes. It upgrades liblockdc to
0.23.0 plus its separately linked PostgreSQL provider, MDF to 0.16.0 and Softline
to 0.13.0. Other selected releases remain as recorded in
`docs/cpkt-bundle-upgrade-spec.md` and `scripts/dependency-pins.tsv`.
The previous complete-facade goal was cleared by the user; its uncommitted
implementation remains preserved. The sections below describe earlier baseline
evidence, not a claim that current pins are unchanged or final gates passed.

New C app factories return public collection/coordinator receiver shells using
the shared app client. Lua storage scopes lazily materialize their native
client after startup/fork and close descendants on stop. Collection operations
preserve incremental raw JSONL plus metadata. XA operation proxies preserve
native result/wait distinctions; a blocking helper waits on actual readiness.
The PostgreSQL adapter uses checked public C89 retrieval and the official Lua
borrow bridge; query failures poison managed scopes rather than becoming EOF.
XA workflow commit receipts enter the existing bounded direct-key wake path.

Focused evidence so far: native PG/borrow/readiness fault tests pass in 0.01s;
storage/collection/XA wake and rollback behavior passes normal/packed in 0.68s;
real rootless Podman Kube PostgreSQL commit/abort, text/binary/NULL binding and
caught SQL/shape errors pass normal/packed, with teardown/unprivileged reset.
Native CMake runtime mapping/reconfiguration checks pass 2/2 in 1.67s; replaced
the corresponding Python controllers. LQL/MDF/pslog source provenance checks
pass with split-prefix fixtures. Native dependency preparation passes with a
curl shim that rejects every network call, proving a verified cache hit uses
zero acquisition requests. These are component checks, not final verification.

Remaining major-upgrade work includes lifecycle controller migration, package
and release fixture cutover, semantic coverage of the remaining changed seams,
complete documentation reconciliation, final native/platform/package/memory
verification, and whole-candidate review. The libcai_app/Smith migration remains
deferred until a released dependency provides it. No release or push occurred.

## Historical complete public API facade baseline

The current uncommitted implementation starts from `e3687ae` on `init`.
At that earlier baseline, dependency pins were unchanged. The
[complete facade specification](lua-public-api-completeness-spec.md) and
[inventory ledger](lua-public-api-inventory.md) own the expanded scope: all
public libvectis and dependency C89 APIs, with true streaming and explicit
materialized names. Earlier C-only exclusions are gaps to implement, not an
approved current policy. Inventory declaration counts do not establish coverage.

Implemented debug slices include the digest/HMAC C receiver and reader/file
paths, all 17 typed Pouch app settings and precedence, and the compression C
receiver with protected Lua reader/writer flow, raw/header/back ownership,
typed allocators, caller-owned buffer operations, native-width checksums and
readonly CRC-table capabilities. LQL and MDF component evidence follows below.
Current focused crypto/compression checks passed 7/7 in 1.14 seconds, including
normal/packed Lua. Digest/HMAC C Memcheck reported no live allocations or
errors; compression ownership probes also passed. These are slice evidence,
not the complete surface gate or a release review. Source-rock, installed SDK,
full builds/platform evidence and the final whole-goal review remain pending.

The required sequence is the entire implementation with focused debug tests,
then the full build/verification, then review and repeated debug-fix/review
until clean. The PostgreSQL checked-retrieval prerequisite recorded in
the [SQL component specification](sql-database-facades-spec.md) is resolved by
cpktdb 0.3.1 in the active upgrade; no native
header or private-layout bypass is authorized.

## Completed libmdf 0.14.0 upgrade baseline

The upgrade starts from `ae48963` and changes only libmdf's seven SDK pins, its
Lua source pin and the expected version. Other dependency versions and archives
remain unchanged. This is development integration evidence, not release approval
or a resumption of the separately paused SQL facade goal.

[The published release](https://github.com/sa6mwa/libmdf/releases/tag/v0.14.0)
identifies `c018a03` and retains shared ABI 5. Comparison with 0.13.0 found no
changes to the public C API beyond generated version macros, Lua module/binding
sources, imported target names or dependency closure. The release contains internal ANSI link-label/wrap and
manual-finish fixes, table code-span pipe handling, emission-capacity failure
classification and HTML/deck variable-font deduplication. Existing receiver,
callback, allocator, ownership and process/thread contracts remain applicable.

| Artifact | 0.14.0 SHA-256 |
| --- | --- |
| x86_64 GNU SDK | `b250a82e2562897102aee69bca2c946e53eadffddd1d85131975eaea00651b65` |
| x86_64 musl SDK | `144827811d9e1477a6e0835b8a94492c2a1ba7eb7fa6d091ee0e0c73f2e7b2ee` |
| aarch64 GNU SDK | `2909a4888a8e422ba0fe59efd6676a105be692fce5f67d0b2d0d3ea3e978afb6` |
| aarch64 musl SDK | `0f6ebaf7dd8754fa08bef3b14b7a15dc8f5074bf64ecd353d39e007aa963a574` |
| armhf GNU SDK | `2534b31a2c6222fac57b30e6ed3f63424d6b981c8bf1d5ad54fdebb00827b136` |
| armhf musl SDK | `1b02a34681d4e730849b8bd90afe52c44f0d825436e52674aa45c70f5e2bdc5f` |
| arm64 Darwin SDK | `d4cf9445fe175810a2cd223a7928374094aab2944894a99b50b3b11cac8972fe` |
| Lua source | `fcff4bf1b4eb9165dadd76fa4dc9a2c4b8d8f409bb7200469f4b6e6177cf30e8` |

The embedded upstream binding links the same static SDK as the CLI; its direct
module and `vectis.libs.libmdf` identity are retained. `vectis.terminal` already
forwards the affected options and translates callback failures; no rename-only
wrapper is needed. Agent Smith checks native renderer failures on its existing
bounded renderer thread, which never calls Lua or Softline directly. The docs
pager uses the same public renderer and checked status. No new app lifecycle or
logging configuration is introduced. Source-rock optional-native loading and
static/shared CMake/pkg-config closure remain required consumer seams.

New deterministic tests cover the fixes through native C and direct/convenience
Lua, including normal and Linux packed execution. The escaped pipe Lua probe and
native bounded-emission status probe both failed against the old 0.13.0 SDK.
These are behavior checks, not assertions that a method merely exists.

The responsible agent reviewed the pin changes, callback cleanup, direct and
convenience calls, source-rock loading, packed execution and SDK consumers.
The existing source-rock smoke had retained the old MQTT upload assertion after
the POST fix; it now checks POST, upload=false and the exact buffered body. No
MQTT implementation was changed. Installed CMake consumers now render the
escaped-pipe fixture through public C receiver methods, and pkg-config consumers
create/destroy an MDF receiver, proving that the declared dependency is usable.

Completed verification:

- `make finalize-slice CTEST='ctest --stop-on-failure'`: lifecycle, tool discovery,
  resolver and privacy contracts; all 213 debug tests (37 fast and 176 broader).
- Focused rendering/Smith/example/C failure checks: all five passed. The new C
  failure test took under 0.01s and combined normal/packed Lua regression about
  0.2s before the full gate; no sleeps or delay-based faults were added.
- Native Memcheck of the C bounded-buffer failure probe and Lua regressions:
  zero errors, zero bytes allocated at exit, including callback failures.
- `make test-install-tree`: direct and extracted static/shared SDK consumers,
  CMake/pkg-config linkage, exact exports and pinned-runtime checks passed.
  Follow-up extracted static/shared checks passed with actual MDF rendering and
  receiver construction in the strengthened consumers.
- `scripts/test_lua_rock.sh`: passed after correcting the stale assertion.
- x86_64 musl release CLI: statically linked; the same rendering regression
  passed in normal and packed execution.
- All seven SDK pins and the Lua source pin match the published checksum
  manifest. GNU SONAME remains `libmdf.so.5`; its glibc symbol requirements match
  0.13.0. Public declarations and Lua sources agree with the released facade,
  allowing generated SDK version macros.

ARM and native Darwin execution were not run for this focused upgrade; their
pins are updated and validated, not runtime-certified. No hosted workflow,
release, push or deployment was performed. Other dependency versions and the
SQL facade prerequisite remain unchanged.

## Historical supervisor integration record

Status: recorded integration evidence for code committed as `e20ab55`. Earlier
iteration results below are historical; the completed verification section is
the final result for that code, not approval of a later release candidate.

This is an agent semantic integration review of the working candidate on `init`,
based on `b1fe538013c6f9e6bfceee03cf9ee006b0f6f48a`. That commit already
changed pins; the previous pin baseline is its parent `2fa69d8`. The reviewed
candidate also contains the supervisor/workflow implementation and the fixes
recorded below. It is not a release approval. Required upstream gaps remain,
and native Darwin/live-provider execution is not established by Linux tests.
A release must repeat the mandatory AGENTS.md reasoning review against its final
committed candidate; scripts, symbol presence and this matrix cannot approve it.

## Artifact identity and comparison

The configured Bootlin stable 2026.08 GNU compiler and current host-debug SDK
were used. Prior digest-addressed release headers, Lua sources, docs and examples
were extracted under ignored `build/lua-seam-audit/baseline`; no adjacent SDK or
repository was modified. Header/doc diff records there substantiate comparison.

| Artifact | Previous pin | Candidate | Candidate GNU SHA-256 |
| --- | --- | --- | --- |
| c.pkt.systems | 0.10.0 | 0.11.0 | `fbd3e3f357c22dc26b94cc7e5e200651b59ecb39357ce7bb8c4cb3d939a2b557` |
| LoneJSON | 0.44.0 | 0.45.0 | `aef11c4511bb6aa8ed4e0646204e14b7934affd6e5cec5b8e8bff36e80217826` |
| LoneJSON Lua | 0.44.0 | 0.45.0 | `1066e37671f0f1e4076aa7f71206a0e29220cafb1508526d510abca0ab9dfb7d` |
| Softline | 0.6.0 | 0.9.0 | `09612ec2cc3fc1ecdcefaf4afff4e5e80555d9618c13a7e6db3405d0ead835f5` |
| Softline Lua | 0.6.0 | 0.9.0 | `98421d66740042aca38a18eb32245e736b49f6495595ffacd282c51c99b139fe` |
| libpid0 header | 0.4.2 | 0.5.0 | `9647a9027e3f5086278cc628b4cfdf853e239a04c202b4f3577691202431c7e1` |
| liblockdc | 0.18.0 unchanged | 0.18.0 | `1e949887d613824716645dd5e9a85a1b89ad2d8d66431838c0a28c999b0e23da` |
| liblockdc Lua | 0.18.0 unchanged | 0.18.0 | `94852adfc713299d748d808cd4ff5cbe3e4d66f0bc6ce5be6e80d97cd981cc89` |
| pslog | 0.12.0 unchanged | 0.12.0 | `21239262bc06eecb504e45e0902f374ccd86751dcb32087dd84af4d2553ea0e2` |
| CAI | 0.6.0 unchanged | 0.6.0 | `f7f78d9f847f1265267bbc33a6d6cb2058602d82967357ab561f36ebb1d0daf8` |
| LQL | 0.3.0 unchanged | 0.3.0 | `fafb04f9af5d9610f77380ac1f848d1edd3d60dcf7ce6a098f6a04e30dd2b47a` |
| MDF | 0.13.0 unchanged | 0.13.0 | `6be317bb232702cab5fb2f7f6041fa6b6cc0cb6d3676cb1230f80a3778c1c4b4` |

The nominal transitive upgrade baseline includes SUS/whisper 1.9.1→1.9.4,
open62541 1.5.4→1.5.8 and libxml2 2.15.3→2.15.4. The extracted
prior **0.10.0 archive manifest already reports** whisper 1.9.4, open62541
1.5.8 and libxml2 2.15.4. These actual digest-pinned artifact contents, rather
than version labels inferred from earlier planning, govern the semantic diff.
SUS/audio public headers are unchanged between those resolved archives.
The resolved open62541 patchset changes from `mqtt-prefix-packet-headers-2`
to `mqtt-prefix-packet-headers-crypto-logging-24`, adding crypto/logging/lifecycle
ownership fixes at the same resolved upstream version.
LoneJSON Lua init files are identical upstream; the Vectis copied partial root
was the omission. LoneJSON public header changes document zero-initializing
curl parser adapters and safe reinitialization; Vectis already zeroes that
state before initialization and cleans it on success/failure. Deployment docs
switch upstream development fixtures to rootless Podman; Vectis does not import
the upstream fixture orchestrator.

The prior GNU digests from `2fa69d8:scripts/deps.sh` are:

| Changed artifact | Prior SHA-256 |
| --- | --- |
| c.pkt.systems | `fb64caa3cad66e01669705412cd88cb4267025ca0a083fa936fe25786011391d` |
| LoneJSON | `5255a32cef6dd89cd4f068c2a4f1bd6a8f8e79d5cffe6d6922cb2506d320c520` |
| LoneJSON Lua | `7a967f0231867357be22cf23d4fa7b5f7437eaccd54765d1c02a857c51b47160` |
| Softline | `b1e4f2fb8ad4d8072acf1d72be31ec23066d8ccbf66a6fe10e15b7243ac8ac2f` |
| Softline Lua | `f8cc7ea7c3dbf3b336c55be10972435a03b5783d8773b8506e928806ebefaa36` |
| libpid0 header | `907fb7f084d192da1c6d92d26b1b6cd93bb4dafe36737856696f47de906dd5e8` |

Unchanged Lua-source digests also checked in the configured manifest:
pslog `35e46f594b2ce12b42bba95357109fd662ce1c2ab7a175702e9b9d141cb0ec2b`,
CAI `977e9b615dd78127fba6574eb2ac8527f52c4e6c578faeece15f39c8ed7eaeab`,
LQL `c4610615f4315081c519afe47d519e72c1242cce449620cf90f8f8828eccdfe1`,
MDF `a4c4ac66c2bf67aeb6d19f76e13ea245e34dce291221803dea079db8d7df1eb8`.

## All dependency and bundled-library decisions

| Dependency | Public/native and Lua boundary | Vectis integration and exclusion |
| --- | --- | --- |
| Lua 5.5.1 | cpkt runtime plus new strict-C89 full Lua facade and warning callback | Existing owner runtime, preload/resources, app/routes and callback ownership retained. Native stack/warning hooks are embedder C facilities; standard Lua warning semantics remain. No arbitrary foreign-state entry. |
| LoneJSON 0.45 | Full upstream core/facade, schemas, records, visitors, Base64/JWT/JWK/JWKS/OIDC/OAuth2 and streaming sinks | Upstream resource import plus minimal schema/rewrite overlay; provider compile targets enabled. Existing auth/REST/OpenAPI/DSV/curl streaming workflows kept. Materialized tables and file-backed spools named accurately. |
| Softline 0.9 | Full automatic native import; queue modes, native output streams/quotes/status/image path; removed boxed layout | `terminal.editor` passes official options. Smith queue/output streaming already uses public methods. Queue/path failures verified; X11 and interactive resize remain separate device gates. Private bundled c-ares 1.34.8 (MIT, plus BSD address sorting) closes its resolver linkage. |
| libpid0 0.5 | Public header integration | Diff removes a 50ms post-child delay; API and ownership unchanged. Existing child/signal/runtime supervision tests verify behavior; no Lua PID wrapper invented. |
| liblockdc 0.18 | Full upstream state/queue/outbox/source/sink import plus newly identified public endpoint helpers | App-owned canonical options/logging bridge, consumer and workflow post-fork clients, real receipt FD delivery, recovery and cleanup. Direct root remains upstream. Pull/auto-renew/kind selection/candidate FD limits detailed below; shared leadership is future Vectis work. |
| pslog 0.12 | Full upstream Lua facade and official native `pslog_lua.h` capability/ref interface | OPC UA native sink interop, exact lengths/FATAL nontermination, native app/logd/service seeds. Function outputs rejected where worker emission is possible. Borrowed native clients close children before seed cleanup. |
| CAI 0.6 | Full upstream facade | Existing local constructors/response/agent/tools/MCP, worker envelopes, genuine CAI client request/output streams and logger inheritance retained. Lua app MCP is deliberately buffered and stateless: public CAI has an HTTP response sink, but this established Lua route collects tool results/responses. Persistent/streamed Lua server behavior needs an explicit future product contract; no MCP streaming claim is made. Provider/MCP remote execution requires opt-in gates; constructor presence is not a live-provider claim. |
| LQL 0.3 | Public receiver binding with local provenance-checked adaptation | Complete component mappings and ownership/callback repairs; native provider unchanged. Focused normal/packed and OOM evidence below, with whole-goal gates pending. |
| MDF 0.13 | Full upstream facade | Terminal Markdown and Smith incremental rendering retained. Renderer document ownership remains explicit; whole-response buffering is not introduced. |
| OpenSSL 3.6.4 | Existing Lua EVP digest/HMAC/sign/verify/codecs/CSPRNG; new SDK C89 facade | Auth/OIDC/cert/TLS/ACME/public signing workflows retained. Native OpenSSL pointer graphs remain C-only; require a concrete ownership-bearing workflow before extending Lua. |
| curl 8.22.0 | Existing public options/upload/download/read/write facade | HTTP/REST/WebDAV/SMTP/MQTT, worker config/logging, HTTP and CAI/DSV streams retained. Public option passthrough provides existing auth mechanisms. No new GSS/SASL context objects are needed to retain curl's current options. Live broker/mail/provider gates remain opt-in. |
| nghttp2 1.70.0 | HTTP/2 dependency plus new C89 facade | Native curl/Kore protocol closure retained; protocol frames and callback pointer machinery remain C-only until a Vectis-owned HTTP/2 operation requires them. |
| zlib 1.3.2 | Full existing Lua compression helpers; configured header guard change | Embedded resources/content compression and static/shared closure retained. |
| libxml2 2.15.4 | Existing XML↔LoneJSON mapping and native writer | Lua parsing materializes mapped values/records, potentially with spooled fields. New Lua `xml.write` genuinely pushes native chunks from private table input; explicit buffered serialization shares its emitter. No DOM stream is claimed. The useful-facade goal supersedes that earlier exclusion: owned element DOM editing, scalar/element XPath and caller-fed SAX now have native/normal/packed and allocation-failure evidence in the current progress record. |
| miniaudio 0.11.25 | Existing cpkt audio public receiver facade/Lua | Decoder/encoder/VOX/PTT and managed copied mailbox work retained. Lua decoder callbacks run under public owner reads; device capture remains opt-in. |
| whisper.cpp 1.9.4 / SUS ABI 0 | Existing catalog/cache/transcriber/segmented methods and global backend logging | Foreign logs copy into bounded owner queue; other Lua callbacks reject foreign entry. Explicit owner log source supports app loops. Per-app backend logger inheritance is excluded until public per-model/context routing exists. Native global continuation severity remains an upstream race before our synchronized sink; independent constructor emitters have no shared serialization. Model inference remains opt-in. |
| open62541 1.5.8 / OPC UA ABI 0 | Full cpkt client/server facade plus structured logger constructors/setters | Public pslog capability retention, managed ownership lease and app sink before startup, joined teardown/mute, duplicate rejection, callback reentry guards and deferred GC in both whole-VM creation orders. Existing subscriptions/events/async/security/discovery/PubSub graph retained; native pointers/plugins excluded. External event-loop delete contract requires embedder quiescence. |
| MQTT-C 1.1.2, commit `0f4c34c8cc00b16cfee094745d68b8cdbaecd8e0` | OPC UA PubSub dependency plus new C89 raw facade | Existing OPC UA graph/config and curl MQTT facade kept. No separate raw packet/session Lua API without an established workflow. Live MQTT is not claimed by disabled graph probes. |
| libssh2 1.11.1 | Public C library plus new C89 facade | Existing Vectis SSH/SFTP/SCP receivers preserved. Raw dependency-native channels deliberately C-only. SSH investigation/live execution parked by task authority. |
| MIT Kerberos 1.22.2 / cpkt GSSAPI | **New public C89 facade**, native security contexts | Deliberate C-only context/credential ownership boundary. Existing curl/auth workflows remain through their official options. Reconsider with a specified app Kerberos context/delegation workflow. |
| Cyrus SASL 2.1.28 / cpkt SASL and SASL-plugin | **New public C89 facades**, mechanisms/plugins/callbacks | Native plugin/credential/concurrency boundary; no current application plugin DSL or LDAP ownership workflow. Reconsider concrete SASL-context integration; do not misclassify as a mere archive. |
| OpenLDAP 2.6.15 | New transitive/public native library | Auth/curl/Postgres deployment closure inspected; no existing Vectis LDAP client product. No new LDAP Lua facade inferred. |
| PostgreSQL 18.6 / cpkt Postgres | New public C89 facade and native library | Lua/database facade explicitly separate task; current task does not claim one. GSS/SASL-related closure remains native until that task defines ownership. |
| SQLite 3.53.4 / cpkt SQLite | New public C89 facade/constants | Explicitly separate task; no persistence migration invented for current Pouch products. |
| libHaru 2.4.6 / cpkt PDF | New full public C89 PDF facade | Explicitly separate PDF task. |
| libpng 1.6.58 | New upstream full native header/library surface | Explicitly separate PNG task; does not imply a Lua facade here. |
| iODBC 3.52.16 | New native deployment/link dependency | No existing Vectis ODBC application interface. Keep SDK consumer closure; a defined ODBC workflow would trigger separate design. |
| vendored Kore `1ff3c0f6f7973dc7fc8bf38369790ef2d8079b3b` | Existing patched server/event/fork control runtime | App worker/control/auth/transports preserved. Supervisor uses a separate bounded public channel; VRC1 stays private. Real worker routes prove owner PID/main-state effects via FD fast path. Native Darwin kqueue workflow retained. |
| Embedded Swagger UI / QR support | Existing resource/vendor inputs | Static assets and TOTP/QR behavior unchanged; no new export inferred. |

## Traced seams and fixes

LoneJSON constructors now reach the authoritative facade rather than a copied
subset. Resources/preload and source self-description expose both overlay and
upstream. The source rock excludes the embedded LoneJSON overlay, preserving legitimate
optional absent native cores without hiding loader exceptions. The native object
also receives JWT/OIDC/OpenSSL compile usage requirements: a successful link alone
had previously left features compiled out.

OPC UA direct client/server/JSON/file constructors install logging before native
initialization; setters retain replacements through C changes and old destruction.
The shared native log adapter maps levels/categories/status and calls exact-length
`log_view`, including NUL and nonterminating FATAL. App service workers inherit
or mute the seed before startup; quiesced cleanup removes the borrowed app sink
before context/logger release. App-owned Lua servers reject concurrent direct
operations and defer GC teardown until the borrow ends.

SUS uses a static mutex/context and bounded copied records. Foreign producers
never touch Lua, oversize/full/error counts are observable, replacement/VM-close
discard queued generations, and Lua executes without the mutex. The first
explicit setup installs a stable native hook before workers; later replacement
and unregister do not rewrite upstream's unsynchronized global callback fields.
Concurrent VM registration claims the subscriber atomically and rejects a foreign
owner. Segment/progress/abort/segmented/cache callbacks guard the creation owner. Explicit `sus_logs`
provides owner-loop pumping without overwriting process-wide sink ownership.

Canonical workflow **and consumer** options resolve actual native app defaults
post-fork, propagate flat crypto/bundles and pass original query syntax to public
native open. Decoded helper checks replace handwritten query-name parsers in
native app and ACME default encryption. App-owned Pouch roots default to shared
writer mode for default and flat configurations as well as explicit shared queries;
exclusive query mode remains native-authoritative and cannot support additional
app-owned clients. Typed exclusive mode is rejected. Percent-encoded explicit key names cannot
be overwritten by a default. App-owned private client children close before the
borrowed app logger. Native app Lockd options reject unknown fields, NUL strings
and malformed arrays rather than silently diverging from child clients; explicit
app mute survives logger derivation. Standalone interop is deliberately excluded because child
handles can retain clients beyond Lua close without an official logger capability.

Supervisor inbox allocation is checked and bounded to 16 MiB. Producer send and
owner receive/start/stop do not wait for a producer-held process-shared mutex;
contention leaves level readiness intact. Signals alternate fairly with ordered
data and rotate among finite kinds. Ingress closes atomically before app-owned
workers join; mapping/fds remain until safe destruction/reprepare. External C
producer threads must be quiesced by their owner before restart/destruction.
Linux robust owner death poisons ingress rather than accepting corrupted frames;
Darwin has no Linux robust-attribute branch and must pass native platform gates.
Apple's current [libpthread mutex implementation](https://github.com/apple-oss-distributions/libpthread/blob/main/src/pthread_mutex.c)
accepts `PTHREAD_PROCESS_SHARED`; an old rwlock manual is not evidence that it
rejects shared mutexes. The tracked native Darwin workflow is narrowly allowed
by the local lifecycle contract, without authorizing a push or hosted run.

Scope failure stores a copied code/message and emits owned service diagnostics;
other stop-policy scopes continue. Handler-returned native status works with a
null error pointer. Cleanup runs once after a failed start/stop; successful
restart clears diagnostics. Timer disable removes native deadlines and enable
resets monotonic scheduling. Meaningful unsupported lane/restart/quota/scope
options fail explicitly. GC callbacks restore live app scope ownership and close
sources before logger teardown. Protected client opens pin immutable endpoint strings across callback mutation
and collection. Protected transactions/opens retain the app and reject lifecycle
changes while private native children borrow its logger, preserving table errors
and all return values. Canonical client validation rejects sparse/dictionary
endpoint lists, native string NULs and truthy substitutes for booleans.
Transaction false or nil/error rolls back;
caught unknown-kind failures remain sticky. Short stateful handlers support guarded
checkpoint update/completion; renewal there fails explicitly.

## Prior iteration behavior evidence and limits

The focused fixtures are executable specifications: `upgrade_seams` covers
schemas/runtime/visitor failures/cleanup, binary Base64, malformed JWT/PKCE,
Softline queues/config/image-path failures and OPC UA native logger lifetimes.
`supervisor_client_options` covers app defaults, encoded query crypto, flat config,
encrypted reopen, client logging, source/workflow GC, copied scope diagnostics
and explicit SUS owner-loop pumping, endpoint mutation/GC and callback lifecycle
rejection without poisoning later successful calls. Both execute normal and Linux packed modes.
Native units exercise pthread and SIGSTOP contention, finite bounds, fairness,
cleanup counts, disabled deadlines and optional error pointers; actual SUS source
fixtures prove foreign callback rejection, bounded logs, replacement/two-VM/
coroutine shutdown. OPC UA source fixtures prove worker-thread exact-length FATAL
records and cleanup. Live network/model/device gates are separate.

Real Kore route tests use two worker processes and owner-main-state effects.
The direct case sets candidate probes to 1,000,000 ms and disables discovery;
the one-shot SUS timer is drained and disabled before HTTP commits begin;
all eight sequential commits produce owner-main-state effects through the FD path.
Eight-commit
saturation/recovery is a separate scenario. Same-process producer auto-notify
statistics alone are not accepted as that proof. See the
[supervisor workflow audit](supervisor-workflow-audit.md) for detailed commands.

Fresh remediation verification was serialized. Logs are under ignored
`build/lua-seam-audit/`; these are working-candidate results, not release approval.

| Command/check | Result and evidence |
| --- | --- |
| `make test-failures-fast` | 32/32 passed. Builds CLI plus the three required native lifetime units before running the existing failure-fast label. New normal/packed upgrade, workflow, worker-route and client fixtures are included. `failure-fast-final.log`. |
| Focused OPC UA e2e/reentry, ACME state, runnable workflow example and upstream stateful contract probe | Passed. Actual local OPC UA service behavior remains separate from constructor probes; the stateful probe demonstrates the upstream defect, not successful renewal. `verification-final.log` (6/6). |
| Workflow and canonical-client normal/packed `ctest --repeat until-fail:5` | Each passed five consecutive repetitions. Initial repeat exposed a test assumption that automatic retry could not immediately repeat; upstream full jitter includes zero. The corrected test verifies bounded attempts and durable retry state, plus an explicit 30-second delayed retry without waiting. `repeat-workflow-final.log`. |
| `vectis_cli_self_describe` repeated three times | Passed. Raw exported `lonejson.upstream` equals the installed authoritative source; stdout's annotated transcript is intentionally a different interface. `repeat-final.log`. |
| Valgrind with full definite/indirect/possible leak checks, origins, exit-on-error 99 | Supervisor/SUS/OPC UA native units and Lua upgrade fixture: zero errors, all heap freed. Workflow and canonical-client/GC fixtures: zero errors and zero lost bytes; respectively 4,955,634 bytes/9 blocks and 4,956,212 bytes/17 blocks reachable in upstream process caches. No suppressions. `memcheck-*-final.log`. |
| Bootlin ASAN+UBSAN supervisor build/run (`detect_leaks=1:abort_on_error=1`, `halt_on_error=1`) | Passed, including same-process producer stop/join/mapping lifetime, SIGSTOP contention and cleanup callback counts. `asan-build-final.log`, `asan-supervisor-final.log`. |
| Static/shared build, install under `build/`, `verify_installed_sdk.sh ... static/shared .cache/deps/host-debug` | Both passed: installed C89/C++98 and pkg-config consumers, exported-symbol policy, 36 example runtime closures per linkage mode and missing-runtime detection. `shared-build-final.log`, `sdk-*-final.log`. |
| `VECTIS_VERSION=0.0.0 VECTIS_DIST_DIR=$PWD/build/lua-seam-audit/dist bash scripts/stage_lua_rock_sources.sh` | Source tarball, rendered rockspec, source rock and archive readback passed. Registered Vectis curl facade retained; embedded LoneJSON overlay excluded. `source-rock-final.log`. No release artifacts were published. |
| Ordinary Lua 5.5 `tests/lua/source_rock_path.lua` with artifact-first path and canonical upstream LoneJSON path | The earlier run used a minimal core identity fixture. Fresh ordinary Lua 5.5.1 artifact loading now first sets empty `package.preload` and `package.cpath`, proving whole-Vectis legitimate source-only loading with available pure helpers and curl/core absence. A separately labeled fixture checks canonical upstream identity, string/table exception preservation, unknown nested absence and custom searchers. Native installation is not claimed. `followup-source-path.log` (genuine no-stub loading and separate identity probes). |
| Format, lifecycle, Lua matrix/behavior, service lifecycle, OPC UA surface, shell syntax, `git diff --check` | Passed. Narrow declared Darwin workflow exception resolves the old blanket ban. The adopted workflow now includes native supervisor, OPC UA lifetime and normal Lua workflow/route gates with `-j1`; it was not dispatched. `format-final.log`, `lifecycle-final.log`, `matrix-final.log`, `behavior-final.log`, `service-final.log`, `opcua-surface-final.log`, `syntax-final.log`, `diff-final.log`. |

Public additions use a separate supervisor receiver shell and free functions;
the existing app vtable/layout is preserved. No released ABI is broken by these
additions, so the shared ABI remains 0. Installed C89/C++98 consumers verify
the public headers and both linkage modes.

Installed consumers and source Lua loading verify different product closures.
Native dependencies are not shipped inside a Vectis source rock. Live provider,
model/device, remote Lockd, claim-expiry endurance and native Darwin checks were
not run here. Those prior iteration checks do not establish the final full-repository gate or final independent review.

## Prior parent review remediation

The interrupted focused session left no result file. Its completed evidence is
`followup-behavior-final.log` (13/13 affected checks),
`followup-memcheck-{queue,opcua,upgrade}.log`, `followup-source-rock.log`, and
`followup-source-path.log`. Source-only artifact loading genuinely begins with
empty preload and C-module paths; later identity/failure fixtures are separate.
No packaging input changed in this parent remediation, so that successful
artifact evidence is retained rather than presented as a fresh package build.

The native signal fix attempts a nonblocking wake for every accepted producer,
even when its separate bounded coalesced slot is already pending. EAGAIN means
an FD token is already armed. Redundant datagrams may cause harmless empty
receives; retained signals remain coalesced, and each receive drain stays bounded.
The actual-source pthread/condition test suspends A immediately before `send`,
completes B, copies the delivered signal while A is paused, then releases and
joins A and verifies its late token produces no duplicate signal. A closed
precheck precedes frame trylock; the existing contention handshake verifies
closed status immediately after completed stop while the producer still owns
the mutex. No sleep or delay-based fault injection is used.

HTTP response sources now carry their owning route's app token into a scoped
strong app reference, shared protected calls and balanced callback depth.
Generic routes, DSV completion and upload completion use that path. Source
read/reset/close and app destruction reentry are guarded; cleanup errors are
caught, and caught teardown failures clear the closing guard. The protected
teardown preserves ordinary close idempotence. Upload open/chunk/complete/close
were already protected, and their rejection is now exercised explicitly.
Standalone DSV and HTTP client producers retain their own dependency ownership;
they are not indiscriminately mapped to an app. When invoked inside an owned
route/source callback, the outer app guard remains active throughout those calls.

| Current check | Exact evidence / limits |
| --- | --- |
| Debug build `vectis_failures_fast`, targeted unit/real Kore tests | Passed. `parent-build-final.log`, `parent-targeted-final.log`, `parent-source-all-contexts.log`. The new Kore fixture runs normal and Linux packed responses, reset/read/close rejection, uncaught read/reset failures (HTTP 500), upload/DSV source ownership and callback guards, later healthy responses, graceful route stop, stop through wait, and repeated ordinary close. |
| Full final `ctest --preset debug -j1 --output-on-failure -L failure-fast` | 35/35 passed; `parent-fast-final.log`. This is the full fast label, not the whole-suite standard gate. |
| Strict Valgrind native source-lifetime and supervisor units | Zero errors and zero lost bytes, no suppressions. Source unit and supervisor parent free all allocations; forked supervisor fixtures retain inherited reachable process state at exit. `parent-unit-memory.log`, `parent-supervisor-memory.log`. Actual CLI source is included; the instrumented destructor delegates to the real native app close exactly once. Tests cover caught teardown failure and clean retry, cleanup reentry, balanced depth and oversized-chunk stack cleanup. |
| Targeted ASan+UBSan (`detect_leaks=1:abort_on_error=1`, `halt_on_error=1`) | 3/3 passed; `parent-asan-build.log`, `parent-asan-tests.log`. Expanded upload/DSV real Kore normal/packed fixture also passed, `parent-asan-all-contexts.log`. Collection occurs inside read/reset/close and other owned callbacks. No suppressions. |
| Real Kore strict Memcheck diagnostic | **Not clean**, `parent-source-memory.log` and its per-process logs. Parent has zero errors. Fork children report Lua interior-pointer possible losses at exit and uninitialized message padding from `kore_msg_send`/worker logging; an error-exit worker respawn also reports 24 bytes of pool-name loss. These reports are not source-lifetime proof, are not suppressed, and this check is not recorded as passed. Full service-memory qualification remains with the parent final gate. |
| Darwin YAML and exact CMake target/CTest readback | Passed, `parent-darwin-readback.log`. OPC UA names were already fixed. SUS and app source lifetime targets/tests are retained alongside the selected Lua fixtures. Every selected target/test matches CMake registrations, including APPLE-only registrations. Native Darwin remains unrun; no hosted dispatch. |
| Formatting, lifecycle and facade contracts; syntax/readback | Passed, `parent-format.log`, `parent-lifecycle.log`, `parent-matrix.log`, `parent-behavior-contract.log`; Python AST, documentation readback/whitespace and `git diff --check` passed. The assertion-enabled unit rebuild and rerun passed, `parent-unit-assertions-build.log`, `parent-unit-assertions-test.log`. |

All pre-existing coherent work is retained. Four SQLite/Postgres/libHaru/libpng
facades remain separate and SSH stays parked. Dependency pins, public ABI and
streaming/materialization contracts are unchanged. That prior iteration left the final independent review, whole-suite gate and
installed closures to the parent. The subsequent remediation below runs the
verification gates; final independent review and verification are recorded below.
This evidence does not approve a release.

## Final independent-review remediation and fresh verification

The independent review recorded at 04:09 UTC found four actionable issues.
All four were implemented without reverting the coherent earlier task edits:

1. Returned HTTP response/source configuration is resolved and pinned inside a
   scoped app-owned protected conversion. Generic route and before/after, DSV,
   upload and native callback-auth conversion share the owning guard; partial
   source state/references are released on error. Upload cleanup remains reachable.
   Numeric strings are rooted, fallback getters preserve selection order, and
   header key normalization leaves Lua iteration keys intact. Configuration
   snapshots never consume or materialize callback payloads.
2. OPC UA setters validate liveness before retention, root their receiver through
   protected getter evaluation, reject close/borrow during that evaluation,
   revalidate ownership afterward and release partial logger retention on every
   failure. Native sinks remain foreign-thread-safe; no arbitrary Lua log output
   is accepted at the native logging boundary.
3. The supervisor route test's fast label is assigned only when Kore registers it.
4. The fast target now builds `vectis_audio_lua_validation` as part of its closure.

The whole standard gate additionally exposed a stale example mock of `dequeue`.
It was migrated to the existing batch-one/client-terminal contract, asserting
page size, correct raw-message ack and exactly-once close. This was fixture repair;
no new queue facade or alternate runtime path was introduced.

Fresh final gates on the uncommitted `init` candidate based on `b1fe538`:

| Gate | Final result and current evidence under `build/lua-seam-audit/` |
| --- | --- |
| Required serial `CMAKE_BUILD_PARALLEL_LEVEL=1 CTEST_PARALLEL_LEVEL=1 TMPDIR=$PWD/build/lua-seam-audit/tmp make -j1 finalize-slice CTEST='ctest -j1'` | Exit 0; formatting and all lifecycle/toolchain/target/facade/privacy contracts passed. **Fast 35/35, 17.04s; standard 176/176, 245.35s; 0 failures, 0 CTest skips.** `remediation-finalize-slice-final.log`. |
| Clean fast target closure | New `clean-fast` directory configured with the debug preset and identical pinned dependencies; actual audio executable compile/link appears in `remediation-clean-fast-build.log`. Initial clean label 35/35 in 15.60s; the final scalar/test refinement passed its native check and the full final fast gate above. No earlier audio binary supplied the clean closure. |
| Supported Kore-disabled configure/build/tests | Fresh `no-kore-final` with runtime OFF and tests ON; build passed, four relevant tests 4/4 in 0.61s. `remediation-no-kore-{configure,build,tests}.log`. |
| Observable HTTP conversion and ownership | Normal and Linux packed fixture runs in the fast gate; 5 contexts × 6 getter phases × 2 modes plus 2 auth cases = **62 conversion cases per execution**, every case followed by healthy service. Callback collection and all **12 upload cleanups** asserted per execution. Native checks also cover partial construction and pinned/normalized values. |
| Final installed SDK core | `cmake --build build/supervisor-shared -j1` rebuilt changed `vectis.c` and `vectis_supervisor.c`; install prefix is under that build. Static and shared each passed 3 C89/C++98/pkg-config consumers plus **36 example runtime closures**, including missing-runtime detection. Canonical export policy returned **0**. `remediation-shared-*-final.log`, `remediation-sdk-{static,shared}-final.log`, `remediation-export-policy-final.log`, `remediation-installed-core-digests.log`. Installed shared bytes differ because installation removes the build runtime path, as the install log records. |
| Source rock and ordinary Lua | Staged version **0.0.0** entirely under build; archive/rockspec/source-rock validation passed. Actual Lua **5.5.1** ran `source_rock_path.lua` against the source rock's extracted nested archive with empty initial preloads/C path. Genuine source-only behavior precedes explicitly separate stub identity/error probes; no native-installation claim. `remediation-source-rock-final.log`, `remediation-source-rock-path-final.log`. |
| Targeted ASan/UBSan final paths | **6/6, 1.90s**: five native lifetime units and normal/packed real service fixture; leak detection, abort/halt on error, no suppressions. `remediation-asan-{build,tests}-final.log`. |
| Strict native Memcheck | **5/5 exit 0**, zero errors and definite/indirect/possible lost bytes, no suppressions. Supervisor fork children retain reachable inherited state. `remediation-native-memory/`. |
| Strict real-service Memcheck | **Not clean: driver exit 1** after HTTP assertions completed. Parent 0 errors/all heap freed; completed worker 0 definite/indirect loss, **181,317 possible bytes / 282 blocks**, **123 errors / 122 contexts**, two other fork logs without terminal summaries. Origins identify Kore message padding. Possible-loss graphs include Lua and Lua-owned LoneJSON schema/runtime resources; they are not all cpkt allocator records. `remediation-service-memory-final.log`, `remediation-service-memory/normal/memory.*.log`. |
| Baseline memory mechanisms | Byte-identical pinned/current Kore send/pool-init bodies; fresh standalone minimized padding exit 99 (1 error), respawn names exit 99 (24 definite bytes / 2 blocks in child; parent clean). `remediation-kore-minimal-*.log`; self-contained `stash/kore-strict-memcheck-baseline.md`. No global shutdown change or suppression. |

The first broad standard run was 175/176 in 244.59s with only the stale example
mock failing. It is preserved in `remediation-finalize-slice.log`; the repaired
example passed before the final whole-gate rerun above. Earlier failed scalar-test
assertions were corrected to the actual strict file-path / numeric-string API;
final native, fast, sanitizer and memory checks cover those assertions.

Three internal standard-suite omissions remain explicit despite successful CTest
exit: optional Paramiko SSH output integration unavailable, live SUS/audio model
validation disabled, and loaded-model example skipped. Provider/model/device and
remote endurance gates are not implied. Native Darwin remains unrun; optimized
musl verification was optional and not performed. Four SQLite/Postgres/PDF/PNG
Lua facades remain separate and SSH investigation stays parked.

A fresh narrow read-only **self-review**, not an independent review, traces final
conversion/partial cleanup, logger ownership/collection, test registration and
clean build closure against the observed results. No additional actionable local
finding remained from that self-review. The subsequent independent review found
the allocation-error boundary fixed below; its re-review and final whole gate
are recorded in the completed verification section. No commit, push, publish,
goal, notification, deployment or out-of-repository change was performed by that
remediation worker. Required upstream contracts and future
local phases below remain distinct; a successful ordinary gate is not a release
approval and does not make the strict service-memory gate clean.

## Final independent P2: protected source processing under Lua OOM

The remaining finding in `build/lua-seam-audit/clean-review-result.md` is fixed
in the working candidate on `init` at base `b1fe538`. Only the CLI Lua source
adapter changes native behavior; core SDK code, public ABI and dependency pins
are unchanged. Earlier remediation and the parent's isolated fixture cwd,
resolved absolute arguments and removed root PID file are preserved.

Read/reset now run callback invocation, EOF/type normalization, numeric string
conversion, pending-reference creation/release and reset state changes inside
one protected operation with the validated owning app rooted and guarded.
Errors return through `lc_error`; outside protection error inspection reads only
actual strings or a fixed fallback, never coercing numeric/table errors. Native
source ownership remains explicit in partial construction until close or response
transfer. Close callbacks remain protected; reference release touches existing
registry/free-list slots and does not allocate or invoke Lua. Stack and lifecycle
guards balance on failure. Generic routes and DSV/upload completion use this same
adapter; producer chunks remain incremental without new payload materialization.

The actual-CLI source-lifetime unit uses a custom Lua allocator, armed only in the
read callback immediately before returning its numeric or oversized string chunk.
Both Lua allocation attempts (including the retry after emergency GC) fail in
each case. Each proves one close while allocation is still limited, unchanged
FD count, removal of the observed spool path, collected weak app/callback roots,
zero stack/guard depth, and healthy conversion after a controlled disarm. The
unit also preserves normal numeric chunks, nil/false EOF, oversized pending
chunks, pending reset, malformed results, getter failures and close reentry.
Normal/packed service cases separately prove numeric/table read/reset errors
through routes, DSV and upload completion, twelve exact-once source closes, four
upload cleanups and healthy requests after each failure. Service fixtures do not
inject Lua allocator OOM; allocator evidence comes from the native unit.

Current focused verification, all serialized:

- `cmake --build --preset debug --target vectis_failures_fast -j1`: passed;
  existing C89 core policy retained (CLI/actual-CLI unit remain project C99).
  `build/lua-seam-audit/source-oom-build.log`.
- Seven debug CTests: actual-source unit, normal/packed lifetime fixture, DSV,
  HTTP, app restart and failure masks 0/15: **7/7 passed**.
  `build/lua-seam-audit/source-oom-final-debug.log`.
- Strict native Valgrind actual-source unit: **zero errors; zero bytes/blocks at
  exit**, no suppressions, definite/indirect/possible leaks are errors, origin
  tracking and error exit 99 enabled.
  `build/lua-seam-audit/source-oom-memcheck.log` and
  `build/lua-seam-audit/source-oom-memcheck-output.log`.
- ASan/UBSan fast target rebuilt with `-j1`; source unit, normal/packed fixture,
  OPC UA cleanup, audio validation, SUS lifetime and supervisor cohort:
  **6/6 passed**, `detect_leaks=1:abort_on_error=1`, `halt_on_error=1`.
  `build/lua-seam-audit/source-oom-asan-build.log` and
  `build/lua-seam-audit/source-oom-asan-tests.log`.
- Focused formatting, Python syntax, documentation readback and diff whitespace:
  `build/lua-seam-audit/source-oom-readback.log` and
  `build/lua-seam-audit/source-oom-diffcheck.log`.

These focused checks preceded the fresh whole gate below. The unchanged
full-service Kore Memcheck baseline was not repeated. Native Darwin (including
the unit's Darwin FD-path branch), optimized musl and live-provider/device/endurance
gates remain unrun. The final independent review traces protected native returns,
explicit ownership and cleanup and reports no actionable findings. Existing
upstream limitations remain as recorded below.

## Completed independent review and final verification

The first independent whole-candidate review found four issues, all fixed in
the remediation above. A second independent review identified the remaining
source allocation-error boundary; the final read-only re-review of that fix is
clean, with **no actionable findings**. Its scope and limitations are preserved
in `build/lua-seam-audit/source-oom-final-review-result.md`. The reviewer traced
the source and recorded evidence; it did not run additional tests.

After that last executable change, the parent ran the standard gate serially:

```sh
CMAKE_BUILD_PARALLEL_LEVEL=1 CTEST_PARALLEL_LEVEL=1 \
TMPDIR="$PWD/build/lua-seam-audit/tmp" \
make -j1 finalize-slice CTEST='ctest -j1'
```

**Exit 0: 35/35 failure-fast checks in 16.05s and 176/176 broader checks in
248.39s**, zero failures and zero CTest skips. Formatting, lifecycle, toolchain,
target, facade and privacy contracts passed. Exact output is retained in
`build/lua-seam-audit/parent-finalize-slice.log`. Internal opt-in omissions listed
above still apply; successful CTest exits do not establish live coverage.

The final correction changes CLI source handling, not core SDK code, public
headers, Lua modules or package inputs. Therefore the earlier installed
static/shared consumers (three consumers plus 36 example closures per mode),
export policy and genuine ordinary-Lua source-rock checks remain applicable to
those unchanged artifacts. The changed CLI has fresh whole-gate, focused
ASan/UBSan and strict native Valgrind evidence above. No claim of a repeated
full-service Memcheck or optimized musl build is made.

The real source fixture now resolves executable/script arguments and runs Kore
in its build fixture directory. Normal/packed and ASan/UBSan fixture checks
passed after this containment change; no generated root `kore.pid` is retained.
The final worktree inventory contains **84 coherent task-owned source, test,
documentation and policy files** (54 modified, 30 new before staging).
Ignored `build/` evidence and self-contained `stash/` reports are excluded from
the commit. Documentation readback and local links are checked before commitment.

This closes the local implementation review and verification loop. It does
**not** approve a release: required upstream contracts remain unresolved,
full-service Memcheck remains nonclean, and native Darwin execution is unrun.
The four new database/PDF/PNG facades remain a separate task as instructed.

## Required upstream contracts and reports

`stash/` contains single self-contained reports with versions, embedded source,
expected/current behavior and no attachments:

- `liblockdc-lua-empty-dequeue.md`: native `LC_OK,NULL` vs Lua truthy closed handle,
  reproduced for both single variants; public empty-batch workaround.
- `liblockdc-lua-batch-message-terminal-leak.md`: measured 400-byte native message
  loss plus retained client after batch ack; public client terminal+close workaround.
- `liblockdc-pouch-stateful-renew-completion.md`: renewal and checkpoint write
  succeed, then deferred completion fails; nonrenewed controls succeed.
- `liblockdc-supervisor-gaps.md`: required upstream contracts for true pull reader,
  native automatic renewal and kind-filtered intake; optional reusable enhancements
  for candidate-ready FD, standalone logger/endpoint integration and future
  Vectis canonical shared leadership. Public leases/transactions can implement
  the latter locally.
- `kore-strict-memcheck-baseline.md`: confirmed unchanged native message padding
  and worker-respawn pool-name ownership mechanisms; embedded minimized C
  reproduction, actual service traces and qualified Lua/fork-exit diagnostics.
- `cpkt-sus-per-model-logging.md`: global setter cannot preserve distinct per-app
  backend ownership; explicit global owner queue delivered without implicit routing.
  Source tracing confirms an additional upstream continuation-severity race;
  static local hook installation does not fix that pre-callback field.

Push streaming is delivered; a pull `payload_source` is not. Manual stateless
renewal is delivered; blocking-handler automatic renewal is not. Stopping one
kind pauses its workflow; different workflow identities remain independent.
Shared runtimes are rejected as a future Vectis-owned mode. Public client
leases and transactions can implement canonical identity and leadership; no
upstream impossibility is claimed. Native recovery discovery defaults to 30
seconds; zero is explicit opt-out and a readiness probe is not discovery. Long
stateful handlers remain blocked by the reproduced renewal/completion defect.

Delivered current serial behavior, deliberate future local phases (isolated
lanes, quotas, restart policies, shared leadership and managed request/reply),
and unmet current public contracts (pull source, blocked-handler automatic
renewal, kind selection and renewed stateful completion) are distinct. The
full authoritative specification remains intact; future work is not silently
accepted as implemented configuration. The unmet current contracts prevent
claiming the full product promise or passing its release gate. Required upstream
fixes or an explicit authority decision must resolve those contracts. Future
local phases do not acquire an invented upstream prerequisite.

### Owned gzip FILE receiver slice

`<vectis/gz.h>` / `src/vectis_gz.c` map public gzopen/gzopen64, gzdopen,
gzbuffer, gzsetparams, gzread/gzwrite, gzfread/gzfwrite, gzputs/gzgets,
gzgetc/gzgetc_, gzputc/gzungetc, gzflush, gzrewind, gzseek/gztell/gzoffset
and applicable *64 forms, gzeof/gzdirect, gzerror/gzclearerr and
close/read-close/write-close. Lua registration uses only that public receiver;
CLI registration, common static/shared sources, install headers and exports
are wired. No private native layouts, borrowed Lua provider userdata or
provider-to-Lua calls. FD ownership and close failure semantics remain native.

Focused evidence: `vectis_unit_gz` includes descriptor transfer/failure and
post-advancement deterministic Lua OOM cleanup. `vectis_lua_gz` uses the
normal/Linux-packed upgrade harness for binary/empty/partial-element flow,
line/character operations, params/flush, buffer timing, offsets, native direct
reads, concatenated members, resumed EOF, corruption/truncation and cleanup.
This is a bounded implementation slice, not the whole-goal semantic review.
Typed gzprintf/gzvprintf, raw stream/config/callback families, CRC-table and
wider native checksum domains remain pending. Full shared/platform/package
consumer checks remain with the parent goal; source rock requires the native
embedding facade as before.

Focused receiver verification: `cmake --preset debug` and
`cmake --build --preset debug --target vectis_bin vectis_unit_gz -j2` passed.
`ctest --preset debug -j1 --stop-on-failure -R
'^(vectis_unit_gz|vectis_lua_gz)$'` passed both tests (including normal/Linux
packed Lua). The focused Valgrind probe exits 99 with one uninitialized-value
branch in installed zlib 1.3.2 `gztell64`, originating in native `gz_open` malloc.
It reports 501 allocations/frees and zero live bytes; no leaks. The pure public
provider reproducer `vectis_unit_gz --native-tell` (gzopen, gzwrite, gztell,
gzclose; no receiver calls) independently produces that same error, with nine
allocations/frees and zero live bytes. Logs are in ignored
`build/gz-valgrind-origins.log` and `build/gz-native-valgrind.log`.
Writer-offset memory correctness remains an unresolved upstream requirement;
no private-layout initialization, hidden seek or Valgrind suppression is used.
The header/API compiles under the strict project C89 flags; there is no C89
availability blocker. This provider defect blocks clean memory/release evidence,
not declaration coverage. Parent investigation/fix is required before approval.

### LQL component implementation evidence (2026-10-07)

This implementation record supersedes the prior LQL gap inventory for the local
Vectis binding. The upstream 0.3.0 leak reproduction remains historical provider
binding evidence: its unadapted spooled callback leaked 65,632 bytes after OOM.
The standalone source is now tracked at `tests/unit/test_lql_callback_oom.c`;
the self-contained upstream report remains in ignored stash. No report was sent.

The candidate is the uncommitted task-owned delta on branch `init`, over the
parent's existing worktree. LQL native/Lua release is 0.3.0 and Lua is 5.5.1.
Exact source, header and archive pins are recorded in `patches/lql/README.md`.
CMake validates both installed hashes, applies a zero-fuzz local MIT patch under
build/generated, and compiles it instead of the unchanged SDK Lua source.
Native liblql, caches and adjacent repositories are unmodified.

The component maps all 41 receiver fields, ten public free operations and nine
callback types through public `lql/lql.h`. Direct `lql` and `vectis.libs.lql`
remain identical. Public term/config/result fields and enum constants are
mapped; lengths and any_count derive from strings/dense arrays with explicit
count agreement checks. Size/time/int domains are validated before narrowing,
with exact decimal strings beyond Lua integer range. Callback user contexts
are pinned Lua state/closure captures, never numeric native addresses.

Protected adapters include reader/writer/replay/value/decision/cancel/clock and
mutation open/read/close, covering allocation and returned-error interpretation.
Pre-acquisition cleanup tokens and operation allocation/reference ledgers prevent
wrapper/setup OOM leaks. Child tokens retain the public receiver through cleanup;
explicit close/manual GC invalidate shells immediately and defer native disposal
while active. Same-client reentry is rejected. Cursors check retained owners and
callback value/writer shells expire on every return, including Lua OOM. Native
FILE requests borrow public Lua IO resources and reject close during use.

The manual now documents usable builders, all term fields/inspection, callbacks,
options, direct FILE serialization/filtering and explicit materialized/spooled
names. Repository callers migrate the removed ambiguous capability spelling.
File-value open/close are explicitly paired; normal/failed/stopped paths retain
first errors. True streams retain caller replay and bounded chunks, emit before
EOF, reject root arrays and return native UNSUPPORTED for projection/mutation
and wide pre-read shapes. No fake stream or silent spool fallback was added.

Focused commands (serialized, debug only):

- `cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug`.
- `cmake --build build/debug --target vectis_bin vectis_unit_lql_callback_oom vectis_unit_lql_allocation_failures -j2`.
- Exact CTest selection: `vectis_lua_lql_public`, `vectis_lql_binding_provenance`, `vectis_unit_lql_callback_oom`, `vectis_unit_lql_callback_oom_stream`, `vectis_unit_lql_allocation_failures`, `-j1 --stop-on-failure --output-on-failure`.
- Focused Valgrind: `--leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=definite,indirect --error-exitcode=99`, on the standalone fixture and allocation test. Zero live bytes and zero errors after VM shutdown.

The Lua marker runs in normal and packed executables through
`upgrade_seams.cmake`; it asserts observable behavior in every operation family,
not just API presence. Allocation probes cover all callback types and 24 growth
fault positions in 17 acquisition/materialization families, including retained
alias expiry after OOM. Provenance probes distinguish source drift from header
drift. Scratch transcripts are under `build/lql-binding/`.

This is component implementation evidence, not the mandatory final whole-goal
semantic review or release approval. Full suite/cross/platform/service/package/
source-rock and final parent review gates were intentionally not run. The source
rock remains Lua-only and requires the corresponding adapted native runtime.

### LQL follow-up implementation verification

Fresh focused checks of the saved binding delta exposed a double release of a
callback registry reference. Protected callback cleanup now removes the matching
operation-ledger entry before unref, preventing later callback slots from
aliasing. A regression alternates repeated nested value/replay writes with
failed replay and GC. The first-error fixture now proves a primary file-reader
failure survives a failing paired close callback; native spooled mutation closes
file-value readers before output delivery, so the former writer-before-close
fixture did not actually exercise the claimed ordering.

The five exact LQL checks passed in 0.40 seconds: provenance/source-header drift,
normal/packed public behavior, spooled and true-stream OOM probes, and allocation
failures. Fresh Valgrind of the repaired standalone spooled probe and the expanded
allocation test reports zero live bytes and zero errors. The allocation test
covers 17 acquisition/materialization families at 24 fault positions plus all
callback families. No full suite, cross build or final whole-goal review ran.

### MDF public surface and callback finding

The MDF component specification records all 29 free declarations, 15 usable
receiver methods, 28 options fields and nested allocator/emission/font records.
Native layout buffering needs separate bounded-working-set proof; buffered
pager-source and materialized string rendering must not masquerade as streams.
A standalone released-binding sink-allocation probe reproduces 12,640 leaked
native bytes after Lua OOM and VM close in about 0.69 seconds. The public C
renderer is not blamed for a normal sink-return failure: the released Lua
adapter allocates its output string before lua_pcall and unwinds across the
native frame. A complete self-contained upstream report is under ignored stash.
At this initial inventory stage, MDF binding repairs and mappings were pending.
The later MDF sections below supersede that implementation status. No cache or
adjacent repository was modified.

### MDF callback/result cleanup and metadata slice

A repository-owned, provenance-checked pair of MIT binding deltas now protects
source/sink/trace allocation and return interpretation, restores callback active
counters, and releases native materialized strings after result-copy OOM.
Whole-value render names are explicitly `render_materialized` in the core,
Lua module, terminal caller, examples, tests and manual. The adapted metadata
surface includes native theme count/zero-based lookup, all 49 numeric public
constants plus the version string, four native font dump variants and immutable
built-in font views with bounded offset reads. No complete font is copied when
constructing those views, and no provider-private state is inspected.

Seven focused debug checks passed in 0.66 seconds: source/core/header provenance,
normal/packed public behavior, existing output policy and rendering regressions,
MDF and terminal examples, and deterministic callback/materialized-copy faults.
Fresh Valgrind of the six-case allocation probe exits zero, with zero live bytes
and zero errors. Source geometry changes remain legal; source EOF/errors, sink/
trace status codes, font alias preservation, force/non-force behavior and EOF
are observed rather than inferred from function presence. Initial fixture
syntax errors were corrected before the passing result; no unresolved test
failure is hidden by these counts.

At this earlier slice, the remaining MDF work included allocator and emission-buffer
resource ownership, memory configuration, arbitrary font readers, buffered source
pager, explicit-sink variants and bounded native-layout proof remain required.
The remaining native table/layout buffering is documented, not approved as an
unconditionally bounded stream. No full suite, SDK/package/source-rock build,
cross/platform build or final whole-goal review ran for this slice.

### MDF captured policies and font-reader adapters

Raw-field configuration snapshots now pin borrowed strings/functions independently
of caller mutation and GC. Native zero int flags remain false; both memory
retention fields and scalar library-owned emission settings reach the C options.
These retention fields are not live working-set limits. Custom mutable emission
data and allocator hooks were pending at this slice; the later allocator/transfer
section supersedes that status. No supplied pointer is guessed.

Regular/italic font readers use retained Lua owners, protected allocation and
error interpretation, bounded chunk delivery and exact native offsets. One-shot
owners contain Lua resources only and remain rooted through renderer destruction;
persistent owners refresh callback thread affinity and reject close during active
font calls. Normal/packed tests cover fragmented binary delivery, nil/error,
over-capacity/empty/exception failures, original table mutation, collection,
creation in a collected coroutine and owner close attempts. Synthetic byte
fixtures prove embedding/callback semantics, not browser font decoding.
The callback allocation test now has eight cases, including one-shot/persistent
font-reader OOM, and fresh Valgrind reports zero live bytes and errors.

Native font emission requires a family; early fixture omissions were corrected
so the fault probes actually enter the font callback. No full suite, platform
build, SDK/package build or final whole-goal review ran. Allocator/mutable-buffer,
buffered pager, explicit-sink and bounded-layout contracts remained required at
this slice; the later allocator/transfer section records current layout evidence.

### Public byte buffers and borrowed MDF emission storage

At this slice, `vectis/buffer.h` supplied two C89 receiver shells and sixteen exported free
functions. Lua `vectis.buffer` covers explicit materialized allocation/copy,
zero-based bounded read/write, prefix-preserving resize, stable readonly/mutable
leases and subspans, parent close/GC survival and typed data capabilities. Native
pointer values never become Lua addresses. Root loading, optional-native source
rock handling, embedded resources, installed header/source and export seams are
updated. The core has no Lua dependency.

MDF accepts mutable buffer/span emission data by acquiring its own public C
lease through the Vectis-owned binding bridge. It does not inspect the other
module's userdata. Scalar/options validation precedes acquisition; oversized
capacity and readonly storage fail explicitly. Ownership transfer was pending
at this slice; the subsequent allocator/transfer section supersedes that status.
Native constructor failure and close release the lease after native destruction.
A one-shot success initially retained a lease until GC; the immediate-resize
regression caught that and now observes synchronous release. Parent or source
span close while a renderer lives cannot free its storage.

Seven focused debug buffer/MDF checks passed in 0.78 seconds, including three
normal/Linux-packed behavior harnesses and provenance. Native buffer faults
exercise each constructor/span allocation, resize preservation and count
boundary with zero-wait tiny fixtures. Fresh MDF allocation Memcheck (nine
cases, including leased result-copy OOM) exits zero: 5,807 allocations/frees,
zero live bytes and zero errors. The focused buffer C Memcheck also passed.
New bindings compile with strict C89; an ISO-only size formatter replaced a
C99 printf modifier before verification.

At this slice the supporting resource did not establish provider transfer/realloc
origins or custom allocator hooks. See the subsequent section for that evidence.
Full suite/static-shared consumer/
source-rock/platform builds and the final whole-goal review remain deferred.

### MDF allocator, transfer and bounded native workspace

The adapted binding maps public alloc/free pairs, optional realloc and native
allocate-copy-free fallback through owned public C buffer receivers. Accepted
Lua buffers are consumed; rejected handles stay usable. Realloc/free receive
readonly scoped views, whose leases expire after success, exceptions and OOM.
Protected cleanup releases physical storage even when a void free observer
fails, and retains the first diagnostic. Pending emission transfer freezes its
Lua owner until renderer construction succeeds or rolls back. Native growth and
free retire managed allocations exactly once. No provider-private layout or
numeric address is used.

GC testing exposed skipped free observers after Lua cleared weak owner-map
values. A preallocated, temporary operation pin now retains callback access
during teardown without introducing a permanent registry cycle. One-shot
source/sink/trace owners also replace registry refs that could outlive failed
option validation. Mutation, collected creation coroutines, callback reentry,
invalid origins, rejected buffers and immediate constructor rollback are tested.

Streaming/incremental paths cap native live workspace at 262144 bytes, charging
the full owned buffer capacity plus 512 bytes per allocation. Exact checked
accounting covers allocate, resize, free, transfer and rollback. Borrowed emission
capacity and lease overhead also count; unused zero-cap storage is not leased or
transferred. Source/sink/trace
chunks are at most 65536 bytes. A generated 130-byte producer processes over
1 MiB and emits from its first chunk before EOF. A pending full HTML table fails
with an explicit workspace diagnostic before EOF; the separately named buffered
layout completes the same source. No source is collected in the Lua adapter.

Ten focused debug checks passed in 2.85 seconds, including normal/packed public,
allocator and streaming behavior, output-policy/regression cases, native buffer
faults, both binding provenance checks and 13 deterministic VM allocation-failure cases. Fresh
allocation-case Memcheck exits zero: 9206 allocations/frees, zero live bytes,
zero errors. The working-set test uses tiny generated fragments and no delay.
The actual CLI allocator/ownership/GC fixture also passes Memcheck with 5155
allocations/frees, zero live bytes and zero errors. Callback contexts refresh on
all native receiver entry points and close, including invocation after creation
coroutine GC. Active cross-coroutine reentry is rejected while legal source
geometry changes remain usable.

Both binding stagers now patch disposable candidates and copy only changed
effective contents. Their zero-wait tests seed old timestamps and verify an
unchanged second configure preserves them. Unrelated configure no longer
recompiles the LQL binding during MDF-only debug work.

At this slice, remaining MDF work included fresh defaults, option/value mappings,
buffered source pager and explicit-sink forms. The next section supersedes its
defaults/configuration status. The complete
dependency sweep remains active. Full builds/platform/package/service gates and
the final whole-goal implementation review remain deferred as instructed.

### MDF native defaults, configuration and value domains

The adapted core/module now expose fresh native options defaults and all native
option names alongside existing convenience names, with explicit canonical
precedence. Default allocator function values become callable typed-buffer
operations without numeric pointer exposure. Canonical regular/italic face
records map format/data/data_len/read/userdata, and built-in immutable typed
faces can be reused without copying their fonts. Trace records map emit/userdata
alongside the function form. Callback userdata survives caller-table mutation
and is released at renderer close. Native renderer format and ANSI integers,
exact terminal descriptors/fallback ints, token lengths and levels are mapped.

Inspection found number-to-string coercion could produce unretained native
configuration pointers. C-string fields are now strict and reject embedded NUL;
font and token payloads remain binary-safe. Token text stays rooted through later
computed-field reads and native use. Native memory limits and streaming/buffered
names remain unchanged.

Eight focused debug checks pass in 2.74 seconds, including normal/packed option,
public, allocator and streaming behavior, rendering policy/regressions,
provenance and 16 deterministic VM allocation-failure cases. Fresh OOM Memcheck
shows 11513 allocations/frees, zero live bytes and zero errors. The actual CLI
option/default/font/trace fixture also has zero live bytes and errors, with 3085
allocations/frees. The tests use small fixtures and no waits.

At this slice, remaining MDF mappings were caller-sized path buffers, explicit
sinks and the buffered source pager; the next section supersedes that status. All other dependency
surfaces and final whole-goal build/review gates retain their original scope.

### MDF explicit sinks, path outputs and buffered pager

All six public explicit-sink free functions are mapped through temporary owned
callback scopes: token/finish/render/feed/flush/document EOF. Module and receiver
`*_to` forms preserve the bound sink and native state transitions. Source/sink
records preserve callback/userdata identity, capture raw fields against mutation
and use protected bounded callbacks. Caller-sized path outputs acquire public
mutable C buffer leases, validate capacity and release both on every exit.
Nullable path identity inputs retain native false/no-alias behavior.

The buffered source pager calls the public native pager without a second Lua
collection or staging layer. Pager ownership commits on a public origin free/
realloc event; native validation and early open/option/non-terminal failures
leave untouched origins usable. Event-driven PTY fixtures inject quit only after
the alternate-screen entry, with no sleeps, and verify EOF, embedded-NUL suffix,
terminal restoration, file/owned-buffer success and source/early-origin failures.
Both normal and packed executions pass; the native VM fault fixture additionally
exercises pager source OOM while terminal arguments are valid.

The new identity regression caught negative Lua stack indexes being evaluated
after temporary pushes, which caused an identical binding to reset pending
input. Indexes now become absolute before comparison. Another regression proves
a native-success rebind retains its installed callback even when allocator
cleanup failure is reported afterward; configured one-byte retention limits
make that failure deterministic. Native synchronous rendering requires reset
after an incremental document, which the mixed-workflow fixture now respects.

Ten focused MDF checks pass in 3.69 seconds, including normal/packed I/O/pager/
streaming behavior and 19 ordinary plus one terminal-only VM allocation-failure
cases. Fresh ordinary fault Memcheck has 13811 allocations/frees, zero live bytes
and zero errors; CLI I/O behavior also has zero live bytes/errors. The later
nullable/non-terminal/rebinding regressions pass their focused normal/packed
harness. Installed libmdf exposes only mdf.h; its 29 free declarations, 15 usable
receiver methods, records and constants have mappings. No symbol count or this
component evidence approves the final whole-goal semantic/platform/package/
consumer gates, which remain deferred until all surfaces are implemented.

### Raw zlib receiver foundation

A reviewed supplemental specification records the entire remaining raw-stream
contract. The new public C89 shell owns native init/basic/extended/version-size,
step, copy, end and destruction, with native statuses separate from ownership
errors. Chunk buffers are bounded, overlap-checked and borrowed only for one
step. NEED_DICT/BUF_ERROR remain recoverable. Native allocator copy faults and
close during active copy callbacks preserve source lifetime.

Lua maps eight native constructor forms, independent copy/end, scalar public
fields and dictionary/reset/keep/tuning/bit/sync/validation controls. Bounded
output and dictionary values have explicit materialized names; native capability
maps to its typed owner, never an address. Private state/reserved are not public
caller-owned fields. Kind checks prevent an inflate operation from silently
becoming deflate, and sizeof(z_stream) is exposed instead of guessed per arch.
Protected result/field construction contains OOM and GC cleanup.

Three focused debug checks pass in 0.29 seconds, including normal/packed Lua and
four deterministic VM allocation faults. Native state and VM-fault Memcheck
runs have zero live bytes/errors (24 and 1741 allocations/frees respectively).
The later injected native copy failure also preserves its source. Raw typed IO,
parameter output, bounds, headers, Lua allocator ownership, back-inflate and
remaining checksum/gz contracts are still implementation work. This partial
surface does not approve the whole-goal release gate or remove any requirement.

### Raw zlib retained IO, parameter output and bounds

The public C89 raw shell now retains independent typed input/output leases,
queries current cursor spans, validates native availability and runs against
bound resources without payload collection. Native copies retain both leases;
closing parent C/Lua handles cannot invalidate copied pointers. Bounds/overlap,
readonly output, invalid replacements and unbind/end lifetimes are checked.
Borrowed-chunk process refuses to overwrite retained bindings.

Lua next_in/next_out map to owned typed views of their native next/avail region,
not addresses. Buffer bridge ownership stays inside the Vectis binding. Protected
field construction roots each adopted C span before the next Lua allocation;
Oom tests include failure after a first view allocation and confirm final lease
release. deflateParams emits into the real bound output and preserves BUF_ERROR
retry; bounds preserve exact native uLong/size_t domains and unsigned arithmetic.
Z_SIZE_MAX/Z_ULONG_MAX avoid fixed arch assumptions in fixtures.

Four focused debug checks pass in 0.45 seconds, including normal/Linux-packed
raw state and retained-IO behavior, native C consumer lifetime/copy and seven
VM allocation-failure cases. Native receiver, OOM and CLI IO Memcheck each show
zero live bytes/errors; the final OOM probe has 3933 allocations/frees and CLI IO
has 1593. Fixtures are tiny and immediate. Typed IO, params and bounds are no
longer remaining integrations; gzip headers, Lua allocator ownership, back-
inflate, CRC/checksum domains and gz varargs remain in the full scope. Final
platform/package/consumer/whole-surface review gates stay deferred.

### Zlib gzip-header records and ownership

All 13 public gz_header fields now map through a dedicated C89 header shell and
Lua typed owner. Native mutable capture and readonly write resources retain
independent byte leases. Shared native copies retain one header record exactly
as the public native copy does; closing its shell/parents does not invalidate
that record. Scalar and used-capacity domains, string terminators, native NULL
replacement, truncation and actual extra_len remain observable. Protected
constructors/getters reconcile temporary/native spans after failure/OOM.

A pure installed-provider C89 probe establishes deflateReset retains the custom
header although zlib.h says it restores defaults. Upstream v1.3.2 tagged source
corroborates retention. The self-contained report is
stash/zlib-1.3.2-deflatereset-gzip-header-bugreport.md; no wrapper/private layout is
needed for reproduction. The adapter retains deflate references until explicit
nil detachment/end and releases successful inflate reset/keep/reset2 capture.
This is a documented native mismatch, not a hidden compatibility shim.

Five focused debug checks passed in 0.56 seconds; later native header lifecycle,
constructor/getter OOM and explicit empty-field regressions pass their selected
checks. Normal/packed tests cover fields, truncation, CRC rejection, shared copy,
parent close, native discard/replacement and failed construction. Fresh native,
nine-case VM OOM and actual CLI header Memcheck all have zero live bytes/errors
(101, 5066 and 1825 allocations/frees). An initial C fixture embedded a NUL in its
Lua source and failed parsing; constructing that byte with string.char corrected
the fixture before the passing evidence. No delay-driven test was added.

Header mappings are implemented; custom Lua allocators, back-inflate, checksum/
CRC-table/vararg gaps and all other dependency surfaces remain. Final full builds,
platform/package/consumer and whole-surface review gates remain deferred.

### Back-inflate and owned buffer write locks

Public C89 back initialization/callback/end and Lua native names are mapped.
The window retains an independent mutable lease; a private owned write lock
prevents mutation through every checked Lua alias during input/output callbacks.
Buffer locks themselves have C/Lua receiver mappings, retain storage, nest
independently and permit reads and parent close. Native callback close/reentry,
overlapping input/window and invalid/readonly/ABI windows are rejected.

Lua uses one bounded input buffer, protected raw callback/userdata captures and
immediate window-sized writer delivery. Unused bytes become retained typed input
before returning; no C stack or transient string pointer survives. Results have
checked uint64 counts and typed unused input, with bounded explicitly materialized
unused bytes. Multiple complete raw streams, source versus sink failure, corruption
and first Lua error identity (including false) are observed. Every native unwind
restores locks; back state uses inflateBackEnd and is excluded from ordinary
inflate/reset/copy/header paths.

Five focused checks pass in 0.67 seconds, including normal/packed 45 KiB window
output, second-stream unused-input handoff, source/sink/error-object cases,
window alias locks, direct C callbacks and nested lock allocation/lifetime faults.
The 12-case VM OOM probe covers back construction/input/output in addition to
prior raw/header/IO faults. Native, VM-fault and actual CLI back behavior Memcheck
all have zero live bytes/errors (18, 6924 and 2062 allocations/frees). The C fixture
initially used a const-discarding legacy in_func cast; a C89-compatible pointer
representation copy corrected it before the passing evidence.

At that slice back-inflate was implemented; allocators, checksum/CRC-table and
vararg domains remained. The following sections supersede that status.
No full platform/package/consumer build or final whole-goal review ran here.

Later validation-failure inspection moved callback capture into the protected
back driver. Partial capture now releases its graph without invalidating an
unadvanced owner; failures after native entry close advanced state. Explicit
owner close also clears captured values. A weak-reference regression proves
captured userdata is collectible after invalid sink-record rejection while
the raw owner remains reusable. The selected five checks pass in 0.44 seconds;
fresh CLI back Memcheck remains clean with 2115 allocations/frees and zero live
bytes/errors. Native and prior 12-case OOM code paths retain their passed proof.

### Protected zlib Lua allocators

The reviewed raw-stream allocator contract is implemented through public C89
hooks and Vectis-owned typed buffers. Constructor records capture the paired
callbacks and opaque. Accepted allocations consume unleased owned handles;
rejections preserve caller ownership. Scoped readonly free views expire even
on Lua OOM, and physical cleanup runs after observer failure. Native copies
independently root their shared context. Reentry/close through siblings is
guarded, the invoking coroutine is refreshed, and temporary GC teardown pins
avoid a permanent registry cycle. First error identity survives native unwind,
including false. Review removed retention of a reported error graph on a
reusable parent; a weak-reference regression proves collection while it stays open.

Six focused checks passed in 0.82 seconds after that repair, including normal/
packed allocator behavior and the then-16-case VM OOM probe. Earlier allocator
memory records are superseded by the enlarged 19-case probe below.

### Native zlib buffers, checksum domains and readonly table

Public C89 helpers and Lua mappings cover all eight compress/uncompress buffer
variants, both compression bounds, native uLong checksum/combine/operator
domains, uInt versus size_t input widths and readonly provider CRC-table values.
Temporary leases surround protected setup/results; no payload is copied or
staged. Native codes, partial output and uncompress2 consumption remain visible.
Overlaps, readonly/locked output and invalid widths fail before writes. Empty
typed buffers remain distinct from NULL checksum initialization even though
their underlying zero-capacity allocation has a NULL pointer. The independent
table shells validate all 256 entries without exposing addresses or mutations.

The pinned provider explicitly returns zero for a zero CRC operator and masks
checksum operands to 32 bits. Reference inspection corrected an initial wider-
result test assumption and the proposed zero-operator rejection. Nonzero
operators with only high bits violate native multiplication's termination
precondition and are rejected before entering it. Wider meaningful arguments
otherwise retain native normalization, as direct C reference tests demonstrate.

Ten affected debug checks passed in 3.65 seconds (normal/packed streaming, raw,
IO, headers, back-inflate, allocators, caller buffers and direct native/VM tests).
Native receiver, 19-case VM allocation-failure and actual CLI buffer fixtures
passed strict Memcheck: 165, 12139 and 2159 allocations/frees respectively,
zero live bytes/blocks and zero errors. Fresh tiny offset-boundary and pager
feedback-path refinements receive the additional focused evidence below.

Typed gzip printf/vprintf remains implementation work; all other dependency
gaps remain in the parent scope. These component checks do not approve the
whole-goal semantic, platform, packaging or release gates.

### Pager test ownership under updated lifecycle guidance

The newly added Python PTY driver was replaced by a compiled C89/POSIX test
under the lifecycle skill's prohibition on Python test controllers. CMake/CTest
still own invocation, assertions and normal/packed selection. The fixture uses
tiny output, event-driven quit, a bounded failure deadline, terminal readback
and exact child cleanup. The OOM case now recognizes its own success marker
before waiting, removing an avoidable deadline wait when child exit races poll.
Existing unrelated Python lifecycle tests were not extended by this repair.

The refined pager check passes in 0.76 seconds; caller-buffer/native boundary
checks plus pager pass 3/3 in 0.96 seconds. Embedded manual readback confirms
the final allocator and native-buffer sections. Fresh actual-CLI allocator
Memcheck passes with 2976 allocations/frees, zero live bytes/blocks and errors.

### libpslog public contract and native-owner OOM repair

The 0.12.0 public native header, Lua binding and public Lua/C interop header were
inspected; the reviewed component contract is `pslog-public-facade-spec.md`.
The existing Lua module does not cover all native config/output/observation/
palette/field/callback/interop families. Its manual no longer claims the complete
native surface is already exposed.

A standalone released-source probe reproduces a derived-owner OOM leak without
linking Vectis: 312 definite and 4,953,576 indirect lost bytes after complete VM
teardown; all VM allocations were freed. The single self-contained report is
`stash/libpslog-lua-derived-owner-oom-bugreport.md`. Callback setup before pcall
and other parsing/lifetime findings remain source-review concerns to reproduce.

A repository-owned binding delta protects the complete adoption of newly
acquired loggers and destroys the handle before propagating adoption failure.
It covers userdata/metatable/callback-rooting setup, preserves existing parent/
coroutine ownership and leaves native libpslog/public interop ABI unchanged.
Staging checks source, native header and interop header identities, uses zero
fuzz and does not rewrite unchanged effective source. SDK caches were not edited.

Five constructor/derivation OOM families prove recovery and cleanup. Normal/
packed child callbacks retain the creator coroutine through parent collection
and release it after child collection. These two focused tests pass in 0.40
seconds. The five-family native memory probe has 2142 allocations/frees, zero
live bytes/blocks and zero errors. This is owner-repair proof, not complete
libpslog callback or public-surface coverage. Remaining pslog and other component
gaps continue under the full parent specification; full platform/package/release
checks remain deferred until that implementation is complete.

Final focused pslog checks (provenance, normal/packed ownership, native-only
sink qualification, repeated app seed cleanup and owner OOM) pass 5/5 in 0.63
seconds. Actual CLI callback/coroutine ownership also passes strict Memcheck:
1611 allocations/frees, zero live bytes/blocks and zero errors. No cross build,
full suite, release or push ran. The broad implementation remains uncommitted
on `init`; these records are component evidence only.

## Authorized LQL 0.4.0 follow-up

The current upgrade specification and native/Lua/packaging seam review are in
`liblql-0.4.0-upgrade.md`. Earlier LQL 0.3.0 findings and counts are historical;
this consumes the released 0.4.0 facade without the withdrawn local extensions.

## Musl worker-stack follow-up

See [the stack review](musl-stack-review.md) for the review after `1068b39`.
The prior main-thread and LQL-only worker evidence did not establish stack bounds
for every owned binding or filesystem workflow. This follow-up checks compiled
frames and representative native/Lua call chains on default musl pthread stacks;
it changes no public ABI and patches no upstream facade.
