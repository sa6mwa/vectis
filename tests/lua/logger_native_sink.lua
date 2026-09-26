local vectis = require("vectis")
local log = require("vectis.log")
local pslog = require("pslog")
local acquire_seed = log._acquire_seed

local function handoff(logger, expected_native)
  log._acquire_seed = function() return logger, {} end
  local app, err = vectis.app.new({
    bind = "127.0.0.1",
    port = 8080,
    tls = { mode = "disabled" },
  })
  if expected_native then
    assert(app, err and err.message)
    app:close()
  else
    assert(app == nil, "Lua-bound logger was accepted for native services")
    assert(err.status == vectis.ERR_INVALID)
    assert(err.message:find("requires a native pslog sink", 1, true))
  end
  collectgarbage("collect")
end

local writes = 0
local callback = assert(log.new({output = function() writes = writes + 1 end}))
handoff(callback, false)
local scoped_callback = callback:with("component", "fixture")
handoff(scoped_callback, false)
handoff(scoped_callback:with_level("error"), false)
handoff(scoped_callback:with_level_field(), false)
assert(writes == 0, "native handoff validation invoked a Lua sink")
callback:info("Lua-only logging still works")
assert(writes > 0)
scoped_callback:close()
callback:close()

local file = assert(io.tmpfile())
local native = assert(pslog.new_json({output = file}))
handoff(native, true)
local scoped_native = native:with("component", "fixture")
handoff(scoped_native, true)
handoff(scoped_native:with_level("error"), true)
handoff(scoped_native:with_level_field(), true)

-- A derived native logger remains qualified after its temporary root dies.
local derived
local co = coroutine.create(function()
  local root = assert(pslog.new_json({output = file}))
  derived = root:with("owner", "coroutine")
end)
assert(coroutine.resume(co))
co = nil
collectgarbage("collect")
handoff(derived, true)
derived:info("native sink survived parent collection")
assert(file:flush())
assert(file:seek("set", 0))
assert(file:read("*a"):find("native sink survived parent collection", 1, true))
derived:close()
scoped_native:close()
native:close()
log._acquire_seed = acquire_seed
collectgarbage("collect")
assert(file:close())
