local vectis = require("vectis")

local endpoint = os.getenv("LOCKD_ENDPOINT") or "https://127.0.0.1:8443"
local bundle = os.getenv("LOCKD_CLIENT_BUNDLE")
local queue = os.getenv("LOCKD_QUEUE") or "lua-orders"

local client, err = vectis.lockd.open({
  endpoints = { endpoint },
  client_bundle_path = bundle,
  default_namespace = os.getenv("LOCKD_NAMESPACE") or "examples",
  disable_mtls = bundle == nil,
  insecure_skip_verify = bundle == nil,
})
assert(client, err and err.message or "lockdc.open failed")

local enqueued, enqueue_err = client:enqueue({
  queue = queue,
  visibility_timeout_seconds = 30,
  ttl_seconds = 3600,
  max_attempts = 5,
  content_type = "application/json",
}, vectis.lockd.encode_json({
  type = "order.created",
  id = "1001",
}))
assert(enqueued, enqueue_err and enqueue_err.message or "enqueue failed")

local message, dequeue_err = client:dequeue({
  queue = queue,
  owner = "vectis-lua-lockd-queue-example",
  visibility_timeout_seconds = 30,
  wait_seconds = 5,
})
assert(message, dequeue_err and dequeue_err.message or "dequeue failed")

local payload, payload_written_or_err = message:payload_json()
if payload == nil then
  error(payload_written_or_err and payload_written_or_err.message or "payload failed")
end
assert(payload_written_or_err > 0)
assert(payload.type == "order.created")
assert(payload.id == "1001")

local acked, ack_err = message:ack()
assert(acked, ack_err and ack_err.message or "ack failed")
client:close()

print("lockd queue example ok")
