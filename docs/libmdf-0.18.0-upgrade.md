# MDF 0.18.0 upgrade

Historical verification for the 0.18.0 upgrade; current MDF selection and new
rendering behavior are recorded in the [0.23.0 review](libmdf-0.23.0-upgrade.md).

## Reviewed specification

Baseline `14c425e` selects MDF 0.16.0. Upgrade all seven SDK pins and the released
Lua source archive to 0.18.0; retain other dependencies. Compare both intervening
[0.17.0](https://github.com/sa6mwa/libmdf/releases/tag/v0.17.0) and
[0.18.0](https://github.com/sa6mwa/libmdf/releases/tag/v0.18.0).
Reviewed public headers, native implementation, Lua core/module, ownership
review, README/examples and upstream regressions. The reviewed source archive
SHA-256 is `890d30bc68ac602840cd95a5a7427d7ca504a9f600cf16c127d3824b96e23cc3`.
The released tag commit is `d6d0ead8d1ae6507a78579142bcb0cc3e879f652`.

0.17 protects complete source/sink/trace callback frames and materialized result
adoption, retains font/token strings across mutation/GC, and rejects coerced or
NUL-containing C strings. Length-aware byte fields retain binary semantics.
Existing Lua API names, native signatures, streaming/table layout, destination
defaults and ABI 6 are preserved. Consume these upstream fixes unchanged;
do not restore the archived Vectis facade additions or renames.

0.18 adds native allocation-free foreground palette lookup (19 slots), configured
renderer lookup and H1–H6 lookup with caller-owned buffers. The receiver method
is appended after the prior layout; ABI remains 6. Lookups do not render, write
to a sink or detect terminal/environment state. Error outputs remain unchanged.
The published Lua facade does not expose these lookups. Current Vectis terminal
helpers and Smith render Markdown without separate themed chrome; no new
convenience or provider wrapper is warranted. Extending MDF's dependency Lua
facade requires an upstream change. A future Vectis-owned terminal workflow may
instead integrate the public C API within its independent convenience boundary.

Vectis helpers already pass options through and translate exceptions into their
documented status errors. Direct MDF calls preserve original Lua error objects.
Smith's native bounded reader/writer thread keeps the same create/render/destroy
contract. No new process/thread, persistence, configuration or transitive native
dependency is introduced. Set installed CMake/pkg-config minimums to 0.18.0.

Implementation and verification: update pins and metadata; add native palette
and installed-consumer behavior checks; exercise upstream callback/GC/string
ownership fixes through normal/packed MDF and Vectis terminal workflows; add
deterministic allocator failures against the exact compiled released core.
Exercise MDF callbacks on the existing default-stack musl worker. Document the
delivered fixes and native-only palette boundary, and update the example/audit.
Finish this implementation before broad verification.

Use full native standard checks, focused musl normal/packed/default-stack checks,
strict memory checks, source-byte provenance, source-rock/manual/example and
relocated static/shared SDK consumers. For other targets, compile/link/run the
MDF public contract probe where possible and compile the released Lua binding
against each selected SDK using imported headers. Reuse the previous upgrade's
unaffected product/platform evidence; an ABI-compatible MDF update does not
require repeating unrelated full cross builds. Darwin execution and QEMU's
known process-shared robust-mutex limits remain explicit. No release or push.

Review conclusion: specification is complete; no product decision is pending.

## Delivered behavior and review

All eight pins and build/installed minimums now select 0.18.0. Both the native
SDK and released Lua core/module are consumed unchanged. No public Vectis API,
production Lua wrapper or ABI changes. Native docs paging keeps its existing
`mdf_pager_source` contract. The example forwards each rendered chunk immediately,
retaining only a bounded suffix for its assertions.

Review traced complete protected callback frames, retained font/token values,
operation guards during materialized adoption, cleanup before exception rethrow,
and native palette copy/error/export paths. Normal direct calls retain original
Lua error identity; Vectis retains its existing structured-error policy. No local
repair or new upstream API is needed for these established workflows.

The review added retained/document sink-replacement assertions and connected
the new allocator test to `vectis_failures_fast`, then repeated focused checks.
No production-code defect was found. No repeated full matrix followed these
test/build-wiring changes.

## Verification

Logs and the isolated SDK consumer CMake fixture are under ignored
`build/mdf-0.18.0-review/`.

| Check | Result |
| --- | --- |
| Full native Debug build and `ctest --test-dir build/debug --output-on-failure -j1` | **284/284 passed**, 469.59s, including Smith's native threaded streaming fixture, normal/packed application workflows, services and lifecycle checks. |
| Final affected Debug checks | **9/9 passed**, 8.14s, after review additions. The fast-check target builds the allocator fixture. |
| MDF behavior | Materialized/streamed parity, early output before producer EOF, reader capacity, original source/sink/trace error identity, healthy recovery, retained/document sink replacement after cleanup error, font mutation, binary font data, token getter GC, failed-construction graph collection, and result-copy finalizers. String coercion/NUL rejection reaches existing Vectis error translation. |
| Deterministic allocation failures | Nine cases use the exact product's compiled released core and Lua module: source body, sink arguments/body, trace arguments/body, result adoption, retained result/callback and incremental-document callback. Recovery/close and Lua allocation balance pass. |
| Strict Valgrind allocator fixture | **7939 allocations/frees**, zero bytes/blocks at exit, **zero errors**, no suppressions. This refreshes the archived MDF 0.16 callback/adoption leak evidence against the upstream fix. |
| Strict Valgrind combined worker | **115669 allocations/frees**, zero bytes/blocks at exit, **zero errors**, no suppressions. |
| Native x86_64 musl | Rebuilt CLI/static library, released Lua binding and relevant tests; **8/8 passed**, 9.51s, including normal/packed MDF, source provenance, allocator failures and the combined worker. |
| Default musl stack | Worker created with **NULL pthread attributes**, observed **133656 bytes**. A native digest reader stays active while MDF streams bounded input to nested Lua buffer callbacks. No enlarged stack. Binding render/stream frames measure **752/608 bytes**; maximum measured binding frame is the path helper at **16480 bytes**. These are individual frames, not a universal call-chain bound. |
| Seven SDK targets | C89 static and C++98 shared native contract probes compile/link; the released Lua core compiles as C99 against each target's imported MDF/Lua headers. Covers native palette/configuration/invalid-output behavior and existing bounded-buffer failures. Static native probes execute on all six Linux targets, ARM under QEMU. Darwin is compile/link only. |
| GNU Release product | CLI/static/shared libraries and public headers rebuilt. Final MDF regression passes normal and Linux packed execution. |
| Relocated GNU SDK archive | C89/C++/pkg-config static and shared consumers pass, including selected 0.18 header and actual new palette calls; all **36 installed C examples per mode** pass the runtime contract. |
| Provenance/manual/rock/example | Actual released binding compilation and exported MDF module bytes match selected sources; normal/packed alias/convenience tests pass. Embedded MDF/terminal and other current manuals match source. Updated rendering example and source-rock helper smoke pass; the rock helper uses native stubs, with native behavior proved separately above. |

The previous upgrade's unrelated full-platform product evidence is not relabeled
as a new seven-platform Vectis runtime matrix. This task rebuilds full GNU and
musl x86_64 products, and verifies the changed MDF public/compiled Lua contracts
across all seven SDKs. Interactive docs paging is an unchanged native contract;
the current manual readback is noninteractive. Existing ARM/QEMU robust-mutex
and Darwin execution limitations remain in the lockdc upgrade review. No broader
app-emulation result or new release approval is claimed.

Final review is clean. The released dependency palette lookup remains C-only;
Vectis-owned convenience integration remains independent. SSH/LZO and other dependency pins are unchanged.
No release, tag or push is performed.
