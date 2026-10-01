local vectis = require("vectis")
local log = require("vectis.log")
local json = require("lonejson")
local policy = assert(arg[1])
local subs = {}

if policy == "all" then
  subs = { lockdc = false, cai = false, http = false }
elseif policy ~= "access" then
  subs[policy] = false
end
assert(log.configure({ env = false, subs = subs }))

for _ = 1, 16 do
  local app = assert(vectis.app.new({
    bind = "127.0.0.1",
    port = 39099,
    worker_count = 1,
    access_log = policy ~= "access",
    tls = { mode = "disabled" },
  }))
  local tools = {
    {
      name = "echo",
      schema_json = '{"type":"object","properties":{}}',
      callback = function() return "{}" end,
    },
  }
  local ok, result, err = pcall(function()
    return app:mcp({ tools = tools, enable_sessions = true })
  end)
  assert(ok and result == nil)
  assert(err.status == vectis.ERR_INVALID)
  assert(err.message:find("session persistence", 1, true))
  assert(json.encode_value({
    call_ok = ok,
    value = tostring(result),
    error = tostring(err),
  }))
  assert(app:mcp({ tools = tools }))
  app:close()
  app:close()
  app = nil
  collectgarbage("collect")
end

print("app log cleanup ok: " .. policy)
