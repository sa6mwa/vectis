# Lua zlib Facade

The embedded Vectis runtime preloads `require("zlib")` as a raw, buffered
facade over the bundled zlib dependency.

## Entry Points

- `zlib.version()` returns the linked zlib version string.
- `zlib.deflate(data[, opts])` compresses a string as a zlib-wrapped stream.
- `zlib.inflate(data[, opts])` decompresses a zlib-wrapped stream.
- `zlib.compress(data[, opts])` aliases `deflate`.
- `zlib.decompress(data[, opts])` auto-detects zlib or gzip input.
- `zlib.gzip(data[, opts])` compresses a string as gzip.
- `zlib.gunzip(data[, opts])` decompresses gzip input.

These APIs are explicitly buffered. They materialize the full input and output
as Lua strings. Do not treat them as streaming helpers.

## Options

- `level`: compression level for `deflate`, `compress`, and `gzip`; default
  `-1`, valid range `-1..9`.
- `max_output_bytes` or `max_output`: decompression and compression output
  safety cap; default `67108864`.

Expected zlib failures return `nil, err`, where `err.status`,
`err.status_string`, and `err.message` follow the Vectis structured error
shape. Programmer misuse, such as a non-table options value or invalid level,
raises a Lua error.

## Example

```lua
local zlib = require("zlib")

local body = ("hello\n"):rep(100)
local compressed = assert(zlib.gzip(body, { level = 9 }))
local restored = assert(zlib.decompress(compressed, {
  max_output_bytes = 1024 * 1024,
}))

assert(restored == body)
```
