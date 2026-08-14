local vectis = require("vectis")

local endpoint = os.getenv("LOCKD_ENDPOINT") or "https://127.0.0.1:8443"
local bundle = os.getenv("LOCKD_CLIENT_BUNDLE")
local key = os.getenv("LOCKD_STATE_KEY") or "lua/accounts/1001"

local lockd_config = {
  endpoints = { endpoint },
  client_bundle = bundle,
  namespace = os.getenv("LOCKD_NAMESPACE") or "examples",
  disable_mtls = bundle == nil,
  insecure_skip_verify = bundle == nil,
}

assert(vectis.lockd.save_json(lockd_config, {
  key = key,
  owner = "vectis-lua-lockd-state-example",
  ttl_seconds = 30,
}, {
  id = "1001",
  status = "active",
  version = 1,
}))

local loaded = assert(vectis.lockd.load_json(lockd_config, {
  key = key,
  owner = "vectis-lua-lockd-state-example",
  ttl_seconds = 30,
}))
assert(loaded.id == "1001")
assert(loaded.status == "active")

print("lockd state example ok")
