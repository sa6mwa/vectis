local vectis = require("vectis")
local http = require("vectis.http")
local lonejson = require("lonejson")

local bind = os.getenv("VECTIS_LUA_METRICS_AUTH_EXAMPLE_BIND") or "127.0.0.1"
local port = tonumber(os.getenv("VECTIS_LUA_METRICS_AUTH_EXAMPLE_PORT") or
    "28621")
local storage_dir = os.getenv("VECTIS_LUA_METRICS_AUTH_EXAMPLE_STORAGE") or
    "vectis-metrics-auth-pouch"
local serve_forever = os.getenv("VECTIS_LUA_METRICS_AUTH_EXAMPLE_SERVE") == "1"
local base_url = "http://" .. bind .. ":" .. tostring(port)
local totp_time = 59
local request_opts = {
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
}

local auth_workflow = vectis.auth.workflow({
  state_key = "auth/v1/store",
  path_prefix = "/_vectis/auth",
  realm = "metrics-example",
  purpose = "metrics",
  credential_purpose = "metrics",
  allowed_modes = { "basic" },
  steps = { "password", "totp" },
  time = totp_time,
  window = 0,
})

local metrics_authorization =
    os.getenv("VECTIS_LUA_METRICS_AUTH_EXAMPLE_AUTHORIZATION")

local app = assert(vectis.app.new({
  app_name = "lua-metrics-auth-example",
  bind = bind,
  port = port,
  lockd = { endpoints = {"pouch://" .. storage_dir .. "?single_writer=false"} },
}))

assert(auth_workflow:mount(app))

assert(app:route({
  path = "/",
  handler = function()
    return {
      status = 200,
      content_type = "text/html; charset=utf-8",
      body = "<!doctype html><title>Hello Vectis</title><h1>Hello, world.</h1>\n",
    }
  end,
}) == true)

assert(app:metrics({
  path = "/.metrics",
  json_path = "/.metrics/snapshot.json",
  title = "lua metrics authenticated example",
  auth = {
    kind = "native",
    state_key = "auth/v1/store",
    realm = "metrics-example",
    purpose = "metrics",
    allowed_modes = { "basic", "bearer" },
  },
  persistence_enabled = true,
  storage_endpoint = "pouch://" .. storage_dir,
  storage_namespace = "vectis.examples.metrics",
  storage_owner = "metrics-auth-example",
}) == true)

assert(app:start() == true)

local page
for _ = 1, 20 do
  page = http.get(base_url .. "/", request_opts)
  if page.ok then
    break
  end
  vectis.sleep(0.1)
end
assert(page.ok == true, page.error and page.error.message)
assert(page.status == 200)
assert(page.body:find("Hello, world.", 1, true))

local anonymous = http.get(base_url .. "/.metrics/snapshot.json", request_opts)
assert(anonymous.status == 401)

if metrics_authorization ~= nil and metrics_authorization ~= "" then
  local metrics_snapshot = http.get(base_url .. "/.metrics/snapshot.json", {
    protocols = "http",
    timeout_ms = 2000,
    connect_timeout_ms = 1000,
    no_signal = true,
    headers = { Authorization = metrics_authorization },
  })
  assert(metrics_snapshot.ok == true,
         metrics_snapshot.error and metrics_snapshot.error.message)
  assert(metrics_snapshot.status == 200)
  local snapshot = assert(lonejson.decode_json(metrics_snapshot.body))
  assert(snapshot.service == "lua metrics authenticated example")
  assert(snapshot.persistence.enabled == true)

  local dashboard = http.get(base_url .. "/.metrics", {
    protocols = "http",
    timeout_ms = 2000,
    connect_timeout_ms = 1000,
    no_signal = true,
    headers = { Authorization = metrics_authorization },
  })
  assert(dashboard.ok == true, dashboard.error and dashboard.error.message)
  assert(dashboard.status == 200)
  assert(dashboard.body:find("lua metrics authenticated example", 1, true))
end

if serve_forever then
  print("lua metrics authenticated example listening on " .. base_url)
  print("login route: " .. base_url .. "/_vectis/auth/login")
  print("metrics dashboard: " .. base_url .. "/.metrics")
  print("metrics JSON: " .. base_url .. "/.metrics/snapshot.json")
  print("set VECTIS_LUA_METRICS_AUTH_EXAMPLE_AUTHORIZATION to test metrics access")
  assert(app:wait() == true)
  app:close()
else
  assert(app:stop() == true)
  app:close()
  print("lua metrics authenticated example ok")
end
