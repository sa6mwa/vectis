local mdf = require("libmdf")
local terminal = require("vectis.terminal")
assert(mdf.version == "0.19.0")
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
-- 0.19 keeps full URI bytes while terminal autowrap accounts for the cursor.
local url = 'https://example.com/abcdefghijklmnopqrstuvwxyz'
local url_cases = {
  {url .. ' after\n', url},
  {'[report](' .. url .. ') after\n', url},
  {'- [report](' .. url .. ') after\n', url},
  {'> - [report](' .. url .. ') after\n', url},
  {'**' .. url .. '** after\n', url},
  {'**https://example**.com/abcdefghijklmnopqrstuvwxyz after\n', url},
  {'//localhost/abcdefghijklmnopqrstuvwxyz a b c d e f\n', '//localhost/abcdefghijklmnopqrstuvwxyz'},
}
for _, case in ipairs(url_cases) do
  for _, left in ipairs({0, 2}) do
    for _, mode in ipairs({'on', 'off'}) do
      local options = {width=20, margin_left=left, margin_right=0, osc8=false, ansi_mode=mode}
      local rendered = mdf.render(case[1], options)
      assert(rendered:find(case[2], 1, true), 'URI must contain no newline, margin or style reset')
      assert(terminal.markdown(case[1], options) == rendered)
      for _, fragment in ipairs({1, 3, 64}) do
        for _, render in ipairs({mdf.render_stream, terminal.markdown_stream}) do
          local traced = {}
          options.write_trace = function(_, chunk) traced[#traced+1]=chunk end
          assert(streamed(render, case[1], options, fragment) == rendered)
          assert(table.concat(traced) == rendered, 'trace and sink decisions agree')
        end
      end
    end
  end
end
local hard = mdf.render(url..'\n', {width=20,margin_right=2,ansi_mode='off'})
assert(not hard:find(url,1,true), 'positive margin keeps hard URL wraps')
-- Exact right edge: the next word must use the delayed-wrap cursor state.
local edge = '//abcdefghijklmnopqr' -- exactly 20 columns
assert(mdf.render(edge..' a b\n',{width=20,ansi_mode='off'}) == edge..'\na b\n')

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
-- Upstream 0.17 ownership fixes remain visible through the existing surface.
local marker = {}
local function same_error(fn)
  local success, failure = pcall(fn)
  assert(not success and failure == marker)
end
same_error(function() mdf.render_stream(function() error(marker) end,function() end) end)
same_error(function()
  local input='# callback\n'
  mdf.render_stream(function() local text=input;input=nil;return text end,
    function() error(marker) end)
end)
same_error(function() mdf.render('trace\n',{write_trace=function() error(marker) end}) end)
assert(not pcall(mdf.new,{theme=12}))
assert(not pcall(mdf.new,{format='html\0ansi'}))
local invalid, problem=terminal.markdown('bad',{theme=12})
assert(not invalid and problem.status==require('vectis.status').ERR_INVALID)
local font={format='html',html_font={family='Retained',regular_format='woff2',regular_data='a\0b'}}
local expected_font=mdf.render('font',font)
local handle=mdf.new(font)
font.html_font.regular_data=nil;font.html_font.family='mutated';font.html_font=nil
collectgarbage('collect')
assert(handle:render('font')==expected_font and expected_font:find('YQBi',1,true))
handle:close();handle:close()
-- Replacing a sink commits the new binding even if old-sink cleanup fails.
for _,document in ipairs({false,true}) do
  local armed, output=false,{}
  local function previous() if armed then error(marker) end end
  local h=document and mdf.document_stream({ansi_mode='on'},previous)
    or mdf.new({ansi_mode='on'})
  if not document then h:set_sink(previous) end
  armed=true
  same_error(function() h:set_sink(function(chunk)output[#output+1]=chunk end) end)
  if document then h:write('healthy\n') else h:feed('healthy\n') end
  h:finish_document();assert(table.concat(output):find('healthy',1,true));h:close()
end
local weak=setmetatable({}, {__mode='v'})
for i=1,8 do
  local retained={};weak[i]=retained
  assert(not pcall(mdf.new,{font_uri={},write_trace=function()return retained~=nil end}))
end
collectgarbage('collect');collectgarbage('collect');assert(next(weak)==nil)
-- Getter-created bytes remain rooted while the level getter collects.
local tokens=mdf.new({width=1200});local token_bytes=0
assert(tokens:set_sink(function(text)token_bytes=token_bytes+#text end))
assert(tokens:write_token(setmetatable({}, {__index=function(_,field)
  if field=='type' then return 'text' end
  if field=='text' then return string.rep('x',1000) end
  if field=='level' then collectgarbage('collect');return 0 end
end})))
assert(tokens:finish() and token_bytes>=1000);tokens:close()
-- Materialized result adoption cannot let GC close the active native owner.
do
  local owner=mdf.new({format='html'})
  local active, attempted, closed, failure=false,false,nil,nil
  collectgarbage('collect')
  local mode=collectgarbage('incremental')
  local pause=collectgarbage('param','pause',0)
  local stepmul=collectgarbage('param','stepmul',100000)
  local stepsize=collectgarbage('param','stepsize',0)
  collectgarbage('stop')
  setmetatable({}, {__gc=function()
    attempted=active
    closed,failure=pcall(owner.close,owner)
  end})
  active=true;collectgarbage('restart')
  assert(owner:render('healthy'):find('healthy',1,true))
  active=false
  assert(attempted and not closed and failure:find('active callback',1,true))
  collectgarbage('param','pause',pause)
  collectgarbage('param','stepmul',stepmul)
  collectgarbage('param','stepsize',stepsize)
  collectgarbage(mode);owner:close()
end
assert(terminal.markdown('healthy\n'))
print("upgrade seam behavior ok: libmdf " .. mdf.version)
