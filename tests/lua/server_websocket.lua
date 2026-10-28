local v,json=require('vectis'),require('lonejson')
local enabled=os.getenv('VECTIS_WS_LOGGING')~='false'
local path='backend.jsonl'
assert(require('vectis.log').configure({env=false,output=path,subs={vectis=enabled}}))
local app=assert(v.app.new({bind='127.0.0.1',
  port=assert(tonumber(os.getenv('VECTIS_SOURCE_PORT'))),worker_count=1,
  worker_set_affinity_disabled=true,tls={mode='disabled'}}))
assert(app:websocket({path='/ws',
  connect=function() assert(not pcall(app.close,app));error('fixture/connect') end,
  message=function(ws,opcode,payload)
    assert(not pcall(app.close,app));assert(opcode==v.websocket.TEXT)
    assert(ws:send_text('lua:'..payload));error('fixture/message')
  end,
  disconnect=function() assert(not pcall(app.close,app));error('fixture/disconnect') end,
}))
collectgarbage('collect')
assert(app:start());print('ready');io.stdout:flush()
assert(app:wait());app:close()
local log=assert(io.open(path,'r'))
local found,count={},0
for line in log:lines() do
  local record=json.decode_value(line)
  if record.msg=='vectis.websocket.callback_failed' then
    assert(enabled and record.lvl=='error' and record.sub=='vectis')
    assert(record.callback=='connect' or record.callback=='message' or record.callback=='disconnect')
    assert(not found[record.callback] and record.error:find('fixture/'..record.callback,1,true))
    found[record.callback]=true;count=count+1
  end
end
log:close();assert(count==(enabled and 3 or 0))
print('websocket behavior ok');io.stdout:flush()
