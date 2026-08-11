local lockdc = require("lockdc")
local vectis = require("vectis")
local http = require("vectis.http")

local function env_or_default(name, fallback)
  local value = os.getenv(name)
  if value == nil or value == "" then
    return fallback
  end
  return value
end

local function shell_quote(value)
  return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function mkdir_p(path)
  local ok = os.execute("mkdir -p " .. shell_quote(path))
  assert(ok == true or ok == 0, "failed to create " .. path)
end

local function join_path(left, right)
  if left:sub(-1) == "/" then
    return left .. right
  end
  return left .. "/" .. right
end

local bind = env_or_default("VECTIS_LUA_CONSUMER_EXAMPLE_BIND", "127.0.0.1")
local port = tonumber(env_or_default("VECTIS_LUA_CONSUMER_EXAMPLE_PORT", "28590"))
local endpoint = env_or_default("LOCKD_ENDPOINT", "https://127.0.0.1:8443")
local bundle = os.getenv("LOCKD_CLIENT_BUNDLE")
local namespace = env_or_default("LOCKD_NAMESPACE", "examples")
local queue = env_or_default("LOCKD_QUEUE", "lua-consumer-service")
local owner = env_or_default("LOCKD_CONSUMER_OWNER", "vectis-lua-consumer-example")
local cache_dir = env_or_default("VECTIS_LUA_CONSUMER_EXAMPLE_CACHE", "vectis-lua-consumer-cache")
local site_id = env_or_default("VECTIS_LUA_CONSUMER_EXAMPLE_SITE_ID", "lua-consumer")
local serve_forever = os.getenv("VECTIS_LUA_CONSUMER_EXAMPLE_SERVE") == "1"
local marker_root = join_path(join_path(join_path(cache_dir, "webdav"), site_id), "content")

mkdir_p(marker_root)
if not serve_forever then
  os.remove(join_path(marker_root, "consumer-processing.txt"))
  os.remove(join_path(marker_root, "consumer-done.txt"))
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

local server = assert(vectis.server.new({
  app_name = "lua-consumer-service-example",
  bind = bind,
  port = port,
  tls = { mode = "disabled" },
  lockd = lockd_config,
}))

assert(server:json({
  path = "/health",
  body = '{"ok":true,"service":"lua-consumer-service-example"}\n',
  cache_control = "no-store",
}) == true)
assert(server:static_directory({
  path_prefix = "/markers",
  root_dir = marker_root,
  content_type = "text/plain",
}) == true)
assert(server:consumer_service({
  queue = queue,
  owner = owner,
  name = "lua-consumer-service-example",
  worker_count = 1,
  wait_seconds = 1,
  visibility_timeout_seconds = 30,
  processing_delay_seconds = tonumber(env_or_default(
      "VECTIS_LUA_CONSUMER_EXAMPLE_DELAY_SECONDS", "1")),
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

assert(server:start() == true)

local function request(path)
  return http.request({
    url = "http://" .. bind .. ":" .. tostring(port) .. path,
    protocols = "http",
    timeout_ms = 2000,
    connect_timeout_ms = 1000,
    no_signal = true,
  })
end

local function wait_for(path, expected)
  local response
  for _ = 1, 40 do
    response = request(path)
    if response.ok == true and response.body == expected then
      return response
    end
    os.execute("sleep 0.25")
  end
  error("timed out waiting for " .. path .. ": " ..
        (response and (response.error and response.error.message or response.body) or "no response"))
end

if serve_forever then
  while true do
    os.execute("sleep 3600")
  end
end

local health = wait_for("/health", '{"ok":true,"service":"lua-consumer-service-example"}\n')
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

wait_for("/markers/consumer-processing.txt", "processing\n")
wait_for("/markers/consumer-done.txt", "handled\n")

assert(server:stop() == true)
server:close()

print("lua consumer service example ok")
