# PostgreSQL XA application adapter

Status: reviewed implementation contract, 2026-10-27. This is the first
`vectis.pg` receiver, for application work on a liblockdc PostgreSQL XA borrow.
Standalone connections and incremental row cursors in the broader SQL design
remain separate work. This adapter does not claim streaming query results.

Use only `lc_xa_borrow_*` and `cpkt/postgres.h`, never libpq or private layouts.
The released cpktdb 0.3.1 checked retrieval API resolves the previously reported
NULL/drainage ambiguity. Copy its allocation-free diagnostic before cleanup;
failure must never become successful drainage, even when a later result exists.

`vectis_pg_from_borrow` retains a validated generic PostgreSQL borrow and returns
a public receiver shell. Validate the token before each native access. The
adapter never resets, frees or replaces the borrowed connection. The caller
serializes exclusive access and closes all results and adapters before prepare.
Closing an adapter releases its reference; results retain their own reference.
Borrow release failures remain observable. Lua construction uses the upstream
public `lc_lua_xa_borrow_retain` bridge, with userdata armed for GC before native
acquisition. No pointer integers or private facade fields cross the boundary.

`execute_materialized(sql, parameters, deadline_ms)` uses the parameterized
extended protocol even for zero parameters. Require an explicit dense Lua array.
SQL is a trusted template, not a sandbox; application SQL must not change the
XA transaction boundary or enter COPY/pipeline mode. Typed C parameters carry
bytes, length, OID, text/binary format and SQL NULL. Lua uses strings for text,
explicit typed binary values, and a module NULL sentinel. Reject embedded NUL in
text/SQL, invalid lengths/OIDs/formats, sparse arrays and implicit conversion.
Preserve result NULL separately from empty strings and binary bytes.

The native result is explicitly materialized. Return a result receiver whose
public checked view borrows the C89 result, allowing all upstream metadata/value
operations without another result copy. Lua exposes counts, ordered field
metadata and `value_materialized(row, column)` using one-based indices. No
result table or concatenated document is collected behind an incremental name.

Execution requires an idle, nonblocking XA branch in a transaction. Send,
flush, consume, checked-retrieve and drain to the native boundary. Use genuine
descriptor readiness and an absolute monotonic deadline; no retry sleeps or
periodic polling. A deadline of -1 explicitly permits an infinite wait. Lua's
default is 30 seconds. Preserve SQLSTATE and checked retrieval error codes in
copied diagnostics; never infer SQLSTATE from text or automatically log SQL,
parameters, credentials or diagnostics.

A query/protocol/timeout failure poisons the adapter. Later SQL is rejected and
close reports failure after releasing resources, so the upstream managed
`with_connection` scope cannot silently commit after swallowed application
errors. Do not attempt SQL replay, connection reset or a hidden compensating
transaction. An undrained connection remains the provider's abort/recovery
responsibility. Successful execution requires exactly one successful terminal
result and native drainage, with the branch still in its transaction.

Use the released coordinator's callback/await machinery for transaction
ownership, rollback-only state, renewal, leases and outbox binding. An explicit
Vectis blocking convenience uses the declared readiness wait; event loops keep
the upstream start/advance/wait interface. Forward returned outbox receipts
through the existing bounded supervisor wake path.

Verification: deterministic C tests cover parameter validation, retained borrow
lifetimes, invalidated tokens, successful drainage, server SQLSTATE, retrieval
failure followed by apparent drainage, readiness and poisoned close. Normal
and packed Lua tests cover shape/NULL/binary validation and lifecycle. A real
PostgreSQL Kube service must prove exact-branch SQL commit, abort, prepared
recovery and cleanup before the integration is declared complete.
