# Useful dependency Lua implementation progress

Goal created 2026-10-28. Specification/review commit: `9fb3265`.
This record tracks the goal; it is not a release approval.
Component sections are historical implementation checkpoints. The final section
and implementation review provide current completion status and evidence.

## Scope and dependencies

Existing upstream Lua facades remain unchanged. The user subsequently authorized
the released LQL 0.4.0 upgrade; see `liblql-0.4.0-upgrade.md`. SSH stays deferred. LZO was explicitly deferred by the user.
Archive is blocked because cpktmisc 0.2.1's public static/shared archive targets
link LZO transitively. No SDK patches or link-closure bypasses were made.

## SQLite standalone receiver

Implemented the public C SQL values/SQLite receivers and embedded Lua modules
`vectis.sqlite`/`vectis.sql`, including root exports, statement reuse, exact signed
integers, NULL, text/blob binding, transactions, parent-child invalidation and
process/thread guards. Public imported static/shared SQLite targets supply the
header/link closure. Source rock registration and manual/example are updated.
App-owned declarations now capture copied configurations before startup,
materialize after start/fork, invalidate children on stop/restart and detach
on app close. Native lifecycle preflight rejects SQL callback reentry and Lua
operations retain the existing app callback guard across protected copies.

Focused debug evidence under `build/useful-lua-facades/`:

- Public header compilation and native/CLI builds passed.
- Native behavior checks passed; normal/packed Lua checks passed after fixing
  an initially missing native `vectis.sqlite` root export.
- Lua allocation-failure sweep passed: 49 injected failure positions and 719
  successful executions, plus deterministic argument mutation/parent close at
  userdata allocation boundaries. Strict Valgrind reports zero errors and zero
  bytes at exit across 591324 allocations/frees. No suppression was used.
- Expanded file/readonly/contention and SQLite app lifecycle native/normal/packed
  tests pass. Source-rock smoke and the affected seven-test selection pass.
  The final affected selection passed 18/18 (6.95 s), including C++ headers,
  four fork-exit cases, app log cleanup, upstream-byte checks and source integrity.
- Native app lifecycle memory checks report zero bytes at exit; strict native
  failure checks also report zero bytes at exit across 888 allocations/frees.
  Embedded SQLite manual matches its current source bytes.

Component review fixed stale argument pointers across Lua allocation, protected
app declaration construction before native app-pointer capture, preserved primary
and secondary transaction diagnostics, rejected silent reopen after unknown
cleanup outcome, reported not-started outcomes, and avoided replacing the SQLite
authorizer per prepare (which would force recompilation of reusable statements).
Tests assert reuse without native reprepare, latched native allocation/SQL errors,
explicit restart after cleanup failure and captured lifecycle violations.
Static/shared installation and platform gates remain part of final aggregate
verification; this component evidence is not approval of the entire goal.

## PDF reports

Implemented public C89 PDF document/page/font/image receivers and embedded Lua
`vectis.pdf`. File and materialized-memory PNG/JPEG loaders, embedded TrueType,
UTF-8 font selection, metadata, text/geometry and explicit file/materialized
output use only the public adapter. Native errors latch until explicit reset.
Child closure preserves native document content; document closure invalidates
children. Original tiny image/font fixtures avoid system-font dependencies.

Focused native and normal/packed behavior plus Lua allocation failures pass
(3/3 in `pdf-memory-input-tests.log`). Memory-image tests poison/free their
encoded input before PDF serialization, verifying native ownership. Native
process/thread tests reject foreign use. Final native Valgrind reports zero
bytes at exit/zero errors (`pdf-native-memory-final.log`); strict Lua fault
Valgrind also passes (`pdf-oom-memory-final.log`). The initial native Valgrind
invocation used all leak kinds in a deliberately forked child, whose inherited
live objects changed its exit status; the completed rerun uses definite/indirect
leak criteria and still checks invalid accesses. No suppression or SDK change.
The affected PDF/native header/preload/source checks pass (`pdf-final-tests.log`
and `compression-cli-tests.log`). Lua fault memory evidence covers 34 injected
failure positions followed by clean execution (23419 allocations/frees).

## Compression

Implemented C89 `vectis/compression.h` receivers using public cpkt adapters for
Zstandard, LZ4 frame and XZ. Embedded/root/source-rock `vectis.compression`
provides process/finish sinks, bounded reader-to-writer helpers and named
materialized output. Exactly one frame, latched failures, explicit completion,
codec option applicability, XZ level zero versus omission and process/thread
ownership are enforced. Existing zlib is unchanged. No LZO binding/link added.

