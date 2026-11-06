local v,lc=require('vectis'),require('lockdc')
local root=assert(os.getenv('VECTIS_UPGRADE_TEST_ROOT'))
assert(lc.version_string()=='0.28.0')
assert(v.libs.lockdc==lc)
local function must(value,err)
  assert(value~=nil,debug.traceback(type(err)=='table' and err.message or tostring(err),2))
  return value
end
local function rejected(value,err,code)
  assert(value==nil and type(err)=='table' and err.message and type(code)=='number' and code~=0)
  return err
end
local function read(c,opts)
  local chunks={}
  local value,result,code,partial=c:read(opts or {},{write=function(bytes)
    assert(#bytes<=65536);chunks[#chunks+1]=bytes;return true
  end})
  return table.concat(chunks),result,code,partial,value
end
for _,owned in ipairs({false,true}) do
  for _,encrypted in ipairs({false,true}) do
    local config={endpoints={'pouch://'..root..'/'..tostring(owned)..'-'..tostring(encrypted)},
      logger_disabled=true,pouch={single_writer=false,background_compaction=false,
        compression=encrypted and 'zlib' or 'none',segment_target_bytes=131072,
        crypto_key=(owned or encrypted) and must(lc.pouch_crypto_generate_key()) or nil}}
    local app,store
    if owned then
      app=must(v.app.new({app_name='pouch-collections',tls={mode='disabled'},lockd=config}))
      store=app:storage({client=config})
      assert(not pcall(store.collection_capabilities,store),'no client before app startup')
      must(app:start())
    else store=must(lc.open(config)) end
    local caps=must(store:collection_capabilities())
    assert(caps.collections and caps.append_staged and caps.append_writer and caps.leases
      and caps.lease_takeover and caps.private_reads and caps.atomic_edit)
    -- Interleave a producer larger than the native codec chunk with another
    -- publication. Pending chunks stay invisible until the final entry commit.
    local streaming=must(store:create_collection({namespace='stream',name='chunks'}))
    local pending=must(streaming:append_begin({append_id='large-push'}))
    must(pending:write('{"data":"'))
    local expected=must(v.digest.new('sha256'))
    must(expected:update('{"data":"'))
    for n=1,32 do
      local piece=string.rep('x',4096)
      must(pending:write(piece));must(expected:update(piece))
      if n==16 then
        assert(read(streaming)=='')
        assert(must(streaming:append({append_id='peer'},'{"first":true}')).position=='1')
      end
    end
    assert(read(streaming)=='{"first":true}\n')
    must(pending:write('"}'));must(expected:update('"}\n'))
    local published=must(pending:commit());pending:close()
    assert(published.position=='2' and published.bytes=='131083')
    local observed=must(v.digest.new('sha256'))
    local count,pieces=0,0
    local _,done,rc=streaming:read({after='1'},{write=function(piece)
      assert(#piece<=65536);count=count+#piece;pieces=pieces+1
      must(observed:update(piece));return true
    end})
    assert(rc==nil and done.complete and count==131084 and pieces>1)
    assert(observed:final_hex()==expected:final_hex())
    observed:close();expected:close();streaming:close()
    local owner=must(store:create_collection({namespace='journal',name='events',max_entries='100',
      lease={owner='initial',ttl_seconds=60,private_read=true}}))
    local public=must(store:open_collection({namespace='journal',name='events'}))
    local seen=0
    must(store:list_collections({namespace='journal'},function(info)
      assert(info.name=='events' and info.lease.owner=='initial' and info.max_entries=='100')
      seen=seen+1
    end))
    assert(seen==1)
    local first=must(owner:append({append_id='one'},'{"n":1}'))
    assert(first.position=='1')
    rejected(public:acquire({owner='initial'}))
    rejected(public:append({append_id='denied'},'{}'))
    local bytes,denied,code=read(public)
    assert(bytes=='' and denied and code~=nil)
    local original=must(owner:head())
    local renewed=must(owner:keepalive({ttl_seconds=120}))
    assert(renewed.active and renewed.fencing_token=='1')
    assert(owner:head().etag==original.etag,'renewal is not a content mutation')
    local writer=must(owner:append_begin({append_id='fenced'}))
    must(writer:write('{}'))
    local observed=must(public:describe()).lease.lease_id
    local next_owner=must(public:takeover({owner='next',private_read=true},observed))
    rejected(public:takeover({owner='stale'},observed))
    rejected(writer:commit());writer:close()
    rejected(owner:keepalive({ttl_seconds=60}));rejected(owner:release())
    owner:close()
    assert(public:append_status('fenced').state=='absent')
    -- The named staged pull streams through the canonical writer, closes its
    -- adapter exactly once, and never resets or replays input.
    local function source(fail)
      local part,closed=0,0
      return {read=function(cap)
        part=part+1
        if part==1 then return '{"data":"' end
        if fail==true or (fail=='late' and part==66) then error('staged source fault') end
        if part<=65 then return string.rep('x',math.min(cap,4096)) end
        if part==66 then return '"}' end
        return nil
      end,reset=function() error('unexpected reset') end,
      close=function() closed=closed+1 end},function() return closed end
    end
    local src,closes=source(false)
    local staged=must(next_owner:append_staged({append_id='staged'},src))
    assert(staged.position=='2' and staged.bytes=='262155' and closes()==1)
    src,closes=source(false)
    assert(next_owner:append_staged({append_id='staged'},src).duplicate and closes()==1)
    for _,failure in ipairs({true,'late'}) do
      src,closes=source(failure)
      rejected(next_owner:append_staged({append_id='source-failed'},src))
      assert(closes()==1 and public:append_status('source-failed').state=='absent')
    end
    must(next_owner:append({append_id='three'},'{"n":3}'))
    local head=must(next_owner:head())
    local request={operation_id='replace',expected_head=head.etag,
      ranges={{after='0',through='2'}},replacement={append_id='baseline'},
      recovery='{"checkpoint":true}',recovery_anchor='replacement'}
    local phase,source_closed=0,0
    local edited=must(next_owner:edit(request,{read=function(cap)
      assert(cap>0);phase=phase+1
      if owned then
        assert(not pcall(next_owner.close,next_owner),'active receiver close')
        assert(not pcall(store.close,store),'active store close')
        assert(not pcall(next_owner.head,next_owner),'callback reentry')
      end
      if phase==1 then return '{"context":' end
      if phase==2 then return '1}' end
      return nil
    end,close=function() source_closed=source_closed+1 end}))
    assert(source_closed==1 and edited.published and edited.deleted_count=='2'
      and edited.live_count=='2' and edited.replacement.position=='4')
    assert(next_owner:edit(request,' { "context":1 } ').duplicate)
    rejected(next_owner:edit(request,'{"context":2}'))
    local receipt=must(next_owner:edit_status('replace'))
    assert(receipt.found and receipt.result.head_etag==edited.head_etag)
    assert(not next_owner:edit_status('absent').found)
    local after=must(next_owner:head())
    assert(after.recovery==request.recovery and after.recovery_anchor=='4'
      and after.recovery_anchor_available)
    local conflict=rejected(next_owner:edit({operation_id='stale',expected_head=head.etag}))
    assert(conflict.current_head.head_etag==after.etag and not conflict.current_head.published)
    local delivered=0
    rejected(next_owner:read({expected_head=head.etag},{write=function(chunk) delivered=delivered+#chunk end}))
    assert(delivered==0)
    bytes,head,code=read(next_owner,{expected_head=after.etag})
    assert(code==nil and head.complete and bytes=='{"n":3}\n{"context":1}\n')
    rejected(next_owner:edit({operation_id='bad-range',expected_head=after.etag,
      ranges={{after='2',through='1'}}}))
    rejected(next_owner:edit({operation_id='bad-marker',expected_head=after.etag,recovery=string.rep('x',1025)}))
    rejected(next_owner:edit({operation_id='bad-input',expected_head=after.etag,
      replacement={append_id='unpublished'}},{read=function() error('edit source fault') end}))
    assert(not next_owner:edit_status('bad-input').found and next_owner:head().etag==after.etag)
    must(next_owner:delete_entry('4'))
    assert(not next_owner:head().recovery_anchor_available)
    assert(next_owner:edit(request,'{"context":1}').duplicate)
    must(next_owner:release())
    if owned then assert(not pcall(next_owner.head,next_owner)) end
    next_owner:close()
    local current=must(public:acquire({owner='again'}))
    local abandoned=must(current:append_begin({append_id='abandoned'}))
    must(abandoned:write('{}'))
    if owned then
      must(app:stop())
      assert(not pcall(current.head,current) and not pcall(abandoned.commit,abandoned))
      must(app:start())
      public=must(store:open_collection({namespace='journal',name='events'}))
      -- Stop closes local resources; durable lease ownership requires takeover.
      assert(public:describe().lease.active and public:append_status('abandoned').state=='absent')
      current=must(public:takeover({owner='restart'},public:describe().lease.lease_id))
    else
      abandoned:close();current:close()
      current=must(public:takeover({owner='restart'},public:describe().lease.lease_id))
    end
    -- Stream a multi-frame replacement and its readback without collecting it.
    src,closes=source(false)
    local large=must(current:edit({operation_id='large-edit',expected_head=current:head().etag,
      replacement={append_id='large-body'},recovery='large',recovery_anchor='replacement'},src))
    assert(large.replacement.position=='5' and closes()==1)
    local expected_hash=must(v.digest.new('sha256'))
    local observed_hash=must(v.digest.new('sha256'))
    must(expected_hash:update('{"data":"'))
    for _=1,64 do must(expected_hash:update(string.rep('x',4096))) end
    must(expected_hash:update('"}\n'))
    local transferred,chunks=0,0
    local _,progress,read_code=current:read({after='4',expected_head=large.head_etag},
      {write=function(piece)
        assert(#piece<=65536);transferred=transferred+#piece;chunks=chunks+1
        must(observed_hash:update(piece));return true
      end})
    assert(read_code==nil and progress.complete and chunks>1 and transferred==262156)
    assert(expected_hash:final_hex()==observed_hash:final_hex())
    expected_hash:close();observed_hash:close()
    must(current:edit({operation_id='clear',expected_head=current:head().etag,recovery=''}))
    assert(current:head().recovery=='')
    must(current:release());current:close()
    public:close()
    if owned then must(app:stop());app:close() else store:close() end
    -- Persisted receipts and remaining permanent positions survive a fresh client.
    local client=must(lc.open(config))
    local c=must(client:open_collection({namespace='journal',name='events'}))
    assert(c:edit_status('replace').found and c:head().upper_position=='5')
    assert(c:head().live_count=='2' and c:head().recovery=='')
    c:close();client:close()
  end
end
local remote=must(lc.open({endpoints={'http://127.0.0.1:1'},logger_disabled=true,disable_mtls=true}))
local caps=must(remote:collection_capabilities())
assert(not caps.collections and not caps.leases and not caps.atomic_edit and not caps.append_staged)
rejected(remote:create_collection({name='unsupported'}));remote:close()
collectgarbage('collect')
print('upgrade seam behavior ok: Pouch collections direct and app-owned Lua')
