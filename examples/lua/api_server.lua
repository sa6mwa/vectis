local vectis = require("vectis")
local http = require("vectis.http")

local bind = os.getenv("VECTIS_LUA_API_EXAMPLE_BIND") or "127.0.0.1"
local port = tonumber(os.getenv("VECTIS_LUA_API_EXAMPLE_PORT") or "28585")
local credentials_path = os.getenv("VECTIS_LUA_API_EXAMPLE_AUTH_PATH") or
    "vectis-lua-api-example-credentials.json"
local serve_forever = os.getenv("VECTIS_LUA_API_EXAMPLE_SERVE") == "1"

local function request(path, options)
  options = options or {}
  return http.request({
    url = "http://" .. bind .. ":" .. tostring(port) .. path,
    method = options.method,
    body = options.body,
    headers = options.headers,
    protocols = "http",
    timeout_ms = 2000,
    connect_timeout_ms = 1000,
    no_signal = true,
  })
end

local function wait_ready()
  local response
  for _ = 1, 20 do
    response = request("/health")
    if response.ok then
      return response
    end
    os.execute("sleep 0.1")
  end
  return response
end

if not serve_forever then
  os.remove(credentials_path)
  os.remove(credentials_path .. ".lock")
end

assert(vectis.auth.store_init({ credentials_path = credentials_path }) == true)
assert(vectis.auth.user_add({
  credentials_path = credentials_path,
  username = "api-user",
  password = "api-password",
}).username == "api-user")

local credential = assert(vectis.auth.webdav_key({
  credentials_path = credentials_path,
  username = "api-user",
  password = "api-password",
}))
local authorization = assert(vectis.auth.basic_authorization(credential))

local server = assert(vectis.server.new({
  app_name = "lua-api-example",
  bind = bind,
  port = port,
}))

assert(server:json({
  path = "/health",
  body = '{"ok":true,"service":"lua-api-example"}\n',
  cache_control = "no-store",
}) == true)
assert(server:json({
  path = "/orders",
  method = "POST",
  status = 201,
  body = '{"ok":true,"created":true}\n',
}) == true)
assert(server:auth_json({
  path = "/admin/status",
  auth = {
    kind = "native",
    credentials_path = credentials_path,
    realm = "lua-api-example",
    purpose = "webdav",
  },
  body = '{"ok":true,"admin":true}\n',
}) == true)

assert(server:start() == true)

local health = wait_ready()
assert(health.ok == true, health.error and health.error.message)
assert(health.status == 200)
assert(health.body == '{"ok":true,"service":"lua-api-example"}\n')

local created = request("/orders", { method = "POST" })
assert(created.ok == true, created.error and created.error.message)
assert(created.status == 201)
assert(created.body == '{"ok":true,"created":true}\n')

local anonymous = request("/admin/status")
assert(anonymous.status == 401)
assert(anonymous.headers:lower():find(
    'www-authenticate: basic realm="lua-api-example"', 1, true))

local guarded = request("/admin/status", {
  headers = { Authorization = authorization },
})
assert(guarded.ok == true, guarded.error and guarded.error.message)
assert(guarded.status == 200)
assert(guarded.body == '{"ok":true,"admin":true}\n')
assert(guarded.headers:lower():find("cache-control: no-store", 1, true))

if serve_forever then
  print("lua api example listening on http://" .. bind .. ":" .. tostring(port))
  while true do
    os.execute("sleep 3600")
  end
else
  assert(server:stop() == true)
  server:close()
  print("lua api server example ok")
end
