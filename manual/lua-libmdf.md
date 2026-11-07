# Lua libmdf

`require("libmdf")` is the dependency-native libmdf Lua module bundled into the
`vectis` runner. It exposes Markdown rendering directly from libmdf for Lua
5.5 scripts that need terminal ANSI or HTML output. Use
[`vectis.terminal`](lua-terminal.md) when the narrower Vectis terminal defaults
are enough.

Vectis consumes this upstream Lua facade unchanged; see the
[boundary and gap assessment](../docs/upstream-lua-facades.md).

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
- `margin_left = n` and `margin_right = n` to set ANSI margins.
- `osc8 = true` to request explicit terminal hyperlinks when ANSI is enabled.
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

The bundled libmdf 0.23.0 preserves hyperlink labels, punctuation and enclosing
styles across fragmented ANSI input and wrapping. Escaped table pipes render
as literal `|`, including inside inline code. HTML and deck output declare each
default font style once, with a variable weight range of 400–700.

With OSC8 off and `margin_right = 0`, visible URLs outside table cells remain
byte-contiguous, including styled URLs and list destinations. The terminal
wraps them naturally; continuation rows have no inserted left margin. Set
`width` to the physical terminal column count so following text starts at the
correct cursor position. A positive right margin retains hard URL wrapping.
Click detection belongs to the terminal/multiplexer; OSC8 is an optional
alternative for supporting terminals and tmux configurations.

For incremental documents, use dependency-native
`libmdf.document_stream(opts, writer)` and its `write`, `finish_document` and
`close` methods. Manual-token rendering uses `libmdf.new(opts)`, `set_sink`,
`write_token` and `finish`; `finish` emits any deferred link label and repeated
successful finish does not duplicate output. Reader/writer failures are
observable errors. Rendering retains unfinished layout decisions. ANSI and HTML
tables follow `table_buffer_mode = "full"|"row"`. Full mode switches to row
output after 65536 retained bytes or 1024 rows, continuing the same table. A
single pending row/prefix exceeding the limit fails; charts have a separate
65536-byte limit. Decks retain one slide at a time.

For literal fragments, use a handle's `write_literal(text)`, then `finish()`;
Markdown punctuation, entities and escapes remain literal, while normal word
wrapping applies. Manual tokens also accept `type = "literal_text"`.
`libmdf.escape_markdown(text)` and `handle:escape_markdown(text)` instead return
a materialized escaped string for later Markdown rendering; they do not render.
Plain intraword underscores and literal NBSP bytes are preserved.
`detect_osc8_support()` now accepts an xterm-compatible `TERM` when more specific
terminal hints are absent, including SSH sessions. Explicit disable controls and
known terminal hints still take precedence; ordinary pipe rendering stays plain.

Handles and document streams expose `set_word_buffer(bytes, disable_spooling)`.
Configure it before input or with no pending decision. Zero selects 4096 bytes;
the maximum is 65536. By default each larger undecided word uses temporary file
backing capped at 1 MiB, replayed in bounded chunks at its normal boundary.
Excess bytes are discarded. With `disable_spooling = true`, only the configured
memory prefix is retained. Reaching a limit never forces premature output.
Temporary files close at completion, reset, failure or destruction; file errors
fail the operation. These are per-word layout limits, not whole-document staging.

## Interactive paging

`libmdf.pager(path, opts)` views a named regular file on the controlling terminal.
Omit `format` for extension detection, or select `"text"`, `"markdown"` or
`"text/markdown; charset=utf-8"`. Navigation includes search, cyclic next/previous
matches and resize reflow. Input bytes must remain stable until it returns;
appended log bytes are not followed.

`cache_budget` configures retained rendered content: zero/default selects 1 MiB,
and the minimum nonzero budget is 65536 bytes. Search/index workspace is separately
bounded; the budget is not a whole-process memory ceiling. The source is read on
demand without a whole-file copy. Evicted Markdown and resize can replay a prefix.
Oversized terminal geometry fails after terminal restoration.

`should_cancel = function() ... end` runs synchronously on the calling Lua state.
A true result restores terminal/signal state and returns `false, "cancelled"`;
ordinary quit returns `true`. Callback exceptions are rethrown unchanged after
cleanup. Cancellation cannot preempt a blocking source or terminal write. Ctrl-C
forwards SIGINT after cleanup; if the restored handler returns or ignores it,
the result is also `false, "cancelled"`.

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

MDF 0.17's upstream Lua lifetime fixes are included: source/sink/trace failures
unwind through native cleanup, materialized result copying keeps its owner alive,
and configured font/token bytes remain pinned through mutation and GC. Direct
MDF calls preserve the original callback error object; Vectis terminal helpers
retain their documented `nil, err` translation. C-string options require actual
NUL-free strings; length-aware font byte strings remain binary-safe.

MDF 0.18's allocation-free theme palette and heading lookups are public C APIs
in the SDK. The released Lua facade does not expose them, and Vectis adds no
local provider extension. Existing `theme` render options remain available.
