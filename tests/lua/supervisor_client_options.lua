local v,lc,pslog,ssl = require('vectis'),require('lockdc'),require('pslog'),require('openssl')
local root=assert(os.getenv('VECTIS_WORKFLOW_TEST_ROOT'))
local file=assert(io.open(root..'/diagnostics.log','w+b'))
assert(v.log.configure({output=file,mode='json',env=false,disable_timestamp=true,level='trace'}))
local function must(value,err) assert(value~=nil and value~=false,type(err)=='table' and err.message or tostring(err)); return value end
local function entry(id) return {operation_id=id,effect_id='one',effect_key=id,kind='job',destination='local',payload_digest=ssl.sha256_materialized(id)},id end
for _, unsupported in ipairs({{disable_mtls=true},{unknown=true}}) do
  local ok,err=pcall(v.app.new,{lockd=unsupported,tls={mode='disabled'}})
  assert(not ok and err:find('unsupported app lockd option',1,true))
end
assert(not pcall(v.app.new,{lockd={pouch={unknown=true}},tls={mode='disabled'}}))
assert(not pcall(v.supervisor.client_config,{pouch=false}))
assert(not pcall(v.supervisor.client_config,{client_bundle={path='ignored'}}))
for _,invalid in ipairs({{endpoints={other='pouch://ignored'}},{endpoints={'pouch://ok',[3]='pouch://gap'}},
  {endpoints={12}},{endpoints={'pouch://bad\0suffix'}},{unix_socket_path='bad\0suffix'},
  {namespace='bad\0suffix'},{pouch={crypto_key='bad\0secret'}},{disable_mtls='false'},
  {client_bundle_source={path='bad\0suffix'}},
  {unknown=true},{logger_disabled=0}}) do
  assert(not pcall(v.supervisor.client_config,invalid))
end

for _, invalid in ipairs({{endpoints={'pouch://bad\0suffix'}},{endpoints={12}},
  {endpoints={other='ignored'}},{pouch_crypto_key='key\0suffix'},
  {['pouch_crypto_key\0suffix']='ignored'},{logger_disabled='yes'},
  {endpoints={'pouch://unused'},timeout_ms='invalid'}}) do
  assert(not pcall(v.app.new,{lockd=invalid,tls={mode='disabled'}}))
end
local aliases=v.supervisor.client_config({client_bundle_path='canonical.pem',bundle_path='alias.pem',client_bundle='embedded',bundle='alias.pem'})
assert(aliases.client_bundle_source.path=='canonical.pem' and aliases.client_bundle=='embedded' and aliases.bundle==nil and aliases.bundle_path==nil)
for _, variant in ipairs({'default','query','flat'}) do
  local appopts={app_name='client-'..variant,tls={mode='disabled'}}
  local key=must(lc.pouch_crypto_generate_key())
  if variant=='query' then
    appopts.lockd={endpoints={(must(v.lockd.pouch_endpoint_build(root..'/query ?#%',{
      {name='single_writer',value='false'},{name='crypto_key',value=key}})):gsub('crypto_key=','%%63rypto_key='))}}
  elseif variant=='flat' then
    appopts.lockd={endpoints={'pouch://'..root..'/flat'},pouch_crypto_key=key}
  end
  local app=must(v.app.new(appopts))
  local scope=app:supervisor({name='consumer'})
  local count=0
  scope:consumer({request={namespace='consumer-'..variant,queue='q',owner='consumer',visibility_timeout_seconds=30},
    handler=function(message) count=count+1; return message:ack() end})
  local workflow=app:workflow({namespace='workflow-'..variant,recovery_probe_ms=1000000})
  local handled=0
  workflow:outbox({kind='job',handler=function(job) handled=handled+1; return job:complete() end})
  must(app:start())
  local config=v.supervisor.client_options(app, appopts.lockd or {})
  local producer=must(app:_open_lockdc_client(config))
  must(producer:enqueue({namespace='consumer-'..variant,queue='q'},'payload'))
  must(app:pump(0)); assert(count==1)
  -- Empty batch never invokes the handler.
  for i=1,4 do must(app:pump(0)) end; assert(count==1)
  local e,p=entry(variant); must(workflow:transaction(function(tx) return tx:append_outbox(e,p) end))
  must(app:pump(0)); assert(handled==1)
  producer:close(); must(app:stop()); app:close()
  -- A fresh native app reopens the same encrypted root through canonical query
  -- parsing, including a reserved root name and an encoded option name.
  local reopened=must(v.app.new(appopts)); local scope2=reopened:supervisor({name='reopen'})
  scope2:consumer({request={namespace='consumer-'..variant,queue='q',owner='consumer'},handler=function() error('empty queue invoked handler') end})
  must(reopened:start()); must(reopened:pump(0)); must(reopened:stop()); reopened:close()
