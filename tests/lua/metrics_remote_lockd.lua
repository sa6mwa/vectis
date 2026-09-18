-- E2E fixture: the supervisor must create its own metrics client from the
-- app's full Lockd configuration while targeting a remote mTLS endpoint.
local vectis = require("vectis")

local endpoint = assert(arg[1], "Lockd endpoint is required")
local bundle = assert(arg[2], "Lockd client bundle is required")
local port = assert(tonumber(arg[3]), "listen port is required")

local app = assert(vectis.app.new({
  app_name = "metrics-remote-lockd-e2e",
  bind = "127.0.0.1",
  port = port,
  lockd = {
    client_bundle_path = bundle,
  },
}))

assert(app:route({
  path = "/health",
  handler = function()
    return {status = 200, body = "ok\n"}
  end,
}))

assert(app:metrics({
  path = "/.metrics",
  json_path = "/.metrics.json",
  persistence_enabled = true,
  storage_endpoint = endpoint,
  storage_namespace = "vectis.metrics",
  storage_owner = "metrics-remote-lockd-e2e",
}))

assert(app:start())
assert(app:wait())
app:close()
