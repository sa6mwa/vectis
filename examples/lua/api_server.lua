local vectis = require("vectis")
local http = require("vectis.http")

local bind = os.getenv("VECTIS_LUA_API_EXAMPLE_BIND") or "127.0.0.1"
local port = tonumber(os.getenv("VECTIS_LUA_API_EXAMPLE_PORT") or "28585")
local credentials_path = os.getenv("VECTIS_LUA_API_EXAMPLE_AUTH_PATH") or
    "vectis-lua-api-example-credentials.json"
local serve_forever = os.getenv("VECTIS_LUA_API_EXAMPLE_SERVE") == "1"

local b64chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

local function base64(data)
  local out = {}
  for i = 1, #data, 3 do
    local a = data:byte(i) or 0
    local b = data:byte(i + 1) or 0
    local c = data:byte(i + 2) or 0
    local n = a * 65536 + b * 256 + c
    local pad = (#data - i == 0) and 2 or ((#data - i == 1) and 1 or 0)
    out[#out + 1] = b64chars:sub(math.floor(n / 262144) % 64 + 1,
                                  math.floor(n / 262144) % 64 + 1)
    out[#out + 1] = b64chars:sub(math.floor(n / 4096) % 64 + 1,
                                  math.floor(n / 4096) % 64 + 1)
    out[#out + 1] = pad >= 2 and "=" or
        b64chars:sub(math.floor(n / 64) % 64 + 1,
                     math.floor(n / 64) % 64 + 1)
    out[#out + 1] = pad >= 1 and "=" or
        b64chars:sub(n % 64 + 1, n % 64 + 1)
  end
  return table.concat(out)
end

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
local authorization =
    "Basic " .. base64(credential.client_id .. ":" .. credential.client_secret)

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