end
-- An explicit exclusive setting cannot serve several app-owned clients.
local exclusive=must(v.app.new({app_name='exclusive',tls={mode='disabled'}}))
assert(not pcall(v.supervisor.client_options,exclusive,{endpoints={'pouch://'..root..'/exclusive'},pouch={single_writer=true,crypto_key=must(lc.pouch_crypto_generate_key())}}))
exclusive:close()
-- Native endpoint query precedence is preserved. Explicit exclusive mode
-- cannot coexist with app-owned children; failure releases every registration.
for _, override in ipairs({'unset','false'}) do
  local key=must(lc.pouch_crypto_generate_key())
  local endpoint=must(v.lockd.pouch_endpoint_build(root..'/exclusive-query-'..override,
    {{name='single_writer',value='true'},{name='crypto_key',value=key}}))
  local exclusive_app=must(v.app.new({tls={mode='disabled'},lockd={endpoints={endpoint}}}))
  local options=v.supervisor.client_options(exclusive_app,{endpoints={endpoint},pouch=override=='false' and {single_writer=false} or {}})
  if override=='false' then assert(options.pouch.single_writer==false) else assert(options.pouch.single_writer==nil) end
  exclusive_app:supervisor({name='exclusive-query'}):consumer({client={endpoints={endpoint},pouch=override=='false' and {single_writer=false} or {}},request={namespace='q',queue='q',owner='q'},handler=function() error('exclusive handler') end})
  local started,start_error=exclusive_app:start()
  assert(started==nil and start_error)
  exclusive_app:close()
end
-- Exact errors survive a protected scoped call, and no TLS configuration leaks
-- into a subsequent direct dependency open.
local app=must(v.app.new({app_name='scope-errors',tls={mode='disabled'}}))
local scope=app:supervisor({name='empty'})
assert(not pcall(scope.consumer,scope,{request={queue='q',with_state=true},handler=function() end}))
must(app:start())
assert(not pcall(app._open_lockdc_client,app,{endpoints={'pouch://bad\0suffix'}}))
assert(not app:_open_lockdc_client({endpoints={'invalid://bad'}}))
-- Endpoint originals must survive callback mutation and a full collection. The
-- Lua validation clone deliberately contains no query; only the pinned native
-- scope carries the key and the authoritative escaped root.
local mutation_key=must(lc.pouch_crypto_generate_key())
local function mutation_endpoint()
  return must(v.lockd.pouch_endpoint_build(root..'/mutation ?#%',{
    {name='single_writer',value='false'},{name='crypto_key',value=mutation_key}}))
end
local original={endpoints={mutation_endpoint()}}
local real_open=lc.open
local close_attempts=0
lc.open=function(validation)
  original.endpoints[1]=nil
  assert(not pcall(app.close,app)); assert(not pcall(app.stop,app))
  assert(not pcall(scope.stop,scope)); close_attempts=close_attempts+1
  collectgarbage('collect')
  for i=1,1000 do local junk=string.rep(tostring(i),256) end
  collectgarbage('collect')
  return real_open(validation)
end
local opened=table.pack(pcall(app._open_lockdc_client,app,original))
lc.open=real_open
assert(opened[1] and opened.n==2 and close_attempts==1)
local mutation_client=must(opened[2])
must(mutation_client:enqueue({namespace='pin',queue='q'},'pinned-original'))
local inspector=must(lc.open({endpoints={must(v.lockd.pouch_endpoint_build(root..'/mutation ?#%'))},
  pouch={crypto_key=mutation_key,single_writer=false}}))
