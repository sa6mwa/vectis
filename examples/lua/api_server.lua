local vectis = require("vectis")
local rest = require("vectis.rest")

local bind = os.getenv("VECTIS_LUA_API_EXAMPLE_BIND") or "127.0.0.1"
local port = tonumber(os.getenv("VECTIS_LUA_API_EXAMPLE_PORT") or "28585")
local credentials_path = os.getenv("VECTIS_LUA_API_EXAMPLE_AUTH_PATH") or
    "vectis-lua-api-example-credentials.json"
local serve_forever = os.getenv("VECTIS_LUA_API_EXAMPLE_SERVE") == "1"

local api = rest.client({
  base_url = "http://" .. bind .. ":" .. tostring(port),
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})

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

assert(rest.route(server, {
  path = "/health",
  decode_json = false,
  handler = function()
    return {
      ok = true,
      service = "lua-api-example",
    }, {
      headers = {
        ["cache-control"] = "no-store",
      },
    }
  end,
}) == true)
assert(rest.route(server, {
  path = "/orders",
  method = "POST",
  handler = function()
    return {
      ok = true,
      created = true,
    }, {
      status = 201,
    }
  end,
}) == true)
assert(rest.route(server, {
  path = "/admin/status",
  auth = {
    kind = "native",
    credentials_path = credentials_path,
    realm = "lua-api-example",
    purpose = "webdav",
  },
  handler = function()
    return {
      ok = true,
      admin = true,
    }
  end,
}) == true)

assert(server:start() == true)

local health
for _ = 1, 20 do
  health = api.get("/health")
  if health.ok then
    break
  end
  os.execute("sleep 0.1")
end
assert(health.ok == true, health.error and health.error.message)
assert(health.status == 200)
assert(health.json.ok == true)
assert(health.json.service == "lua-api-example")
assert(health.headers:lower():find("cache-control: no-store", 1, true))

local created = api.post("/orders")
assert(created.ok == true, created.error and created.error.message)
assert(created.status == 201)
assert(created.json.ok == true)
assert(created.json.created == true)

local anonymous = api.get("/admin/status")
assert(anonymous.status == 401)
assert(anonymous.headers:lower():find(
    'www-authenticate: basic realm="lua-api-example"', 1, true))

local guarded = api.get("/admin/status", {
  headers = { Authorization = authorization },
})
assert(guarded.ok == true, guarded.error and guarded.error.message)
assert(guarded.status == 200)
assert(guarded.json.ok == true)
assert(guarded.json.admin == true)
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
