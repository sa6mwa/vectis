local v,lc=require('vectis'),require('lockdc')
local root=assert(os.getenv('VECTIS_UPGRADE_TEST_ROOT'))
local function must(value,err) assert(value~=nil and value~=false,debug.traceback(type(err)=='table' and err.message or tostring(err),2));return value end
local config={endpoints={'pouch://'..root..'/queue'},pouch={crypto_key=must(lc.pouch_crypto_generate_key()),single_writer=false}}
local inspector=must(lc.open(config))
local diagnostics={}
local client_methods=getmetatable(inspector).__index
local real_nack=client_methods.queue_nack
client_methods.queue_nack=function(self,req)
  if req.last_error_json then diagnostics[#diagnostics+1]=lc.decode_json(req.last_error_json).message end
  return real_nack(self,req)
end
local function request(queue) return {namespace='seams',queue=queue,owner='test',visibility_timeout_seconds=30,wait_seconds=0} end
local function enqueue(queue) return must(v.lockd.enqueue_json(config,request(queue),{hello='world'})) end
local empty,err=v.lockd.with_dequeued_json(config,request('empty'),function() error('empty called handler') end)
assert(empty==nil and err==nil)
local late
for _,action in ipairs({'ack','nack'}) do
  enqueue(action)
  local result=must(v.lockd.with_dequeued_json(config,request(action),function(payload,message,client,count)
    assert(payload.hello=='world' and type(count)=='number' and client)
    late=message
    if action=='ack' then must(message:ack()) else must(message:nack({intent='defer',delay_seconds=30})) end
    return action
  end))
  assert(result==action and not late:is_open())
  assert(not pcall(late.info,late) and not pcall(late.ack,late))
  local none,none_err=v.lockd.with_dequeued_json(config,request(action),function() error('terminal delivery returned') end)
  assert(none==nil and none_err==nil)
end
for _,variant in ipairs({'throw','nil-error','no-result','terminal-error'}) do
  enqueue(variant)
  local fault={message='helper '..variant}
  local ran,result,failure=pcall(v.lockd.with_dequeued_json,config,request(variant),function(_,message,client)
    late=message
    if variant=='throw' then error(fault,0) end
    if variant=='nil-error' then return nil,fault end
    if variant=='terminal-error' then must(client:queue_ack(message:info()));return message:ack() end
  end)
  assert(not late:is_open() and not pcall(late.read_payload,late))
  if variant=='throw' then assert(not ran and result==fault)
  elseif variant=='no-result' then assert(ran and result==true)
  else assert(ran and result==nil and type(failure)=='table') end
end
-- A deterministic consumer source observes each failure and preserves the
-- diagnostic into the public retry operation. Only selected tokens ack.
local app=must(v.app.new({app_name='queue-diagnostics',tls={mode='disabled'}}))
local scope=app:supervisor({name='queue'})
local sources={}
for _,variant in ipairs({'throw','nil-error','reject','missing','ack','nack','terminal-error'}) do
  enqueue('source-'..variant)
  sources[variant]=scope:consumer({name=variant,every_ms=1000000,client=config,request=request('source-'..variant),handler=function(message)
    late=message
    assert(not pcall(app.close,app)); assert(not pcall(app.stop,app));assert(not pcall(scope.native.stop,scope.native))
    if variant=='throw' then error({message='consumer structured boom'},0) end
    if variant=='nil-error' then return nil,'consumer returned error' end
    if variant=='reject' then return false,'consumer rejected' end
    if variant=='ack' then return message:ack() end
    if variant=='nack' then return message:nack({intent='defer',delay_seconds=30}) end
    if variant=='terminal-error' then must(inspector:queue_ack(message:info()));return message:ack() end
  end})
end
for _,bad in ipairs({{unknown=true},{name=5},{every_ms='bad'}}) do
  bad.request=request('unused');bad.handler=function() end
  assert(not pcall(scope.consumer,scope,bad))
end
assert(not pcall(scope.receiver,scope,{handler=function() end,unknown=true}))
assert(not pcall(scope.sus_logs,scope,{unknown=true}))
-- Metadata is copied exactly by the public sender. Restart deliberately keeps
-- queued frames/signals until app close or a full app runtime restart.
local events={}
local transport=app:supervisor({name='transport',channel={signal_kinds={'hint'}}})
transport:receiver({kind='frame',handler=function(event) events[#events+1]=event end})
transport:receiver({kind='hint',handler=function(event) events[#events+1]=event end})
for _,opts in ipairs({{correlation_id=-1},{correlation_id=4294967296},{flags=4294967296},{flags=1.5},{unknown=0}}) do
  assert(not pcall(transport.channel(transport).send,transport:channel(),'frame','copied',opts))
end
must(app:start())
-- Probe sources explicitly instead of letting a fault stop the whole scope.
for variant,source in pairs(sources) do
  local ok,failure=pcall(source.probe)
  if variant=='terminal-error' then assert(not ok and failure) else assert(ok) end
  assert(not pcall(late.info,late))
end
for _,variant in ipairs({'throw','nil-error','reject','missing'}) do
  local source=sources[variant]
  assert(source:stats().failures==1 and #source.last_error.message<=512)
end
assert(sources.throw.last_error.message=='consumer structured boom')
assert(sources['nil-error'].last_error.message=='consumer returned error')
assert(sources.reject.last_error.message=='consumer rejected')
-- The real public nack receives the diagnostic; a reopened delivery proves
-- durable failure accounting. Queue metadata is query-hidden upstream.
assert(table.concat(diagnostics,'|'):find('consumer structured boom',1,true))
must(scope:stop())
must(transport:channel():send('frame','a\0b',{correlation_id=4294967295,flags=7}))
must(transport:channel():signal('hint'))
must(transport:stop());must(transport:start());must(app:pump(0));must(app:pump(0))
assert(#events==2)
local frame=events[1].type=='message' and events[1] or events[2]
assert(frame.payload=='a\0b' and frame.correlation_id==4294967295 and frame.flags==7)
for _,bound in ipairs({-1,0,4294967297}) do
  local candidate=must(v.app.new({tls={mode='disabled'}}))
  assert(not pcall(candidate.supervisor,candidate,{name='bound',channel={capacity=bound}}));candidate:close()
end
assert(not pcall(transport.native.stop,transport.native,-1))
assert(not pcall(app.pump,app,-1))
assert(not pcall(transport.native.timer,transport.native,10,2147483648))
local candidate=must(v.app.new({tls={mode='disabled'}}))
assert(not pcall(candidate.supervisor,candidate,{name='frame-bound',channel={max_frame_size=4294967297}}));candidate:close()
must(app:stop());app:close();inspector:close()
client_methods.queue_nack=real_nack
local reopened=must(lc.open(config))
for _,variant in ipairs({'throw','nil-error','reject','missing'}) do
  local batch=must(reopened:dequeue_batch(request('source-'..variant)))
  assert(#batch==1 and batch[1]:info().failure_attempts==1)
  must(reopened:queue_ack(batch[1]));batch[1]:close()
end
reopened:close()
print('upgrade seam behavior ok')
