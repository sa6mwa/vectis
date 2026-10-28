local curl = require('vectis.curl.handles.core')
local url = 'file://' .. CURL_OOM_SOURCE
local session=assert(curl.session())
local output={}
local result=assert(session:perform({url=url, protocols='file',headers={['x-test']='value'},write=function(bytes) output[#output+1]=bytes;return true end,progress=function()return true end}))
assert(result.ok and result.body==nil and #table.concat(output)>0)
assert(session:reset())
result=assert(session:perform({url=url,protocols='file'}));assert(result.ok and #result.body>0)
assert(session:close())
local multi=assert(curl.multi())
local first=assert(multi:add({url=url,protocols='file'}))
local second=assert(multi:add({url=url,protocols='file',write=function(bytes) assert(#bytes>0);return true end}))
assert(first~=second)
while assert(multi:step())~=0 do assert(multi:poll(0)) end
local results=assert(multi:completions())
assert(#results==2 and results[1].ok and results[2].ok)
assert(#multi:completions()==0 and multi:close())
return true
