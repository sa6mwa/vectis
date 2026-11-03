# liblql 0.4.1 upgrade

## Reviewed specification

Baseline `3d7445e` selects LQL 0.4.0. Select all seven SDK archives and the
released Lua source archive for
[0.4.1 from c89systems/liblql](https://github.com/c89systems/liblql/releases/tag/v0.4.1).
Release commit: `c6b25361e505e382140062b50f2bc6ea30c60831`.
Reviewed source SHA-256:
`00b94466c2fb650cb267b62081e7b8c0053cf9616e81e5f0736bcadb03898157`.

The complete archive comparison finds no changes to native implementation,
public headers, Lua core/module/adjacent headers, examples or runtime tests.
Differences are version metadata, C89 systems attribution, Go tooling module,
LuaRocks homepage, source archive reconstruction/version ownership, dependency
cache validation and release preflight/lifecycle tests. Public ABI remains 0.
There is no new runtime or SDK dependency; the Go oracle remains test tooling.

No changes to callbacks, errors, ownership, memory quota, stack use, concurrency,
streaming/spooled behavior, persistence or configuration are introduced.
Consume the upstream Lua facade unchanged and keep direct module identity.
Vectis has no additional owned lifecycle or convenience seam to add. Lockdc
0.27's minimum LQL 0.4.0 accepts this patch release without changing that SDK.
Other pins, Vectis ABI and existing workflow boundaries stay unchanged.
The known persistent-OOM closer-entry limitation in the
[0.4.0 review](liblql-0.4.0-upgrade.md) is not changed or claimed fixed.

Update verified pins, build/installed requirements, runtime version assertions,
manual/matrix/audit and source-rock metadata in one pass. Use existing observable
LQL tests for execution, callbacks, cleanup, limits, normal/packed behavior and
native default/small worker stacks, plus Pouch query/collection integration.
Rebuild GNU and musl x86_64 products; check memory, actual selected binding/header
inputs, embedded Lua/manual bytes, source rock and relocated static/shared SDK
consumers. Compile C89 static/C++98 shared LQL probes and the released Lua core
against all seven SDKs, executing Linux native probes where supported. Reuse
unaffected prior platform/product checks; do not repeat unrelated full cross
builds for identical runtime sources. Darwin is compile/link only.
No new test framework, facade extension, release, tag or push.

Specification review: complete; no unresolved decision.

## Delivered change and review

All eight pins now select verified C89 systems 0.4.1 artifacts. Build discovery
is exact; installed CMake/pkg-config minimums are 0.4.1. Runtime assertions,
manual, source-rock description, coverage matrix and audit identify the selected
release. The existing example requires no code change.

No production native binding, upstream Lua facade or Vectis convenience
implementation changes are required. The GNU shared library retains SONAME
`liblql.so.0` and exactly the same exported symbol names. Its only shared runtime
dependency remains libc. Lockdc 0.27 and its PostgreSQL provider remain unchanged;
their imported LQL minimum accepts 0.4.1. No app configuration, logger, fork,
thread, persistence, process placement or notification seam changes.

Final review is clean for this patch: unchanged runtime/public/Lua sources and
existing observable tests establish that there is no new surface to integrate
or local provider extension to add. Source version/reconstruction and release
preflight changes belong to upstream tooling; Vectis consumes the released SDKs.

## Verification

Evidence and isolated SDK CMake fixture are under ignored
`build/lql-0.4.1-review/`; dependency acquisition used the owned
`build/useful-lua-facades/dependency-cache`.

| Check | Result |
| --- | --- |
| Source comparison | `src/`, `include/`, `lua/` and examples are byte-identical to the reviewed 0.4.0 archive; full differences are confined to the metadata/tooling listed above. |
| Native Debug build and affected CTest checks | **13/13 passed**, 15.70s. LQL/client version 0.4.1, native and Lua close/error/limits, normal/packed execution, source provenance, native default/small stacks, Lua allocation failures, full Lua smoke, Pouch conversion/collections and storage integration. |
| Native musl product | Rebuilt CLI/static/shared library and relevant fixtures; **13/13 passed**, 16.25s, including normal/packed LQL and Pouch workflows and the combined default-stack worker with NULL pthread attributes. |
| Existing local-data pipeline example | Normal and packed execution **2/2 passed** on both GNU and musl, exercising the unchanged public Lua streaming selector workflow. |
| Strict Valgrind LQL allocation sweep | **82803 allocations/frees**, zero live bytes and **zero errors**, no suppressions. Callback/result-adoption faults remain covered; the known closer-entry limit still appears at fault positions 3, 4, 7 and 8. No claim of arbitrary Lua cleanup under persistent OOM. |
| Strict Valgrind combined worker | **117963 allocations/frees**, zero live bytes and **zero errors**, no suppressions, including native Pouch/LQL scan and released Lua callback paths. |
| Seven SDK contracts | C89 static/C++98 shared compile/link probes and C99 compilation of the exact released Lua core pass against each selected SDK's imported headers. Native default and small-stack probes execute successfully on all six Linux targets: **12 runs**, ARM under QEMU. The small stack is 64 KiB or the platform minimum if larger. Darwin compile/link only. |
| GNU Release product | CLI/static/shared rebuilt; final LQL regression passes normal and Linux packed execution. |
| Relocated GNU SDK | Actual archive generated and extracted to a new prefix; static/shared C89/C++/pkg-config consumers pass, including actual native version 0.4.1. All **36 installed C examples per mode** pass runtime contracts. |
| Embedded sources/manual | Released binding compilation, module identity and exported module bytes match selected sources. Embedded LQL and other current manuals match source. |
| Source rock and formatting | Vectis source-rock helper smoke passes with owned build/tree paths and native stubs; native behavior is established separately above. `scripts/check_format.sh` and `git diff --check` pass. |

This refresh uses focused registered tests and artifact checks for the changed
dependency. It does not relabel the preceding lockdc upgrade's broader native
suite or other full platform product builds as new 0.4.1 evidence. Other ARM
product binaries are not rebuilt for identical runtime sources; their selected
LQL SDKs and binding compilation are checked above. Existing ARM app-emulation,
Darwin execution and historical OPC UA shutdown limits remain in that review.
No release gate or release approval is claimed. No release, tag or push.
