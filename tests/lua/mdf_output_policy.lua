local libmdf = require("libmdf")
local terminal = require("vectis.terminal")
assert(libmdf.version == "0.13.0")

local ansi_markdown = "# UTF-8 café\n\n**styled** [link](https://example.test)\n"
local plain_markdown = libmdf.render(ansi_markdown, { osc8 = true })
assert(plain_markdown:find("café", 1, true))
assert(not plain_markdown:find("\27", 1, true))
local styled_markdown = libmdf.render(ansi_markdown, { ansi_mode = "on" })
assert(styled_markdown:find("\27[", 1, true))
local escaped_markdown = libmdf.render("\27[31mred\27[0m café\n", {
  ansi_mode = "off", osc8 = true,
})
assert(escaped_markdown:find("café", 1, true))
assert(not escaped_markdown:find("\27", 1, true))
for _, mode in ipairs({"auto", "on", "off"}) do
  local offset, streamed = 1, {}
  local options = { ansi_mode = mode, osc8 = true }
  assert(libmdf.render_stream(function(cap)
    if offset > #ansi_markdown then return nil end
    local last = math.min(offset + math.min(cap, 3) - 1, #ansi_markdown)
    local chunk = ansi_markdown:sub(offset, last)
    offset = last + 1
    return chunk
  end, function(chunk)
    streamed[#streamed + 1] = chunk
  end, options))
  assert(table.concat(streamed) == libmdf.render(ansi_markdown, options))
end
local borrowed_output = assert(io.tmpfile())
local borrowed_fd = libmdf.file_descriptor(borrowed_output)
assert(type(borrowed_fd) == "number")
assert(not libmdf.render(ansi_markdown, { output_fd = borrowed_fd }):find("\27", 1, true))
assert(borrowed_output:write("descriptor is borrowed"))
assert(borrowed_output:close())

assert(not assert(terminal.markdown(ansi_markdown)):find("\27", 1, true))
assert(assert(terminal.markdown(ansi_markdown, {ansi_mode = "on"})):find("\27[", 1, true))
local input_done, chunks = false, {}
assert(terminal.markdown_stream(function()
  if input_done then return nil end
  input_done = true
  return ansi_markdown
end, function(chunk) chunks[#chunks + 1] = chunk end, {ansi_mode = "on"}))
assert(table.concat(chunks):find("\27[", 1, true))
