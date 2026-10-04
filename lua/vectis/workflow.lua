-- One threadless producer configuration and one persistent host dispatcher.
local supervisor = require("vectis.supervisor")
local lockd = require("vectis.lockd")
local lockdc = require("lockdc")
local M, Workflow = {}, {}
Workflow.__index = Workflow
local checked, copy = supervisor.checked, supervisor.copy
local private = setmetatable({}, { __mode = "k" })
local config_keys = {
  "owner", "transaction_ttl_seconds", "claim_ttl_seconds", "max_attempts",
  "notification_capacity", "retry_initial_delay_seconds", "retry_max_delay_seconds",
  "host_retry_delay_max_seconds", "recovery_interval_seconds", "shutdown_timeout_ms",
  "replay_dead_letters_on_startup", "dead_letter_retention_seconds",
  "dead_letter_max_count", "dead_letter_max_bytes",
}
local function state(self) return assert(private[self], "workflow is closed") end
local function close_resources(w, ms)
  local failure
  local function capture(ok, result, err)
    if not ok or result == nil or result == false then
      local value = ok and err or result
      if type(value) == "string" and value:find("lockdc outbox dispatcher is closed",1,true) then return end
      failure = failure or value or "workflow cleanup failed"
    end
  end
  local function already_closed(value, name)
    local expected={dispatcher="lockdc outbox dispatcher is closed",
      worker_outbox="lockdc outbox is closed",outbox="lockdc outbox is closed",
      worker_client="lockdc client is closed",client="lockdc client is closed"}
    return type(value)=="string" and value:find(expected[name],1,true)
  end
  if w.dispatcher then capture(pcall(w.dispatcher.stop,w.dispatcher,ms or 0)) end
  for _, name in ipairs({"dispatcher","worker_outbox","outbox","worker_client","client"}) do
    local handle=w[name]; w[name]=nil
    if handle then
      local ok,err=pcall(handle.close,handle)
      if not ok and not already_closed(err,name) then failure=failure or err end
    end
  end
  if failure then error(type(failure)=="table" and failure.message or tostring(failure)) end
end
local function client_options(w)
  local domain = w.scope.native:domain()
  assert(domain == "producer" or domain == "owner", "workflow clients materialize only after app start/fork")
  return supervisor.client_options(w.app, w.client_config)
end
local function producer(w)
  local config = client_options(w)
  if not w.client then
    w.client = checked(w.app:_open_lockdc_client(config))
    local outbox, err = w.client:new_outbox(copy(w.config))
    if not outbox then w.client:close(); w.client = nil; checked(outbox, err) end
    w.outbox = outbox
  end
  return w.outbox
end
local function job_adapter(w, registration, raw)
  local active, chosen = true, nil
  local info = raw:info()
  local job = copy(info)
  local function live() assert(active, "outbox job is outside its handler lifetime") end
  local function outcome(action, value)
    live(); assert(not chosen, "outbox handler selected more than one outcome")
    chosen = { action = action, value = value }; return chosen
  end
  function job:complete(value) return outcome("complete", value) end
  function job:retry(value)
    if type(value) == "string" then value = { diagnostic = value } end
    return outcome("retry", value)
  end
  function job:dead_letter(diagnostic) return outcome("dead_letter", diagnostic) end
  function job:write_payload(sink)
    live()
    local value, written = raw:write_payload(sink)
    if value == nil and type(written) == "number" then return true, written end
    return value, written
  end
  function job:renew(ttl)
    live()
    assert(not w.with_state, "renewed stateful completion is blocked by liblockdc 0.18.0 Pouch version preconditions")
    return raw:renew(ttl or w.config.claim_ttl_seconds or 300)
  end
  function job:state()
    live()
    local lease = raw:state()
    if not lease then return nil end
    return setmetatable({}, { __metatable = "workflow checkpoint", __index = function(_, name)
      live()
      assert(type(name)=="string" and not name:match("^_"), "checkpoint exposes public operations only")
      assert(name ~= "close" and name ~= "release", "checkpoint lease is owned by the handler")
      local method = lease[name]
      if type(method) == "function" then return function(_, ...) live(); return method(lease, ...) end end
      assert(method == nil, "checkpoint exposes operations only")
      return nil
    end })
  end
  local ok, result = pcall(registration.handler, job)
  active = false
  if not ok then error(result) end -- upstream retries thrown errors, bounded diagnostic
  if chosen and result == chosen then
    if chosen.action == "complete" then checked(raw:complete(chosen.value))
    elseif chosen.action == "retry" then checked(raw:retry(chosen.value))
    else checked(raw:dead_letter(chosen.value)) end
  end -- no outcome: dependency pump automatically retries
