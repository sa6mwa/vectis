# Lua zlib Facade

The embedded Vectis runtime preloads `require("zlib")` as a
dependency-native facade over the bundled zlib dependency.

## Entry Points

- `zlib.version()` returns the linked zlib version string.
- `zlib.deflate(data[, opts])` compresses a string as a zlib-wrapped stream.
- `zlib.inflate(data[, opts])` decompresses a zlib-wrapped stream.
- `zlib.compress(data[, opts])` aliases `deflate`.
- `zlib.decompress(data[, opts])` auto-detects zlib or gzip input.
- `zlib.gzip(data[, opts])` compresses a string as gzip.
- `zlib.gunzip(data[, opts])` decompresses gzip input.
- `zlib.deflate_file(opts)` compresses `input_path` to `output_path` as zlib.
- `zlib.inflate_file(opts)` decompresses zlib `input_path` to `output_path`.
- `zlib.compress_file(opts)` aliases `deflate_file`.
- `zlib.decompress_file(opts)` auto-detects zlib or gzip file input.
- `zlib.gzip_file(opts)` compresses `input_path` to `output_path` as gzip.
- `zlib.gunzip_file(opts)` decompresses gzip `input_path` to `output_path`.

The string APIs are explicitly buffered. They materialize the full input and
output as Lua strings. The file APIs are file-backed and process data through
bounded chunks; they do not materialize the full payload in Lua.

## Options

- `level`: compression level for `deflate`, `compress`, and `gzip`; default
  `-1`, valid range `-1..9`.
- `max_output_bytes` or `max_output`: decompression and compression output
  safety cap; default `67108864`.
- `input_path` or `path`: file input for `*_file()` helpers.
- `output_path` or `to`: file output for `*_file()` helpers.
- `remove_output_on_failure`: defaults true for `*_file()` helpers.

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

```lua
local zlib = require("zlib")

local packed = assert(zlib.gzip_file({
  input_path = "site.tar",
  output_path = "site.tar.gz",
  level = 9,
}))
assert(packed.output_bytes > 0)

assert(zlib.gunzip_file({
  input_path = "site.tar.gz",
  output_path = "site-restored.tar",
  max_output_bytes = 1024 * 1024 * 1024,
}).ok)
```
