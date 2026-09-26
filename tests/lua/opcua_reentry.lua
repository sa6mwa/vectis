local opcua = require("opcua")
local node = opcua.node_id_numeric(1, 7101)
local endpoint = OPCUA_ENDPOINT

local function check_busy(client)
  for _, method in ipairs({"close", "disconnect", "connect", "iterate", "__gc"}) do
    local fn = method == "__gc" and getmetatable(client).__gc or client[method]
    local ok, err = pcall(fn, client, endpoint)
    assert(not ok and tostring(err):find("opcua client is busy", 1, true))
  end
end

-- Satisfy the existing live fixture's observable method-call contract.
local client = assert(opcua.connect(endpoint))
local object = opcua.node_id_numeric(1, 7102)
local method = opcua.node_id_numeric(1, 7104)
for i = 1, 2 do
  local value, err = client:call_method(object, method, {})
  assert(value == nil and err.result == opcua.ERR_RANGE)
end

local calls = 0
assert(client:read_async(node, function(result)
  assert(result.ok)
  calls = calls + 1
  check_busy(client)
  collectgarbage("collect")
  assert(client:read_async(node, function(next_result)
    assert(next_result.ok)
    check_busy(client)
    calls = calls + 1
  end))
end))
for i = 1, 100 do
  assert(client:iterate(10))
  if calls == 2 then break end
end
assert(calls == 2)
assert(client:read(node))

local called = false
assert(client:read_async(node, function()
  called = true
  client:close() -- Uncaught misuse becomes the existing structured callback error.
end))
local observed = false
for i = 1, 100 do
  local ok, err = client:iterate(10)
  if not ok then
    assert(err.result == opcua.ERR_CALLBACK)
    assert(err.message:find("opcua client is busy", 1, true))
    observed = true
    break
  end
end
assert(called and observed)
assert(client:iterate(0))
assert(client:close())
assert(client:close())

local closing, callback_calls
callback_calls = 0
local co = coroutine.create(function()
  closing = assert(opcua.connect(endpoint))
  assert(closing:read_async(node, function()
    callback_calls = callback_calls + 1
    check_busy(closing)
    for _, fn in ipairs({closing.read, closing.read_async}) do
      local ok, err = pcall(fn, closing, node, function() end)
      assert(not ok and tostring(err):find("opcua client is busy closing", 1, true))
    end
    collectgarbage("collect")
  end))
end)
assert(coroutine.resume(co))
co = nil
collectgarbage("collect")
assert(closing:close())
assert(callback_calls == 1)
assert(closing:close())
closing = nil
collectgarbage("collect")
