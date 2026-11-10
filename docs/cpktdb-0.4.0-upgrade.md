# cpktdb 0.4.0 upgrade

## Reviewed specification

Upgrade baseline `f200c30` from cpktdb 0.3.1 to the seven released
[0.4.0 SDKs](https://github.com/c89systems/cpktdb/releases/tag/v0.4.0), commit
`97f9d1191a46a1e49f4e7113f805d469883a210b`. Released source SHA-256:
`4e6f41a82d871c7088383497236824f040d24ef9a2484938f9a8511959f58b2b`.
Retain cpkt 0.7.0 and all other pins. Preserve the staged `LICENSE` edit.
No release, tag, push, sibling edit or upstream facade modification.

Reviewed public headers, facade sources, manifests, recipes, documentation and
examples against released 0.3.1 source/SDK bytes. PostgreSQL 18.6 and iODBC
3.52.16 public interfaces, source digests and facade implementations are unchanged.
SQLite advances from 3.53.4 to 3.54.1; shared ABI majors remain unchanged.
The database SDK now requires compatible core >=0.7.0, already selected by Vectis.
Use imported targets and current extraction timestamps to prove actual new
headers and libraries reach compilation and linkage.

SQLite adds allocator-owned arithmetic compilation worklists, recursive
compiler/planner headroom checks and heap-backed pathname traversal. Deep SQL
may return an ordinary resource error on small workers; preserve that diagnostic,
cleanup, transaction outcome and subsequent connection reuse. Keep the expression
depth limit at 1,000. Verify exact arithmetic/indexed/NULL results, shallow success,
deep controlled errors, actual filesystem resolution and default worker guards.
Exercise Vectis C receivers, direct Lua and app-owned Lua connections, including
normal/packed execution and native musl workers. No worker stack is enlarged.

The [SQLite 3.54 changes](https://www.sqlite.org/releaselog/3_54_1.html) also
include planner simplifications, JSON formatting/date modifiers, stricter ALTER
TABLE special-rowid handling, new authorizer notifications for DEFAULT functions,
temporary-file handling and schema/trigger-step limits. Test representative SQL
through existing queries and the owned authorizer/transaction seams. No new
persistence format or Vectis public C ABI change is needed.

New native `cpkt_sqlite_incomplete` and `cpkt_sqlite_context_result_string`
remain directly available in the SDK. Vectis has no interactive SQL editor or
SQL-function callback registration; wrappers for those APIs have no current
workflow. Reconsider if either workflow is introduced. The optional diskused
extension, SQLite shell changes and configurable provider-global limits are
outside this upgrade's product scope. Existing public receiver/binding and
convenience workflows remain the application surface; no unused Lua methods.

Implement the complete change before broad verification. Use focused Debug
checks first, then the standard suite, PostgreSQL/XA services, native musl
default-worker checks, strict memory probes, seven SDK compile/link checks with
applicable Linux execution, embedded manuals, source-rock packaging and relocated
static/shared consumers. Distinguish SDK checks from full product builds and
compile-only Darwin evidence. Review fixes use focused Debug verification.

Specification review: sufficiently specified; no product decision is pending.

## Implementation and evidence

All seven cpktdb pins now select the published 0.4.0 SHA-256 digests. The build
requires SQLite 3.54.1 exactly; installed CMake/pkg-config consumers require at
least 3.54.1. The selected core remains 0.7.0. PostgreSQL/iODBC headers, facade
sources and component source digests were compared and are unchanged. The
verified 0.3.1 source archive SHA-256 is
`c571930ddd2a311945cc5195e6942a1270084c03888443d1f31946a7d4b66d80`.

Existing C/Lua bindings and app-owned convenience receivers already propagate
the new SQL behavior and stack-resource diagnostics correctly. No additional
production facade method, provider patch, thread policy or Vectis C ABI change
was necessary. SQLite's new native schema/trigger limits were queried through
the released API: 10,000,000 objects and 65,000 statements respectively. The
manual documents these defaults and the changed ALTER TABLE behavior.

Verification completed on 2026-11-10, with operational commands serialized:

| Check | Evidence |
| --- | --- |
| Focused Debug | **6/6 passed**, 1.99 seconds: normal/packed Lua SQLite, combined worker, Lua OOM, native receivers, new native worker and deterministic native failure/rollback cleanup. Updated Lua example also passes. |
| Standard GNU Debug suite | **295/295 passed**, 500.12 seconds. Includes real PostgreSQL connection/streaming and XA services, normal/packed applications, SQL lifecycle/process boundaries, runtime closure and editor/toolchain checks. |
| Owned SQL behavior | C and Lua evaluate exactly 1,000 arithmetic terms and reject the extra term normally, then reuse the connection. Indexed/unindexed operands, all nine boolean/NULL pairs, shallow/deep nested functions, failed-transaction rollback, DEFAULT functions, JSON formatting, date modifiers and changed ALTER behavior pass. Lua cases run through standalone and app-owned declarations. |
| Worker/pathname behavior | Native worker reports **131,072 bytes** with a 4,096-byte guard in Debug; musl reports **133,640 bytes** with an 8,192-byte guard. NULL pthread attributes are used. Opening through 40 symlinks, writing, reopening the canonical database, querying and cleanup pass. Generated SQL and pathname storage are heap-owned. |
| Seven SDKs | C89 static and C++98 shared consumers compile/link against all seven SDKs. C++ uses explicit C linkage around the C89 header. Each Linux target passes **2/2 runtime cases** (12 total), with ARM under QEMU; Darwin is compile/link only. Native probes verify incompleteness as an opaque zero/nonzero result, COPY/XFER/FINISH string ownership, invalid-context ownership, arithmetic depth/error/reuse and defaults on worker threads. These are SDK checks, not seven full Vectis builds. |
| GNU/musl products | Executable, static/shared libraries and public-header checks build with pinned toolchains. Final GNU normal/packed SQL cases pass. The musl product selection passes **7/7**, 2.83 seconds, including normal/packed Lua, native/default-worker/error/OOM checks and C++ headers. Its combined callback worker reports **133,624 bytes**. No stack is enlarged. |
| Strict memory | Seven qualified probes pass with zero live blocks and zero errors, without suppressions and with every leak kind fatal: native SQLite worker, deterministic failures, Lua OOM, combined worker, actual Lua/app workflow, static SDK archives and shared SDK. The worker balances 8,589 allocations/frees; native failures 895; Lua OOM 596,509; combined worker 153,253; Lua/app workflow 48,651; static SDK 6,870; shared SDK 6,871. Lua OOM covers **54 allocation failures** and 714 completed runs. |
| Installed SDK | Fresh GNU development archive relocated to a new prefix. Static/shared C89/C++98 and pkg-config consumers execute successfully and check actual SQLite 3.54.1. Each mode builds **36 packaged examples** and passes runtime-closure checks. Shared relative loading and private-symbol rejection also pass. |
| Provenance | Actual compilation uses the selected cpktdb include prefix; native link commands reference its static/shared libraries. The acquired facade header matches the reviewed release bytes (SHA-256 `d3105306dee0d1866fe6080ae07d2c927db9750199542599f86e3d3fbd31f6de`). Runtime SQL and native API checks establish the engine version independently of pin metadata. Existing epoch-mtime invalidation rebuilds changed inputs. |
| Docs/rock | Updated README and SQLite manual match embedded bytes and SHA-256 digests in both final GNU/musl executables. Lua source-rock staging/validation/smoke passes; its native stubs establish packaging separately from native execution above. |
| Formatting | `scripts/check_format.sh` and `git diff --check` pass. |

The first standalone SDK fixture needed standard explicit core/db prefixes for
dependency discovery and explicit C linkage for its C++ caller. Darwin's modern
headers also rejected a deprecated formatting call in the scratch fixture; fixed
string assembly preserves its literal SQL. These were fixture corrections,
without provider changes or repeated passed platform builds.

A fully static libc memory probe initially reported zero allocations; that is
not qualified heap evidence. Its replacement links the same static database
archives with dynamic pinned libc, allowing Memcheck interposition. The final
probe observes 6,870 allocations/frees and zero live blocks/errors. Fully static
product execution is verified separately. The other six passed memory probes
were reused; no full suite or cross matrix was repeated for this correction.

Final semantic review traced the changed upstream contracts through native and
Lua SQL preparation, typed values, cursor cleanup, errors, transactions,
app/fork/thread placement, PostgreSQL/XA ownership, diagnostics, documentation,
packed execution, rocks and installed closure. No actionable integration defect
or required upstream handoff remains. Existing pslog/LQL/MDF Lua facades remain
unchanged. Native completeness/context-result APIs remain C-only for the concrete
workflow reasons above; SQL features are accessible through ordinary queries.

Limits: downstream tests are representative, not exhaustive SQLite API parity or
an arbitrary SQL/callback stack bound. Upstream private allocation-fault and
full planner stress suites were not rerun. No external ODBC-driver workflow was
introduced. Darwin runtime and full ARM Vectis applications were not executed.
The new persistence fixture exercises canonical-file reuse; it is not a general
cross-version database migration test. No migration layer is added.

Reproduction uses `scripts/deps.sh`, the Debug and x86_64 GNU/musl presets,
`ctest --test-dir <build> -j1 --output-on-failure`,
`scripts/verify_installed_sdk.sh <relocated-prefix> static|shared`, and
`scripts/stage_lua_rock_sources.sh`. Strict Memcheck uses
`--error-exitcode=99 --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all`.
Scratch SDK/memory probes use native CMake/CTest. Sources, comparisons, commands,
logs and development artifacts are under ignored `build/cpktdb-0.4.0-review/`.
The task commit on `init` is relative to `f200c30`; the staged `LICENSE` attribution
edit is excluded. No release, tag, push or sibling modification was performed.
