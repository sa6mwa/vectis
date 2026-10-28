# SQLite and PostgreSQL receiver facades

Status: component design under the complete public API facade goal. The
[complete facade specification](lua-public-api-completeness-spec.md) supersedes
earlier scope exclusions and delivery sequencing here. Its final sequence is
implementation with focused debug tests, full build, then review with debug-only
fix iterations until clean. The previous PostgreSQL C89 result-retrieval
prerequisite is resolved by cpktdb 0.3.1. The first XA borrow adapter has a
separate [implementation contract](postgres-xa-adapter-spec.md). This broader
standalone connection/cursor design is not a finalized implementation
authority or a delivered API. The completion record must identify the tested
candidate and limitations.

## Scope and evidence

Deliver `vectis.sqlite` and `vectis.pg`, backed by public receiver shells in
libvectis. The C library owns execution, diagnostics, transaction boundaries,
resource limits and cleanup; Lua binds that implementation. Keep language
runtime types and private Lua layouts out of the C SDK.

The pinned c.pkt.systems 0.11.0 SDK supplies SQLite 3.53.4 through
`cpkt/sqlite.h`/`cpkt::sqlite`, and PostgreSQL 18.6 through
`cpkt/postgres.h`/`cpkt::postgres`. The latter is a client, not a server. Inspect
the installed public headers, facade guides and imported targets as authority;
the earlier temporary four-library Lua proposal is superseded for these two
databases. PDF and PNG have separate component contracts within the complete
public API goal. A PostgreSQL facade fix is required
before finalizing the error contract and starting implementation.

All database operations must use those public C89 facades. Native sqlite3/libpq
headers and calls are forbidden in the implementation and binding. If a
required operation cannot be implemented through the C89 facade, pause the
active goal and report the precise missing public operation before proceeding.
Do not bypass it with a native dependency call or a private-header adapter.

### Historical upstream prerequisite (resolved in cpktdb 0.3.1)

The previously pinned header documents `cpkt_postgres_get_result()` returning NULL as
drainage. In the inspected producer source, `cpkt_postgres_track_result()` also
frees a received result and returns NULL when notice-lifetime bookkeeping or
event preparation fails. There is no checked retrieval status distinguishing
that failure from drainage, and result SQLSTATE can be lost before the consumer
sees it. This was established by header/source inspection, not a runtime fault
reproduction against the installed binary.

A terminal-success requirement below prevents a premature NULL from being
accepted as successful execution, but cannot recover the discarded diagnostic.
Do not silently substitute a generic EOF/protocol error for the promised
provider failure contract. Require an upstream public C89 checked retrieval
operation, with explicit result/drained/failure states and an allocation-safe
client error record. Reinspect the released facade and prove its allocation
failure behavior before removing this prerequisite. The self-contained upstream
report is under ignored `stash/cpkt-postgres-result-retrieval-bugreport.md`.

The upgraded public facade supplies `cpkt_postgres_get_result_checked` and
`cpkt_postgres_receive_checked`, distinct RESULT/DRAINED/FAILED states, and an
allocation-free diagnostic containing discarded SQLSTATE and server text.
Vectis's XA adapter uses checked retrieval. Deterministic adapter tests inject
FAILED rather than accepting apparent drainage; actual provider allocation
fault and live-service checks remain part of final verification.

Use the existing strict C89 policy, Lua 5.5.1, receiver conventions, export
allowlist, selected Bootlin execution runtime, source-rock boundary and static
Linux CLI packaging. PostgreSQL's official imported target owns its LDAP,
SASL, Kerberos/GSSAPI, OAuth, curl, OpenSSL and platform linkage closure.

## Execution policy

Every Vectis convenience execution operation uses a prepared statement or the PostgreSQL
parameterized protocol, including statements with zero parameters. Lua callers
must provide a dense parameter array, including `{}` for a static command.
C callers provide a typed array and explicit count; a NULL array is valid only
for count zero. These convenience receivers expose no raw SQL execution,
interpolation or multi-statement script executor. The later complete public API
requirement also includes faithful direct dependency bindings; those retain
their native operations and documented risks. They are separate from the
safety policy of `vectis.sqlite` and `vectis.pg`, and must use typed ownership
and public C89 calls rather than private handles.

