# Typed Pouch settings through Vectis

Status: implementation specification in the full public API facade goal.

liblockdc 0.18.0 exposes 17 root-open settings in `lc_pouch_settings`. Its C
endpoint query grammar remains supported, but direct upstream Lua `lockdc.open`
rejects query options and expects `pouch={...}`. Selected typed fields have final
precedence over endpoint queries and dedicated C crypto/compression settings.
Do not describe a failure caused by an already-exclusive app root as evidence
that a query overrode a child's typed `single_writer=false` setting.

Expose all 17 settings on `app.new({lockd={pouch=...}})` and a matching C receiver
method/free function `vectis_app_set_pouch_settings`. Copy the setting record and
its selected strings; no caller storage or Lua table is borrowed after capture.
Consume one existing reserved app method slot so existing struct size and method
offsets remain unchanged. Do not grow `vectis_lockd_config` or the embedded app
configuration ABI merely to add this convenience.

Capture settings before app startup/materialization; reject later changes.
Validate field names, masks, bools, integer domains and strings before changing
the app's captured configuration. Lua names follow the authoritative upstream
table: `background_compaction`, `disable_compaction_throttling`, `query_indexing`
map to their longer C field names. Unsigned settings accept nonnegative Lua
integers and explicit unsigned 32-bit `{high,low}` words for the full uint64
domain. Direct dependency Lua limitations must remain separately inventoried.

App clients, workers, metrics, auth, workflow producers and consumers must derive
the same typed policy from the app capture. Preserve user-selected settings and
their per-field precedence; insert shared writer mode only when neither a typed
field nor the native endpoint selects it. Keep Vectis-owned encryption defaults
and the explicit `VECTIS_POUCH_CRYPTO_KEY` override. An exclusive selection is
incompatible with multiple app-owned clients; reject that lifecycle without
silently rewriting it. Remote/Unix endpoints must reject nonempty Pouch policy.

Prefer the typed app table in examples. The endpoint builder remains the owner
of escaping; do not interpolate unescaped roots or secret query values. Direct
lockdc examples use query-free endpoints. Document typed field names/domains,
zero/default semantics, capture lifetime and precedence accurately.

Fast tests prove real root opening/reopening, conflicting legacy/typed writer
settings with an independent shared reader, bool/type/unknown-field rejection,
config mutation after capture, high-word numeric settings, environment/default
encryption, and equivalent app/workflow behavior in normal and packed execution.
No full cross builds run for this slice. Full build and independent review wait
for the whole public API implementation, then review fixes use debug tests only.
