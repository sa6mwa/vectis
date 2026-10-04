local v, lc, openssl = require("vectis"), require("lockdc"), require("openssl")
assert(v.supervisor == require("vectis.supervisor") and v.workflow == require("vectis.workflow"))
local root = assert(os.getenv("VECTIS_WORKFLOW_TEST_ROOT"))
local key = assert(lc.pouch_crypto_generate_key())
local client_config = { endpoints = { "pouch://" .. root }, pouch = { single_writer = false, crypto_key = key }, logger_disabled = true }
local function must(value, err) assert(value ~= nil, type(err) == "table" and err.message or tostring(err)); return value end
local app = must(v.app.new({ app_name = "workflow-test", tls = { mode = "disabled" } }))
assert(type(app) == "userdata")
local first = app:supervisor({ name = "first", channel = { capacity = 1, max_frame_size = 4, signal_kinds = {"repair"} } })
local second = app:supervisor({ name = "second" })
assert(not pcall(first.consumer,first,{client={endpoints={"invalid://invalid"}},request={queue="invalid"},every_ms=0,
  handler=function() error("rejected source must not attach") end}))
assert(not pcall(app.workflow,app,{namespace="bad-probe",client=client_config,recovery_probe_ms=0}))
local no_orphan=app:supervisor({name="@workflow:bad-probe",start=false})
must(coroutine.resume(coroutine.create(function()
  assert(not pcall(app.supervisor, app, {name="coroutine-scope"}))
end)))
local events, count, retained = {}, {}, nil
first:receiver({ name = "bytes", handler = function(event) events[#events + 1] = event end })
first:receiver({ name = "repair", handler = function(event) events[#events + 1] = event end })
local channel = first:channel()
assert(app:ipc("first") == channel)
assert(channel:send("bytes", "abc"))
local ok, status = channel:send("bytes", "def"); assert(ok == nil and status == "full")
assert(channel:signal("repair")); assert(channel:signal("repair"))
assert(not pcall(channel.signal, channel, "unknown"))
assert(not pcall(channel.send, channel, "bytes", {}))
assert(not pcall(function() app:supervisor({name="duplicate-signals",channel={signal_kinds={"x","x"}}}) end))
assert(not pcall(function() app:supervisor({name="lanes",lua_lanes=2}) end))
for _, bad_name in ipairs({false,12,"","bad\0name"}) do
  assert(not pcall(app.supervisor,app,{name=bad_name}))
  assert(not pcall(first.receiver,first,{kind=bad_name,handler=function() end}))
  assert(not pcall(first.timer,first,{name=bad_name,every_ms=1,handler=function() end}))
  assert(not pcall(channel.send,channel,bad_name,""))
end
assert(not pcall(app.supervisor,app,{name="bad-array",channel={signal_kinds={unknown="repair"}}}))
assert(not pcall(app.supervisor,app,{name="bad-start",start="yes"}))
local w = app:workflow({ namespace = "orders", client = client_config, ipc_capacity = 1, recovery_probe_ms = 1000000,
  claim_ttl_seconds = 30, max_attempts = 5, retry_initial_delay_seconds = 30, retry_max_delay_seconds = 30 })
local w2 = app:workflow({ namespace = "inventory", client = client_config, recovery_probe_ms = 1000000 })
local defaults = app:workflow({namespace="native-defaults", recovery_probe_ms=1000000})
local undersized = app:workflow({namespace="undersized", client=client_config, ipc_max_frame_size=1, recovery_probe_ms=1000000})
local default_count = 0
defaults:outbox({kind="default", handler=function(job) default_count=default_count+1; return job:complete() end})
local function deliver(job)
  assert(first:state() == "running" or second:state() == "running")
  local _, main = coroutine.running(); assert(main)
  if job.kind == "one" or job.kind == "two" or job.kind == "throws" or job.kind == "missing" or job.kind == "retry" or job.kind == "dead" then
    assert(job.max_attempts == 5, "workflow max_attempts must reach dispatcher claims")
  end
  must(job:renew(30))
  assert(job:state() == nil)
  count[job.effect_key] = (count[job.effect_key] or 0) + 1
  local pieces, bytes = {}, 0
  must(job:write_payload({ write = function(chunk)
    collectgarbage("collect")
    bytes = bytes + #chunk; pieces[#pieces + 1] = chunk; return true
  end }))
  if job.effect_key == "stream" then
    assert(bytes == 262144 and #pieces > 1)
    for _, piece in ipairs(pieces) do assert(piece == string.rep("s", #piece)) end
  else assert(table.concat(pieces) == "payload-" .. job.effect_key) end
  retained = job
  if job.kind == "throws" then error("deterministic handler failure") end
  if job.kind == "missing" then return end
  if job.kind == "retry" then return job:retry({delay_seconds=30,diagnostic="explicit retry"}) end
  if job.kind == "dead" then return job:dead_letter("permanent") end
  return job:complete({ delivery_reference = "local" })
end
first:outbox({ workflow = w, kind = "one", handler = deliver })
second:outbox({ workflow = w, kind = "two", handler = deliver })
w:outbox({kind="throws", handler=deliver})
w:outbox({kind="missing", handler=deliver})
w:outbox({kind="retry", handler=deliver})
w:outbox({kind="dead", handler=deliver})
w2:outbox({kind="inventory", handler=deliver})
undersized:outbox({kind="inventory", handler=deliver})
assert(not pcall(function() second:outbox({workflow=w, kind="one", handler=deliver}) end))
assert(not pcall(function() app:workflow({namespace="orders",client=client_config}) end))
local stateful = app:workflow({namespace="stateful",client=client_config,with_state=true,recovery_probe_ms=1000000})
local checkpoint, stateful_count
stateful_count = 0
stateful:outbox({kind="checkpoint",handler=function(job)
  assert(not pcall(job.renew,job,30))
  checkpoint = assert(job:state())
  assert(not pcall(function() return checkpoint._core end))
  assert(not pcall(function() checkpoint:close() end))
  assert(not pcall(function() checkpoint:release() end))
  must(checkpoint:update_json({seen=true}))
  stateful_count = stateful_count + 1
  return job:complete()
end})
assert(not pcall(function() w:transaction(function() end) end))
local function entry(id, kind)
  local payload = "payload-" .. id
  return {operation_id="op-"..id, effect_id="effect", effect_key=id, payload_digest=openssl.sha256(payload),
    kind=kind or "one", destination="local", content_type="text/plain"}, payload
end
local function append(workflow, id, kind)
  return must(workflow:transaction(function(tx)
    local e, payload = entry(id, kind)
    return tx:append_outbox(e, { read = (function()
      local at = 1
      return function(n) local chunk = payload:sub(at, at+n-1); at=at+#chunk; return chunk end
    end)() })
  end))
end
local ticks, queue_count, queue_delivery = 0, 0, nil
local queue_outcomes = {}
local clock = app:supervisor({name="clock",failure_policy="stop"})
clock:timer({name="once",every="1ms",handler=function(timer) ticks=ticks+1; timer:disable(); return true end})
local bad_scope=app:supervisor({name="fault",failure_policy="stop"})
bad_scope:receiver({name="fault",handler=function() error("deterministic receiver failure") end})
local guard=app:supervisor({name="guard"})
guard:receiver({name="guard",handler=function()
  local started,start_error=no_orphan:start()
  assert(not started and start_error:find("inside a supervisor callback",1,true))
  assert(no_orphan:state()=="declared")
  assert(not pcall(app.close,app))
  local stopped, err=app:stop(); assert(not stopped and err.message:find("inside a supervisor callback",1,true))
  assert(not app:pump(0))
end})
local queue_scope = app:supervisor({name="queue"})
queue_scope:consumer({client=client_config,request={namespace="queue",queue="jobs",owner="queue-test",visibility_timeout_seconds=30},
  handler=function(message)
    queue_count=queue_count+1; queue_delivery=message
    local payload=""; must(message:write_payload({write=function(bytes) payload=payload..bytes; return true end}))
    queue_outcomes[payload] = (queue_outcomes[payload] or 0) + 1
    if payload == "queue-throws" then error("queue callback failed") end
    if payload == "queue-missing" then return end
    assert(payload=="queue-body")
    return message:ack()
  end})
local queue_producer=must(lc.open(client_config))
must(queue_producer:enqueue({namespace="queue",queue="jobs",max_attempts=3,visibility_timeout_seconds=30},"queue-body"))
queue_producer:close()
must(app:start())
-- A transaction owns live native children and borrowed app loggers. Lifecycle
-- changes from the callback must fail without poisoning the transaction/app.
must(w2:transaction(function(tx)
  for _,method in ipairs({app.close,app.stop,app.restart,app.start,app.run,app.wait,app.pump}) do
    local ok,err=pcall(method,app,0)
    assert(not ok and err:find("inside a workflow or client callback",1,true))
  end
  assert(not pcall(first.stop,first))
  assert(not pcall(first.start,first))
  local e,p=entry("lifecycle-guard","inventory"); return tx:append_outbox(e,p)
end))
assert(not pcall(function() first:receiver({handler=function() end}) end))
must(app:pump(0)); must(app:pump(0))
assert(#events == 2 and events[1].payload == "abc" and events[2].type == "signal")
assert(queue_count==1)
assert(not pcall(queue_delivery.info,queue_delivery))
local fault_producer=must(lc.open(client_config))
for _, payload in ipairs({"queue-throws", "queue-missing"}) do
  must(fault_producer:enqueue({namespace="queue",queue="jobs",max_attempts=1,visibility_timeout_seconds=30}, payload))
end
for i=1,20000 do must(app:pump(0)); if queue_count == 3 then break end end
assert(queue_outcomes["queue-throws"]==1 and queue_outcomes["queue-missing"]==1)
local empty=must(fault_producer:dequeue_batch({namespace="queue",queue="jobs",owner="inspector",wait_seconds=0,page_size=1,visibility_timeout_seconds=30}))
assert(#empty==0, "failed handlers must terminalize through max-attempt queue policy")
assert(not pcall(queue_delivery.info,queue_delivery))
fault_producer:close()
for i=1,20000 do must(app:pump(0)); if ticks==1 then break end end
assert(ticks==1)
must(bad_scope:channel():send("fault","")); must(guard:channel():send("guard","")); must(app:pump(0))
assert(bad_scope:state()=="failed" and first:state()=="running")
assert(bad_scope:last_error().message:find("deterministic receiver failure",1,true))
must(bad_scope:start()); assert(bad_scope:last_error()==nil)
must(bad_scope:stop(0))
local rejected, reason=bad_scope:channel():send("fault",""); assert(not rejected and reason=="closed")
append(stateful, "checkpoint", "checkpoint"); must(app:pump(0)); assert(stateful_count==1)
assert(not pcall(function() checkpoint:update_json({seen=false}) end))
for _, rejection in ipairs({false, "error"}) do
  local ok, err = pcall(w.transaction,w,function(tx)
    local e,payload = entry("rollback-"..tostring(rejection),"one")
    tx:append_outbox(e,payload)
    local participant=must(tx:acquire({key="callback-rollback-"..tostring(rejection)}))
    must(participant:update_json({changed=true}))
    if rejection == false then return false end
    return nil, "intentional rollback"
  end)
  assert(not ok and err)
end
local result = append(w, "direct", "one")
assert(#result.outbox_receipts == 1 and not count.direct)
local before = w:stats()
must(app:pump(0))
assert(count.direct == 1, "direct receipt must reach handler in the first zero-wait pump")
assert(w:stats().received_keys == before.received_keys + 1)
assert(w:stats().dispatcher.direct_notifications > before.dispatcher.direct_notifications)
assert(w:stats().dispatcher_starts == 1)
assert(not pcall(retained.renew, retained, 30))
assert(not pcall(retained.state, retained))
assert(not pcall(retained.write_payload, retained, {write=function() return true end}))
append(w, "direct", "one") -- duplicate never produces a second effect
must(app:pump(0)); assert(count.direct == 1)
append(w, "kind-two", "two"); must(app:pump(0)); assert(count["kind-two"] == 1)
append(w2, "inventory", "inventory"); must(app:pump(0)); assert(count.inventory == 1)
append(defaults, "default-storage", "default"); must(app:pump(0)); assert(default_count == 1)
append(undersized, "oversized-key", "inventory")
for i=1,20000 do must(app:pump(0)); if count["oversized-key"] == 1 then break end end
assert(count["oversized-key"] == 1)
assert(undersized:stats().failed_wakes == 1 and undersized:stats().reconcile_wakes == 1)
assert(undersized:stats().received_keys == 0 and undersized:stats().received_reconciles == 1)
local stream_entry = entry("stream", "one")
stream_entry.payload_digest = openssl.sha256(string.rep("s", 262144))
local produced, reads = 0, 0
must(w:transaction(function(tx)
  return tx:append_outbox(stream_entry, {read=function(n)
    reads = reads + 1
    local size = math.min(n, 512, 262144-produced)
    produced = produced + size
    return string.rep("s", size)
  end})
end))
assert(produced == 262144 and reads > 512)
must(app:pump(0)); assert(count.stream == 1)
local caught = pcall(function()
  w:transaction(function(tx)
    local participant = must(tx:acquire({ key = "domain-state" }))
    must(participant:update_json({ changed = true }))
    local e, payload = entry("orphan", "unregistered")
    assert(not pcall(tx.append_outbox, tx, e, payload))
    e, payload = entry("poisoned-valid", "one")
    tx:append_outbox(e, payload)
    return true
  end)
end)
assert(not caught, "caught unknown kind must poison enclosing transaction")
local inspect = must(lc.open(client_config))
for _, rejection in ipairs({false,"error"}) do
  local participant=must(inspect:acquire({namespace="orders",owner="inspector",ttl_seconds=30,key="callback-rollback-"..tostring(rejection)}))
  local _,metadata=participant:read_json(); assert(metadata.no_content)
  must(participant:release())
end
local completed = must(inspect:acquire({namespace="orders",owner="inspector",ttl_seconds=30,key=result.outbox_receipts[1].outbox_key}))
assert(must(completed:read_json()).dispatch_state=="completed")
must(completed:release())
local state_lease = must(inspect:acquire({namespace="orders",owner="inspector",ttl_seconds=30,key="domain-state"}))
local _, meta = state_lease:read_json(); assert(meta.no_content)
must(state_lease:release())
for _, kind in ipairs({"throws", "missing", "retry", "dead"}) do
  local r = append(w, kind, kind)
  for i=1,20000 do must(app:pump(0)); if (count[kind] or 0) >= 1 then break end end
  -- Automatic retry uses upstream full jitter, whose valid delay includes zero.
  -- A single owner pump may also service the source continuation. Assert its
  -- bounded attempt budget, rather than inventing an exactly-once retry rule.
  assert(count[kind] and count[kind] >= 1 and count[kind] <= 5,
    "handler count "..tostring(count[kind])..": "..kind)
  if kind == "retry" or kind == "dead" then assert(count[kind]==1) end
  local lease = must(inspect:acquire({namespace="orders",owner="inspector",ttl_seconds=30,key=r.outbox_receipts[1].outbox_key}))
  local envelope = must(lease:read_json())
  assert(envelope.dispatch_state == (kind == "dead" and "dead_letter" or "retry_wait"), lc.encode_json(envelope))
  must(lease:release())
end
-- Saturate the one-frame key inbox with a transaction returning several keys.
local overflow = must(w:transaction(function(tx)
  for i=1,4 do local e,p=entry("overflow-"..i,"one"); must(tx:append_outbox(e,p)) end
end))
assert(#overflow.outbox_receipts == 4)
assert(w:stats().reconcile_wakes == 1)
-- Recovery is asynchronous upstream. Zero-wait owner probes allow its thread
-- to run without sleeps, timer expiry, or a namespace scan on the direct path.
for i=1,20000 do
  must(app:pump(0))
  local done=true; for j=1,4 do if count["overflow-"..j] ~= 1 then done=false end end
  if done then break end
end
for i=1,4 do assert(count["overflow-"..i] == 1, "overflow recovery missing job "..i) end
assert(not count.orphan and not count["poisoned-valid"] and not count["rollback-false"] and not count["rollback-error"])
-- Stopped scopes don't run effects; another workflow remains independent.
must(second:stop())
append(w2,"independent","inventory"); must(app:pump(0)); assert(count.independent==1)
must(second:start())
assert(w:stats().dispatcher_starts==1)
inspect:close()
must(app:stop())
-- Durable wake loss while stopped: raw producer commits with no local IPC.
local offline = must(lc.open(client_config)); local outbox = must(offline:new_outbox({namespace="orders"}))
local e,p=entry("restart","one"); local tx=must(outbox:append(e,p)); must(tx:commit()); tx:close(); outbox:close(); offline:close()
must(app:start())
assert(w:stats().dispatcher_starts == 2)
for i=1,20000 do must(app:pump(0)); if count.restart == 1 then break end end
assert(count.restart == 1, "startup reconciliation must recover committed wake loss")
must(app:stop()); app:close(); app:close()
assert(not pcall(first.state, first))
collectgarbage("collect")
-- Partial source materialization fails deterministically and remains closable.
local broken = must(v.app.new({app_name="broken-workflow", tls={mode="disabled"}}))
local bad_workflow = broken:workflow({namespace="broken", client={endpoints={"invalid://invalid"},logger_disabled=true}})
bad_workflow:outbox({kind="broken", handler=function() error("must not run") end})
assert(not broken:start())
broken:close(); broken:close()
print("supervisor workflow behavior ok")
