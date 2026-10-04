# Lua DSV Facade

Vectis preloads `vectis.dsv` in the embedded Lua runtime. The facade exposes
the C SDK DSV/CSV/TSV helpers to Lua without hiding materialization or spill
behavior.

## Entry Points

- `vectis.dsv.parse(opts)` parses DSV into a Lua table.
- `vectis.dsv.parse_json(opts)` parses DSV into a generated JSON array string.
- `vectis.dsv.each(opts)` invokes a Lua callback for each parsed row.
- `vectis.dsv.parse_spill(opts)` parses DSV into a JSON array using the C SDK
  spill policy and returns either `data` (also exposed as `memory`) or `path`.
- `vectis.dsv.to_string(opts)` materializes DSV from tables or LoneJSON records.
- `vectis.dsv.write(opts, sink)` writes native chunks directly to a sink function
  or `{write=function(chunk) ... end}` table, returning `true, bytes`.
- `vectis.app:dsv(opts)` registers a streaming DSV upload route; see
  [Lua app](lua-app.md).

`parse()` and `parse_json()` are materialized. `each()` uses the C parser's
source-to-callback path when `schema` is provided; without a schema it iterates
the materialized parse result directly.

Expected parse, serialization, spill, and row-callback stop failures return
`nil, err`, where `err` is a structured table with at least `message`,
`status`, `status_string`, `source`, and `source_code`. Callback stops use
`source = "vectis"`; generated JSON decode failures use
`source = "lonejson"`. Programmer misuse, such as omitting the `each()`
callback, raises a Lua error. Invalid `to_string()` row assignments raise the
schema assignment error after releasing accumulated serialization output.

## Options

- `schema`: LoneJSON schema userdata. Required for typed rows, streaming
  `each()`, `write()`, and `to_string()`. Untyped `each()` materializes input.
  Typed DSV parsing supports LoneJSON dynamic and
  fixed-capacity string fields.
- `data`, `dsv`, `csv`, or `tsv`: DSV string input.
- `path`: DSV file path input.
- `format`: `csv` or `tsv`; default `csv`.
- `delimiter`, `quote`, `escape`: one-byte character controls.
- `header`: false for headerless input/output.
- `headerless` or `rows_only`: aliases for headerless input.
- `columns`: explicit column names, required for typed headerless input.
- `comment_prefix`: skips unquoted rows whose first field starts with the
  prefix.
- `strict_row_width`: default true.
- `trim_cr`: default true.
- `indented_comments`: default true.
- `max_field_bytes`: per-field safety limit.
- `memory_limit_bytes`, `directory`, `prefix`: spill controls for
  `parse_spill()`.

`write()` and `to_string()` share the native row emitter; only `to_string()`
collects output. `write()` accepts `rows` (or `records`) as an array of value
tables or a zero-argument iterator returning the next table, then `nil` at EOF.
Iterator throws or `nil, error` raise after cleanup. Each row is assigned into
private record storage and emitted before the next iterator call; sink failure
stops the producer immediately. Empty rows still emit the header unless disabled.
The sink runs synchronously on the caller state, receives chunks up to 4096 bytes,
and remains caller-owned. No whole-output buffer or output staging file is used.
Sink `false`, `nil, error`, or throw returns `nil, err`; no return is success.

Callback writes reject externally mutable record userdata because public
LoneJSON interop has no mutation lease. Buffered `to_string()` retains record
support. The schema, copied column names and option strings stay rooted through
callbacks/GC; changing original columns/options cannot change later rows.
Assignment errors release private record/output resources before raising.
Spooled input assignment follows the schema's storage policy, without
materializing the whole output.

Both writers preserve optional scalar presence: an absent LoneJSON scalar
emits an empty cell rather than a synthetic zero or `false`. It applies the
same configured quoting policy to strings, numbers, and booleans, including
when a scalar contains the delimiter or begins with `comment_prefix`.

## Example

```lua
local dsv = require("vectis.dsv")
local lonejson = require("lonejson")

local schema = lonejson.schema("row", {
  lonejson.field("id", lonejson.string({required = true})),
  lonejson.field("count", lonejson.i64({required = true})),
})

local rows = assert(dsv.parse({
  schema = schema,
  data = "id,count\nalpha,2\n",
}))
assert(rows[1].count == 2)
```
