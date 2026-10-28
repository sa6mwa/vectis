# Standalone PostgreSQL

`require('vectis.pg')` (also `vectis.pg`) provides owned standalone connections
and copied `app:pg` declarations. The existing `from_borrow` adapter remains the
managed XA application path described in [storage](lua-storage.md).

```lua
local pg = require('vectis.pg')
local db <close> = assert(pg.connect({parameters={
  host='db.example',dbname='app',user='app',
  -- Native service/passfile/environment defaults remain available.
}}))
local rows <close> = assert(db:query('SELECT $1::bigint AS id',{math.maxinteger}))
local first = assert(rows:next())
assert(first[1] == tostring(math.maxinteger))
local eof,err = rows:next()
assert(eof==nil and err==nil)
```

`connect(opts)` accepts `parameters` as string keyword/value pairs,
`connect_timeout_ms` (default 5000) and `io_timeout_ms` (default 30000).
Parameters are copied; no connection string is assembled. Missing `sslmode`
becomes `verify-full`. Native Unix-socket transport and environment/service/
passfile inheritance retain their provider behavior. Explicit `sslmode` is
preserved. DNS/auth plugins may block outside the polling budget, so connect
does not promise a hard overall deadline. No connection starts a worker thread.

`connect_start(opts)` returns a pending receiver. `db:poll()` returns `true`
when ready, `false` while pending, or `nil,error`. `db:readiness()` returns
`{fd,read,write}` for the next external readiness wait. The caller drives this
flow and owns its external wait/deadline policy; no Kore registration is implicit.
Use `connect` for the built-in blocking progression.

`db:query(sql,params,{timeout_ms=...})` returns a cursor. Params must be an
explicit dense array, including `{}`. Strings, exact signed Lua integers,
booleans, `pg.NULL` and typed `{value,oid,binary}` records are supported.
`pg.binary(bytes,oid)` builds a binary record; explicit lengths preserve NUL
bytes. SQL and text params reject NUL. Non-integer numbers require explicit
decimal text, avoiding implicit precision/locale decisions.

`cursor:next()` returns a dense row or `nil,nil` at checked EOF. Errors return
`nil,error`, including SQL failure after earlier rows. All non-NULL results are
native text strings, including bigint/numeric/timestamps and PostgreSQL's text
bytea representation; no implicit binary-result codecs or numeric conversion
are applied. NULL shares identity with `vectis.sql.NULL` and the XA adapter.
`cursor:columns()` returns `{name,oid,format}` records after the first received
row or terminal result, including duplicate names and empty query results.

`cursor:next_ready()` performs a nonblocking progression and returns
`row,'row'`, `nil,'pending'`, `nil,nil` at EOF or `nil,error`.
`cursor:readiness()` supplies the next wait interests. `next` drives the same
state with a cumulative blocking I/O budget; per-query `timeout_ms` overrides
the connection default. Zero performs an immediate probe. Time in application
code between calls is not charged. External readiness consumers own their
separate waiting deadline.

The engine selects native single-row mode immediately after send and keeps
only one row result plus provider transport buffers. Default limits are
1 MiB SQL, 256 parameters, 16 MiB parameter/cell/row bytes and 1024 columns.
Native provider buffering is separate from facade limits. Terminal success and
checked drainage are required before EOF or connection reuse.

`db:exec(sql,params,opts)` drives completion without retaining rows and returns
`{command,affected}`. `affected` is exact native decimal text, or an empty string
where the command supplies no count. Use `query` when rows are needed.
`db:prepare(name,sql,{parameter_types={oid,...},timeout_ms=...})` acknowledges
server preparation and returns a reusable view with `stmt:query(params,opts)`.
The caller-named server statement belongs to the connection; closing its client
view does not send SQL. Explicit SQL DEALLOCATE or connection close removes it.

`db:transaction(fn,opts)` runs a local owned transaction. Options are isolation
`read_committed` (default), `repeatable_read` or `serializable`, and
`read_only=false`. A successful callback commits and returns `true` plus its
values, preserving nil positions. `nil,error` rolls back and returns an error
with the original value as `cause`. Exceptions roll back, then rethrow the exact
original object. Nesting, manual transaction control (including prepared control
statements), close and app lifecycle attempts fail the transaction even if
caught. A caught native SQL error still prevents commit and preserves its first
SQLSTATE. Deferred constraint failures at COMMIT are reported as confirmed
rollback when the server acknowledges that failure.

Transaction errors expose `transaction_outcome`: `not_started`, `committed`,
`rolled_back` or `unknown`; `cleanup_error` preserves secondary failure.
`db:transaction_diagnostic()` returns the last outcome/error record, including
after a thrown callback. Losing a COMMIT receipt never reports confirmed rollback.
Errors also carry status, source, operation, dependency/retrieval codes and
SQLSTATE where available.

Each connection owns one active cursor. Abandoning a cursor, transport failure,
deadline failure or unsupported COPY/pipeline response invalidates the connection.
There is no network drainage in GC. An active cursor escaping a transaction
invalidates its connection; rollback outcome is unknown when unacknowledged.
Close resources explicitly or use `<close>`. Lua close is idempotent. Connection
closure invalidates retained child views. Resources are process/thread-confined;
inherited live connections cannot be used or closed from the child, which must
not send Terminate on its parent's session.

`app:pg({name=...,parameters=...,connect_timeout_ms=...,io_timeout_ms=...})`
copies a named declaration before startup. The first operation after startup/
fork opens that process's connection. Stop/restart invalidates materialized
connections and children; restart allows a fresh connection. App close detaches
the declaration. A failed/invalid connection requires explicit reopen or app
restart; no live receiver reconnects silently. Replication, large objects, COPY,
pipelines and authentication plugin management are outside this surface.
