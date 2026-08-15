local vectis = require("vectis")

local requests = vectis.mailbox.new({ capacity = 4, max_payload_bytes = 256 })
local replies = vectis.mailbox.new({ capacity = 4, max_payload_bytes = 256 })

local request_id = assert(requests:request({
  kind = "route.opcua.write",
  payload = '{"path":"/machine/1","value":42}',
}))

local worker_count = assert(requests:pump(function(event)
  assert(event.kind == "route.opcua.write")
  assert(event.correlation_id == request_id)
  assert(event.expects_reply == true)
  assert(event.payload:match('"value":42'))
  assert(replies:reply(event.correlation_id, {
    kind = "worker.result",
    payload = '{"status":"ok","action":"opcua.write"}',
  }))
end, { max = 1, timeout_ms = 0 }))

assert(worker_count == 1)

local reply = assert(replies:next(0))
assert(reply.kind == "worker.result")
assert(reply.correlation_id == request_id)
assert(reply.payload:match('"status":"ok"'))

local request_stats = requests:stats()
local reply_stats = replies:stats()
assert(request_stats.requests_published == 1)
assert(request_stats.drained == 1)
assert(request_stats.pump_calls == 1)
assert(request_stats.pump_events == 1)
assert(reply_stats.replies_published == 1)
assert(reply_stats.drained == 1)

requests:close()
replies:close()
print("mailbox pipeline completed")
