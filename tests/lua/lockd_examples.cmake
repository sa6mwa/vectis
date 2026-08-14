set(script "${WORK_DIR}/vectis-lockd-examples.lua")

file(WRITE "${script}" [=[
local lonejson = require("lonejson")

local opened_configs = {}
local closed_clients = 0
local released_leases = 0
local closed_leases = 0
local closed_messages = 0
local acked_messages = 0
local state_by_key = {}
local queued_payload
local queued_req
local dequeued_req

package.loaded.lockdc = {
  json_null = lonejson.json_null,
  encode_json = lonejson.encode_json,
  decode_json = lonejson.decode_json,
  open = function(config)
    opened_configs[#opened_configs + 1] = config
    local client = {}
    function client:enqueue(req, body)
      queued_req = req
      queued_payload = body
      return true
    end
    function client:dequeue(req)
      dequeued_req = req
      local message = {}
      function message:payload_json()
        return lonejson.decode_json(queued_payload), #queued_payload
      end
      function message:ack()
        acked_messages = acked_messages + 1
        return true
      end
      function message:close()
        closed_messages = closed_messages + 1
      end
      return message
    end
    function client:acquire(req)
      local lease = {}
      function lease:update_json(value)
        state_by_key[req.key] = value
        return true
      end
      function lease:get_json()
        return state_by_key[req.key], { etag = "stub-etag" }
      end
      function lease:release()
        released_leases = released_leases + 1
        return true
      end
      function lease:close()
        closed_leases = closed_leases + 1
      end
      return lease
    end
    function client:close()
      closed_clients = closed_clients + 1
    end
    return client
  end,
}

dofile(assert(arg[1], "lockd_state.lua path is required"))
dofile(assert(arg[2], "lockd_queue.lua path is required"))

assert(#opened_configs == 4)
for _, config in ipairs(opened_configs) do
  assert(config.default_namespace == "examples")
  assert(config.namespace == nil)
  assert(config.disable_mtls == true)
  assert(config.insecure_skip_verify == true)
end
assert(closed_clients == 4)
assert(released_leases == 2)
assert(closed_leases == 0)
assert(queued_req.queue == "lua-orders")
assert(queued_req.content_type == "application/json")
assert(lonejson.decode_json(queued_payload).type == "order.created")
assert(dequeued_req.owner == "vectis-lua-lockd-queue-example")
assert(acked_messages == 1)
assert(closed_messages == 1)

print("vectis-lockd-examples-ok")
]=])

execute_process(
  COMMAND "${VECTIS_BIN}" "${script}"
          "${VECTIS_SOURCE_DIR}/examples/lua/lockd_state.lua"
          "${VECTIS_SOURCE_DIR}/examples/lua/lockd_queue.lua"
  RESULT_VARIABLE lockd_examples_result
  OUTPUT_VARIABLE lockd_examples_stdout
  ERROR_VARIABLE lockd_examples_stderr)
if(NOT lockd_examples_result EQUAL 0)
  message(FATAL_ERROR
          "vectis lockd examples test failed: ${lockd_examples_stdout}${lockd_examples_stderr}")
endif()
if(NOT lockd_examples_stdout MATCHES "vectis-lockd-examples-ok")
  message(FATAL_ERROR
          "vectis lockd examples test did not report success: ${lockd_examples_stdout}${lockd_examples_stderr}")
endif()
