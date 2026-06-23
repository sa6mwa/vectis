local lockdc = require("lockdc")

local endpoint = assert(os.getenv("LOCKD_ENDPOINT"), "LOCKD_ENDPOINT is required")
local bundle = assert(os.getenv("LOCKD_CLIENT_BUNDLE"), "LOCKD_CLIENT_BUNDLE is required")
local namespace = os.getenv("LOCKD_NAMESPACE") or "examples"

local client, err = lockdc.open({
  endpoints = { endpoint },
  client_bundle_path = bundle,
  default_namespace = namespace,
})
assert(client, err and err.message or "lockdc.open failed")

local info, info_err = client:info()
assert(info, info_err and info_err.message or "client:info failed")
assert(info.default_namespace == namespace)

client:close()
client:close()

print("lockdc lua open/close ok")
