local vectis = require("vectis")
local http = require("vectis.http")
local lonejson = require("lonejson")

local bind = os.getenv("VECTIS_LUA_METRICS_AUTH_EXAMPLE_BIND") or "127.0.0.1"
local port = tonumber(os.getenv("VECTIS_LUA_METRICS_AUTH_EXAMPLE_PORT") or
    "28621")
local credentials_path = os.getenv("VECTIS_LUA_METRICS_AUTH_EXAMPLE_AUTH_PATH") or
    "vectis-metrics-auth-example-credentials.json"
local state_path = os.getenv("VECTIS_LUA_METRICS_AUTH_EXAMPLE_STATE_PATH") or
    "vectis-metrics-auth-example-state.json"
local storage_dir = os.getenv("VECTIS_LUA_METRICS_AUTH_EXAMPLE_STORAGE") or
    "vectis-metrics-auth-pouch"
local serve_forever = os.getenv("VECTIS_LUA_METRICS_AUTH_EXAMPLE_SERVE") == "1"
local base_url = "http://" .. bind .. ":" .. tostring(port)
local totp_secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
local totp_time = 59
local totp_code = "287082"
local request_opts = {
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
}

do
  local credentials = io.open(credentials_path, "rb")
  if credentials == nil then
    error("create metrics-admin first with: vectis -a users --store " ..
          credentials_path .. " --add metrics-admin --password " ..
          "metrics-password --totp", 0)
  end
  credentials:close()
end

local browser_flow = vectis.auth.browser_flow({
  credentials_path = credentials_path,
  state_path = state_path,
  path_prefix = "/_vectis/auth",
  realm = "metrics-example",
  purpose = "webdav",
  allowed_modes = { "basic" },
  required_factors = { "password", "totp" },
  time = totp_time,
  window = 0,
})

local browser_provider = assert(browser_flow:provider())
local machine_credential = assert(vectis.auth.issue({
  credentials_path = credentials_path,
  state_path = state_path,
  subject = "metrics-agent",
  purpose = "metrics",
  modes = { "bearer" },
}))
local machine_provider = assert(vectis.auth.provider_native({
  credentials_path = credentials_path,
  state_path = state_path,
  realm = "metrics-example",
  purpose = "metrics",
  allowed_modes = { "bearer" },
}))

local metrics_provider = assert(vectis.auth.provider_callback(function(request)
  local browser = assert(browser_provider:authenticate({
    authorization = request.authorization,
    resource = request.resource,
  }))
  if browser.action == "allow" then
    return browser
  end

  local machine = assert(machine_provider:authenticate({
    authorization = request.authorization,
    resource = request.resource,
  }))
  if machine.action == "allow" then
    return machine
  end

  return {
    action = "required",
    status_code = 401,
    www_authenticate = 'Basic realm="metrics-example"',
    content_type = "text/plain; charset=utf-8",
    body = "metrics authentication required\n",
  }
end))

local browser_authorization
if not serve_forever then
  local password_only_authorization, password_only_error =
      browser_flow:webdav_authorization({
        username = "metrics-admin",
        password = "metrics-password",
      })
  assert(password_only_authorization == nil)
  assert(password_only_error.status == vectis.ERR_INVALID)

  browser_authorization = assert(browser_flow:webdav_authorization({
    username = "metrics-admin",
    password = "metrics-password",
    totp_code = totp_code,
    time = totp_time,
    window = 0,
  }))
end
local machine_authorization = "Bearer " .. machine_credential.api_key

local server = assert(vectis.server.new({
  app_name = "lua-metrics-auth-example",
  bind = bind,
  port = port,
}))

assert(browser_flow:mount(server))

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
  title = "lua metrics authenticated example",
  auth = {
    provider = metrics_provider,
    purpose = "metrics",
    allowed_modes = { "basic", "bearer" },
  },
  persistence_enabled = true,
  storage_endpoint = "pouch://" .. storage_dir,
  storage_namespace = "vectis.examples.metrics",
  storage_owner = "metrics-auth-example",
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

local login = http.get(base_url .. "/_vectis/auth/login", request_opts)
assert(login.ok == true, login.error and login.error.message)
assert(login.status == 200)
assert(login.body:find('action="/_vectis/auth/continue"', 1, true))

local anonymous = http.get(base_url .. "/.metrics/snapshot.json", request_opts)
assert(anonymous.status == 401)
assert(anonymous.body == "metrics authentication required\n")

local browser_metrics = http.get(base_url .. "/.metrics/snapshot.json", {
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
  headers = { Authorization = browser_authorization },
})
assert(browser_metrics.ok == true,
       browser_metrics.error and browser_metrics.error.message)
assert(browser_metrics.status == 200)
local browser_snapshot = assert(lonejson.decode_json(browser_metrics.body))
assert(browser_snapshot.service == "lua metrics authenticated example")
assert(browser_snapshot.persistence.enabled == true)

local machine_dashboard = http.get(base_url .. "/.metrics", {
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
  headers = { Authorization = machine_authorization },
})
assert(machine_dashboard.ok == true,
       machine_dashboard.error and machine_dashboard.error.message)
assert(machine_dashboard.status == 200)
assert(machine_dashboard.body:find("lua metrics authenticated example", 1, true))

if serve_forever then
  print("lua metrics authenticated example listening on " .. base_url)
  print("login route: " .. base_url .. "/_vectis/auth/login")
  print("metrics dashboard: " .. base_url .. "/.metrics")
  print("metrics JSON: " .. base_url .. "/.metrics/snapshot.json")
  print("browser user: metrics-admin / metrics-password / configured TOTP")
  print("m2m bearer token: " .. machine_credential.api_key)
  while true do
    os.execute("sleep 3600")
  end
else
  assert(server:stop() == true)
  server:close()
  print("lua metrics authenticated example ok")
end