Native end-to-end pipelines pass 1 MiB of deterministically generated
incompressible input through encoder and decoder into a verifying sink, with
observable delivery before finish and <=64 KiB callback chunks. The original
extremely compressible fixture did not fill a bounded codec output buffer before
finish, so it was replaced with a fixture that can falsify hidden whole-input
collection. One-byte decoding, empty frames, errors, truncation/trailing bytes,
sink rejection, active close attempts and fork/thread refusal pass. Normal/packed
Lua behavior and decoder memory/window propagation also pass. The affected
seven-test selection passed in 1.25 s (`compression-cli-tests.log`). Final
component Valgrind checks report zero bytes at exit/zero errors: native 199
allocations/frees, Lua fault sweep 59 injected failures followed by a clean
execution (27680 allocations/frees). See `compression-native-valgrind.log` and
`compression-oom-valgrind.log`. Source-rock smoke passes for SQLite, PDF and
compression (`pdf-compression-rock.log`). Formatting passes; embedded manual
readback passes for all three manuals. The final affected selection passed
15/15 in 6.44 s (`sqlite-pdf-compression-final-tests.log`), including unchanged
upstream source/byte checks and error-shape assertions.

## PNG image interchange

Implemented public C89 PNG row receivers and embedded/root/source-rock
`vectis.png`. Files and protected bounded callbacks support noninterlaced RGBA8
reads, RGBA/RGB/gray8 writes, selected metadata and configured limits. Explicit
materialized decode also handles interlaced input; encode takes packed pixels.
Native setjmp boundaries never cross Lua callbacks. Failed row/metadata copies
close the receiver; callback errors retain identity; native errors stay latched.

Native tests use an independent public-libpng producer for palette/tRNS,
interlace and 16-bit gray/alpha inputs. Normal/packed tests also prove copied
metadata, file/callback I/O, explicit EOF/trailer checks, early close without
callbacks, large-image row delivery before source EOF and generated PNG-to-PDF
composition without staging. A callback test was corrected to accept failure
at write_row or finish, since libpng can deliver output at either call.
Lua fault sweep passes 64 failure positions plus clean execution; strict
Valgrind reports zero bytes/zero errors across 31382 allocations/frees
(`png-oom-valgrind.log`). Native memory/process/thread tests also report zero
bytes/zero errors across 577 allocations/frees (`png-native-valgrind.log`).
Source-rock smoke, embedded-manual byte readback, formatting and diff whitespace
checks pass (`png-rock.log`, `png-manual.log`, `png-format.log`). The combined
SQLite/PDF/PNG/compression affected selection passes 18/18 in 8.09 s
(`sqlite-pdf-png-compression-final-tests.log`). The generated debug link graph
contains neither libarchive nor liblzo. Aggregate installed/static/shared and
platform gates remain pending for the whole goal.

## PostgreSQL standalone/app ownership

Added independent public C connection/cursor/prepared-view receivers in
`vectis/pg.h` and `src/vectis_pg_connection.c`, preserving existing XA shell
layouts. Connection parameters are copied, sslmode defaults to verify-full,
connect progress/readiness and blocking budgets are explicit. Single-row checked
retrieval preserves text/NULL/OID/format, late SQL errors, drainage and failures.
Local transaction outcomes and copied lazy app declarations are implemented.
Inherited live resources reject cleanup that could send Terminate to the parent's
session. Cursor abandonment invalidates its connection without network drainage.

The first real rootless Podman Kube run passed native standalone behavior and
unchanged XA normal/packed behavior (`pg-native-live-tests.log`). Native checks
include exact bigint text/binary/NULL parameters, duplicate columns, prepared
reuse, a SQL error after a delivered row, commit/rollback, caught manual control,
escaped cursors, copied app lifecycle/fork/thread ownership, deterministic lost
COMMIT receipt (unknown outcome) and a zero-wait advisory-lock probe. The fixture
runner now accepts optional native/test-script paths so the same owned service
can exercise standalone and XA paths. The owned Lua binding is separate from
the existing XA core. New root/standalone/app declarations, dense exact params,
prepared views, readiness, protected row/metadata copies and transaction result/
error preservation pass normal and packed live tests (`pg-cli-tests.log`).
Review added first-SQLSTATE retention after caught errors and recognition of
acknowledged deferred-COMMIT failure without a redundant rollback. The native
and Lua tests cover both. The allocation sweep passes 73 failure positions.

