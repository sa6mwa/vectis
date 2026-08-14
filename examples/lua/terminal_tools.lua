local terminal = require("vectis.terminal")

local ansi = assert(terminal.markdown("# Vectis\n\n**terminal** helper\n", {
  width = 72,
}))
assert(ansi:find("terminal", 1, true))

local chunks = {
  "## Streamed\n\n",
  "- callback reader\n",
  "- callback writer\n",
}
local index = 1
local rendered = {}
assert(terminal.markdown_stream(function()
  local chunk = chunks[index]
  index = index + 1
  return chunk
end, function(chunk)
  rendered[#rendered + 1] = chunk
end, {
  width = 72,
}) == true)
local streamed = table.concat(rendered)
assert(streamed:find("Streamed", 1, true))
assert(streamed:find("callback reader", 1, true))

local html = assert(terminal.markdown("# HTML\n", {
  format = "html",
  width = 72,
}))
assert(html:find("<!doctype html>", 1, true))

local invalid, invalid_err = terminal.markdown("# Bad\n", {
  format = "text",
})
assert(invalid == nil)
assert(invalid_err.status == require("vectis").ERR_INVALID)
assert(invalid_err.source == "vectis")
assert(invalid_err.message:find("unknown format", 1, true))

local editor = assert(terminal.editor({ line_max_len = 64 }))
assert(editor:set_buffer("vectis"))
assert(editor:insert(" terminal"))
assert(editor:buffer() == "vectis terminal")
assert(editor:set_cursor(6))
assert(editor:insert(" lua"))
assert(editor:buffer() == "vectis lua terminal")
editor:close()

print("lua terminal tools example ok")
