local vectis = require("vectis")
local log = require("vectis.log")
local mask = assert(tonumber(arg[1]))
local checks = 0

assert(log.configure({ env = false, subs = {
  lockdc = mask & 1 == 0, cai = mask & 2 == 0, http = mask & 4 == 0,
} }))

local function config(extra)
  local value = {
    bind = "127.0.0.1", port = 39099, worker_count = 1,
    access_log = mask & 8 == 0, tls = { mode = "disabled" },
  }
  for key, entry in pairs(extra or {}) do value[key] = entry end
  return value
end

local function rejected(label, call, message, throws)
  local ok, value, err = pcall(call)
  if throws then
    assert(not ok and tostring(value):find(message, 1, true), label)
  else
    assert(ok and value == nil and type(err) == "table", label)
    assert(err.status == vectis.ERR_INVALID, label .. ": status")
    assert(err.message:find(message, 1, true), label .. ": " .. err.message)
  end
  checks = checks + 1
end

local invalid_configs = {
  { { port = 0 }, "app port", true },
  { { port = 65536 }, "app port", true },
  { { profile = "unknown" }, "app profile", true },
  { { supervision_policy = "unknown" }, "supervision_policy", true },
  { { service_failure_policy = "unknown" }, "service_failure_policy", true },
  { { quiescence_policy = "unknown" }, "quiescence_policy", true },
  { { worker_death_policy = "unknown" }, "worker_death_policy", true },
  { { client_ip = false }, "client_ip", true },
  { { tls = false }, "tls must be a table", true },
  { { tls = { mode = "unknown" } }, "tls.mode", true },
  { { tls = { version = "unknown" } }, "tls.version", true },
  { { tls = { port = 0 } }, "tls.port", true },
  { { shutdown_grace_ms = -1 }, "shutdown_grace_ms", false },
  { { worker_count = 65536 }, "worker_count", false },
}
for index, case in ipairs(invalid_configs) do
  rejected("constructor " .. index, function()
    return vectis.app.new(config(case[1]))
  end, case[2], case[3])
  collectgarbage("collect")
end

local function tool(callback)
  return { name = "echo", schema_json = '{"type":"object"}', callback = callback }
end

-- Each failure is retried on the same path to prove it did not register a
-- route, retain a callback, or poison the app's subsequent registrations.
local invalid_routes = {
  { function() return {} end, "tools must be a table" },
  { function() return { tools = {} } end, "tools must not be empty" },
  { function() return { tools = { false } } end, "entries must be tables" },
  { function() return { tools = {{ schema_json = "{}" }} } end, "name is required" },
  { function() return { tools = {{ name = "echo" }} } end, "schema_json is required" },
  { function() return { tools = { tool(false) } } end, "callback is required" },
  { function(callback)
      return { tools = { tool(callback), { name = "bad" } } }
    end, "schema_json is required" },
  { function(callback) return { tools = { tool(callback), false } } end,
    "entries must be tables" },
  { function(callback)
      return { tools = { tool(callback) }, enable_sessions = true }
    end, "session persistence" },
  { function(callback)
      return { tools = { tool(callback) }, methods = { "UNKNOWN" } }
    end, "method", true },
  { function(callback)
      return { path = "relative", tools = { tool(callback) } }
    end, "route path must start" },
}

local function collect()
  collectgarbage("collect")
  collectgarbage("collect")
end

for _, cleanup in ipairs({ "close", "gc", "unwind" }) do
  for index, case in ipairs(invalid_routes) do
    local weak = setmetatable({}, { __mode = "v" })
    local function exercise()
      local app = assert(vectis.app.new(config()))
      local label = cleanup .. "/route/" .. index
      do
        local token = {}
        weak.failed = token
        local opts = case[1](function() return token and "{}" end)
        opts.path = opts.path or "/retry"
        rejected(label, function() return app:mcp(opts) end, case[2], case[3])
        assert(app:route_count() == 0, label .. ": failed route registered")
      end
      collect()
      assert(weak.failed == nil, label .. ": rejected callback retained")
      do
        local token = {}
        weak.live = token
        assert(app:mcp({ path = "/retry", tools = {
          tool(function() return token and "{}" end),
        } }))
      end
      collect()
      assert(weak.live ~= nil, label .. ": live callback lost")
      assert(app:route_count() == 1)
      weak.app = app
      if cleanup == "close" then
        app:close()
        app:close()
        rejected(label .. "/closed", function() return app:mcp({}) end,
          "vectis app is closed", true)
        collect()
        assert(weak.live == nil, label .. ": closed callback retained")
      elseif cleanup == "unwind" then
        error("expected scope failure", 0)
      end
    end
    local ok, err = pcall(exercise)
    if cleanup == "unwind" then
      assert(not ok and err == "expected scope failure", tostring(err))
    else
      assert(ok, tostring(err))
    end
    collect()
    assert(weak.app == nil and weak.live == nil,
      cleanup .. "/route/" .. index .. ": finalizer retained app or callback")
    checks = checks + 1
  end
end

print("app failure modes ok: mask=" .. mask .. " checks=" .. checks)
