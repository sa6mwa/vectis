local vectis = require("vectis")
local http = require("vectis.http")
local lonejson = require("lonejson")

local bind = os.getenv("VECTIS_LUA_METRICS_PERSISTENT_EXAMPLE_BIND") or
    "127.0.0.1"
local port = tonumber(os.getenv("VECTIS_LUA_METRICS_PERSISTENT_EXAMPLE_PORT") or
    "28620")
local storage_dir = os.getenv("VECTIS_LUA_METRICS_PERSISTENT_EXAMPLE_STORAGE") or
    "vectis-metrics-persistent-pouch"
local serve_forever =
    os.getenv("VECTIS_LUA_METRICS_PERSISTENT_EXAMPLE_SERVE") == "1"
local base_url = "http://" .. bind .. ":" .. tostring(port)
local request_opts = {
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
}

local server = assert(vectis.server.new({
  app_name = "lua-metrics-persistent-example",
  bind = bind,
  port = port,
}))

assert(server:route({
  path = "/",
  handler = function()
    return {
      status = 200,
      content_type = "text/html; charset=utf-8",
      body = "<!doctype html><title>Hello Vectis</title><h1>Hello, world.</h1>\n",
    }
  end,
}) == true)

assert(server:metrics({
  path = "/.metrics",
  json_path = "/.metrics/snapshot.json",
  title = "lua metrics persistent example",
  persistence_enabled = true,
  storage_endpoint = "pouch://" .. storage_dir,
  storage_namespace = "vectis.examples.metrics",
  storage_owner = "metrics-persistent-example",
}) == true)

assert(server:start() == true)

local page
for _ = 1, 20 do
  page = http.get(base_url .. "/", request_opts)
  if page.ok then
    break
  end
  os.execute("sleep 0.1")
end
assert(page.ok == true, page.error and page.error.message)
assert(page.status == 200)
assert(page.body:find("Hello, world.", 1, true))

local metrics = http.get(base_url .. "/.metrics/snapshot.json", request_opts)
assert(metrics.ok == true, metrics.error and metrics.error.message)
assert(metrics.status == 200)
local snapshot = assert(lonejson.decode_json(metrics.body))
assert(snapshot.service == "lua metrics persistent example")
assert(snapshot.persistence.enabled == true)

local dashboard = http.get(base_url .. "/.metrics", request_opts)
assert(dashboard.ok == true, dashboard.error and dashboard.error.message)
assert(dashboard.status == 200)
assert(dashboard.body:find("lua metrics persistent example", 1, true))

if serve_forever then
  print("lua metrics persistent example listening on " .. base_url)
  print("metrics dashboard: " .. base_url .. "/.metrics")
  print("metrics JSON: " .. base_url .. "/.metrics/snapshot.json")
  while true do
    os.execute("sleep 3600")
  end
else
  assert(server:stop() == true)
  server:close()
  print("lua metrics persistent example ok")
end
