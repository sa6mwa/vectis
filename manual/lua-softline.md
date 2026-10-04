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

## Softline 0.9 queue and output behavior

Constructor fields are strict. Removed `bounded`, `screen_x`, `screen_y`,
`screen_height` and unknown `height` raise errors; `screen_width` remains an
ordinary readline wrapping hint. Vectis `terminal.editor(opts)` passes these
options directly through to the official binding.

`queue_append`, `queue_peek`, `queue_replace` and `queue_take` use one-based
oldest-first indices. `queue_mode(index)` returns `QUEUE_MODE_QUEUED` or
`QUEUE_MODE_STEER`; `queue_set_mode(index, mode)` sets delivery intent and
replacement preserves it. `set_queue_profile("default"|"queued_turns")`
and `set_queue_delivery("auto"|"manual")` control policy. Busy queued turns
stay queued in either delivery mode; the host explicitly selects steer entries
at a safe seam. Queue storage is finite (`prompt_queue_max_entries`).

The new native `output_stream_begin/write/end` sends validated UTF-8 and ANSI
SGR chunks immediately. Quote prefix/style and status-message/color methods
configure bounded output; they do not retain a complete producer transcript.
Smith already uses the native output/queue operations and streams CAI through
MDF. Terminal reflow and concurrent physical resize have upstream limits;
non-TTY tests do not prove every interactive rendering boundary.

`disable_image_paste=true` disables built-in Ctrl-V image capture.
`image_paste_path_template` and `set_image_paste_path_template(template)` copy
an absolute template containing exactly one `*`, accepting HOME/XDG tokens;
NUL is rejected. The actual X11 clipboard capture requires a display and is
outside the device-free queue/path tests. Softline bundles MIT c-ares for this
path; no extra Vectis resolver API is introduced.
