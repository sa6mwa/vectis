# App storage, collections and PostgreSQL XA

`app:storage({client=...})` declares one app storage scope before startup.
It opens its client lazily after the owning process starts. Use the same Pouch
configuration and encryption key for storage and workflows that compose in one
transaction. Stop closes resources in reverse creation order; retained handles
reject later use. Restart permits new handles. Direct `lockdc` access remains
available.

```lua
local v = require('vectis')
local app = assert(v.app.new({app_name='events', tls={mode='disabled'}}))
local store = app:storage({client={endpoints={'pouch:///absolute/store'},
  pouch={crypto_key=assert(require('lockdc').pouch_crypto_generate_key()),
    single_writer=false}}}) -- persist/reuse the key for subsequent opens
assert(app:start())
local collection = assert(store:create_collection({namespace='events',name='audit'}))
assert(collection:append({append_id=require('lockdc').xid_new()},'{"action":"created"}'))
local _, progress = collection:read({on_metadata=function(metadata)
  -- Accept entry/progress metadata separately from the immutable JSONL bytes.
end}, {write=function(chunk) io.stdout:write(chunk); return true end})
assert(progress.complete)
collection:close()
assert(app:stop())
app:close()
```

Collections expose native `describe`, `metadata`, `append`, `append_status`,
`entry_metadata`, `read`, `delete_entry`, `drop` and `close` methods.
`store:open_collection`, `store:list_collections(options, visitor)` and
`store:collection_capabilities()` use the same app storage lifecycle. Positions
and counts are exact decimal uint64 strings. Deletes leave permanent position
gaps. Append IDs support idempotency/status lookup; an ambiguous source-backed
append must be retried with a fresh source and the same ID.

`read` preserves bounded raw JSONL delivery and separate metadata callbacks.
Options include native LQL filtering, exclusive `after`, inclusive `through`,
`first_n`, resume cursors and checkpoints. First-N counts live input entries
before filtering. Persist a cursor only after durably accepting its referenced
output; discard an incomplete final object before resuming. A completed
checkpoint starts a fresh window; a resume cursor preserves the outstanding
window. Collection expiry and entry expiry are separate policies. Omitted
retention preserves policy, explicit zero clears it. Collections do not enlist
in XA. Remote collection operations return native unsupported errors.

`store:xa_coordinator(config)` returns an app-owned managed XA coordinator.
Its native `register_start`, `begin_start`, `adopt_start`, `recover_start` and
`retire_start` operations expose `advance`, `wait`, `result`, `cancel` and
`close_blocking`. Pending is determined by `result().done`, not by the absence
of an error. `require('vectis.storage').await_blocking(operation)` explicitly
waits for the native descriptor/deadline; event loops can drive those public
operations with their own readiness scheduler. This helper does not add polling
intervals or periodic recovery scans.

`coordinator:transaction_blocking(callback)` uses the upstream managed callback
scope. Within the callback, `tx:enroll(resource_id)`, `tx:renew()`,
`tx:acquire(request)` and `tx:with_connection(resource_id, adapter, callback)`
retain native ownership, rollback-only state and cleanup behavior. Renew the
owner explicitly during long application work. Direct collection appends remain
independent even when called from this callback.

Register the separately linked provider using
`coordinator:register_start('postgres', {resource_id=stable_xid,
connection_info=connection_info})` and drive/close the returned operation.
The resource ID must persist across recovery; do not generate a replacement on
each restart. The provider requires PostgreSQL 14 or later, prepared
transactions enabled, and the documented `pg_control_system()` privilege.
Vectis does not change server configuration or privileges automatically.
`require('lockdc.postgres')` remains directly available.

```lua
local pg = require('vectis.pg') -- also require('vectis').pg
local completed = assert(coordinator:transaction_blocking(function(tx)
  assert(tx:enroll(resource_id))
  return tx:with_connection(resource_id, pg.from_borrow, function(db)
    local result = assert(db:execute_materialized(
      'INSERT INTO audit (id, body) VALUES ($1, $2)', {id, body}))
    assert(result:close())
  end)
end))
assert(completed.applied)
```

The adapter uses the exact branch connection and only the public C89 facade.
All SQL uses the parameterized protocol, including zero-parameter commands
with `{}`. SQL templates remain trusted application code: bound values are
protected from interpolation, but dynamic identifiers and transaction-control
SQL are the application's responsibility. Do not control the XA transaction,
enter COPY/pipeline mode or reset/free its connection.

Parameters are text strings, `pg.NULL`, or `{value=bytes,oid=oid,binary=true}`
(`pg.binary(bytes, oid)`). Text/SQL cannot contain NUL; binary may. Numeric
values require explicit application formatting, preserving bigint/numeric
precision. Dense arrays are required. `execute_materialized` accepts an optional
absolute monotonic deadline in milliseconds; default 30 seconds, `-1` explicitly
infinite. It uses actual readiness and checked retrieval; a discarded result
cannot masquerade as successful drainage. Errors are `nil, error`, with copied
`sqlstate`, `retrieval_code`, `status`, `status_string`, `source`, `source_code`
and `message`, following the shared Vectis error envelope. No SQL or diagnostic
is automatically logged by this convenience facade.

Adapter and result calls protect native ownership across Lua allocation and
GC finalizers. Closing or reentering an active receiver raises an error; exact
parameter strings remain rooted until native execution has copied them. A
failed result accessor releases its call guard so the result remains usable.

Results are explicitly native materialized results, with `rows()`, ordered
`columns()` (`name`, `oid`, `format`), and one-based
`value_materialized(row,column)`. Values remain strings, including precise
numbers; SQL NULL is the module sentinel. Close every result and adapter before
prepare. A direct transaction borrow also offers `borrow:postgres()` with
app-owned descendant tracking. Results retain their token independently of
adapter close. Query, timeout or argument failures poison an adapter; close
releases it and reports failure so a caught query error cannot silently commit
in `with_connection`. Lua close is idempotent.

For SQL plus workflow outbox work, use
`workflow:xa_transaction(coordinator, function(tx) ... end)`. It supports the
same managed application operations plus `tx:append_outbox(entry, payload)` for
registered workflow kinds. It binds the workflow producer to XA, forwards the
returned commit receipts through the existing bounded supervisor wake path,
and marks caught rejected appends rollback-only. The transaction callback owns
no commit/rollback/close operation. A successful durable commit is not undone
by wake saturation; the existing bounded reconciliation signal handles that
explicit overflow case. Recovery resolves durable decisions and never replays
application SQL.

Each managed XA call begins a new global transaction. An existing outbox entry
can conflict with appending that identity from a different global transaction;
the native error propagates and the new transaction rolls back. This helper
does not add business-request deduplication or replay application work. Recovery
resolves the original durable transaction identity. The ordinary outbox-only
`workflow:transaction` retains its own documented duplicate behavior.

C consumers include `vectis/storage.h` for app-configured factories returning
the upstream C89 collection/coordinator receivers. These use the app's existing
shared client. Close all children before the app. `vectis/pg.h` declares the
PostgreSQL adapter and result receiver shells, typed parameters and copied
diagnostics. The result's checked `view` borrows the public C89 materialized
result for complete metadata/value access; never free or mutate that view.