SQL text is a trusted application template. Bound values cannot change its
syntax. A caller that concatenates untrusted text into the template remains
unsafe: this API is not a SQL sandbox or a proof of template provenance.
Table/column names, keywords and dynamic ordering require application-owned
selection from allowed templates. No SQL dialect builder or ORM is introduced.

PostgreSQL uses `$1`, `$2`, etc.; its extended protocol enforces one statement.
SQLite uses positional bindings and validates complete coverage before stepping.
Use SQLite's preparation tail and a second preparation to distinguish trailing
whitespace/comments from additional executable SQL. Install a deny-all public
`cpkt_sqlite_set_authorizer` guard around tail preparation and restore the
owned connection's policy on every exit. Any authorized SQL action in the tail
is rejected; whitespace/comments require no SQL action. Reject an additional
statement or invalid trailing SQL before stepping the first, without allowing
the tail's prepare-time PRAGMA effects. First-statement preparation itself may
have provider-defined PRAGMA effects; validation is not a rollback guarantee
for those effects. Check authorizer installation/restoration failures. Do not
scan or split SQL with an ad hoc parser. Missing/extra bindings, holes, NUL in SQL
and empty statements are errors.
[PostgreSQL parameters](https://www.postgresql.org/docs/18/libpq-exec.html),
[SQLite preparation](https://www.sqlite.org/c3ref/prepare.html) and
[SQLite binding](https://www.sqlite.org/c3ref/bind_blob.html) define provider rules.

`exec` reports command completion; `query` returns a cursor. `exec` rejects row
results. This rejection is not a promise to roll back a statement that has
already executed, notably PostgreSQL DML with RETURNING. SQLite can reject
known row-producing statements before stepping. Query may return an empty
cursor for a command that produces no rows.

## Public C shape

Install `vectis/sql.h`, `vectis/sqlite.h` and `vectis/pg.h`. Stateful operations
are receiver method fields with explicit `self`, private `impl`, eight reserved
method slots and corresponding exported free functions. Constructors and app
registration helpers are ordinary free functions. Additive new shells must not
change an existing receiver's size or existing method offsets.

| Receiver | Required operations |
| --- | --- |
| `vectis_sqlite` | prepare, query, exec, transaction, diagnostic, close |
| `vectis_sqlite_statement` | bind, next, columns, reset, diagnostic, close |
| `vectis_pg` | connect advancement/readiness, query, exec, transaction, diagnostic, close |
| `vectis_pg_cursor` | next_ready, readiness, blocking next, columns, command completion, diagnostic, close |

Configuration has matching `*_config_init` functions. Failure returns
`vectis_status` with `vectis_error`; produced outputs are initialized safely.
SQL diagnostics additionally preserve operation, SQLite extended result code,
PostgreSQL SQLSTATE and a secondary cleanup failure, using a separate new SQL
diagnostic record rather than changing `vectis_error`'s ABI. Copy diagnostics
before freeing the provider object. SQLSTATE must come from the server result,
not guessed from message text. Do not automatically log SQL, values, passwords,
connection strings or provider diagnostics. Provider diagnostics can themselves
contain application data. Startup warnings retain the upstream process-default
diagnostic policy; Vectis must not silently replace process-global callbacks.

`sql.h` provides typed parameters, SQL NULL, exact signed 64-bit values,
length-bearing text/blob values, ordered columns and row views. It is a value
vocabulary, not a common database receiver. PostgreSQL metadata retains type
OID/format; SQLite metadata describes current dynamic storage classes. C row
and metadata bytes are borrowed until the documented next/reset/close boundary;
Lua makes owned copies before advancing. Duplicate column names remain intact.

C close frees an owned shell and tolerates NULL; using that freed C pointer is
invalid. Closing a database invalidates its retained child shells without
freeing the caller's child shell pointers. Children retain the private owner
state until individually closed. Lua close is idempotent and retained userdata
returns a state error after close. Native cleanup must not depend on allocation.
During a transaction callback, reject explicit database destruction and app
stop/start/restart/close; retain owners until callback processing and cleanup
finish. C destruction with no error-return channel must defer final reclamation
and invalidate further callback use, causing the transaction to fail. It cannot
free a shell still on the transaction helper's stack.

## Lua surface and values

`require("vectis.sqlite")`, `require("vectis.pg")` and the equivalent `vectis`
table members are public. Their small Lua modules load native core bindings
that call only the public C database API. The source rock includes those Lua
modules and accurately describes the requirement for the embedded native
runtime; it must not pretend an unavailable native module is standalone Lua.
The root loader omits SQL members only for recognized missing native cores in
the source-rock environment. Direct SQL-module loading reports the embedded
runtime requirement. Genuine module exceptions propagate. Update native-core
exception lists, rock module lists, embedded resources and the source-rock probe.

| Lua operation | Behavior |
| --- | --- |
| `sqlite.open(path, opts)` | Open an owned SQLite database. |
| `sqlite.blob(bytes)` | Explicit BLOB parameter; other strings are TEXT. |
| `db:prepare(sql)` | SQLite reusable statement, initially unbound. |
| `stmt:bind(params)` | Validate and bind all parameters before execution. |
| `stmt:reset()` | Reset and clear bindings; preserve native reset failure. |
| `pg.connect(opts)` | Explicitly blocking connection convenience. |
| `pg.binary(bytes, oid)` | Explicit PostgreSQL binary parameter with type OID. |
| `db:query(sql, params, opts)` | Cursor; PostgreSQL submits without collecting results. |
| `db:exec(sql, params, opts)` | Checked command metadata. |
| `db:transaction(fn, opts)` | Checked local transaction callback. |
| `cursor:next()` | Row, nil/nil at EOF, or nil/error. |
| `cursor:columns()` | Ordered copied metadata; PG may receive and retain the first row. |
| `handle:close()` | Idempotent native cleanup. |

Both SQL modules expose the same per-state NULL sentinel, also available from
`vectis.sql.NULL`. Rows are dense arrays. SQLite preserves INTEGER, REAL, TEXT,
BLOB and NULL; signed 64-bit integers must not pass through floating point.
PostgreSQL initially returns text strings and NULL with type OIDs, preserving
bigint/numeric precision, timestamps, arrays and booleans. Binary result codecs
are not delivered; bytea retains its text representation. Integer/boolean
parameters are formatted exactly and finite floating-point values use a
locale-independent round-trippable representation. Reject non-finite numbers
and NUL in PostgreSQL text parameters; explicit binary parameters accept NUL.

Wrong types/unknown options raise argument errors before native acquisition or
submission where possible; SQLite preparation effects have the caveat above.
Operational failure returns nil/error with current Vectis error fields plus
operation, dependency code and SQLSTATE when applicable. EOF and execution
failure are latched; subsequent reads cannot restart a command or convert a
failure into EOF. Explicit close is a separate state. No iterator may silently
turn a database error into loop termination.

Resources use full userdata, `__close` and `__gc`. Install a cleanup owner before
native acquisition. Lua allocation failure may raise the VM's memory error but
must not leak acquired native resources. GC neither drains the network nor
calls user code. Protect transaction callbacks; never longjmp through a native
provider stack. Reject concurrent/reentrant use of one handle while allowing
the transaction's callback to use its owning database normally. Callback
results and errors remain pinned while native commit/rollback runs. If row or
metadata copying raises an allocation error after acquiring a native row, latch
the cursor as failed and clean up its native resources; retry cannot skip that
row and continue. PostgreSQL cleanup of an unfinished cursor closes its owned
connection. Pin database/app userdata across callbacks and extend the existing
app-owned callback guard to reject lifecycle operations, even when a callback
catches the resulting Lua error. GC defers reclamation of protected owners.

## SQLite ownership and defaults

Default mode is readwrite_create; readwrite and readonly are explicit options.
URI interpretation is disabled unless selected. Do not enable extension loading
or silently change journal/WAL policy. Default busy timeout is zero, giving
immediate contention feedback; a caller may explicitly select a bounded
nonnegative busy timeout. Opening a file is an explicit caller-selected effect.

An unsuccessful cpkt open can still return an owned receiver: inspect the
native error code, copy diagnostics and close it. Each statement belongs to its
database. Copy borrowed column bytes before stepping/reset/finalization. Empty
TEXT, empty BLOB and NULL are distinct. Query-owned statements release native
resources at EOF but retain their latched EOF; reusable prepared statements
support explicit reset/rebind. Parent close finalizes every native statement.

Exec metadata contains exact changes and last_insert_rowid with native SQLite
semantics; last rowid is not proof that this command inserted a row.

## PostgreSQL transport and defaults

Connection options are explicit libpq keyword/string pairs; do not synthesize
a connection string. Use connect_start_params/connect_poll and reacquire the
socket after each applicable state transition. C exposes connection progress
and readiness so a caller can drive its own event loop. The blocking convenience
uses monotonic elapsed time and a default 5000 ms polling budget. DNS and provider
authentication plugins can block outside that budget; do not claim a hard
end-to-end resolver/plugin timeout. [Connection control](https://www.postgresql.org/docs/18/libpq-connect.html).

Insert sslmode=verify-full when omitted, deliberately overriding an implicit
environment/service SSL default; explicit recognized modes remain caller
choices. Unix sockets follow local transport semantics. Document libpq's
service/passfile/environment inheritance; do not prompt or change credentials.
Disable server notice delivery on the owned connection with checked callback
installation, while retaining the upstream startup-diagnostic policy above.

Check `cpkt_postgres_set_nonblocking(connection, 1)` before query submission;
keep it enabled through execution and destruction, including output-pressure
cleanup. Use send_query_params and immediately select single-row mode, then
checked flush/consume_input/is_busy and the required upstream checked retrieval
operation. One active operation per connection. Ordinary NULL retrieval without
terminal success is a latched failure and closes the connection; it never means
successful EOF. This defensive rule does not remove the upstream prerequisite.
Advance on consumer demand and retain at most one row result plus native
transport buffering; individual rows/values remain materialized. Do not collect
all rows behind a cursor. [Asynchronous processing](https://www.postgresql.org/docs/18/libpq-async.html)
and [single-row mode](https://www.postgresql.org/docs/18/libpq-single-row-mode.html).

`next_ready` never waits on a socket and distinguishes row, EOF and pending.
Readiness reports the current FD, read/write interest and immediate progress.
Blocking next/exec account cumulative active I/O with a default 30000 ms budget;
time in application code between row reads is excluded. Handle EINTR, socket
changes, output backpressure and both read/write readiness without spinning.
Lua blocking methods are documented as such; they are not coroutine-async or
automatically scheduled on Kore's shared loop. Native readiness is the explicit
integration path for callers that need nonblocking scheduling.

Require terminal success and checked drainage before marking EOF or
reusing the connection. Earlier rows may precede a terminal SQL error. Drain
ordinary server errors within the I/O budget, retaining a reusable connection
where libpq permits; an aborted transaction still needs rollback. Transport
failure, deadline expiry, unsupported COPY, or early cursor abandonment closes
the physical connection. Close and GC never wait for network drainage. Exec
metadata preserves the command tag and exact decimal affected-row count.

## Resource limits

Initial configurable defaults are 1 MiB SQL, 256 parameters, 1024 result
columns, 16 MiB per parameter/cell, 16 MiB aggregate parameter bytes and 16 MiB
total copied row. Check lengths, overflow and native int limits before narrowing
or allocation. Guard C row
views too, not only Lua copies. These are facade input/output limits, not a
hard ceiling on provider allocations, query work, or a single received PG value.
Reject excess rather than truncate values or metadata.

## Transactions and future liblockdc integration

Local transaction helpers check begin, callback and commit separately. Lua
returns true followed by callback results on success; nil/error requests
rollback; a raised error rolls back and re-raises the original object. An ordinary
return including no values requests commit. Preserve the primary failure if
rollback fails, with secondary cleanup diagnostics. Reject nesting and detect
transactions started through SQL. A callback may not leave active cursors or
statements; close them and fail the transaction instead of committing. An idle
reusable statement created before the helper is not an active result; newly
created statement/cursor handles may not escape the callback. An
unfinished PostgreSQL cursor closes the connection, so do not send rollback
through a freed connection. SQLite defaults to DEFERRED with explicit IMMEDIATE
or EXCLUSIVE; PostgreSQL retains provider-default isolation initially.

Parameterization does not forbid deliberate transaction-control SQL. Callbacks
must not manually begin/end the helper's transaction. Check native transaction
state and completion tags after callback operations; an ownership-changing
completion fails the helper, even if the callback catches an earlier error.
PostgreSQL must be IN_TRANSACTION before committing, and the completion tag must
be COMMIT, not ROLLBACK. An IN_ERROR transaction is rolled back and fails. These
checks enforce the trusted-client contract, not a SQL sandbox or a guarantee
against arbitrary hostile functions. SQLite rollback failure or a still-active
transaction invalidates/closes the owned connection, preserving both failures.

Return transaction outcome separately from transport status: not_started,
committed, rolled_back or unknown. Connection loss after commit submission can
mean the server committed without an acknowledgement; report unknown rather
than claim rollback or encourage blind replay. A confirmed COMMIT followed by
drainage failure remains committed with a transport error. Roll back only while
the original transaction is known active and no commit acknowledgement or
ambiguous commit submission rules that out. Lua error records and C diagnostics
must carry this outcome, including cleanup failures.

liblockdc will own the PostgreSQL XA driver, enlistment, global identifiers,
durable decisions, fencing and recovery. None is implemented in this task.
Keep physical connection identity and native ownership distinct from query and
local-transaction helpers. A future official adapter must use the same enlisted
connection, prohibit conflicting local transaction ownership, and honor borrow
lifetimes. Do not expose raw native handles to Lua, invent placeholder XA APIs,
duplicate a coordinator, or promise atomicity with today's Lockdc outbox.
The separate upstream handoff is intentionally stored under ignored stash/.

## App lifecycle

Provide matching C app registration helpers and Lua `app:sqlite(opts)` /
`app:pg(opts)`. Registration captures a copied configuration and unique name
before startup without opening a native database. Each using process lazily
opens its own native handle after the applicable app start/fork boundary.
Standalone receivers open explicitly outside app ownership.

App stop/restart closes materialized process-local databases and invalidates
outstanding children; a retained app declaration can rematerialize after a
successful restart. App close detaches/invalidate declarations without leaving
a borrowed app/logger pointer. Explicit receiver close unregisters safely.
Copied configurations survive caller mutation and Lua GC. An inherited live
standalone database fails PID ownership checks; it cannot silently reconnect,
commit or close its parent's active transaction. Connections are also confined
to their owning thread. No pool, transparent retry, reconnection or hidden
worker thread is added. Blocking Lua calls inside HTTP callbacks remain explicit
blocking work; C readiness does not constitute an automatic HTTP scheduler.

## Verification and delivery sequence

1. Investigate provider ownership, statement/transport behavior and packaging;
   write this specification and the self-contained upstream XA proposal.
2. Review the specification against headers, guides, implementation precedents
   and failure scenarios; repair it before implementation.
3. Implement C shells/common values, SQLite, PostgreSQL, Lua, app lifecycle,
   examples/manual, dependency metadata/export and packaging seams. Use only
   focused fast builds/tests during this phase.
4. Once the whole public API implementation has landed, run the full native
   suite, memory/package gates and supported platform builds, preserving QEMU
   and native-Darwin evidence requirements. Distinguish native execution from
   cross compilation. Do not push or launch hosted checks without authorization.
5. Review the complete implementation after that full verification. Fix
   findings with focused debug tests only and repeat review until clean. Do
   not repeat full platform builds for small fixes; state their proof limits.

Fast tests use in-memory/tiny SQLite databases, immediate BUSY, deliberate bad
bindings, NUL/NULL/empty values, exact integer boundaries, duplicate columns,
tail rejection before effects, latched EOF/errors, callback/commit/rollback
failure, live-child cleanup, use-after-close, OOM and fork/PID ownership. Verify
rejected tail PRAGMAs leave settings unchanged, copying OOM latches failure,
callback-result preservation survives allocation faults, and lifecycle errors
caught inside transaction callbacks leave ownership intact.
PostgreSQL adds deterministic fragmented protocol fixtures, readiness/backpressure,
late server errors, SQLSTATE, early close, active-cursor exclusivity and timeout
logic using injected events/clocks rather than deliberate delays.
Include dropped-result retrieval faults, nonblocking output-pressure cleanup,
swallowed statement errors followed by COMMIT, explicit transaction-control
violations, and lost commit acknowledgement with an unknown outcome.

Real PostgreSQL e2e uses rootless Podman kube play, generated local credentials
and removable host-owned data under build/devenv/. Verify parameters containing
injection-shaped values, NULL/binary inputs, text bigint precision, constraints,
transactions, streaming rows/terminal errors, connection reuse, TLS and cleanup.
Never treat a fake server as proof of PostgreSQL SQL semantics. Add normal/Linux
packed Lua tests, C/C++ standalone headers, installed static/shared CMake and
pkg-config consumers, exact exports, source-rock contents and target integer/
linkage checks. Tests must be deterministic, bounded and produce fast feedback.
