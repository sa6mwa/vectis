local v,lc=require('vectis'),require('lockdc')
local storage=require('vectis.storage')
local root=assert(os.getenv('VECTIS_UPGRADE_TEST_ROOT'))
local function describe(err)
  if type(err)=='table' then return tostring(err.message)..(err.cause and ': '..describe(err.cause) or '') end
  return tostring(err)
end
local function must(value,err) assert(value~=nil,describe(err));return value end
local function fails(fn,...) assert(not pcall(fn,...)) end
local config={endpoints={'pouch://'..root..'/pouch'},pouch={single_writer=false,crypto_key=assert(lc.pouch_crypto_generate_key())},logger_disabled=true}
local app=must(v.app.new({app_name='storage-test',tls={mode='disabled'}}))
local store=app:storage({client=config})
local retention={completed_retention_seconds=3600,completed_max_count=100,
  inbox_retention_seconds=3601,inbox_max_count=101,command_retention_seconds=3602,command_max_count=102}
local opened,open=0,lc.open
lc.open=function(...)
  local client,err,code=open(...)
  if client then
    local create=client.new_outbox
    client.new_outbox=function(self,options,...)
      if options.namespace=='xa-events' then
        for name,value in pairs(retention) do assert(options[name]==value,name..' was not propagated') end
        opened=opened+1
      end
      return create(self,options,...)
    end
  end
  return client,err,code
end
local workflow_options={namespace='xa-events',client=config,recovery_probe_ms=1000000}
for name,value in pairs(retention) do workflow_options[name]=value end
local workflow=app:workflow(workflow_options)
local delivered=0
workflow:outbox({kind='created',handler=function(job)
  delivered=delivered+1
  return job:complete()
end})
local pg=assert(v.pg)
assert(v.libs.lockdc_postgres==require('lockdc.postgres'))
assert(pg==require('vectis.pg') and pg.binary('a\0b',17).binary)
fails(pg.from_borrow,{})
fails(store.collection_capabilities,store)
fails(app.storage,app,{client=config})
must(app:start())
must(v.lockd.with_client(config,function(client)
  local lease=must(client:acquire({namespace='renewal',key='ordinary',owner='test',ttl_seconds=30}))
  for revision=1,2 do
    must(lease:update_json({revision=revision}))
    local version=lease:info().version
    must(lease:keepalive({ttl_seconds=30}))
    assert(lease:info().version==version,'renewal changed committed body version')
    assert(must(lease:read_json()).revision==revision)
  end
  must(lease:release());return true
end))
local req={namespace='state-retention',key='record',owner='test',ttl_seconds=30}
must(v.lockd.save_json(config,req,{value=1},{retention_seconds=3600}))
local function state()
  return must(v.lockd.with_client(config,function(client)
    local value,meta=client:read_json({namespace=req.namespace,key=req.key})
    assert(value and value.value==1)
    return meta
  end))
