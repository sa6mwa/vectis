# Lua DSV Facade

Vectis preloads `vectis.dsv` in the embedded Lua runtime. The facade exposes
the C SDK DSV/CSV/TSV helpers to Lua without hiding materialization or spill
behavior.

## Entry Points

- `vectis.dsv.parse(opts)` parses DSV into a Lua table.
- `vectis.dsv.parse_json(opts)` parses DSV into a generated JSON array string.
- `vectis.dsv.each(opts)` invokes a Lua callback for each parsed row.
- `vectis.dsv.parse_spill(opts)` parses DSV into a JSON array using the C SDK
  spill policy and returns either `data` or `path`.
- `vectis.dsv.to_string(opts)` serializes Lua tables or LoneJSON records to DSV.
- `vectis.server:dsv(opts)` registers a streaming DSV upload route; see
  [Lua server](lua-server.md).

`parse()` and `parse_json()` are materialized. `each()` uses the C parser's
source-to-callback path when `schema` is provided; without a schema it iterates
the materialized parse result directly.

Expected parse, serialization, spill, and row-callback stop failures return
`nil, err`, where `err` is a structured table with at least `message`,
`status`, `status_string`, `source`, and `source_code`. Callback stops use
`source = "vectis"`; generated JSON decode failures use
`source = "lonejson"`. Programmer misuse, such as omitting the `each()`
callback, raises a Lua error.

## Options

- `schema`: LoneJSON schema userdata. Required for typed rows, `each()`, and
  `to_string()`. Typed DSV parsing supports LoneJSON dynamic and
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
