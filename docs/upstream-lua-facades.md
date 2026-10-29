# Upstream Lua facade boundary and restoration

Vectis consumes the released pslog, LQL and MDF Lua facades unchanged. Their
omitted C operations may be deliberate API or performance choices. Native C
declaration parity is not a reason to extend, rename or patch these facades.
Flag a concrete needed capability or defect for upstream instead. This boundary
does not apply to Vectis-owned convenience APIs or bindings for libraries such
as zlib that have no upstream Lua facade.

For dependencies in cpkt, cpktdb and other bundles that lack upstream Lua
facades, Vectis should supply bindings for useful public API surfaces. Scope
each surface to a concrete Vectis or bundled application use case, with tests
for the promised behavior. Neither exhaustive C API parity nor an unused
dependency surface is a requirement. Existing bindings are retained, but this
restoration does not establish that all useful dependency bindings are complete.

On 2026-10-28 the local adaptations were archived before removal under ignored
`stash/upstream-lua-handoffs-2026-10-28/`, one draft PR/review handoff per project.
The handoffs include exact aggregate patches, released source baselines,
original deltas, licenses, tests, specifications and the earlier bug reports.
All three patches apply with zero fuzz and reproduce the last compiled local
cores/modules byte-for-byte. All 117 archived files have verified SHA-256 sums.
No upstream PR or out-of-repository change was made.

The production build now compiles the SDK's released C binding sources directly
and embeds the SDK's released Lua modules. Local patch staging, extra provider
headers and adaptation-only test targets were removed. Historical tests and
coverage claims for the withdrawn implementation are not current guarantees.
The older exhaustive API specification is superseded by this useful-surface scope.

## Vectis convenience APIs and remaining gaps

| Vectis surface | Released upstream support | Remaining need for local provider modifications |
| --- | --- | --- |
| `vectis.log.new`, `from_env`, fields and error helpers | pslog constructors, existing `logger:with(...)` and severity methods | None. `withf`, typed output records and native field mirrors are not needed. The logging example uses existing `with`. |
| App/service native logger handoff | Official public `pslog_lua.h` check/ref/native-sink capabilities | None. These are existing upstream C interop APIs. A new Lua predicate or retained-token API is unnecessary. Vectis-owned optional predicate checking remains separate from the native handoff gate. |
| `vectis.terminal.markdown` | MDF's existing `render` | None. The local `render_materialized` rename was removed from the call site; the Vectis method and its materialized result remain unchanged. |
| `vectis.terminal.markdown_stream` | MDF's existing `render_stream` | None for the current workflow. Native unfinished/table layout policy still applies; additional allocator/workspace guarantees are not silently promised. |
| `vectis.libs.lql` | Unmodified upstream `lql` table | None. Vectis has no convenience workflow requiring the added selector/AST/value mirror. |

There is therefore no current convenience feature gap that justifies retaining
these provider extensions. Vectis native buffer, digest, storage, PostgreSQL XA,
zlib and other owned APIs are preserved independently of this removal.

The archived issues concern failure guarantees, not missing feature mirrors.
The reports describe native owner/adoption leaks under Lua OOM in pslog,
callback OOM cleanup in LQL 0.3.0, and callback/result-copy cleanup in MDF.
The user-authorized LQL 0.4.0 upgrade consumes upstream cleanup fixes unchanged;
see `liblql-0.4.0-upgrade.md` for refreshed integration evidence. The old LQL
report and measurements remain historical, and its mixed API-expansion patch
is not reapplied. Their recorded versions and measurements must be respected when proposing
upstream fixes. Restoring the released bindings does not inherit the withdrawn
patches' allocation-failure guarantees. An outer Vectis `pcall` cannot repair
native resources already abandoned by a Lua unwind inside the upstream binding;
the corresponding protected callback/adoption fix belongs upstream. No local
workaround or additional upstream API is introduced here.

`vectis_lua_upstream_sources` checks actual compile inputs and exported embedded
bytes against the selected SDK sources. `vectis_lua_upstream_facades` exercises
the released APIs and retained Vectis logging/terminal convenience behavior in
ordinary and Linux packed execution. These checks prove the restoration scope,
not resolution of every upstream fault report or a full migration release gate.
LQL 0.4.0 memory/cleanup evidence and its remaining closer-entry limitation are
recorded separately in `liblql-0.4.0-upgrade.md`.

Historical restoration evidence (before LQL 0.4.0): 19 affected CTest checks passed, including normal/packed
execution and source-byte verification. Lua-only rock smoke, repository format,
shell syntax and whitespace checks passed. Five embedded manuals match their
current source files, and all 117 archived handoff files were reverified.

MDF follow-up, 2026-10-29: the [0.18.0 upgrade](libmdf-0.18.0-upgrade.md) consumes
upstream 0.17's callback/result-copy and string ownership fixes unchanged.
The MDF 0.16 failure measurements above are historical. No archived API expansion
is restored. Native 0.18 palette lookup does not require a current Vectis Lua
workflow. A dependency-facade palette extension belongs upstream; Vectis-owned
convenience workflows may independently use the public C API when warranted.
