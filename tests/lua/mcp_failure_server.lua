local vectis = require("vectis")
local cai = require("cai")
local json = require("lonejson")
assert(require("vectis.log").configure({ env = false,
  subs = { lockdc = false, cai = false, http = false } }))

local app = assert(vectis.app.new({
  bind = "127.0.0.1", port = assert(tonumber(arg[1])), worker_count = 1,
  supervision_policy = assert(arg[2]), access_log = false,
  tls = { mode = "disabled" },
}))
local tools = {}
local callbacks = {
  good = function(raw)
    if next(json.decode_value(raw)) ~= nil then
      return nil, "expected empty arguments"
    end
    return '{"ok":true}'
  end,
  throws = function() error("expected callback exception", 0) end,
  nil_result = function() return nil end,
  nil_message = function() return nil, "expected callback rejection" end,
  table_error = function() error({ reason = "expected non-string error" }) end,
  wrong_type = function() return {} end,
  invalid_json = function() return "{invalid" end,
  oversized = function() return '"' .. string.rep("x", 4096) .. '"' end,
  collecting = function()
    collectgarbage("collect")
    return '{"ok":true}'
  end,
}
for name, callback in pairs(callbacks) do
  tools[#tools + 1] = { name = name, schema_json =
    '{"type":"object","properties":{},"additionalProperties":false}',
    callback = callback }
end
assert(app:mcp({ path = "/mcp", tools = tools, request_max_bytes = 2048,
  tool_output_max_bytes = 1024, response_max_bytes = 8192 }))
io.stdout:write("PROTOCOL " .. cai.MCP_PROTOCOL_VERSION .. "\n")
io.stdout:flush()
assert(app:run())
app:close()
app:close()
print("mcp failure cleanup ok")
