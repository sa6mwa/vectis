# Lua Embedded Assets

`vectis.embedded` exposes packed asset helpers for self-contained Vectis
applications. The module is available both as `require("vectis.embedded")` and
as `require("vectis").embedded`; both forms return the same table.

This module is for assets included with `vectis -a pack --assets ...`. It does
not expose the packed Lua script itself.

## Functions

- `vectis.embedded.has_assets()` returns `true` when the running binary has an
  embedded asset tree.
- `vectis.embedded.default_extract_policy()` returns the pack-time default
  extraction policy.
- `vectis.embedded.tree_sha256()` returns the asset tree SHA-256, or `nil, err`
  when no asset tree exists.
- `vectis.embedded.stat(path)` returns metadata for one embedded path:
  `path`, `kind`, `size`, `mode`, `content_type`, `sha256`, and `etag` when
  available.
- `vectis.embedded.read(path)` returns the full embedded file body.
- `vectis.embedded.chunks(path[, chunk_size])` returns an iterator over bounded
  file chunks.
- `vectis.embedded.list(path)` returns embedded paths under a prefix.
- `vectis.embedded.extract(opts)` extracts the asset tree to disk. `opts.to`
  selects the destination directory and `opts.policy` can override the default
  policy.

`read`, `stat`, and `chunks` return structured Vectis errors for missing assets.
`tree_sha256`, `chunks`, and `extract` return structured Vectis state errors
when the binary has no embedded assets.

```lua
local embedded = require("vectis.embedded")

if embedded.has_assets() then
  local index = assert(embedded.read("/index.html"))
  local info = assert(embedded.stat("/assets/app.css"))

  for chunk in embedded.chunks("/assets/app.css", 8192) do
    -- write chunk to a response, file, or downstream sink
  end

  assert(embedded.extract({
    to = "/var/lib/myapp/site",
    policy = "repair",
  }))
end
```
