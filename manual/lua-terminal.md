# Lua Terminal Helpers

`require("vectis").terminal` is a small Vectis-owned DX layer over the
dependency-native `libmdf` and `softline` Lua modules. It exists for
terminal-facing workflows that need Markdown rendering or bounded line editing
without hiding the direct modules.

Direct access remains available through:

- `vectis.terminal.libs.libmdf`
- `vectis.terminal.libs.softline`

## Entry Points

- `terminal.markdown(markdown[, opts])` renders a Markdown string with
  `libmdf.render`. The default format is `ansi`; pass `format = "html"` when
  HTML output is required.
- `terminal.markdown_stream(reader, writer[, opts])` renders Markdown from a
  callback reader into a callback writer with `libmdf.render_stream`. This is a
  real callback-backed flow; it does not concatenate all input before rendering.
- `terminal.editor(opts)` creates a `softline` editor with a default
  `line_max_len` of `4096` when none is supplied.

The Markdown helpers forward libmdf's `ansi_mode` and `output_fd` options.
String and callback destinations default to escape-free UTF-8 output. Pass
`ansi_mode = "on"` for terminal styling, or
`output_fd = terminal.libs.libmdf.file_descriptor(io.stdout)` when the output
will be written to stdout and should follow its terminal status.

The helpers pass wrapping, OSC8 hyperlinks and HTML/deck options through to
libmdf, including `html_c89` branding and its link-label/escaped-table-pipe fixes. Persistent
incremental documents and manual tokens remain available through
`terminal.libs.libmdf`; they do not need an additional Vectis wrapper.

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

The bundled MDF 0.18.0 includes the upstream callback, result-copy and font/token
ownership fixes from 0.17.0. Options still pass through unchanged. Native palette
lookup is available in the C SDK; the upstream Lua facade has no palette API.
