local lockdc = require("lockdc")

local endpoint = os.getenv("LOCKD_ENDPOINT") or "https://127.0.0.1:8443"
local bundle = os.getenv("LOCKD_CLIENT_BUNDLE")

local client, err = lockdc.open({
  endpoints = { endpoint },
  client_bundle_path = bundle,
  default_namespace = os.getenv("LOCKD_NAMESPACE") or "examples",
  disable_mtls = bundle == nil,
  insecure_skip_verify = bundle == nil,
})
assert(client, err and err.message or "lockdc.open failed")

local lease, acquire_err = client:acquire({
  key = os.getenv("LOCKD_STATE_KEY") or "lua/accounts/1001",
  owner = "vectis-lua-lockd-state-example",
  ttl_seconds = 30,
})
assert(lease, acquire_err and acquire_err.message or "acquire failed")

local ok, update_err = lease:update_json({
  id = "1001",
  status = "active",
  version = 1,
})
assert(ok, update_err and update_err.message or "update failed")

local loaded, get_err = lease:get_json()
assert(loaded, get_err and get_err.message or "get failed")
assert(loaded.id == "1001")
assert(loaded.status == "active")

local released, release_err = lease:release()
assert(released, release_err and release_err.message or "release failed")
client:close()

print("lockd state example ok")
