local xml = require('vectis.xml')
assert(require('vectis').xml.document == xml.document)
local source = '<r xmlns="urn:r" xmlns:q="urn:q"><q:x a="">a&amp;b</q:x><q:x a="2"/></r>'
local doc = assert(xml.document(source))
local root = assert(doc:root())
local name,uri,prefix = root:name()
assert(name=='r' and uri=='urn:r' and prefix==nil)
assert(root:text()=='a&b' and root:attribute('missing')==nil)
local nodes = assert(doc:xpath('//q:x',{q='urn:q'}))
assert(#nodes==2 and nodes[1]:attribute('a')=='' and nodes[2]:attribute('a')=='2')
assert(doc:xpath('count(//q:x)',{q='urn:q'})==2)
assert(doc:xpath('boolean(//q:x)',{q='urn:q'})==true)
assert(doc:xpath('string(//q:x[2]/@a)',{q='urn:q'})=='2')
assert(doc:xpath('//q:x/@a',{q='urn:q'})==nil)
local value,err=doc:xpath('[');assert(value==nil and err.operation=='xpath' and err.dependency_code~=0)
assert(root:set_attribute('a','<&',{namespace='urn:q',prefix='q'}))
assert(root:name()==nil and nodes[1]:text()==nil)
root=assert(doc:root())
assert(root:attribute('a','urn:q')=='<&')
assert(root:set_attribute('a','x',{namespace='urn:other',prefix='q'})==nil)
assert(root:set_attribute('a','x',{namespace='urn:other',prefix='xml'})==nil)
assert(root:set_text('\0')==nil and root:set_text('\255')==nil)
local child=assert(root:append_child('item',{text='literal &text;'}))
assert(select(2,child:name())=='urn:r')
assert(root:name()==nil)
local output=assert(doc:serialize_materialized(true))
assert(output:find('literal &amp;text;',1,true) and output:find('q:a="&lt;&amp;"',1,true))
local reloaded=assert(xml.document(output));assert(reloaded:xpath('count(//r:item)',{r='urn:r'})==1);reloaded:close()
assert(child:remove() and child:text()==nil)
assert(doc:root():remove()==nil)
assert(doc:close() and doc:close())
assert(child:text()==nil and child:close())
assert(not pcall(doc.root,doc))
assert(xml.document('<r>')==nil)
assert(xml.document('<!DOCTYPE r [<!ENTITY a "x">]><r>&a;</r>')==nil)
assert(xml.document('<!DOCTYPE r SYSTEM "file:///unused"><r/>')==nil)
assert(xml.document(source,{max_bytes=1})==nil)
assert(not pcall(xml.document,source,{bogus=true}))
local limited=assert(xml.document(source,{max_xpath_nodes=1}));assert(limited:xpath('//*')==nil);limited:close()
local view=assert(xml.document('<r>retained</r>')):root();collectgarbage('collect');assert(view:text()=='retained');view:close()
local starts,ends,text={},{},{}
local callbacks={
 start_element=function(n,a,ns) starts[#starts+1]={name=n,attrs=a,ns=ns} end,
 end_element=function(n,ns) ends[#ends+1]={name=n,ns=ns} end,
 text=function(bytes) assert(#bytes<=65536);text[#text+1]=bytes end,
}
local sax=assert(xml.sax(callbacks)); callbacks.start_element=function() error('not copied') end
assert(sax:feed('<r xmlns="urn:r" xmlns:q="urn:q" a="plain" q:a="&amp;"><q:x>'))
assert(#starts==2 and #ends==0) -- real delivery before document completion
assert(starts[1].ns.uri=='urn:r' and #starts[1].ns.declarations==2)
assert(starts[1].attrs[1].value=='plain' and starts[1].attrs[2].value=='&' and starts[1].attrs[2].namespace=='urn:q')
assert(sax:feed('hello &#x1f30d; &lt;world&gt;</q:x></r>'))
assert(sax:finish() and sax:finish() and #ends==2)
assert(table.concat(text)=='hello 🌍 <world>')
assert(sax:feed('')==nil and sax:close() and sax:close())
assert(not pcall(sax.feed,sax,'<r/>'))
for _,kind in ipairs({'exception','return','reenter','close'}) do
 local marker={kind=kind};local parser;local calls=0
 parser=assert(xml.sax({start_element=function()
   calls=calls+1
   if kind=='exception' then error(marker) end
   if kind=='return' then return nil,marker end
   if kind=='reenter' then assert(parser:feed('')==nil) end
   if kind=='close' then assert(parser:close()==nil) end
 end}))
 local ok,failure=parser:feed('<r/>');assert(ok==nil and calls==1)
 if kind=='exception' or kind=='return' then assert(failure==marker) end
 assert(parser:finish()==nil and calls==1 and parser:close())
end
local parser=assert(xml.sax({start_element=function() return false end}));assert(parser:feed('<r/>') and parser:finish() and parser:close())
parser=assert(xml.sax({}));assert(parser:feed('<r>') and parser:finish()==nil and parser:close())
parser=assert(xml.sax({}));assert(parser:feed('<!DOCTYPE r SYSTEM "file:///unused"><r/>')==nil and parser:close())
parser=assert(xml.sax({},{max_metadata_bytes=2}));assert(parser:feed('<root/>')==nil and parser:close())
parser=assert(xml.sax({}));assert(parser:feed(string.rep('x',65537))==nil);assert(parser:feed('<r/>') and parser:finish() and parser:close())
local many={'<r'};for i=1,257 do many[#many+1]=' a'..i..'="x"' end;many[#many+1]='/>'
parser=assert(xml.sax({}));assert(parser:feed(table.concat(many))==nil and parser:close())
local delivered=0
parser=assert(xml.sax({text=function(bytes) delivered=delivered+#bytes end}))
assert(parser:feed('<r>'))
for i=1,257 do assert(parser:feed('<x>'..string.rep('a',65520)..'</x>')) end
assert(delivered>16777216) -- no cumulative materialization ceiling
assert(parser:feed('</r>') and parser:finish() and parser:close())
collectgarbage('collect')
print('upgrade seam behavior ok')
