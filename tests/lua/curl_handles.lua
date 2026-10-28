local curl=require('curl')
local origin=assert(os.getenv('VECTIS_CURL_ORIGIN'))
local secure=assert(os.getenv('VECTIS_CURL_TLS_ORIGIN'))
local ca=assert(os.getenv('VECTIS_CURL_CA'))
local root=assert(os.getenv('VECTIS_UPGRADE_TEST_ROOT'))
local session=assert(curl.session())
local function request(path,opts)
 opts=opts or {};opts.url=origin..path;opts.protocols='http';opts.proxy='';opts.timeout_ms=1500
 return assert(session:perform(opts))
end
local first=request('/identity');local second=request('/identity')
assert(first.ok and second.ok and first.body==second.body) -- actual TCP reuse
assert(session:reset())
assert(request('/identity').body==first.body) -- reset clears options, keeps connections
local pieces={}
local result=request('/echo',{method='POST',body='a\0b',headers={['X-Test']='yes'},write=function(bytes) pieces[#pieces+1]=bytes;return true end})
assert(result.ok and result.body==nil and result.streamed and table.concat(pieces)=='a\0b')
assert(result.headers:find('X%-Seen%-Header: yes') and result.headers:find('X%-Seen%-Method: POST'))
result=request('/echo',{body='credentials',username='user',password='pass',user_agent='VectisTest',accept_encoding='identity'})
assert(result.ok and result.headers:find('X%-Seen%-Authorization: Basic dXNlcjpwYXNz') and result.headers:find('X%-Seen%-User%-Agent: VectisTest') and result.headers:find('X%-Seen%-Encoding: identity'))
result=request('/redirect',{method='POST',body='replayable',follow_redirects=true});assert(result.ok and result.body=='replayable')
local remaining=1048583
result=request('/redirect',{method='POST',follow_redirects=true,read=function(capacity)if remaining==0 then return nil end;local n=math.min(capacity,remaining);remaining=remaining-n;return string.rep('x',n) end})
assert(not result.ok and result.code~=0)
local uploaded=0;local source='stream\0'..string.rep('x',131072)
result=request('/echo',{method='POST',read=function(capacity)
 assert(capacity>0 and capacity<=65536)
 if uploaded==#source then return nil end
 local bytes=source:sub(uploaded+1,uploaded+math.min(capacity,4096));uploaded=uploaded+#bytes;return bytes
end})
assert(result.ok and uploaded==#source and result.body==source)
result=request('/large');assert(not result.ok and result.failure and result.failure.message:find('limit'))
local received=0
result=request('/large',{write=function(bytes)assert(#bytes<=65536);received=received+#bytes;return true end})
assert(result.ok and result.body==nil and received==4096*2050)
result=request('/headers');assert(not result.ok and result.failure)
result=request('/partial');assert(not result.ok and result.code~=0)
local progress=0
result=request('/large',{progress=function(value)assert(type(value.downloaded)=='number' and value.upload_total>=0);progress=progress+1;return false end})
assert(not result.ok and progress>0)
for _,callback in ipairs({'read','write','progress'}) do
 for _,kind in ipairs({'exception','return','reenter','close'}) do
  local marker={callback=callback,kind=kind};local opts={method=callback=='read' and 'POST' or 'GET'}
  opts[callback]=function()
   if kind=='exception' then error(marker) end
   if kind=='return' then return nil,marker end
   if kind=='reenter' then assert(not pcall(session.reset,session)) end
   if kind=='close' then assert(session:close()==nil) end
   return callback=='read' and 'x' or true
  end
  result=request(callback=='read' and '/echo' or '/large',opts)
  assert(not result.ok)
  if kind=='exception' or (kind=='return' and callback~='progress') then assert(result.callback_error==marker and result.error==marker) end
 end
end
result=request('/echo',{method='POST',read=function()return '' end});assert(not result.ok)
result=request('/echo',{method='POST',read=function(capacity)return string.rep('x',capacity+1) end});assert(not result.ok)
local f=assert(io.open(root..'/upload.bin','wb'));assert(f:write('file\0bytes'));assert(f:close())
result=request('/echo',{method='POST',upload_path=root..'/upload.bin',download_path=root..'/download.bin'})
assert(result.ok and result.body==nil)
f=assert(io.open(root..'/download.bin','rb'));assert(f:read('a')=='file\0bytes');f:close()
assert(session:perform({url=secure..'/identity',protocols='https',proxy='',ca_file=ca,timeout_ms=1500}).ok)
assert(not session:perform({url=secure..'/identity',protocols='https',proxy='',timeout_ms=1500}).ok) -- TLS options reset
assert(session:perform({url=secure..'/identity',protocols='https',proxy='',verify_peer=false,verify_host=false,timeout_ms=1500}).ok)
local invalid,err=session:perform({url=origin,bogus=true});assert(invalid==nil and tostring(err):find('unsupported'))
for _,opts in ipairs({{url=origin,headers={['bad name']='v'}},{url=origin,headers={['x']='a\nb'}},{url=origin,method='NIL'},{url=origin,body='x',read=function()end},{url=origin,write=function()end,download_path=root..'/x'},{url=origin,upload_path=root..'/missing'}}) do assert(session:perform(opts)==nil) end
assert(request('/echo',{method='PUT',body='clean'}).body=='clean') -- reusable after failures
assert(session:close() and session:close());assert(not pcall(session.perform,session,{url=origin}))
local multi=assert(curl.multi())
local ids={}
local copied={url=origin..'/echo',method='POST',body='copied',headers={['X-Test']='original'},proxy='',timeout_ms=1500}
local copied_id=assert(multi:add(copied));copied.body='changed';copied.headers['X-Test']='changed'
for i=1,2 do ids[assert(multi:add({url=origin..'/pair?group=first&item='..i,protocols='http',proxy='',timeout_ms=1500}))]=tostring(i) end
local missing=assert(multi:add({url='file:///vectis-deliberately-missing',protocols='file'}))
local count=0
repeat
 local running=assert(multi:step())
 for _,completion in ipairs(assert(multi:completions())) do
  count=count+1
  if completion.id==copied_id then assert(completion.ok and completion.body=='copied' and completion.headers:find('X%-Seen%-Header: original'))
  elseif completion.id==missing then assert(not completion.ok)
  else assert(completion.ok and completion.body==ids[completion.id]) end
 end
 if running==0 then break end
 assert(multi:poll(100))
until false
assert(count==4 and #multi:completions()==0)
for _,kind in ipairs({'exception','reenter','close'}) do
 local marker={kind=kind}
 local id=assert(multi:add({url=origin..'/large',proxy='',timeout_ms=1500,write=function()
  if kind=='exception' then error(marker) end
  if kind=='reenter' then assert(not pcall(multi.step,multi)) end
  if kind=='close' then assert(multi:close()==nil) end
  return true
 end}))
 local completions={}
 repeat local running=assert(multi:step());for _,c in ipairs(multi:completions()) do completions[#completions+1]=c end;if running==0 then break end;assert(multi:poll(100)) until false
 assert(#completions==1 and completions[1].id==id and not completions[1].ok)
 if kind=='exception' then assert(completions[1].callback_error==marker) end
end
local failed=assert(multi:add({url=origin..'/large',proxy='',timeout_ms=1500,write=function() assert(not pcall(multi.step,multi));return true end}))
local clean=assert(multi:add({url=origin..'/echo',method='POST',body='unaffected',proxy='',timeout_ms=1500,write=function(bytes) assert(bytes=='unaffected');return true end}))
local mixed={}
repeat local running=assert(multi:step());for _,c in ipairs(multi:completions()) do mixed[c.id]=c end;if running==0 then break end;assert(multi:poll(100)) until false
assert(not mixed[failed].ok and mixed[clean].ok)
for i=1,2 do assert(multi:add({url='file://'..root..'/upload.bin',protocols='file'})) end
while assert(multi:step())~=0 do assert(multi:poll(0)) end
assert(not pcall(multi.completions,multi,0) and not pcall(multi.completions,multi,65))
local one=assert(multi:completions(1));local two=assert(multi:completions(1))
assert(#one==1 and #two==1 and one[1].id~=two[1].id and one[1].ok and two[1].ok and #multi:completions(1)==0)
local late=0
for i=1,64 do assert(multi:add({url=origin..'/identity',proxy='',write=function()late=late+1;return true end})) end
assert(not pcall(multi.add,multi,{url=origin}))
assert(multi:close() and multi:close() and late==0) -- no callbacks from close
assert(not pcall(multi.step,multi))
local http=require('vectis.http')
local defaults={proxy='',timeout_ms=1500,headers={['X-Test']='captured'}}
local client=assert(http.session(defaults));defaults.headers['X-Test']='mutated'
first=assert(client.get(origin..'/identity'));second=assert(client.get(origin..'/identity'))
assert(first.ok and first.transport_ok and first.body==second.body)
result=assert(client.post(origin..'/echo',{body='owned'}))
assert(result.ok and result.body=='owned' and result.headers:find('X%-Seen%-Header: captured') and result.headers:find('X%-Seen%-Method: POST'))
result=assert(client.get(origin..'/status'));assert(not result.ok and result.transport_ok and result.error.kind=='http_status')
local marker={failure='owned client'}
result=assert(client.get(origin..'/large',{write=function()error(marker)end}))
assert(not result.ok and not result.transport_ok and result.callback_error==marker)
assert(client.reset() and client.close() and client.close())
assert(not pcall(client.get,origin..'/identity'))
collectgarbage('collect')
print('upgrade seam behavior ok')
