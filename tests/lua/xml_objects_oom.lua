local xml = require('vectis.xml.objects.core')
local doc = assert(xml.document('<r xmlns:q="urn:q"><q:x a="1">hello</q:x><q:x a="2"/></r>'))
local root = assert(doc:root())
assert(root:name() == 'r')
assert(root:text() == 'hello')
local nodes = assert(doc:xpath('//q:x',{q='urn:q'}))
assert(#nodes==2 and nodes[1]:attribute('a')=='1')
assert(doc:xpath('count(//q:x)',{q='urn:q'})==2)
assert(doc:xpath('string(//q:x)',{q='urn:q'})=='hello')
assert(doc:xpath('boolean(//q:x)',{q='urn:q'}))
local child = assert(root:append_child('item',{namespace='urn:i',prefix='i',text='<&'}))
assert(child:set_attribute('value','<&',{namespace='urn:v',prefix='v'}))
assert(doc:root():set_text('literal &text;'))
assert(doc:serialize_materialized(true):find('&amp;text;',1,true))
for _,node in ipairs(nodes) do assert(node:close()) end
assert(root:close() and child:close() and doc:close())
local starts, ends, text = {},{},{}
local sax = assert(xml.sax({
  start_element=function(name,attrs,ns) starts[#starts+1]={name,attrs,ns} end,
  end_element=function(name,ns) ends[#ends+1]={name,ns} end,
  text=function(bytes) text[#text+1]=bytes end,
}))
assert(sax:feed('<r xmlns="urn:r" xmlns:q="urn:q" q:a="&amp;"><q:x>'))
assert(sax:feed('hello &lt;world&gt;</q:x></r>'))
assert(sax:finish() and sax:close())
assert(starts[1][2][1].value=='&' and starts[1][3].declarations[2].prefix=='q')
assert(table.concat(text)=='hello <world>')
return true