Valgrind reports zero definite/indirect/possible loss and no invalid accesses.
Native and 74-state Lua sweeps retain the same 4235 bytes/37 blocks of reachable
process-global GSS provider state from libpq's default credential probe. The
initial all-reachable-bytes invocation failed only on these 32 global contexts;
the final all-lost-allocation-kinds check passes (`pg-oom-valgrind-final.log`).
No provider patch, global-default override or suppression was added.
The service rerun passes normal/packed Lua (`pg-memory-service-final-tests.log`).

Fast local normal/packed tests use invalid native option parsing without network
waits and prove copied app declaration configuration/failure restart. The
combined affected selection passes 19/19 in 7.69 s
(`sql-pdf-png-compression-final-tests.log`). Installed consumer link lists were
corrected to name every new public imported dependency target explicitly;
aggregate installation/platform checks remain pending. Source-rock, embedded
manual and formatting checks pass (`pg-rock.log`, `pg-manual.log`,
`pg-format.log`). The standalone live CTest passes in 6.69 s
(`pg-ctest-live.log`); the unchanged XA normal/packed regression also passes
(`pg-xa-regression.log`). No implementation commit yet; the whole goal remains
in progress alongside the preserved pre-existing migration changes.

## Reusable OpenSSL keys and materialized AES-GCM

Implemented public C89 `vectis/crypto.h` with owned PEM/file private/public/
certificate keys, repeated digest signing/verification, metadata and explicit
AES-128/192/256-GCM materialization. `openssl` adds only load_key and the two GCM
helpers; existing helpers remain intact. Native password callbacks never prompt
or enter Lua. Decrypt publishes bytes only after authentication; result-owner
guards release buffers on Lua allocation failure.

Native/normal/packed behavior and existing OpenSSL checks pass 5/5 in 1.06 s
(`crypto-cli-tests.log`). Public native fixture generation creates ordinary,
encrypted and certificate/public PEM inputs without a new host tool dependency.
Tests prove input ownership, repeated signing, old/new helper interoperability,
Vectis certificate generation -> reusable-key verification, known AES-GCM output,
all key widths, empty/binary data, AAD and altered key/nonce/tag rejection.
Native fork/thread guards and bounds also pass. Lua fault sweep covers 56
failure positions plus clean execution; strict Valgrind reports zero bytes/
zero errors across 62452 allocations/frees (`crypto-oom-valgrind.log`). Native
memory tests also report zero bytes/zero errors across 21697 allocations/frees
(`crypto-native-valgrind.log`). Final affected/docs/format checks follow.

## XML documents, XPath and caller-fed SAX

Implemented public C89 `vectis/xml.h`, separate DOM/SAX engines and Lua bindings.
Existing XML/LoneJSON mapping is preserved. Owned element views retain their
parent; mutation invalidates every prior view. XPath returns copied scalars or
ordered element arrays. Explicit materialized serialization and literal XML
UTF-8 text/attribute/child editing reject stale views and reserved namespace
conflicts. DTDs, external resources and unused schema/XSLT/node graphs are
excluded with concrete rationale in the specification/manual.

SAX uses a custom non-DOM handler and protected synchronous Lua callbacks.
Copied names, attributes and namespace declarations have explicit event limits;
text chunks stay bounded. Native and Lua tests prove delivery before document
completion and inputs larger than the materialized DOM ceiling. Callback
exceptions/returned errors retain identity, failures latch and caught receiver
reentry/close fails the outer call. Finish verifies completeness; GC/close
never calls user callbacks. Foreign process/thread access is rejected.

The affected native/normal/packed/C++ and existing crypto checks pass 6/6 in
1.53 s (`xml-cli-tests.log`). The Lua allocation sweep covers 88 failure
positions; strict Valgrind reports zero bytes/zero errors across 59203
allocations/frees (`xml-oom-valgrind.log`). Native memory/fork tests report no
lost bytes or invalid accesses; 41 bytes in two libxml2 diagnostic allocations
remain reachable in the parent (`xml-native-valgrind.log`). No suppression or
global-loader/provider patch was added.

## Curl sessions, multi and Vectis HTTP ownership

