# SQLite Lua receivers

`require("vectis.sqlite")` and `vectis.sqlite` expose embedded SQLite through
the installed public cpkt C89 API. This is a Vectis-owned binding. It requires
the native embedded runtime; the Lua source rock alone does not supply SQLite.

```lua
local sqlite = require("vectis.sqlite")
local db <close> = assert(sqlite.open(":memory:"))
assert(db:exec("create table item (id INTEGER PRIMARY KEY, value TEXT)", {}))
assert(db:transaction(function(connection)
  assert(connection:exec("insert into item values (?,?)", {1, "saved"}))
end))
local rows <close> = assert(db:query("select id,value from item", {}))
local row, err = rows:next()
assert(row, err and err.message)
assert(row[1] == 1 and row[2] == "saved")
```

`sqlite.open(path[, opts])` opens an explicit path or `:memory:`. Options are
`mode` (`readwrite_create`, `readwrite`, `readonly`), `uri` (default false), and
`busy_timeout_ms` (default zero). Unknown options are errors. No implicit WAL,
extension loading or global callback configuration is performed.

`db:exec(sql, params)` returns `{changes, last_insert_rowid}`. It rejects row
results; use `db:query(sql, params)` for a cursor. All execution requires an
explicit dense parameter array, including `{}` for no parameters. One statement
is permitted. Trailing executable SQL is rejected before stepping; native SQL
preparation can itself have provider-defined effects. This is not a SQL sandbox.

Parameters accept Lua integers, finite numbers, booleans (0/1), binary-length
text strings, `sqlite.blob(bytes)` and `sqlite.NULL`. The NULL identity is also
`require("vectis.sql").NULL` and `vectis.pg.NULL`. NULL differs from empty text
and empty BLOB. Integers preserve the exact signed Lua integer range.

`cursor:next()` returns a dense row array, `nil,nil` at EOF or `nil,error` on
execution failure. Text and BLOB output are Lua byte strings. NULL stays the
sentinel, so the array has no holes. `cursor:columns()` returns ordered
`{name, declared_type}` records and preserves duplicate names. Metadata must be
read before cursor EOF, which finalizes its native statement. Neither rows nor
errors are silently skipped; repeated reads preserve EOF or failure.

`db:prepare(sql)` creates a reusable statement with the same `next` and
`columns` methods. Call `stmt:bind(params)` before stepping. After EOF or failure,
`stmt:reset()` resets execution and clears bindings; bind again before reuse.
A native reset failure is returned even though reset has cleared execution.

`db:transaction(fn[, {mode=...}])` supports `DEFERRED` (default), `IMMEDIATE`
and `EXCLUSIVE`. It invokes `fn(db)` and returns `true` plus its values on commit.
Returning `nil,error` requests rollback; the returned structured error retains
that value as `cause`. A thrown error rolls back and rethrows the original
object. Errors include `transaction_outcome` (`not_started`, `committed`,
`rolled_back` or `unknown`). Nested/manual transaction control, closing the
database during the callback, and escaping active/new native statements fail
the helper even if an attempted operation's error was caught.

`db:transaction_diagnostic()` returns the last transaction's diagnostic,
including `transaction_outcome` and a separate `cleanup_error` if rollback
failed. It is also available after catching a callback exception. A rollback
failure invalidates the connection; an app declaration can reopen only after
an explicit app stop/start or restart, never silently after an unknown outcome.

Expected failures return `nil,error` with `status`, `status_string`, `source`,
`source_code`, `dependency_code`, `operation` and `message`. Argument misuse
raises. SQL is limited to 1 MiB, 256 parameters and 1024 columns; total parameter
bytes and copied row bytes are each limited to 16 MiB. Excess fails without
truncation. These limits do not bound all provider allocations or query work.

Resources have idempotent Lua `close`, `__close` and `__gc`. Database close
invalidates retained statements. Receivers belong to their creating process and
thread; inherited live databases cannot reconnect or execute in a child. Native
public equivalents are in `<vectis/sql.h>` and `<vectis/sqlite.h>`.

`app:sqlite({name="local-data", path="data.sqlite", ...})` declares an app-owned
database before startup. It accepts the open options above and copies the
configuration. No file opens during declaration. The using process materializes
its own handle after app startup/fork. Stop invalidates live statements; after
restart the declaration can open again. App close detaches the declaration.
App lifecycle changes are forbidden inside a transaction callback, even if the
attempted operation's error is caught. The declaration retains its Lua app owner
through protected operations and rejects use before start or after app close.
