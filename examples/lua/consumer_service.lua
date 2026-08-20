local lockdc = require("lockdc")
local vectis = require("vectis")
local http = require("vectis.http")

local bind = os.getenv("VECTIS_LUA_CONSUMER_EXAMPLE_BIND") or "127.0.0.1"
local port = tonumber(os.getenv("VECTIS_LUA_CONSUMER_EXAMPLE_PORT") or "28590")
local endpoint = os.getenv("LOCKD_ENDPOINT") or "https://127.0.0.1:8443"
local bundle = os.getenv("LOCKD_CLIENT_BUNDLE")
local namespace = os.getenv("LOCKD_NAMESPACE") or "examples"
local queue = os.getenv("LOCKD_QUEUE") or "lua-consumer-service"
local owner = os.getenv("LOCKD_CONSUMER_OWNER") or
    "vectis-lua-consumer-example"
local cache_dir = os.getenv("VECTIS_LUA_CONSUMER_EXAMPLE_CACHE") or
    "vectis-lua-consumer-cache"
local site_id = os.getenv("VECTIS_LUA_CONSUMER_EXAMPLE_SITE_ID") or
    "lua-consumer"
local serve_forever = os.getenv("VECTIS_LUA_CONSUMER_EXAMPLE_SERVE") == "1"
local marker_root = cache_dir .. "/webdav/" .. site_id .. "/content"

assert(vectis.mkdir_p(marker_root) == true)
if not serve_forever then
  os.remove(marker_root .. "/consumer-processing.txt")
  os.remove(marker_root .. "/consumer-done.txt")
end

local lockd_config = {
  endpoints = { endpoint },
  namespace = namespace,
}
if bundle == "embedded" then
  lockd_config.client_bundle = "embedded"
elseif bundle ~= nil and bundle ~= "" then
  lockd_config.client_bundle_path = bundle
end

local app = assert(vectis.app.new({
  app_name = "lua-consumer-service-example",
  bind = bind,
  port = port,
  tls = { mode = "disabled" },
  lockd = lockd_config,
}))

assert(app:json({
  path = "/health",
  body = '{"ok":true,"service":"lua-consumer-service-example"}\n',
  cache_control = "no-store",
}) == true)
assert(app:static_directory({
  path_prefix = "/markers",
  root_dir = marker_root,
  content_type = "text/plain",
}) == true)
assert(app:consumer_service({
  queue = queue,
  owner = owner,
  name = "lua-consumer-service-example",
  worker_count = 1,
  wait_seconds = 1,
  visibility_timeout_seconds = 30,
  processing_delay_seconds = tonumber(
      os.getenv("VECTIS_LUA_CONSUMER_EXAMPLE_DELAY_SECONDS") or "1"),
  handler = {
    kind = "webdav_marker",
    cache_dir = cache_dir,
    site_id = site_id,
    processing_path = "/consumer-processing.txt",
    done_path = "/consumer-done.txt",
    processing_body = "processing\n",
    done_body = "handled\n",
  },
}) == true)

assert(app:start() == true)

if serve_forever then
  print("lua consumer service example listening on http://" .. bind .. ":" ..
      tostring(port))
  assert(app:wait() == true)
  app:close()
  return
end

local health
for _ = 1, 40 do
  health = http.request({
    url = "http://" .. bind .. ":" .. tostring(port) .. "/health",
    protocols = "http",
    timeout_ms = 2000,
    connect_timeout_ms = 1000,
    no_signal = true,
  })
  if health.ok == true and
      health.body == '{"ok":true,"service":"lua-consumer-service-example"}\n' then
    break
  end
  vectis.sleep(0.25)
end
assert(health.ok == true, health.error and health.error.message or
    "health route was not ready")
assert(health.status == 200)

local client_config = {
  endpoints = { endpoint },
  default_namespace = namespace,
  disable_mtls = bundle == nil or bundle == "",
  insecure_skip_verify = bundle == nil or bundle == "",
}
if bundle == "embedded" then
  client_config.client_bundle_source = assert(vectis.embedded_lockd_bundle_source())
elseif bundle ~= nil and bundle ~= "" then
  client_config.client_bundle_path = bundle
end
local client, open_err = lockdc.open(client_config)
assert(client, open_err and open_err.message or "lockdc.open failed")
local enqueued, enqueue_err = client:enqueue({
  queue = queue,
  visibility_timeout_seconds = 30,
  ttl_seconds = 3600,
  max_attempts = 5,
  content_type = "text/plain",
}, "lua-consumer-service-message")
assert(enqueued, enqueue_err and enqueue_err.message or "enqueue failed")
client:close()

local processing_marker
for _ = 1, 40 do
  processing_marker = http.request({
    url = "http://" .. bind .. ":" .. tostring(port) ..
        "/markers/consumer-processing.txt",
    protocols = "http",
    timeout_ms = 2000,
    connect_timeout_ms = 1000,
    no_signal = true,
  })
  if processing_marker.ok == true and processing_marker.body == "processing\n" then
    break
  end
  vectis.sleep(0.25)
end
assert(processing_marker.ok == true, processing_marker.error and
    processing_marker.error.message or "processing marker was not written")

local done_marker
for _ = 1, 40 do
  done_marker = http.request({
    url = "http://" .. bind .. ":" .. tostring(port) ..
        "/markers/consumer-done.txt",
    protocols = "http",
    timeout_ms = 2000,
    connect_timeout_ms = 1000,
    no_signal = true,
  })
  if done_marker.ok == true and done_marker.body == "handled\n" then
    break
  end
  vectis.sleep(0.25)
end
assert(done_marker.ok == true, done_marker.error and
    done_marker.error.message or "done marker was not written")

assert(app:stop() == true)
app:close()

print("lua consumer service example ok")
