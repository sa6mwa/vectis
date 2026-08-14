# Lua softline

`require("softline")` is the dependency-native softline Lua module bundled into
the `vectis` runner. It exposes the softline editor handle for Lua 5.5 scripts
that need bounded line-editing or prompt-state manipulation. Use
[`vectis.terminal`](lua-terminal.md) when the narrower Vectis editor defaults
are enough.

## Entry Points

- `softline.new([opts])` creates an editor handle.

Common constructor options:

- `line_max_len = n` bounds the editable buffer length.

Editor handles used by the Vectis tests and examples expose:

- `editor:set_buffer(text)`
- `editor:buffer()`
- `editor:set_cursor(offset)`
- `editor:insert(text)`
- `editor:close()`

The handle owns its buffer and terminal/editor state. Close it when the prompt
workflow is done.

## Example

```lua
local softline = require("softline")

local editor = assert(softline.new({ line_max_len = 64 }))
assert(editor:set_buffer("vectis"))
assert(editor:insert(" orders"))
assert(editor:buffer() == "vectis orders")
assert(editor:set_cursor(6))
assert(editor:insert(" local"))
assert(editor:buffer() == "vectis local orders")
editor:close()
```

This module is intentionally dependency-native. Do not add a Vectis-owned
wrapper for individual editor operations; keep `vectis.terminal` focused on
defaults and cross-library terminal workflows.