end
local function pump(w)
  if not w.dispatcher then return end
  -- The upstream pull has no kind filter. Pause intake when any selected
  -- scope is stopped rather than claiming jobs with no runnable owner.
  for _, registration in pairs(w.registrations) do
    if registration.scope:state() ~= "running" then return end
  end
  local count, err = w.dispatcher:pump({ timeout_ms = 0, max_jobs = 1, with_state = w.with_state, handlers = w.handlers })
  checked(count, err)
  w.evidence.handled = w.evidence.handled + count
  if count == 1 then checked(w.scope.native:schedule()) end
end
function M.declare(app, opts)
  opts = opts or {}
  local allowed = {namespace=true,client=true,shared_runtime=true,with_state=true,
    recovery_probe_ms=true,ipc_capacity=true,ipc_max_frame_size=true}
  for _, key in ipairs(config_keys) do allowed[key] = true end
  supervisor.check_options(opts, allowed, "workflow")
  assert(opts.client == nil or type(opts.client) == "table", "workflow client must be a table")
  local app_state = supervisor.state(app)
  assert(opts.shared_runtime == nil or type(opts.shared_runtime) == "boolean", "workflow shared_runtime must be boolean")
  local namespace = opts.namespace
  assert(type(namespace) == "string" and namespace ~= "" and not namespace:find("\0", 1, true), "workflow namespace is required")
  assert(not app_state.workflows[namespace], "duplicate workflow identity: " .. namespace)
  assert(opts.shared_runtime ~= true, "cross-runtime leadership is a future Vectis mode; shared_runtime is currently unsupported")
  assert(opts.with_state == nil or type(opts.with_state) == "boolean", "workflow with_state must be boolean")
  local probe_ms = opts.recovery_probe_ms or 100
  assert(type(probe_ms) == "number" and probe_ms % 1 == 0 and probe_ms > 0 and probe_ms <= 2147483647,
    "workflow recovery_probe_ms must be positive bounded milliseconds")
  local object = setmetatable({}, Workflow)
  local w = { app = app, with_state = opts.with_state == true, config = { namespace = namespace }, client_config = supervisor.client_config(opts.client or app_state.lockd),
              registrations = {}, handlers = {}, evidence = { forwarded_keys = 0, reconcile_wakes = 0, closed_wakes = 0, failed_wakes = 0, received_keys = 0, received_reconciles = 0, handled = 0, dispatcher_starts = 0 } }
  for _, key in ipairs(config_keys) do w.config[key] = opts[key] end
  if w.config.recovery_interval_seconds == nil then w.config.recovery_interval_seconds = 30 end
  -- This is a host-owned workflow key inbox, separate from each public scope.
  w.scope = supervisor.declare(app, { name = "@workflow:" .. namespace,
    channel = { capacity = opts.ipc_capacity or 64, max_frame_size = opts.ipc_max_frame_size or 4096,
                signal_kinds = { "reconcile" } } })
  w.scope:receiver({ handler = function(event)
    if event.type == "message" then
      w.evidence.received_keys = w.evidence.received_keys + 1
      checked(w.dispatcher:notify_outbox_key(event.payload))
    elseif event.kind == "reconcile" then
      w.evidence.received_reconciles = w.evidence.received_reconciles + 1
      checked(w.dispatcher:reconcile())
    end
    pump(w)
  end })
  w.channel = w.scope:channel()
  local source = {}
  function source:start()
    w.worker_client = checked(w.app:_open_lockdc_client(client_options(w)))
    w.worker_outbox = checked(w.worker_client:new_outbox(copy(w.config)))
    w.dispatcher = checked(w.worker_outbox:dispatcher())
    w.evidence.dispatcher_starts = w.evidence.dispatcher_starts + 1
    checked(w.dispatcher:reconcile()) -- durable startup recovery, never the direct-key fast path
  end
  function source:stop(ms) close_resources(w, ms) end
  function source:probe() pump(w) end
  w.scope.sources[#w.scope.sources + 1] = source
  -- Upstream asynchronous recovery/retry notifications have no public FD.
  -- This probe consumes already indexed candidates, never scans a namespace.
  w.scope:timer({ name = "recovery-ready", every_ms = probe_ms,
                  handler = function() pump(w) end })
  private[object] = w
  app_state.workflows[namespace] = object
  return object
end
function Workflow:_register(scope, opts)
  supervisor.check_options(opts, {kind=true,handler=true,workflow=true,supervisor=true}, "outbox")
  local w = state(self)
  assert(scope.app == w.app, "workflow and supervisor must belong to the same app")
  assert(w.scope:state() == "declared" and scope:state() == "declared", "outbox handlers must be declared before app start")
  assert(type(opts.kind) == "string" and opts.kind ~= "" and not opts.kind:find("\0", 1, true), "outbox kind is required")
  assert(type(opts.handler) == "function", "outbox handler is required")
  assert(not w.registrations[opts.kind], "duplicate workflow/kind handler: " .. opts.kind)
  local registration = { scope = scope, handler = opts.handler }
  w.registrations[opts.kind] = registration
  w.handlers[opts.kind] = function(raw) job_adapter(w, registration, raw) end
  return self
end
function Workflow:outbox(opts)
  return self:_register(opts.supervisor or supervisor.default(state(self).app), opts)
end
function Workflow:transaction(handler)
  assert(type(handler) == "function", "workflow transaction requires a callback")
  local w = state(self)
  return w.app:_workflow_call(function()
    local result, err = producer(w):transaction(function(raw)
      local failed, active = false, true
      local tx = {}
      function tx:append_outbox(entry, payload)
        assert(active, "transaction is outside its callback lifetime")
        failed = true -- remains rollback-only even when a rejected append is caught
        assert(type(entry) == "table" and w.registrations[entry.kind], "unregistered Vectis outbox kind")
        local receipt, append_err = raw:append(entry, payload)
        if not receipt then error(append_err.message or tostring(append_err)) end
        failed = false
        return receipt
      end
      local poisoned = false
      local append = tx.append_outbox
      tx.append_outbox = function(self, ...)
        local ok, value = pcall(append, self, ...)
        if not ok then poisoned = true; error(value) end
        return value
      end
      setmetatable(tx, { __index = function(_, name)
        assert(name ~= "commit" and name ~= "rollback" and name ~= "close" and name ~= "append" and name ~= "_core", "transaction lifecycle belongs to workflow")
        local method = raw[name]
        if type(method) == "function" then return function(_, ...) assert(active, "transaction is outside its callback lifetime"); return method(raw, ...) end end
      end })
      local ok, value, callback_error = pcall(handler, tx)
      active = false
      if not ok then error(value) end
      if value == false or (value == nil and callback_error ~= nil) then
        error(callback_error or "workflow transaction rejected", 0)
      end
      assert(not poisoned and not failed, "workflow transaction is rollback-only after rejected append")
      return value
    end)
    if not result then return nil, err end
    -- Receipt forwarding is automatic and never changes a successful commit.
    for _, receipt in ipairs(result.outbox_receipts or {}) do
      local ok, send_err = w.scope.native:send(0, receipt.outbox_key)
      if ok then w.evidence.forwarded_keys = w.evidence.forwarded_keys + 1
      elseif send_err ~= "closed" then
        if send_err ~= "full" then w.evidence.failed_wakes = w.evidence.failed_wakes + 1 end
        local signaled = w.channel:signal("reconcile")
        w.evidence.reconcile_wakes = w.evidence.reconcile_wakes + 1
        if not signaled then w.evidence.closed_wakes = w.evidence.closed_wakes + 1 end
        break
      else w.evidence.closed_wakes = w.evidence.closed_wakes + 1; break end
    end
    return result
  end)
end
function Workflow:stats()
  local w = state(self)
  local stats = copy(w.evidence)
  if w.dispatcher then stats.dispatcher = checked(w.dispatcher:stats()) end
  return stats
end
function Workflow:reconcile()
  return state(self).channel:signal("reconcile")
end
return M
