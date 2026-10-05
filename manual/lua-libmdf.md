# Lua libmdf

`require("libmdf")` is the dependency-native libmdf Lua module bundled into the
`vectis` runner. It exposes Markdown rendering directly from libmdf for Lua
5.5 scripts that need terminal ANSI or HTML output. Use
[`vectis.terminal`](lua-terminal.md) when the narrower Vectis terminal defaults
are enough.

## Entry Points

- `libmdf.version` is the linked libmdf version string.
- `libmdf.render(markdown[, opts])` renders a Markdown string.
- `libmdf.render_stream(reader, writer[, opts])` renders Markdown from a
  callback reader into a callback writer.

Common options:

- `format = "ansi"` for terminal output.
- `format = "html"` for HTML output.
- `format = "deck"` for HTML slides.
- `width = n` to constrain wrapping where the renderer supports it.
- `ansi_mode = "auto"|"on"|"off"` to control ANSI escapes independently of
  decoration. The default `auto` enables styling only for a known terminal.
- `output_fd = libmdf.file_descriptor(io.stdout)` to detect the actual output
  destination. This descriptor is borrowed and is neither written to nor closed.

String and callback sinks without `output_fd` default to escape-free UTF-8
output at width 80. Use `ansi_mode = "on"` when the returned string or callback
output must contain terminal styling; `off` also removes input escape sequences.
HTML rendering is unaffected.

`render_stream` is callback-backed: the reader returns one string chunk at a
time and returns `nil` at EOF; the writer receives output chunks as they are
produced. Do not treat it as a buffered string helper.

The bundled libmdf 0.14.0 preserves hyperlink labels, punctuation and enclosing
styles across fragmented ANSI input and wrapping. Escaped table pipes render
as literal `|`, including inside inline code. HTML and deck output declare each
default font style once, with a variable weight range of 400–700.

For incremental documents, use dependency-native
`libmdf.document_stream(opts, writer)` and its `write`, `finish_document` and
`close` methods. Manual-token rendering uses `libmdf.new(opts)`, `set_sink`,
`write_token` and `finish`; `finish` emits any deferred link label and repeated
successful finish does not duplicate output. Reader/writer failures are
observable errors. Rendering may retain unfinished layout decisions; HTML
tables retain the whole table and ANSI tables follow `table_buffer_mode`.

## Example

```lua
local libmdf = require("libmdf")

local html = assert(libmdf.render("# Vectis\n\n**ready**\n", {
  format = "html",
  width = 72,
}))
assert(html:find("Vectis", 1, true))

local input = {"# Stream\n\n", "- one\n", "- two\n"}
local index = 1
local output = {}
assert(libmdf.render_stream(function()
  local chunk = input[index]
  index = index + 1
  return chunk
end, function(chunk)
  output[#output + 1] = chunk
end, {
  format = "html",
}))
assert(table.concat(output):find("Stream", 1, true))
```

This module is intentionally dependency-native. Do not add a broader
Vectis-owned Markdown API unless the workflow crosses Vectis concepts such as
server responses, packed assets, service logging, or terminal prompt handling.
