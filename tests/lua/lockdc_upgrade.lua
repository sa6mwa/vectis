local v, lc = require('vectis'), require('lockdc')
local root = assert(os.getenv('VECTIS_UPGRADE_TEST_ROOT'))
local key = assert(lc.pouch_crypto_generate_key())
local function must(value, err)
  assert(value ~= nil, debug.traceback(type(err)=='table' and err.message or tostring(err),2))
  return value
end
local function rejected(value, err, code)
  assert(value == nil and type(err)=='table' and err.message and code ~= 0)
end
local function closed(fn, self) assert(not pcall(fn, self)) end
local function open(mode)
  local config={endpoints={'pouch://'..root..'/converted'}, logger_disabled=true,
    pouch={single_writer=false, background_compaction=false, crypto_key=key, compression=mode}}
  local app=must(v.app.new({app_name='lockdc-upgrade',tls={mode='disabled'},lockd=config}))
  local store=app:storage({client=config})
  must(app:start())
  return app,store,config
end
local function mode_is(mode)
  local f <close> = assert(io.open(root..'/converted/manifest','rb'))
  assert(f:read('*a'):find('compression='..mode,1,true))
end
local app,store,config=open('none')
local caps=store:collection_capabilities()
assert(caps.count_retention and caps.append_writer and caps.append_collection_retention)
local c=must(store:create_collection({namespace='journal',name='events',max_entries='2'}))
assert(c:describe().max_entries=='2')
local request={append_id='push-1',collection_retention_seconds=3600}
local writer=must(c:append_begin(request))
must(writer:write('')) -- Empty chunks do not finish the JSON value.
must(writer:write('{ "data": "'))
local chunk=string.rep('x',4096)
for _=1,64 do must(writer:write(chunk)) end
must(writer:write('", "n": 1 }'))
assert(c:append_status('push-1').state=='absent')
local entry=must(writer:commit())
assert(entry.position=='1' and not entry.duplicate)
assert(tonumber(entry.collection_retention_expires_at_unix)>0)
assert(c:describe().retention_expires_at_unix==entry.collection_retention_expires_at_unix)
rejected(writer:write('{}'))
writer:close();writer:close()
closed(writer.commit,writer)
local current=must(c:metadata({retention_seconds=0,max_entries='3'}))
assert(current.max_entries=='3' and tonumber(current.retention_expires_at_unix)==0)
-- Retry preserves the original observation without reapplying the directive.
local retry=must(c:append_begin(request))
must(retry:write('{"data":"'))
for _=1,64 do must(retry:write(chunk)) end
must(retry:write('","n":1}'))
local duplicate=must(retry:commit());retry:close()
assert(duplicate.duplicate and duplicate.position==entry.position)
assert(duplicate.collection_retention_expires_at_unix==entry.collection_retention_expires_at_unix)
assert(tonumber(c:describe().retention_expires_at_unix)==0)
local conflict=must(c:append_begin(request))
must(conflict:write('{"n":2}'));rejected(conflict:commit());conflict:close()
local aborted=must(c:append_begin({append_id='aborted',collection_retention_seconds=10}))
must(aborted:write('{"incomplete":'));must(aborted:abort());must(aborted:abort());aborted:close()
assert(c:append_status('aborted').state=='absent')
assert(tonumber(c:describe().retention_expires_at_unix)==0)
local invalid=must(c:append_begin({append_id='invalid'}))
must(invalid:write('{'));rejected(invalid:commit());invalid:close()
assert(c:append_status('invalid').state=='absent')
local abandoned=must(c:append_begin({append_id='closed'}))
must(abandoned:write('{}'));abandoned:close()
assert(c:append_status('closed').state=='absent')
rejected(store:open_collection({namespace='journal',name='events',max_entries='0'}))
rejected(c:entry_metadata('1',{max_entries='0'}))
for _,limit in ipairs({2,'01','-1','18446744073709551616'}) do
  rejected(c:metadata({max_entries=limit}))
