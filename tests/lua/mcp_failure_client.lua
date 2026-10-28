local http,json=require('vectis.http'),require('lonejson')
local base,protocol=assert(arg[1]),assert(arg[2])
local checks=0
local function request(value,extra)
  local headers={Accept='application/json, text/event-stream',['Content-Type']='application/json',
    ['MCP-Protocol-Version']=protocol}
  for name,value in pairs(extra or {}) do headers[name]=value end
  local response=http.post({url=base..'/mcp',headers=headers,
    body=type(value)=='string' and value or json.encode_value(value),
    retry=false,proxy='',timeout_ms=3000,connect_timeout_ms=1000})
  assert(response.transport_ok,response.error_message)
  assert(response.status~=503)
  local body=response.body
  local event=body:match('data: ([^\n]+)')
  if event then body=event end
  local ok,decoded=pcall(json.decode_value,body)
  checks=checks+1
  return response.status,ok and decoded or body
end
local function call(name,arguments)
  return request({jsonrpc='2.0',id=1,method='tools/call',
    params={name=name,arguments=arguments or json.json_object({})}})
end
local function good(name)
  local status,value=call(name or 'good')
  assert(status==200 and value.result.structuredContent.ok==true)
end
local status,value=request({jsonrpc='2.0',id=1,method='initialize',
  params={protocolVersion=protocol,capabilities=json.json_object({}),clientInfo={name='failure-tests',version='1'}}})
assert(status==200 and value.result.protocolVersion==protocol)
good()
for _,name in ipairs({'throws','nil_result','nil_message','table_error','wrong_type',
  'invalid_json','oversized','missing_tool'}) do
  status,value=call(name)
  if status==500 and (name=='invalid_json' or name=='oversized') then
    assert(type(value)=='string' and value:find('MCP',1,true))
  else
    assert(status==200 and (value.error or value.result and value.result.isError))
  end
  if name=='throws' or name=='nil_message' then
    assert(json.encode_value(value):find('expected callback',1,true))
  end
  good()
end
status,value=call('good',{unexpected=true})
assert(status==200 and (value.error or value.result and value.result.isError));good()
for _,payload in ipairs({'{invalid','{"jsonrpc":"2.0","id":1,"method":"missing"}',string.rep('x',4096)}) do
  status,value=request(payload)
  assert(status>=400 or value.error);good()
end
status,value=request('{"jsonrpc":"2.0","id":1,"method":"tools/list"}',{Origin='https://untrusted.example'})
assert(status==400 or status==403);good('collecting');good()
print(checks..' MCP failure/recovery checks passed')
