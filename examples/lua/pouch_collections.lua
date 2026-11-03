local v,lc=require('vectis'),require('lockdc')
local app=assert(v.app.new({app_name='collection-journal',tls={mode='disabled'}}))
local store=app:storage() -- encrypted app defaults; materializes after startup
assert(app:start())
local journal=assert(store:create_collection({namespace='journal',name=lc.xid_new(),
  lease={owner='example',ttl_seconds=60,private_read=true}}))
-- Each append commits independently under the same held collection lease.
assert(journal:append({append_id=lc.xid_new()},'{"event":"created"}'))
assert(journal:keepalive({ttl_seconds=60})) -- explicit renewal, separate from retention
local head=assert(journal:head())
local phase=0
local request={operation_id=lc.xid_new(),expected_head=head.etag,
  ranges={{after='0',through=head.upper_position}},replacement={append_id=lc.xid_new()},
  recovery='{"replay":"baseline"}',recovery_anchor='replacement'}
local edited=assert(journal:edit(request,
  {read=function(cap)
    assert(cap>=16);phase=phase+1
    if phase==1 then return '{"state":' end
    if phase==2 then return '"ready"}' end
    return nil
  end}))
print('baseline position '..edited.replacement.position)
local _,progress=journal:read({expected_head=edited.head_etag},{write=function(chunk)
  io.stdout:write(chunk);return true
end})
assert(progress.complete)
-- Keep request for edit_status(request.operation_id) reconciliation after an
-- uncertain result. An identical retry needs a fresh source and the same IDs.
assert(journal:release()) -- closes the credentialed receiver on success
assert(app:stop());app:close()