end
local expiry=state().retention_expires_at_unix
assert(expiry and tonumber(expiry)>0)
must(v.lockd.save_json(config,req,{value=1}))
assert(state().retention_expires_at_unix==expiry)
must(v.lockd.save_json(config,req,{value=1},{retention_seconds=0}))
assert(tonumber(state().retention_expires_at_unix)==0)
assert(not v.lockd.save_json(config,req,{value=1},{retention_seconds=-1}))
assert(store:collection_capabilities().collections)
local c=must(store:create_collection({namespace='test',name='journal'}))
local id1,id2=lc.xid_new(),lc.xid_new()
local entry=must(c:append({append_id=id1},'{"value":1}'))
local second=must(c:append({append_id=id2},'{"value":2}'))
assert(entry.position=='1' and second.position=='2')
assert(c:append_status(id1).entry.position=='1')
assert(c:append({append_id=id1},'{"value":1}').position=='1')
local payload,metadata={},{}
local ok,result=c:read({on_metadata=function(value)
  metadata[#metadata+1]=value
  fails(store.close,store);fails(app.close,app)
end},{write=function(chunk) payload[#payload+1]=chunk;return true end})
assert(ok==nil and result.complete and result.completed_records=='2')
assert(table.concat(payload)=='{"value":1}\n{"value":2}\n' and #metadata>=2)
local function read(options)
  local chunks={}
  local failure,progress,partial=c:read(options,{write=function(chunk)
    assert(#chunk<=65536);chunks[#chunks+1]=chunk;return true
  end})
  return table.concat(chunks),progress,partial,failure
end
local bytes,window=read({after='1',through='2',first_n='1'})
assert(bytes=='{"value":2}\n' and window.complete and window.completed_records=='1')
assert(type(window.checkpoint_cursor)=='string')
local empty,zero=read({first_n='0'})
assert(empty=='' and zero.complete and zero.completed_records=='0')
local cancelled,cancel=read({cancelled=function() return true end})
assert(cancelled=='' and not cancel.complete and cancel.stop_reason=='cancelled')
assert(type(cancel.resume_cursor)=='string')
local resumed,finished=read({cursor=cancel.resume_cursor})
assert(resumed=='{"value":1}\n{"value":2}\n' and finished.complete)
local failure,err,code,partial=c:read({}, {write=function() error('deterministic sink fault') end})
assert(not failure and err and code~=0 and partial and partial.stop_reason=='error')
local healthy,progress=read({})
assert(healthy=='{"value":1}\n{"value":2}\n' and progress.complete)
-- Exercise real producer/sink chunk boundaries without collecting the body.
local large=must(store:create_collection({namespace='streaming',name='large'}))
local digest=require('vectis.digest')
local expected,observed=must(digest.new('sha256')),must(digest.new('sha256'))
local remaining,phase=262144,0
local large_entry=must(large:append({append_id=lc.xid_new()},{read=function(max)
  local chunk
  if phase==0 then chunk='{"data":"';phase=1
  elseif remaining>0 then local n=math.min(remaining,max,4096);remaining=remaining-n;chunk=string.rep('x',n)
  elseif phase==1 then chunk='"}';phase=2
  else return nil end
  must(expected:update(chunk));return chunk
end}))
assert(large_entry.position=='1' and remaining==0)
must(expected:update('\n'))
local chunks,transferred=0,0
local _,streamed=large:read({}, {write=function(chunk)
  assert(#chunk<=65536);chunks=chunks+1;transferred=transferred+#chunk
  must(observed:update(chunk));return true
end})
assert(streamed.complete and streamed.completed_records=='1' and chunks>1 and transferred==262156)
assert(expected:final_hex()==observed:final_hex())
expected:close();observed:close()
local fail_id=lc.xid_new()
local failed,source_error=large:append({append_id=fail_id},{read=function() error('deterministic source fault') end})
assert(not failed and source_error and large:describe().live_count=='1')
assert(large:append_status(fail_id).state=='absent')
must(large:append({append_id=fail_id},'{"recovered":true}'))
assert(large:describe().live_count=='2')
local changed=must(c:metadata({retention_seconds=3600}))
assert(tonumber(changed.retention_expires_at_unix)>0)
changed=must(c:metadata({retention_seconds=0}))
assert(tonumber(changed.retention_expires_at_unix)==0)
local entry_meta=must(c:entry_metadata('2',{retention_seconds=3600}))
assert(tonumber(entry_meta.retention_expires_at_unix)>0)
must(c:entry_metadata('2',{retention_seconds=0}))
local retained=must(store:open_collection({namespace='test',name='journal'}))
must(c:delete_entry('1'))
local third=must(c:append({append_id=lc.xid_new()},'{"value":3}'));assert(third.position=='3')
local listed={};local listed_ok,list_error=store:list_collections({namespace='test'},function(info) listed[#listed+1]=info.name end)
assert(not list_error and listed[1]=='journal')
local co=must(store:xa_coordinator())
local committed=must(workflow:xa_transaction(co,function(transaction)
  fails(app.close,app)
  local payload='{"value":42}'
  must(transaction:append_outbox({kind='created',operation_id=lc.xid_new(),
    effect_id=lc.xid_new(),effect_key='created:42',destination='test',
    payload_digest=require('openssl').digest_materialized_hex('sha256',payload)},payload))
  return 'committed'
end))
assert(committed.applied and committed.decision==lc.XA_DECISION_COMMIT)
assert(committed.values[1]=='committed' and workflow:stats().forwarded_keys==1)
must(app:pump(0))
assert(workflow:stats().received_keys==1 and delivered==1)
assert(opened>=1);lc.open=open
local rejected=workflow:xa_transaction(co,function(transaction)
  fails(transaction.append_outbox,transaction,{kind='unregistered'},'{}')
end)
assert(rejected==nil and workflow:stats().forwarded_keys==1)
local op=must(co:begin_start());must(storage.await_blocking(op))
local tx=must(op:take_transaction());op:close_blocking()
assert(type(tx:id())=='string' and #tx:id()>0)
local rollback=must(tx:rollback_start());must(storage.await_blocking(rollback))
local outcome=rollback:result();assert(outcome.done and outcome.decision==lc.XA_DECISION_ABORT and outcome.applied)
rollback:close_blocking();tx:close_blocking();co:close_blocking()
must(app:stop())
fails(retained.describe,retained)
must(app:start())
local reopened=must(store:open_collection({namespace='test',name='journal'}))
assert(reopened:describe().live_count=='2')
must(app:stop());app:close()
fails(reopened.describe,reopened)
collectgarbage('collect')
print('upgrade seam behavior ok')
