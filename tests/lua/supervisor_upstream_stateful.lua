-- Direct official facade proof of the 0.23 renewal/version repair. No Vectis
-- convenience wrapper, timing fault, expiry wait or dependency stub.
local lc=require('lockdc')
local root=assert(os.getenv('VECTIS_WORKFLOW_TEST_ROOT'))
local key=assert(lc.pouch_crypto_generate_key())
local cases={{order='read',decision='complete'},{order='update',decision='complete'}}
for _,order in ipairs({'renew-update','update-renew','repeated'}) do
  for _,decision in ipairs({'complete','retry','dead'}) do
    cases[#cases+1]={order=order,decision=decision}
  end
end
for index,case in ipairs(cases) do
  local client=assert(lc.open({endpoints={'pouch://'..root..'/'..index},
    pouch={single_writer=false,crypto_key=key}}))
  local box=assert(client:new_outbox({namespace='probe',claim_ttl_seconds=30}))
  local result=assert(box:transaction(function(tx)
    return tx:append({operation_id='probe',effect_id='probe',effect_key='probe',kind='probe',destination='local',
      payload_digest='2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824'},'hello')
  end))
  local dispatcher=assert(box:dispatcher())
  assert(dispatcher:notify_outbox_key(result.outbox_receipts[1].outbox_key))
  local state_info,retained
  local count,err=dispatcher:pump({with_state=true,max_jobs=1,timeout_ms=0,handlers={probe=function(job)
    local state=assert(job:state());retained=state;state_info=state:info()
    if case.order=='renew-update' or case.order=='repeated' then assert(job:renew(30)) end
    if case.order~='read' then assert(state:update_json({seen=true,revision=1})) end
    if case.order=='update-renew' or case.order=='repeated' then assert(job:renew(30)) end
    if case.order=='repeated' then
      assert(job:renew(30));assert(state:update_json({seen=true,revision=2}));assert(job:renew(30))
    end
    if case.decision=='retry' then return job:retry({delay_seconds=3600,diagnostic='renewal regression'}) end
    if case.decision=='dead' then return job:dead_letter('renewal regression') end
    return job:complete()
  end}})
  assert(count==1,err and err.message)
  local lease=assert(client:acquire({namespace='probe',key=result.outbox_receipts[1].outbox_key,owner='inspector',ttl_seconds=30}))
  local expected=case.decision=='complete' and 'completed' or case.decision=='retry' and 'retry_wait' or 'dead_letter'
  assert(lease:read_json().dispatch_state==expected);assert(lease:release())
  -- Obtain the reserved checkpoint identity from its public lease info rather
  -- than reconstructing private key grammar. Verify persisted data separately.
  lease=assert(client:acquire({namespace=state_info.namespace,key=state_info.key,owner='inspector',ttl_seconds=30}))
  local value,meta=lease:read_json()
  -- Completion intentionally retires its private checkpoint. Retry/dead-letter
  -- preserve it for a later delivery/replay.
  if case.decision=='complete' then assert(value==nil and meta.no_content)
  else assert(value.seen and value.revision==(case.order=='repeated' and 2 or 1)) end
  assert(lease:release())
  local valid=pcall(retained.info,retained);assert(not valid,'terminal job retained a usable checkpoint')
  assert(dispatcher:stop(1000));dispatcher:close();box:close();client:close()
end
print('eleven stateful renewal/outcome cases passed; supervisor workflow behavior ok')
