local v, lc, openssl = require("vectis"), require("lockdc"), require("openssl")
local root = assert(os.getenv("VECTIS_WORKFLOW_ROUTE_ROOT"))
local port = assert(tonumber(os.getenv("VECTIS_WORKFLOW_ROUTE_PORT")))
local key = assert(lc.pouch_crypto_generate_key())
local app = assert(v.app.new({ app_name="workflow-route", bind="127.0.0.1", port=port,
  worker_count=2, worker_set_affinity_disabled=true, shutdown_grace_ms=1000,
  request_body_spool_dir=root.."/spool", tls={mode="disabled"} }))
local scope = app:supervisor({ name="effects" })
local other = app:supervisor({ name="other-effects" })
local probe_ms = assert(tonumber(os.getenv("VECTIS_WORKFLOW_ROUTE_PROBE_MS")))
local client={endpoints={"pouch://"..root.."/pouch"},pouch={single_writer=false,crypto_key=key}}
local workflow = app:workflow({ namespace="route-effects", ipc_capacity=4, recovery_probe_ms=probe_ms,recovery_interval_seconds=0,
  client=client })
local xa= os.getenv('VECTIS_WORKFLOW_ROUTE_XA')=='1'
local store=xa and app:storage({client=client})
-- Fixture pump proves explicit SUS log safe points run during blocking wait.
-- Real foreign-thread copying/delivery is tested by the actual-source pthread unit.
local sus=require("sus")
local original_log_pump=sus.pump_logs
local log_timer
sus.pump_logs=function(max)
  assert(max==2 and scope.native:domain()=="owner" and select(2,coroutine.running()))
  log_timer:disable()
  io.stdout:write("sus drained\n"); io.stdout:flush()
  return 0,{}
end
log_timer=scope:sus_logs({name="logs",every_ms=1,max=2})
local effects=0
local function handle(job, selected)
  assert(selected.native:domain()=="owner", "effects must run in owner PID domain")
  assert(selected:state()=="running", "effects must run in supervisor owner state")
  assert(select(2, coroutine.running()), "effects must run on main caller")
  effects=effects+1
  local body=""
  assert(job:write_payload({write=function(bytes) body=body..bytes; return true end}))
  assert(body==job.effect_key)
  io.stdout:write("effect ",job.effect_key,"\n"); io.stdout:flush()
  return job:complete()
end
scope:outbox({workflow=workflow,kind="one",handler=function(job) return handle(job,scope) end})
other:outbox({workflow=workflow,kind="two",handler=function(job) return handle(job,other) end})
assert(app:route({path="/effect",method="POST",body="buffered",max_body_bytes=64,handler=function(request)
  assert(scope:state()=="declared" and effects==0, "producer must never run effect handlers")
  assert(not pcall(app.close,app), "owned HTTP callback must reject close")
  local id=request.body
  local function append(tx)
    assert(not pcall(app.close,app)); assert(not pcall(app.stop,app))
    assert(not pcall(app.pump,app,0)); assert(not pcall(scope.stop,scope))
    assert(not pcall(scope.native.stop,scope.native))
    if xa then
      local state=assert(tx:acquire({namespace='route-domain',key=id,owner='route',ttl_seconds=30}))
      assert(state:update_json({id=id,accepted=true}))
    end
    tx:append_outbox({operation_id=id,effect_id="effect",effect_key=id,
      payload_digest=openssl.sha256_materialized(id),kind=id:find("two",1,true) and "two" or "one",
      destination="local",content_type="text/plain"},id)
  end
  local result,err,receipts
  if xa then
    local co=assert(store:xa_coordinator())
    result,err=workflow:xa_transaction(co,append)
    co:close()
    receipts=0
    for _,outbox in ipairs(result and result.outboxes or {}) do
      receipts=receipts+#(outbox.outbox_receipts or {})
    end
  else
    result,err=workflow:transaction(append)
    receipts=result and #(result.outbox_receipts or {})
  end
  if not result then return {status=500,body=lc.encode_json(err)} end
  return {status=200,content_type="application/json",body=lc.encode_json({
    receipts=receipts,effects_in_producer=effects})}
end}))
assert(app:start())
io.stdout:write("ready\n"); io.stdout:flush()
local ok, err=app:wait(); assert(ok,err and err.message)
assert(workflow:stats().dispatcher_starts==1)
if probe_ms==1000000 then
  assert(workflow:stats().received_keys>=8 and workflow:stats().received_reconciles==0,
    "direct worker keys must wake the owner wait loop without timer recovery")
end
io.stdout:write("stopped ",effects,"\n"); io.stdout:flush()
app:close(); sus.pump_logs=original_log_pump
