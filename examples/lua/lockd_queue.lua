local vectis = require("vectis")

local endpoint = os.getenv("LOCKD_ENDPOINT") or "https://127.0.0.1:8443"
local bundle = os.getenv("LOCKD_CLIENT_BUNDLE")
local queue = os.getenv("LOCKD_QUEUE") or "lua-orders"

local lockd_config = {
  endpoints = { endpoint },
  client_bundle = bundle,
  namespace = os.getenv("LOCKD_NAMESPACE") or "examples",
  disable_mtls = bundle == nil,
  insecure_skip_verify = bundle == nil,
}

assert(vectis.lockd.enqueue_json(lockd_config, {
  queue = queue,
  visibility_timeout_seconds = 30,
  ttl_seconds = 3600,
  max_attempts = 5,
}, {
  type = "order.created",
  id = "1001",
}))

assert(vectis.lockd.with_dequeued_json(lockd_config, {
  queue = queue,
  owner = "vectis-lua-lockd-queue-example",
  visibility_timeout_seconds = 30,
  wait_seconds = 5,
}, function(payload, message, _, payload_written)
  assert(payload_written > 0)
  assert(payload.type == "order.created")
  assert(payload.id == "1001")
  return message:ack()
end))

print("lockd queue example ok")