Implemented public C89 `vectis/curl.h` and a separate Lua binding, using only
public libcurl easy/multi/poll/completion/callback APIs. Session reset preserves
connection reuse and clears settings/results. Multi owns a bounded 64-request
active/completion queue; completion copying acknowledges only after the whole
batch succeeds. Read/write chunks are <=64 KiB, and callback response delivery
has no hidden body collection or cumulative limit. Headers remain <=64 KiB;
explicit buffered responses retain the existing 8 MiB bound. Read failures never
become EOF. Callback errors preserve identity and native failures remain copied.
Caught reentry/close fails the offending transfer without contaminating others.
File read/write/close failures are observable; cleanup never enters user Lua.
Foreign process/thread use is rejected.

The user explicitly confirmed ordinary transfer scope; MIME, SMTP, schema and
automatic retry remain on existing helpers. The reviewed spec records that
clarification. `vectis.http.session` owns a native session and copied defaults,
merges headers, normalizes HTTP/transport failure, and supplies request/verb and
reset/close methods. It is created after the caller's process/fork boundary.

Focused behavior passes 12/12 in 5.35 s (`curl-final-tests.log`), including
existing curl contracts, XML/crypto regression and normal/packed HTTP/TLS.
The local HTTP origin proves TCP connection reuse and real concurrent requests
with a pair barrier; TLS tests prove CA/verification option reset. Tests also
cover large buffered failure vs bounded callback success, binary files/body,
source/sink/cancellation, copied headers, per-transfer failures, early close,
capacity and Vectis status/ownership. No detached fixture services are created.

The first strict Lua memory sweep covered 137 allocation failures with zero
bytes/zero errors (`curl-oom-valgrind.log`, 77498 allocations/frees). Native
memory/fork checks report zero bytes/zero errors (`curl-native-valgrind.log`,
3644 allocations/frees in parent). The expanded completion-copy recovery test
also injects file-close failure: all queued results recover and the close error
survives failed copying (`curl-file-fault-tests.log`). Final aggregate memory,
package and review evidence follows; these counts describe their scoped runs.

## Remaining implementation

- Archive after the stated upstream dependency blocker is resolved.
- Final aggregate behavior, memory/service, package/static/shared and promised
  platform verification, documentation reconciliation and implementation review.


## Implementation pass complete; verification phase

All selected SQLite, PostgreSQL, PDF, PNG, Zstd/LZ4-frame/XZ, curl, OpenSSL and
XML surfaces are implemented, including native ownership, Lua integration,
Vectis-owned convenience seams, docs/examples and packaging declarations. The
user-authorized LQL 0.4.0 upgrade consumes its released facade unchanged. The
remaining archive scope is deferred with LZO; SSH and upstream provider changes
remain excluded. The implementation review records the final integration pass.

The user required completing the whole implementation before testing. The earlier
platform loop was stopped after six successful Linux builds and before a completed
Darwin build. Final verification is a separate phase; reuse valid unchanged-input
evidence, and do not start another broad matrix for small fixes.

## Final completion evidence

Implementation and verification of the agreed useful surface are complete.
The final serial debug suite passed 279/279 in 481.22 seconds. All seven targets
compile/link; six Linux targets execute the LQL, combined facade and updated
example checks, plus default/small native worker-stack probes. ARM runtime uses
QEMU. Darwin runtime is untested on this Linux host. The small-stack test was
corrected for aarch64 GNU's 128 KiB minimum, with focused debug verification;
product binaries and the completed platform matrix were not rebuilt for it.

No-Kore behavior, source-rock helper smoke, debug/Release embedded manuals,
GNU installed/relocated static/shared consumers and ELF/Darwin export closure
pass. Each installed/package mode proves runtime contracts for 36 executables,
in addition to the C/C++/pkg-config new-engine and native LQL 0.4.0 calls.
Final curl native and Lua fault memory checks have zero lost allocations/errors
and zero bytes at parent/VM exit. Component provider-reachable allocations and
the remaining upstream LQL closer-entry limit remain explicitly distinguished.

The implementation review contains exact current evidence and limitations.
The initial 264-path SDK/lifecycle/owned-Lua foundation is preserved in separate
prerequisite commit `cf0d818`. Its files support the dependency and verification
closure; the useful facade changes are committed separately. No unrelated new
path was identified. Scratch and local upstream drafts remain ignored under
`build/` and `stash/`. No push or release is part of this goal.
