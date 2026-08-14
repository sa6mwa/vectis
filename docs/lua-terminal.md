# Lua Terminal Helpers

`require("vectis").terminal` is a small Vectis-owned DX layer over the raw
`libmdf` and `softline` Lua modules. It exists for terminal-facing workflows
that need Markdown rendering or bounded line editing without hiding the raw
modules.

Raw access remains available through:

- `vectis.terminal.raw.libmdf`
- `vectis.terminal.raw.softline`

## Entry Points

- `terminal.markdown(markdown[, opts])` renders a Markdown string with
  `libmdf.render`. The default format is `ansi`; pass `format = "html"` when
  HTML output is required.
- `terminal.markdown_stream(reader, writer[, opts])` renders Markdown from a
  callback reader into a callback writer with `libmdf.render_stream`. This is a
  real callback-backed flow; it does not concatenate all input before rendering.
- `terminal.editor(opts)` creates a `softline` editor with a default
  `line_max_len` of `4096` when none is supplied.

Unsupported render formats return `nil, err` with Vectis status/source
metadata. Programmer misuse, such as passing a non-string Markdown value or
missing stream callbacks, raises a Lua error.

```lua
local terminal = require("vectis.terminal")

local ansi = assert(terminal.markdown("# Vectis\n\n**ready**\n", {
  width = 72,
}))

local editor = assert(terminal.editor({line_max_len = 128}))
assert(editor:set_buffer("vectis"))
assert(editor:insert(" terminal"))
assert(editor:buffer() == "vectis terminal")
editor:close()
```
