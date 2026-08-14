set(script "${WORK_DIR}/lockd-helpers.lua")

file(WRITE "${script}" [[
local opened_configs = {}
local enqueue_req
local enqueue_body
local closed_clients = 0
local acquired_req
local closed_leases = 0
local released_leases = 0
local saved_value
local loaded_value = { type = "account.loaded", id = "1001" }

package.loaded.vectis = {
  ERR_STATE = 3,
  status_string = function(status)
    return status == 3 and "state" or tostring(status)
  end,
  embedded_lockd_bundle_source = function()
    return nil, "no embedded lockd bundle"
  end,
}

package.loaded.lockdc = {
  json_null = {},
  encode_json = function(value)
    if value == nil then
      return "null"
    end
    return '{"type":"' .. value.type .. '","id":"' .. value.id .. '"}'
  end,
  decode_json = function(payload)
    return { payload = payload }
  end,
  open = function(config)
    opened_configs[#opened_configs + 1] = config
    local client = {}
    function client:enqueue(req, body)
      enqueue_req = req
      enqueue_body = body
      return true
    end
    function client:acquire(req)
      acquired_req = req
      local lease = {}
      function lease:get_json()
        if req.key == "explode-load" then
          error("load exploded")
        end
        return loaded_value, { etag = "loaded-etag" }
      end
      function lease:close()
        closed_leases = closed_leases + 1
      end
      function lease:update_json(value)
        if req.key == "explode-update" then
          error("update exploded")
        end
        saved_value = value
        return true
      end
      function lease:release()
        released_leases = released_leases + 1
        return true
      end
      return lease
    end
    function client:close()
      closed_clients = closed_clients + 1
    end
    return client
  end,
}

package.loaded["vectis.lockd"] = nil
local lockd = require("vectis.lockd")

assert(type(lockd.enqueue_json) == "function")
assert(type(lockd.with_acquired_lease) == "function")

assert(lockd.enqueue_json({
  endpoints = {"https://127.0.0.1:1"},
  namespace = "helpers",
  client_bundle = "/tmp/client.pem",
}, {
  queue = "orders",
}, {
  type = "order.created",
  id = "1001",
}) == true)
assert(opened_configs[1].default_namespace == "helpers")
assert(opened_configs[1].namespace == nil)
assert(opened_configs[1].client_bundle == nil)
assert(opened_configs[1].client_bundle_path == "/tmp/client.pem")
assert(enqueue_req.queue == "orders")
assert(enqueue_req.content_type == "application/json")
assert(enqueue_body == '{"type":"order.created","id":"1001"}')
assert(closed_clients == 1)

local saved = assert(lockd.save_json({
  endpoints = {"https://127.0.0.1:1"},
}, {
  key = "accounts/1001",
  owner = "vectis-lockd-save-test",
  ttl_seconds = 30,
}, {
  type = "account.saved",
  id = "1001",
}))
assert(saved == true)
assert(saved_value.type == "account.saved")
assert(released_leases == 1)
assert(closed_clients == 2)

local loaded, loaded_meta = assert(lockd.load_json({
  endpoints = {"https://127.0.0.1:1"},
}, {
  key = "accounts/1001",
  owner = "vectis-lockd-load-test",
}))
assert(loaded.type == "account.loaded")
assert(loaded_meta.etag == "loaded-etag")
assert(released_leases == 2)
assert(closed_clients == 3)

local update_exploded = pcall(function()
  lockd.save_json({}, { key = "explode-update" }, { type = "bad" })
end)
assert(update_exploded == false)
assert(closed_leases == 1)
assert(closed_clients == 4)

local load_exploded = pcall(function()
  lockd.load_json({}, { key = "explode-load" })
end)
assert(load_exploded == false)
assert(closed_leases == 2)
assert(closed_clients == 5)

local handler_seen_client
local lease_result = assert(lockd.with_acquired_lease({
  endpoints = {"https://127.0.0.1:1"},
}, {
  key = "accounts/1001",
  owner = "vectis-lockd-helper-test",
}, function(lease, client)
  handler_seen_client = client
  assert(lease:update_json({ type = "account.updated", id = "1001" }) == true)
  return "lease-ok"
end))
assert(lease_result == "lease-ok")
assert(acquired_req.key == "accounts/1001")
assert(type(handler_seen_client) == "table")
assert(closed_leases == 3)
assert(closed_clients == 6)

local failed, failed_err = lockd.with_acquired_lease({}, { key = "boom" },
  function()
    return nil, { message = "handler failed" }
  end)
assert(failed == nil)
assert(failed_err.message == "handler failed")
assert(closed_leases == 4)
assert(closed_clients == 7)

print("vectis-lockd-helpers-ok")
]])

execute_process(COMMAND "${VECTIS_BIN}" "${script}"
                RESULT_VARIABLE lockd_helpers_result
                OUTPUT_VARIABLE lockd_helpers_stdout
                ERROR_VARIABLE lockd_helpers_stderr)
if(NOT lockd_helpers_result EQUAL 0)
  message(FATAL_ERROR
          "vectis lockd helper test failed: ${lockd_helpers_stdout}${lockd_helpers_stderr}")
endif()
if(NOT lockd_helpers_stdout MATCHES "vectis-lockd-helpers-ok")
  message(FATAL_ERROR
          "vectis lockd helper test did not report success: ${lockd_helpers_stdout}${lockd_helpers_stderr}")
endif()
