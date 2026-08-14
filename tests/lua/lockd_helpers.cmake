set(script "${WORK_DIR}/lockd-helpers.lua")

file(WRITE "${script}" [[
local opened_configs = {}
local enqueue_req
local enqueue_body
local closed_clients = 0
local acquired_req
local dequeued_req
local closed_leases = 0
local closed_messages = 0
local released_leases = 0
local acked_messages = 0
local saved_value
local loaded_value = { type = "account.loaded", id = "1001" }
local dequeued_value = { type = "order.created", id = "1001" }

package.loaded.vectis = {
  ERR_STATE = 3,
  status_string = function(status)
    return status == 3 and "state" or tostring(status)
  end,
  embedded_lockd_bundle_source = function()
    return nil, "no embedded lockd bundle"
  end,
}

package.loaded["vectis.status"] = {
  ERR_STATE = 3,
  ERROR_SOURCE_VECTIS = 1,
  ERROR_SOURCE_LOCKDC = 3,
  status_string = function(status)
    return status == 3 and "state" or tostring(status)
  end,
  error_source_string = function(source)
    if source == 1 then return "vectis" end
    if source == 3 then return "lockdc" end
    return tostring(source)
  end,
  decorate_error = function(err, defaults)
    defaults = defaults or {}
    if type(err) ~= "table" then
      err = { message = tostring(err or defaults.message or "vectis error") }
    end
    err.status = err.status or defaults.status
    err.status_string = err.status_string or
        package.loaded["vectis.status"].status_string(err.status)
    err.source_code = err.source_code or defaults.source_code
    err.source = err.source or
        package.loaded["vectis.status"].error_source_string(err.source_code)
    return err
  end,
  error = function(err)
    err.status_string = err.status_string or
        package.loaded["vectis.status"].status_string(err.status)
    err.source = err.source or
        package.loaded["vectis.status"].error_source_string(err.source_code)
    return err
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
    if config.fail_open then
      return nil, { message = "open failed" }
    end
    local client = {}
    function client:enqueue(req, body)
      if req.queue == "fail-enqueue" then
        return nil, { message = "enqueue failed" }
      end
      enqueue_req = req
      enqueue_body = body
      return true
    end
    function client:get_json()
      return loaded_value, { etag = "client-etag" }
    end
    function client:update_json()
      return true
    end
    function client:query_raw()
      return { matched = 1 }
    end
    function client:queue_ack()
      return true
    end
    function client:queue_nack()
      return true
    end
    function client:dequeue_batch()
      return {}
    end
    function client:dequeue_with_state(req)
      return self:dequeue(req)
    end
    function client:acquire(req)
      if req.key == "fail-acquire" then
        return nil, { message = "acquire failed" }
      end
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
        if req.key == "fail-release" then
          return nil, { message = "release failed" }
        end
        released_leases = released_leases + 1
        return true
      end
      function lease:mutate()
        return true
      end
      function lease:metadata()
        return { metadata = true }
      end
      function lease:remove()
        return true
      end
      function lease:attach()
        return true
      end
      function lease:get_attachment()
        return "attachment"
      end
      return lease
    end
    function client:dequeue(req)
      dequeued_req = req
      local message = {}
      function message:payload_json()
        if req.queue == "explode-payload" then
          error("payload exploded")
        end
        if req.queue == "missing-payload" then
          return nil, { message = "payload missing" }
        end
        return dequeued_value, 41
      end
      function message:ack()
        acked_messages = acked_messages + 1
        return true
      end
      function message:nack()
        return true
      end
      function message:extend()
        return true
      end
      function message:state()
        return nil
      end
      function message:close()
        closed_messages = closed_messages + 1
      end
      return message
    end
    function client:close()
      closed_clients = closed_clients + 1
    end
    return client
  end,
}

package.loaded["vectis.lockd"] = nil
local lockd = require("vectis.lockd")

local function assert_status_error(err, source_code, message)
  assert(type(err) == "table", "err is " .. type(err))
  assert(err.status == package.loaded["vectis.status"].ERR_STATE,
      "status=" .. tostring(err.status))
  assert(err.status_string == "state",
      "status_string=" .. tostring(err.status_string))
  assert(err.source_code == source_code,
      "source_code=" .. tostring(err.source_code))
  assert(err.source ==
      package.loaded["vectis.status"].error_source_string(source_code),
      "source=" .. tostring(err.source))
  assert(err.message == message,
      "message=" .. tostring(err.message))
end

assert(type(lockd.enqueue_json) == "function")
assert(type(lockd.with_acquired_lease) == "function")
assert(type(lockd.with_dequeued_json) == "function")

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

local open_failed, open_failed_err = lockd.open({ fail_open = true })
assert(open_failed == nil)
assert_status_error(open_failed_err,
    package.loaded["vectis.status"].ERROR_SOURCE_LOCKDC, "open failed")

local enqueue_failed, enqueue_failed_err = lockd.enqueue_json({}, {
  queue = "fail-enqueue",
}, {
  type = "order.created",
  id = "1001",
})
assert(enqueue_failed == nil)
assert_status_error(enqueue_failed_err,
    package.loaded["vectis.status"].ERROR_SOURCE_LOCKDC, "enqueue failed")
assert(closed_clients == 2)

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
assert(closed_clients == 3)

local loaded, loaded_meta = assert(lockd.load_json({
  endpoints = {"https://127.0.0.1:1"},
}, {
  key = "accounts/1001",
  owner = "vectis-lockd-load-test",
}))
assert(loaded.type == "account.loaded")
assert(loaded_meta.etag == "loaded-etag")
assert(released_leases == 2)
assert(closed_clients == 4)

local acquire_failed, acquire_failed_err = lockd.load_json({}, {
  key = "fail-acquire",
})
assert(acquire_failed == nil)
assert_status_error(acquire_failed_err,
    package.loaded["vectis.status"].ERROR_SOURCE_LOCKDC, "acquire failed")
assert(closed_clients == 5)

local release_failed, release_failed_err = lockd.load_json({}, {
  key = "fail-release",
})
assert(release_failed == nil)
assert_status_error(release_failed_err,
    package.loaded["vectis.status"].ERROR_SOURCE_LOCKDC, "release failed")
assert(closed_leases == 1)
assert(closed_clients == 6)

local update_exploded = pcall(function()
  lockd.save_json({}, { key = "explode-update" }, { type = "bad" })
end)
assert(update_exploded == false)
assert(closed_leases == 2)
assert(closed_clients == 7)

local load_exploded = pcall(function()
  lockd.load_json({}, { key = "explode-load" })
end)
assert(load_exploded == false)
assert(closed_leases == 3)
assert(closed_clients == 8)

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
assert(closed_leases == 4)
assert(closed_clients == 9)

local failed, failed_err = lockd.with_acquired_lease({}, { key = "boom" },
  function()
    return nil, { message = "handler failed" }
  end)
assert(failed == nil)
assert_status_error(failed_err,
    package.loaded["vectis.status"].ERROR_SOURCE_VECTIS, "handler failed")
assert(closed_leases == 5)
assert(closed_clients == 10)

local dequeue_result = assert(lockd.with_dequeued_json({
  endpoints = {"https://127.0.0.1:1"},
}, {
  queue = "orders",
  owner = "vectis-lockd-dequeue-test",
}, function(payload, message, client, payload_written)
  assert(payload.type == "order.created")
  assert(payload.id == "1001")
  assert(payload_written == 41)
  assert(type(client) == "table")
  assert(message:ack() == true)
  return "dequeue-ok"
end))
assert(dequeue_result == "dequeue-ok")
assert(dequeued_req.queue == "orders")
assert(acked_messages == 1)
assert(closed_messages == 1)
assert(closed_clients == 11)

local payload_missing, payload_missing_err = lockd.with_dequeued_json({}, {
  queue = "missing-payload",
}, function()
  error("missing payload handler should not run")
end)
assert(payload_missing == nil)
assert_status_error(payload_missing_err,
    package.loaded["vectis.status"].ERROR_SOURCE_LOCKDC, "payload missing")
assert(closed_messages == 2)
assert(closed_clients == 12)

local payload_exploded = pcall(function()
  lockd.with_dequeued_json({}, { queue = "explode-payload" }, function()
    error("payload explosion handler should not run")
  end)
end)
assert(payload_exploded == false)
assert(closed_messages == 3)
assert(closed_clients == 13)

local native_client = assert(lockd.native.open({}))
for _, method in ipairs({
  "acquire",
  "get_json",
  "update_json",
  "query_raw",
  "queue_ack",
  "queue_nack",
  "enqueue",
  "dequeue",
  "dequeue_batch",
  "dequeue_with_state",
}) do
  assert(type(native_client[method]) == "function", method)
end
local native_lease = assert(native_client:acquire({ key = "native-direct" }))
for _, method in ipairs({
  "get_json",
  "update_json",
  "mutate",
  "metadata",
  "remove",
  "attach",
  "get_attachment",
  "release",
}) do
  assert(type(native_lease[method]) == "function", method)
end
local native_message = assert(native_client:dequeue({ queue = "native-direct" }))
for _, method in ipairs({
  "payload_json",
  "ack",
  "nack",
  "extend",
  "state",
}) do
  assert(type(native_message[method]) == "function", method)
end
native_message:close()
native_lease:close()
native_client:close()

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
