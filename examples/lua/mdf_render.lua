local libmdf = require("libmdf")

local html = libmdf.render("# Vectis\n\n**libmdf** Lua example\n\n"
    .. "| value |\n| --- |\n| `left\\|right` |\n", {
  format = "html",
  width = 72,
})

assert(html:match("Vectis"))
assert(html:match("libmdf"))
assert(html:find(">left|right</", 1, true))
io.write(html)

local chunks = {
  "\n## Streaming\n\n",
  "- bounded reads\n",
  "- direct sink writes\n",
}
local index = 1
-- Deliver chunks immediately; retain only a bounded suffix for example checks.
local tail, saw_heading, saw_body = '', false, false

local ok = libmdf.render_stream(function()
  local chunk = chunks[index]
  index = index + 1
  return chunk
end, function(chunk)
  io.write(chunk)
  tail = (tail .. chunk):sub(-1024)
  saw_heading = saw_heading or tail:find('Streaming',1,true) ~= nil
  saw_body = saw_body or tail:find('bounded reads',1,true) ~= nil
end, {
  format = "html",
  width = 72,
})

assert(ok == true)
assert(saw_heading and saw_body)
-- Literal fragments share normal wrapping without interpreting Markdown.
local literal = libmdf.new({ansi_mode='off'})
assert(literal:set_sink(function(chunk)io.write(chunk)end))
assert(literal:set_word_buffer(4096,false)) -- larger words may use bounded backing
assert(literal:write_literal('**literal punctuation** foo_bar\n'))
assert(literal:finish());literal:close()
print("lua mdf render example ok")
