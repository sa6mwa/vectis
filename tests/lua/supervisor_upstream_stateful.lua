-- A passing probe establishes an upstream blocker, not stateful workflow support.
local lc = require("lockdc")
local root = assert(os.getenv("VECTIS_WORKFLOW_TEST_ROOT"))
local key = assert(lc.pouch_crypto_generate_key())
for index, variant in ipairs({"read", "update", "renew-update"}) do
  local client = assert(lc.open({endpoints={"pouch://"..root.."/"..index},
    pouch={single_writer=false,crypto_key=key}}))
  local outbox = assert(client:new_outbox({namespace="probe",claim_ttl_seconds=30}))
  local result = assert(outbox:transaction(function(tx)
    return tx:append({operation_id="probe",effect_id="probe",effect_key="probe",kind="probe",destination="local",
      payload_digest="2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"},"hello")
  end))
  local dispatcher=assert(outbox:dispatcher())
  assert(dispatcher:notify_outbox_key(result.outbox_receipts[1].outbox_key))
  local phase = "claim"
  local count,err=dispatcher:pump({with_state=true,max_jobs=1,timeout_ms=0,handlers={probe=function(job)
    if variant=="renew-update" then phase="renew"; assert(job:renew(30)); phase="renewed" end
    local state=assert(job:state())
    if variant~="read" then phase="update"; assert(state:update_json({seen=true})); phase="updated" end
    phase="terminal"
    return job:complete()
  end}})
  if variant=="renew-update" then
    assert(count==nil and err.message:find("pouch state version precondition failed",1,true))
    assert(phase=="terminal", "renewal and checkpoint update must succeed before the terminal failure")
    print("stateful renewal failure phase: "..phase)
  else
    assert(count==1, err and err.message)
    local completed=assert(client:acquire({namespace="probe",key=result.outbox_receipts[1].outbox_key,owner="report",ttl_seconds=30}))
    assert(completed:read_json().dispatch_state=="completed");assert(completed:release())
  end
  assert(dispatcher:stop(1000)); dispatcher:close(); outbox:close(); client:close()
end
print("upstream stateful renewal blocker reproduced; supervisor workflow behavior ok")
