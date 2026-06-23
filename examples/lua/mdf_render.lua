local libmdf = require("libmdf")

local html = libmdf.render("# Vectis\n\n**libmdf** Lua example\n", {
  format = "html",
  width = 72,
})

assert(html:match("Vectis"))
assert(html:match("libmdf"))
io.write(html)

local chunks = {
  "\n## Streaming\n\n",
  "- bounded reads\n",
  "- direct sink writes\n",
}
local index = 1
local streamed = {}

local ok = libmdf.render_stream(function()
  local chunk = chunks[index]
  index = index + 1
  return chunk
end, function(chunk)
  streamed[#streamed + 1] = chunk
end, {
  format = "html",
  width = 72,
})

assert(ok == true)
local rendered = table.concat(streamed)
assert(rendered:match("Streaming"))
assert(rendered:match("bounded reads"))
io.write(rendered)
