# Lua pslog

`require("pslog")` is the raw libpslog Lua module bundled into the `vectis`
runner. Vectis preloads it unchanged for applications that need the complete
upstream logging surface. Use [`vectis.log`](lua-log.md) when the narrower
Vectis helper defaults are enough.

## Entry Points

- `pslog.new(output_or_opts[, opts])` creates a logger.
- `pslog.new_json(output_or_opts[, opts])` creates a JSON logger.
- `pslog.new_structured(...)` aliases `new_json`.
- `pslog.from_env([prefix,] opts)` creates a logger from environment-backed
  pslog configuration.
- `pslog.version()` returns the linked libpslog version string.
- `pslog.level_string(level)` and `pslog.parse_level(name)` convert levels.
- `pslog.available_palettes()` returns console palette names.

Common value wrappers are also exposed: `bytes`, `time`, `time_unix`,
`duration`, `duration_ns`, `duration_us`, `duration_ms`, `duration_s`, `errno`,
`trusted`, and `u64`.

## Example

```lua
local pslog = require("pslog")

local chunks = {}
local logger = assert(pslog.new_json({
  output = function(chunk)
    chunks[#chunks + 1] = chunk
  end,
  disable_timestamp = true,
  no_color = true,
})):with("service", "orders")

logger:info("order accepted", "id", "1001", "bytes", pslog.u64(42))
logger:close()

assert(table.concat(chunks):find('"service":"orders"', 1, true))
```

`pslog` is a raw dependency facade. Expected Vectis helper errors are not added
to raw pslog calls; use `vectis.log.log_error()` when Vectis structured error
metadata should be written as fields.
