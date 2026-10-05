local mdf = require("libmdf")
local terminal = require("vectis.terminal")
assert(require("vectis").libs.libmdf == mdf)
assert(terminal.libs.libmdf == mdf)

local function count(text, needle)
  local total, offset = 0, 1
  while true do
    local first, last = text:find(needle, offset, true)
    if not first then return total end
    total, offset = total + 1, last + 1
  end
end

local function plain(text)
  return (text:gsub("\27%]8;;.-\27\\", ""):gsub("\27%[[%d;]*m", ""))
end

local function streamed(render, text, opts, fragment)
  local offset, chunks = 1, {}
  assert(render(function(cap)
    if offset > #text then return nil end
    local last = math.min(#text, offset + math.min(cap, fragment) - 1)
    local chunk = text:sub(offset, last)
    offset = last + 1
    return chunk
  end, function(chunk) chunks[#chunks + 1] = chunk end, opts))
  return table.concat(chunks)
end

local table_md = "| value |\n| --- |\n| `left\\|right` |\n"
for _, format in ipairs({"html", "deck"}) do
  local opts = {format = format}
  local html = mdf.render(table_md, opts)
  assert(html:find(">left|right</", 1, true), "escaped code pipe")
  assert(not html:find("left\\|right", 1, true), "table escape is consumed")
  assert(count(html, "@font-face{") == 2, "one font face per style")
  assert(count(html, "data:font/woff2;base64,") == 2, "one payload per style")
  assert(count(html, "font-weight:400 700;font-style:normal;") == 1)
  assert(count(html, "font-weight:400 700;font-style:italic;") == 1)
  assert(assert(terminal.markdown(table_md, opts)) == html)
  for _, fragment in ipairs({1, 3, 64}) do
    assert(streamed(mdf.render_stream, table_md, opts, fragment) == html)
    assert(streamed(terminal.markdown_stream, table_md, opts, fragment) == html)
  end
end

local opts = {ansi_mode = "on", osc8 = true, width = 12}
local link_md = "# [one two three](https://x) after\n\nabc [  hi](https://x)\n"
local expected = "# one two\n  three\n  after\n\nabc   hi\n"
local ansi = mdf.render(link_md, opts)
assert(plain(ansi) == expected, "wrapped labels and leading spaces")
assert(count(ansi, "\27]8;;https://x\27\\") == 3)
assert(count(ansi, "\27]8;;\27\\") == 3, "balanced hyperlink boundaries")
for _, fragment in ipairs({1, 3, 64}) do
  assert(streamed(mdf.render_stream, link_md, opts, fragment) == ansi)
  assert(streamed(terminal.markdown_stream, link_md, opts, fragment) == ansi)
end

for _, fragment in ipairs({1, 64}) do
  local chunks = {}
  local handle = mdf.new({ansi_mode = "on", osc8 = true, width = 80})
  assert(handle:set_sink(function(chunk) chunks[#chunks + 1] = chunk end))
  local text = "([hello](https://x))"
  for offset = 1, #text, fragment do
    assert(handle:write_token({type = "text", text = text:sub(offset, offset + fragment - 1)}))
  end
  assert(handle:finish())
  assert(plain(table.concat(chunks)) == "(hello)\n", "manual finish flushes label")
  local writes = #chunks
  assert(handle:finish())
  assert(#chunks == writes, "finish does not duplicate output")
  handle:close()
  handle:close()
  assert(not pcall(handle.finish, handle), "closed renderer is unusable")
end

local first_emitted, suffix, reads = false, "", 0
assert(terminal.markdown_stream(function()
  reads = reads + 1
  if reads == 1 then return "# First\n\n" end
  assert(first_emitted, "document content must reach writer before input EOF")
  if reads == 2 then return "second\n" end
  return nil
end, function(chunk)
  suffix = (suffix .. chunk):sub(-64)
  first_emitted = first_emitted or suffix:find(">First<", 1, true) ~= nil
end, {format = "html"}))

local sent = false
local ok, err = terminal.markdown_stream(function()
  if sent then return nil end
  sent = true
  return "hello\n"
end,
  function() return false end)
assert(ok == nil and type(err) == "table", "sink failure is observable")
ok, err = terminal.markdown_stream(function() error("reader failure") end,
  function() end)
assert(ok == nil and type(err) == "table" and type(err.message) == "string")
assert(assert(terminal.markdown("healthy\n")):find("healthy", 1, true))
print("upgrade seam behavior ok: libmdf " .. mdf.version)
