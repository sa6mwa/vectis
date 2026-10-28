-- Serial owner-state supervisors. The native app userdata remains the app.
local M = {}
local Scope = {}
Scope.__index = Scope
local states = setmetatable({}, { __mode = "k" })
local function copy(value)
  if type(value) ~= "table" then return value end
  local out = {}
  for k, v in pairs(value) do out[k] = copy(v) end
  return out
end
M.copy = copy
function M.check_options(opts, allowed, context)
  for key in pairs(opts) do assert(allowed[key], "unsupported " .. context .. " option: " .. tostring(key)) end
end
-- Translate the app's C configuration into the upstream Lua settings contract.
-- A workflow owns this snapshot; producer and dispatcher use identical copies.
function M.client_config(config)
  assert(config == nil or type(config) == "table", "client options must be a table")
  config = copy(config or {})
  M.check_options(config, {endpoints=true,unix_socket_path=true,default_namespace=true,namespace=true,timeout_ms=true,client_bundle=true,client_bundle_path=true,client_bundle_source=true,bundle=true,bundle_path=true,pouch=true,pouch_crypto_key=true,pouch_crypto_key_file=true,pouch_crypto_generate_key_file=true,pouch_compression=true,disable_mtls=true,insecure_skip_verify=true,prefer_http_2=true,disable_logger_sys_field=true,http_json_response_limit_bytes=true,logger_disabled=true}, "workflow client")
  if config.endpoints ~= nil then
    assert(type(config.endpoints)=="table" and #config.endpoints<=64, "endpoints must be a bounded array")
    for key,endpoint in pairs(config.endpoints) do
      assert(type(key)=="number" and key%1==0 and key>=1 and key<=#config.endpoints, "endpoints must be a dense array")
      assert(type(endpoint)=="string" and endpoint~="" and not endpoint:find("\0",1,true), "endpoints must contain nonempty NUL-free strings")
    end
    for index=1,#config.endpoints do assert(config.endpoints[index]~=nil,"endpoints must be a dense array") end
  end
  for _,name in ipairs({"unix_socket_path","default_namespace","namespace","client_bundle","bundle",
    "client_bundle_path","bundle_path","pouch_crypto_key","pouch_crypto_key_file","pouch_compression"}) do
    local value=config[name]
    assert(value==nil or (type(value)=="string" and not value:find("\0",1,true)),name.." must be a NUL-free string")
  end
  for _,name in ipairs({"pouch_crypto_generate_key_file","disable_mtls","insecure_skip_verify","prefer_http_2",
    "disable_logger_sys_field","logger_disabled"}) do
    assert(config[name]==nil or type(config[name])=="boolean",name.." must be boolean")
  end
  for _, name in ipairs({"client_bundle","bundle"}) do
    assert(config[name] == nil or (type(config[name]) == "string" and config[name] ~= ""), name .. " must be embedded or a nonempty bundle path")
  end
  if type(config.client_bundle_source)=="table" and config.client_bundle_source.path~=nil then
    local path=config.client_bundle_source.path
    assert(type(path)=="string" and not path:find("\0",1,true), "client bundle source path must be a NUL-free string")
  end
  assert(config.pouch == nil or type(config.pouch) == "table", "pouch settings must be a table")
  config.pouch = config.pouch or {}
  for name,value in pairs(config.pouch) do
    assert(type(value)~="string" or not value:find("\0",1,true), "pouch strings cannot contain NUL: "..tostring(name))
  end
  local legacy = { pouch_crypto_key = "crypto_key", pouch_crypto_key_file = "crypto_key_file",
                   pouch_crypto_generate_key_file = "crypto_generate_key_file", pouch_compression = "compression" }
  for old, new in pairs(legacy) do
    if config[old] ~= nil then
      if config.pouch[new] == nil then config.pouch[new] = config[old] end
      config[old] = nil
    end
  end
  -- Match native app precedence when both a canonical name and alias appear.
  if config.client_bundle_path == nil then config.client_bundle_path = config.bundle_path end
  config.bundle_path = nil
  if config.client_bundle_path then config.client_bundle_source = {path=config.client_bundle_path}; config.client_bundle_path = nil end
  if config.client_bundle == nil then config.client_bundle = config.bundle end
  config.bundle = nil
  local environment_key = os.getenv("VECTIS_POUCH_CRYPTO_KEY")
  if environment_key then
    assert(environment_key ~= "", "VECTIS_POUCH_CRYPTO_KEY must not be empty")
    config.pouch.crypto_key, config.pouch.crypto_key_file = environment_key, nil
    config.pouch.crypto_generate_key_file = false
  end
  return config
end

-- Shared canonical resolver for owner/producer workflows and queue sources.
-- Endpoint grammar remains exclusively owned by liblockdc.
function M.client_options(app, source)
  local config = M.client_config(source)
  local lockd = require("vectis.lockd")
  local need_endpoint = not config.unix_socket_path and #(config.endpoints or {}) == 0
  local pouch, query_crypto, query_writer = need_endpoint, false, false
  for _, endpoint in ipairs(config.endpoints or {}) do
    if endpoint:match("^pouch://") then
      pouch = true
      local key, err = lockd.pouch_endpoint_has_option(endpoint, "crypto_key")
      if key == nil then error(err.message) end
      local file, ferr = lockd.pouch_endpoint_has_option(endpoint, "crypto_key_file")
      if file == nil then error(ferr.message) end
      query_crypto = query_crypto or key or file
      local writer, werr=lockd.pouch_endpoint_has_option(endpoint,"single_writer")
      if writer==nil then error(werr.message) end
      query_writer=query_writer or writer
    end
  end
  local need_key = pouch and not query_crypto and not config.pouch.crypto_key and not config.pouch.crypto_key_file
  if need_endpoint or need_key then
    local endpoint, key_file = app:_supervisor_storage_defaults(need_endpoint, need_key)
    M.checked(endpoint, key_file)
    if need_endpoint then config.endpoints = {endpoint} end
    if need_key then config.pouch.crypto_key_file = key_file; config.pouch.crypto_generate_key_file = true end
  end
  -- App-owned Pouch roots have multiple native owner/producer clients.
  -- Selected typed fields override query values. An already-exclusive app
  -- root still cannot be shared by child clients, regardless of their policy.
  if pouch then
    assert(config.pouch.single_writer ~= true, "app-owned Pouch clients require shared writer mode (single_writer=false)")
    if config.pouch.single_writer == nil and not query_writer then config.pouch.single_writer = false end
  end
  return M.checked(lockd.config(config))
end

local function checked(value, err)
  if value == nil or value == false then
    error(type(err) == "table" and (err.message or "supervisor source failed") or tostring(err), 3)
  end
  return value
end
M.checked = checked
local function duration(value)
  if type(value) == "number" and value % 1 == 0 and value > 0 then return value end
  if type(value) == "string" then
    local n, unit = value:match("^(%d+)(%a+)$")
    local scale = { ms = 1, s = 1000, m = 60000, h = 3600000 }
    if n and scale[unit] and tonumber(n) > 0 then return tonumber(n) * scale[unit] end
  end
  error("timer every must be positive milliseconds or a duration such as 1s or 1h", 3)
end
local function declaring(scope)
  assert(scope.native:state() == "declared", "sources must be declared before app start")
end
local function label_id(scope, label)
  assert(type(label) == "string" and label ~= "" and not label:find("\0", 1, true), "kind must be a nonempty string")
  return assert(scope.ids[label], "undeclared channel kind: " .. label)
end
function M.declare(app, opts)
  local state = assert(states[app], "supervisor app is closed")
  opts = opts or {}
  assert(type(opts) == "table", "supervisor options must be a table")
  M.check_options(opts, {name=true,channel=true,start=true,lua_lanes=true,failure_policy=true,module=true,entry=true}, "supervisor")
  assert(opts.lua_lanes == nil or opts.lua_lanes == 1, "isolated Lua lanes are future work; serial lua_lanes=1 is supported")
  assert(opts.module == nil and opts.entry == nil, "module/entry lanes are future work")
  assert(opts.start == nil or type(opts.start) == "boolean", "supervisor start must be boolean")
  assert(opts.name == nil or type(opts.name) == "string", "supervisor name must be a string")
  local name = opts.name or "default"
  assert(type(name) == "string" and name ~= "" and not name:find("\0", 1, true), "supervisor name is required")
  assert(not state.scopes[name], "duplicate supervisor name: " .. name)
  assert(opts.failure_policy == nil or opts.failure_policy == "fail_app" or opts.failure_policy == "stop", "failure_policy must be fail_app or stop")
  local scope = setmetatable({ app = app, name = name, ids = {}, labels = {}, receivers = {}, timers = {}, sources = {}, next_kind = 1 }, Scope)
  local channel = opts.channel or {}
  M.check_options(channel, {capacity=true,max_frame_size=true,signal_kinds=true}, "channel")
  local config = { name = name, capacity = channel.capacity, max_frame_size = channel.max_frame_size,
                   start = opts.start, stop_on_failure = opts.failure_policy == "stop", signal_kinds = {} }
  if channel.signal_kinds ~= nil then
    assert(type(channel.signal_kinds) == "table", "signal_kinds must be an array")
    for key in pairs(channel.signal_kinds) do
      assert(type(key) == "number" and key % 1 == 0 and key >= 1 and key <= #channel.signal_kinds, "signal_kinds must be a dense array")
    end
  end
  for _, label in ipairs(channel.signal_kinds or {}) do
    assert(type(label) == "string" and label ~= "" and not label:find("\0", 1, true), "signal kind must be a nonempty string")
    assert(not scope.ids[label], "duplicate signal kind: " .. tostring(label))
    local id = scope.next_kind
    scope.next_kind = id + 1
    scope.ids[label], scope.labels[id] = id, label
    config.signal_kinds[#config.signal_kinds + 1] = id
  end
  scope.native = checked(app:_supervisor_native(config, function(event)
    if event.type == "start" then
      for _, source in ipairs(scope.sources) do if source.start then source:start() end end
    elseif event.type == "stop" then
      local failure
      for i = #scope.sources, 1, -1 do
        local source = scope.sources[i]
        if source.stop then
          local ok, err = pcall(source.stop, source, event.deadline_ms)
          if not ok and not failure then failure = err end
        end
      end
      if failure then error(failure) end
    elseif event.type == "probe" then
      for _, source in ipairs(scope.sources) do if source.probe then source:probe() end end
    elseif event.type == "timer" then
      local timer = scope.timers[event.kind]
      if timer and timer.enabled then
        local ok, result = pcall(timer.handler, timer)
        if not ok then error(result) end
        if result == false or result == "disable" then timer:disable() end
      end
    else
      local receiver = scope.receivers[event.kind] or scope.receivers[0]
      if receiver then
        event.kind = scope.labels[event.kind] or event.kind
        local result, err = receiver.handler(event)
        if result == false or (result == nil and err ~= nil) then error(err or "receiver rejected event") end
      end
    end
  end))
  state.scopes[name] = scope
  return scope
end
function Scope:channel()
  if self.endpoint then return self.endpoint end
  local scope = self
  self.endpoint = {
    send = function(_, kind, bytes, opts)
      assert(type(bytes) == "string", "channel send requires copied bytes")
      local id = label_id(scope, kind)
      return scope.native:send(id, bytes, opts)
    end,
    signal = function(_, kind)
      return scope.native:signal(label_id(scope, kind))
    end,
  }
  return self.endpoint
end
function Scope:receiver(opts)
  declaring(self)
  assert(type(opts) == "table" and type(opts.handler) == "function", "receiver requires a handler")
  M.check_options(opts, {handler=true,kind=true,name=true}, "receiver")
  assert(opts.kind == nil or opts.name == nil or opts.kind == opts.name, "receiver kind and name disagree")
  assert(opts.kind == nil or type(opts.kind) == "string", "receiver kind must be a string")
  assert(opts.name == nil or type(opts.name) == "string", "receiver name must be a string")
  local id = 0
  if opts.kind ~= nil or opts.name ~= nil then
    local label = opts.kind or opts.name
    assert(type(label) == "string" and label ~= "" and not label:find("\0", 1, true), "receiver kind must be a nonempty string")
    id = self.ids[label]
    if not id then
      id = self.next_kind
      self.next_kind = id + 1
      self.ids[label], self.labels[id] = id, label
    end
  end
  assert(not self.receivers[id], "duplicate receiver kind")
  local receiver = { handler = opts.handler, kind = opts.kind or opts.name }
  self.receivers[id] = receiver
  return receiver
end
function Scope:timer(opts)
  declaring(self)
  assert(type(opts) == "table" and type(opts.handler) == "function", "timer requires a handler")
  assert(type(opts.name) == "string" and opts.name ~= "" and not opts.name:find("\0", 1, true), "timer name must be a nonempty string")
  for _, existing in pairs(self.timers) do assert(existing.name ~= opts.name, "duplicate timer name") end
  local id = self.next_kind
  self.next_kind = id + 1
  M.check_options(opts, {name=true,handler=true,every=true,every_ms=true}, "timer")
  local timer = { name = opts.name, handler = opts.handler, enabled = true }
  local native = self.native
  function timer:disable() checked(native:timer_enable(id, false)); self.enabled = false end
  function timer:enable() checked(native:timer_enable(id, true)); self.enabled = true end
  checked(self.native:timer(id, duration(opts.every or opts.every_ms)))
  self.timers[id] = timer
  return timer
end
-- Opt-in owner safe point for a process-global SUS sink. It installs no sink
-- and never routes logging implicitly between apps.
function Scope:sus_logs(opts)
  opts = opts or {}
  assert(type(opts)=="table", "SUS log source options must be a table")
  M.check_options(opts, {name=true,every_ms=true,max=true}, "SUS log source")
  assert(opts.max == nil or (type(opts.max) == "number" and opts.max % 1 == 0 and opts.max >= 0 and opts.max <= 64), "SUS log source max must be between 0 and 64")
  return self:timer({name=opts.name or "sus-logs", every_ms=opts.every_ms or 50,
    handler=function() require("sus").pump_logs(opts.max or 64) end})
end
function Scope:start() self.app:_workflow_idle(); return self.native:start() end
function Scope:stop(ms) self.app:_workflow_idle(); return self.native:stop(ms or 0) end
function Scope:wait()
  if self:state() == "stopped" then return true end
  return nil, "stop serial supervisor before wait"
end
function Scope:state() return self.native:state() end
function Scope:last_error() return self.native:last_error() end
function Scope:outbox(opts)
  assert(type(opts) == "table" and opts.workflow, "outbox requires a workflow")
  return opts.workflow:_register(self, opts)
end
-- Queue pull uses public threadless dequeue with a zero-wait request, one
-- handler per timer tick. No dependency consumer thread calls into Lua.
function Scope:consumer(opts)
  declaring(self)
  assert(type(opts) == "table" and type(opts.handler) == "function" and type(opts.request) == "table", "consumer requires request and handler")
  M.check_options(opts, {handler=true,request=true,client=true,name=true,every_ms=true}, "consumer")
  M.check_options(opts.request, {namespace=true,queue=true,owner=true,txn_id=true,
    start_after=true,visibility_timeout_seconds=true,wait_seconds=true,page_size=true}, "consumer request")
  assert(opts.client == nil or type(opts.client) == "table", "consumer client must be a table")
  assert(opts.request.wait_seconds == nil or opts.request.wait_seconds == 0, "consumer wait_seconds must be zero")
  assert(opts.request.page_size == nil or opts.request.page_size == 1, "consumer page_size must be one")
  assert(opts.name == nil or type(opts.name)=="string", "consumer name must be a string")
  local source = { failures = 0, app = self.app, config = M.client_config(opts.client or M.state(self.app).lockd), request = copy(opts.request) }
  source.request.wait_seconds = 0
  source.request.page_size = 1
  local client
  function source:start() client = checked(self.app:_open_lockdc_client(M.client_options(self.app, self.config))) end
  function source:stop()
    local closing=client; client=nil
    if closing then
      local ok,err=pcall(closing.close,closing)
      -- At VM teardown upstream userdata may finalize before its owning source.
      if not ok and not (type(err)=="string" and err:find("lockdc client is closed",1,true)) then error(err,0) end
    end
  end
  local function consume()
    local message, err = require("vectis.lockd")._dequeue_one(client, source.request)
    if not message then
      if err and err.code ~= require("lockdc").ERR_TIMEOUT then error(err.message) end
      return
    end
    local active, outcome = true, nil
    local delivery = {}
    local function live() assert(active, "queue delivery is outside its handler lifetime") end
    function delivery:info() live(); return message:info() end
    function delivery:write_payload(sink)
      live(); local value, written = message:write_payload(sink)
      if value == nil and type(written) == "number" then return true, written end
      return value, written
    end
    function delivery:extend(req) live(); return message:extend(req) end
    local function select(action, request)
      live(); assert(not outcome, "queue handler selected more than one outcome")
      outcome = { action = action, request = request }; return outcome
    end
    function delivery:ack() return select("ack") end
    function delivery:nack(req)
      assert(req == nil or type(req) == "table", "nack request must be a table")
      M.check_options(req or {}, {delay_seconds=true,intent=true,last_error_json=true}, "consumer nack")
      return select("nack", req)
    end
    local ok, result, handler_err = pcall(opts.handler, delivery)
    active = false
    local terminal_ok, terminal, terminal_err = pcall(function()
      -- Batch handles remain batch-owned after message:ack/nack, but the
      -- upstream Lua methods discard their pointer. Public client terminal
      -- operations preserve our handle so close() can release it afterwards.
      if ok and handler_err == nil and result == outcome and outcome then
        if outcome.action == "ack" then return message:ack() end
        local request = outcome.request or {}
        return message:nack({delay_seconds=request.delay_seconds,
          intent=request.intent, last_error_json=request.last_error_json})
      end
      local fault = not ok and result or handler_err or "queue handler returned no selected outcome"
      local diagnostic = type(fault)=="table" and rawget(fault,"message") or fault
      if type(diagnostic)~="string" then diagnostic="queue handler failed" end
      diagnostic=diagnostic:sub(1,512)
      source.failures=source.failures+1
      source.last_error={message=diagnostic}
      return message:nack({intent="failure",last_error_json=require("lockdc").encode_json(source.last_error)})
    end)
    message:close()
    if not terminal_ok then error(terminal) end
    checked(terminal, terminal_err)
  end
  function source.probe() return source.app:_workflow_call(consume) end
  function source:stats() return {failures=self.failures,last_error=self.last_error and copy(self.last_error)} end
  self:timer({ name = opts.name or ("consumer:" .. tostring(#self.sources + 1)), every_ms = opts.every_ms or 100, handler = source.probe })
  self.sources[#self.sources + 1] = source
  return source
end
function M.state(app) return assert(states[app], "supervisor app is closed") end
function M.default(app)
  return M.state(app).scopes.default or M.declare(app, { name = "default" })
end
function M.attach(app, opts)
  local state = { scopes = {}, workflows = {}, lockd = copy(opts.lockd or {}) }
  states[app] = state
  local methods = getmetatable(app).__index
  methods.set_pouch_settings = function(self, settings)
    local state = M.state(self)
    assert(next(state.scopes) == nil and next(state.workflows) == nil,
      "Pouch settings must be captured before supervisor/workflow declarations")
    local captured = copy(settings)
    local ok, err = self:_set_pouch_settings_native(captured)
    if ok == nil then return nil, err end
    state.lockd.pouch = captured
    return true
  end
  methods.supervisor = M.declare
  methods.workflow = function(self, config) return require("vectis.workflow").declare(self, config) end
  methods.storage = function(self, config) return require("vectis.storage").declare(self, config) end
  methods.ipc = function(self, name)
    if name == nil then return M.default(self):channel() end
    return assert(M.state(self).scopes[name], "unknown supervisor channel: " .. tostring(name)):channel()
  end
end
return M