local batch=must(inspector:dequeue_batch({namespace='pin',queue='q',owner='inspector',wait_seconds=0,page_size=1}))
assert(#batch==1)
local bytes=''; local payload,count=batch[1]:write_payload({write=function(chunk) bytes=bytes..chunk; return true end})
assert(payload==nil and count==15) -- public push sink returns nil, byte count
assert(bytes=='pinned-original')
must(inspector:queue_ack(batch[1]:info())); batch[1]:close(); inspector:close(); mutation_client:close()
-- A failed protected open preserves the original error object and releases both
-- the app call guard and the endpoint TLS scope.
local identity={message='original loader error'}
lc.open=function() error(identity) end
local success,failure=pcall(app._open_lockdc_client,app,{endpoints={mutation_endpoint()}})
lc.open=real_open
assert(not success and failure==identity)
local returned=table.pack(app:_workflow_call(function() return nil,identity,false end))
assert(returned.n==3 and returned[1]==nil and returned[2]==identity and returned[3]==false)

local direct=must(lc.open({endpoints={'pouch://'..root..'/direct'},pouch={crypto_key=must(lc.pouch_crypto_generate_key())}})); direct:close()
must(app:stop()); app:close()
-- App-level mute reaches the native app and its private scoped clients.
local muted=must(v.app.new({app_name='muted-client',lockd={logger_disabled=true},tls={mode='disabled'}}))
local muted_scope=muted:supervisor({name='muted'})
local muted_count=0
muted_scope:consumer({request={namespace='muted',queue='q',owner='muted'},handler=function(message)
  muted_count=muted_count+1
  assert(not pcall(message.nack,message,{unknown=true})); return message:ack()
end})
must(muted:start())
local muted_producer=must(muted:_open_lockdc_client(v.supervisor.client_options(muted,{logger_disabled=true})))
must(muted_producer:enqueue({namespace='muted',queue='q'},'muted')); must(muted:pump(0))
assert(muted_count==1)
muted_producer:close(); must(muted:stop()); muted:close()
-- Unreachable app/source/workflow cycles must execute cleanup while the app's
-- borrowed logger is still alive. Weak-map restoration is exercised by GC.
local weak=setmetatable({}, {__mode='v'})
do
  local collected=must(v.app.new({app_name='gc-sources',tls={mode='disabled'}}))
  local s=collected:supervisor({name='gc-consumer'})
  s:consumer({request={namespace='gc',queue='q',owner='gc'},handler=function() error('empty') end})
  local w=collected:workflow({namespace='gc-workflow',recovery_probe_ms=1000000})
  w:outbox({kind='job',handler=function(job) return job:complete() end})
  must(collected:start()); must(collected:pump(0)); weak[1]=collected
end
collectgarbage('collect'); collectgarbage('collect'); assert(weak[1]==nil)
local diagnostic=must(v.app.new({app_name='diagnostic',tls={mode='disabled'}}))
local fault=diagnostic:supervisor({name='visible-failure',failure_policy='stop'})
fault:receiver({kind='fail',handler=function() error('owned failure evidence') end})
local table_fault=diagnostic:supervisor({name='table-failure',failure_policy='stop'})
table_fault:receiver({kind='fail',handler=function() error({message='table failure evidence'}) end})
local safe=diagnostic:supervisor({name='safe'}); local safe_count=0
safe:receiver({kind='ok',handler=function() safe_count=safe_count+1 end})
local sus=require('sus'); local original_pump=sus.pump_logs; local drains=0
sus.pump_logs=function(max) assert(max==2); local _,main=coroutine.running(); assert(main); drains=drains+1; return 0,{} end
local log_source=diagnostic:supervisor({name='sus-safe-point'})
local timer=log_source:sus_logs({every_ms=1,max=2})
assert(not pcall(log_source.sus_logs,log_source,{max=65}))
assert(not pcall(diagnostic.supervisor,diagnostic,{name='limits',limits={jobs=2}}))
must(diagnostic:start()); must(table_fault:channel():send('fail','')); must(fault:channel():send('fail','')); must(safe:channel():send('ok','')); must(diagnostic:pump(0))
assert(table_fault:last_error().message=='table failure evidence')
assert(fault:last_error().status==v.ERR_STATE and fault:last_error().message:find('owned failure evidence',1,true) and safe_count==1)
for i=1,10 do if drains>0 then break end; must(diagnostic:pump(10)) end
assert(drains>0); timer:disable(); local drained=drains; must(diagnostic:pump(0)); assert(drains==drained)
must(fault:start()); assert(fault:last_error()==nil)
must(diagnostic:stop()); diagnostic:close(); sus.pump_logs=original_pump
file:flush(); file:seek('set',0); local logs=file:read('a')
assert(logs:find('owned failure evidence',1,true) and logs:find('"scope":"visible-failure"',1,true))
assert(logs:find('"app":"client-query"',1,true) and logs:find('"sub":"lockdc"',1,true))
for line in logs:gmatch('[^\n]+') do
  assert(not (line:find('"app":"muted-client"',1,true) and line:find('"sub":"lockdc"',1,true)))
end
print('supervisor workflow behavior ok')
