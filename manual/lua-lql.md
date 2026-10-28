# Lua lql

`require("lql")` is the released liblql 0.4.0 Lua facade. It is the same table as
`require("lql.core")` and `require("vectis").libs.lql`. Vectis consumes it unchanged.
See the [upstream boundary](../docs/upstream-lua-facades.md) and
[upgrade review](../docs/liblql-0.4.0-upgrade.md).

## Handles and cleanup

`lql.new()` creates a client. Clients, selectors, projections and mutations have
idempotent `:close()` returning true and support Lua scoped cleanup:

```lua
local lql = require("lql")
local client <close> = assert(lql.new())
local selector <close> = assert(client:selector_parse('/status="ready"'))
local projection <close> = assert(client:projection_parse({"/id", "/status"}))
assert(projection:path_count() == 2)
local result, err = client:apply_string_spooled(selector,
  '{"id":7,"status":"ready"}\n', {projection=projection})
assert(result, err and err.message)
assert(result.records_matched == 1)
```

Closing a client invalidates child operations. Closing a selector expires its
borrowed cursors; callback values and range writers expire when their callback
returns. Closing an active handle succeeds immediately, fails the enclosing
operation and defers native destruction until unwind. Ordinary nested calls are
supported. Create clients on their execution thread after any application fork;
Vectis does not supply an app-owned LQL service or share clients between workers.

## Native operations

Module functions: `new`, `version`, `status_string`, `path_is_regular_file`.
Client methods include `version`, `capabilities`, `selector_capabilities`,
`selector_parse`, `selector_parse_or`, `selector_parse_json`, the upstream
`selector_build_*` constructors, `projection_parse`, `mutation_parse`,
`stream_apply`, `stream_apply_spooled`, `apply_string_spooled`,
`filter_file_spooled` and `rewrite_file_inline_spooled`. Selectors expose
`capabilities`, `is_empty`, `root` and `write_json`; projections expose
`path_count`/`path`, mutations `count`. Selector expressions use the upstream
JSON-pointer grammar, for example `/status="ready"`, not JSONPath.

`client:stream_apply(reader, selector, options)` consumes strict NDJSON with
bounded native buffers. `reader(capacity)` must return at most capacity bytes,
`nil` at EOF, or `nil, error` on failure. For decision-only filtering:

```lua
local result, err = client:stream_apply(function(capacity)
  return input:read(capacity) -- caller-owned file, closed by its owner
end, selector, {limits={max_records=1000}})
assert(result, err and err.message)
```

Options include the upstream `on_decision`, `on_value`, `writer`, `range_writer`,
`cancelled`, `time_now` and `limits`. Selected-record output and value callbacks
in true streaming require compact input and caller-owned range replay. There is
no hidden fallback: projection/mutation output needs the explicitly spooled API.
`stream_apply_spooled(reader, selector, options)` may materialize a record and
spill it to a temporary file. `apply_string_spooled(selector, input, options)`
already receives a materialized string. File helpers take `(selector, path,
options)`; inline rewrite owns its output and rejects an external writer.
The native receiver's shared 8 MiB allocation budget includes parsed handles,
plans and compatibility spools. Exhaustion returns a structured failure.

## File-backed mutations and failures

`client:mutation_parse(expressions, options)` accepts `file_value_open(path)`
returning a fresh reader function. Optional `file_value_close(reader_function)`
is captured alongside the opener when the mutation is parsed. Each successfully
opened reader receives one protected cleanup attempt on success, read failure,
early stop or active close. No open readers survive between applications.
A closer requires an opener; return true or no values on success, or false /
`nil, error` on failure. Exceptions become operation failures.

Ordinary operation failures return `nil, error`; the error has numeric `status`,
`status_string` and `message`. Invalid/closed userdata arguments can raise Lua
errors. Callback failure is terminal for that operation, preserves its first
error, and does not prevent reuse of a still-open mutation in a later operation.
Cleanup failure may be reported after output was written or an inline rewrite
committed: an error does not promise rollback. Option getter errors and Lua
allocation failures are cleaned up inside the released upstream facade.

Under persistent Lua allocator failure, protected cleanup may fail before entering
a reader's Lua closer. Vectis does not guarantee execution of Lua cleanup code
when Lua cannot allocate its call frame. The 0.4.0 integration fault test records
this remaining upstream limit; see the upgrade review for the reproduction.
