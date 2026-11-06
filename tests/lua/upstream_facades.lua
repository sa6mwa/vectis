local pslog = require('pslog')
local lql = require('lql')
local mdf = require('libmdf')
local vectis = require('vectis')
local chunks = {}
local root = assert(pslog.new_json({output=function(chunk)
  chunks[#chunks+1]=chunk
end, disable_timestamp=true,no_color=true}))
local child = root:with('operation','upstream facade','attempt',1)
child:info('existing derivation')
assert(child:close() and root:close())
assert(table.concat(chunks):find('"operation":"upstream facade"',1,true))
-- A callback/output/owner cycle must be collectable in the embedded VM.
local weak=setmetatable({}, {__mode='v'})
do
  local owner={}
  owner.logger=assert(pslog.new_json({output=function(chunk)
    owner.last=chunk
  end,disable_timestamp=true,no_color=true}))
  weak[1]=owner;weak[2]=owner.logger
  owner.logger:info('callback owner cycle')
  assert(owner.last:find('callback owner cycle',1,true))
end
collectgarbage('collect');collectgarbage('collect')
assert(weak[1]==nil and weak[2]==nil)
assert(vectis.log.native == pslog)
local client <close> = assert(lql.new())
assert(client:capabilities().stream_apply_spooled == true)
assert(type(client:version()) == 'string')
assert(vectis.libs.lql == lql)
local markdown = '# Upstream\n\n**facade**\n'
local html = mdf.render(markdown,{format='html'})
assert(html:find('Upstream',1,true))
assert(vectis.terminal.markdown(markdown,{format='html'}) == html)
local source, streamed = markdown, {}
assert(vectis.terminal.markdown_stream(function()
  local chunk=source; source=nil; return chunk
end,function(chunk)streamed[#streamed+1]=chunk end,{format='html'}))
assert(table.concat(streamed) == html)
assert(vectis.terminal.libs.libmdf == mdf)
print('upgrade seam behavior ok')