end
assert(c:metadata({max_entries='18446744073709551615'}).max_entries=='18446744073709551615')
assert(c:metadata({max_entries='0'}).max_entries=='0')
assert(tonumber(c:metadata({retention_seconds=7200}).retention_expires_at_unix)>0)
local pull=must(c:append({append_id='pull-2',collection_retention_seconds=0},'{"n":2}'))
assert(tonumber(pull.collection_retention_expires_at_unix)==0)
assert(tonumber(c:describe().retention_expires_at_unix)==0)
-- Count policy is propagated, but append never synchronously trims.
assert(c:metadata({max_entries='1'}).max_entries=='1')
assert(c:describe().live_count=='2')
local seen=0
store:list_collections({namespace='journal'},function(info)
  assert(info.max_entries=='1');seen=seen+1
end)
assert(seen==1)
-- A writer owns its native collection independently of its public proxy.
local survivor=must(c:append_begin({append_id='survivor'}));must(survivor:write('{"n":3}'))
c:close();assert(must(survivor:commit()).position=='3');survivor:close()
c=must(store:open_collection({namespace='journal',name='events'}))
local _,partial=c:read({cancelled=function() return true end},{write=function() error('cancelled before delivery') end})
assert(partial.resume_cursor)
local cursor=partial.resume_cursor
local stop_writer=must(c:append_begin({append_id='stop-abort'}));must(stop_writer:write('{}'))
must(v.lockd.save_json(config,{namespace='ordinary',key='state',owner='test',ttl_seconds=30},{answer=42}))
must(v.lockd.with_client(config,function(client)
  local lease=must(client:acquire({namespace='ordinary',key='state',owner='test',ttl_seconds=30}))
  must(lease:attach({name='payload',content_type='application/octet-stream'},'a\0b'))
  must(lease:release());return true
end))
must(app:stop());closed(stop_writer.commit,stop_writer)
must(app:start())
local reopened=must(store:open_collection({namespace='journal',name='events'}))
assert(reopened:append_status('stop-abort').state=='absent')
must(app:stop());app:close()
-- Both explicit conversions and omission preserve identity/body/continuations.
for _,mode in ipairs({'zlib','inherit','none'}) do
  app,store,config=open(mode~='inherit' and mode or nil)
  mode_is(mode=='inherit' and 'zlib' or mode)
  c=must(store:open_collection({namespace='journal',name='events'}))
  assert(c:describe().max_entries=='1' and c:describe().live_count=='3')
  local observed,bytes=0,0
  local _,progress=c:read({cursor=cursor},{write=function(piece)
    assert(#piece<=65536);bytes=bytes+#piece;return true
  end})
  assert(progress.complete and progress.completed_records=='3' and bytes>262144)
  assert(c:append_status('push-1').entry.etag==entry.etag)
  must(v.lockd.with_client(config,function(client)
    assert(must(client:read_json({namespace='ordinary',key='state'})).answer==42)
    local lease=must(client:acquire({namespace='ordinary',key='state',owner='test',ttl_seconds=30}))
    local _,meta=lease:get_attachment({selector={name='payload'}},
      {write=function(piece) observed=observed+#piece;return true end})
    assert(meta and meta.attachment and observed==3);must(lease:release());return true
  end))
  -- A live peer prevents conversion, while its stored mode remains usable.
  if mode=='zlib' then
    local peerconfig={endpoints=config.endpoints,logger_disabled=true,
      pouch={single_writer=false,crypto_key=key,compression='none'}}
    local peer=must(lc.open(peerconfig));mode_is('zlib');peer:close()
  end
  must(app:stop());app:close()
end
-- A failed conversion request must fail startup without changing the root.
local bad=must(v.app.new({app_name='failed-conversion',tls={mode='disabled'},
  lockd={endpoints=config.endpoints,logger_disabled=true,pouch={single_writer=false,
    crypto_key=must(lc.pouch_crypto_generate_key()),compression='zlib'}}}))
local started,failure=bad:start()
assert(not started and failure and failure.message)
bad:close();mode_is('none')
-- Direct upstream ownership survives explicit collection and client close too.
local direct=must(lc.open(config))
local native=must(direct:open_collection({namespace='journal',name='events'}))
assert(native:describe().live_count=='3' and native:append_status('push-1').entry.etag==entry.etag)
local held=must(native:append_begin({append_id='direct-owned'}))
native:close();direct:close();must(held:write('{}'));must(held:commit());held:close()
collectgarbage('collect')
print('upgrade seam behavior ok: liblockdc 0.27.0')
