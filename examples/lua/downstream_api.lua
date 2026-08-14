local vectis = require("vectis")
local http = require("vectis.http")

local bind = os.getenv("VECTIS_LUA_DOWNSTREAM_EXAMPLE_BIND") or "127.0.0.1"
local port = tonumber(os.getenv("VECTIS_LUA_DOWNSTREAM_EXAMPLE_PORT") or "28588")
local serve_forever = os.getenv("VECTIS_LUA_DOWNSTREAM_EXAMPLE_SERVE") == "1"

local base_url = "http://" .. bind .. ":" .. tostring(port)

local function request_opts(path, opts)
  opts = opts or {}
  opts.url = base_url .. path
  opts.protocols = "http"
  opts.timeout_ms = 2000
  opts.connect_timeout_ms = 1000
  opts.no_signal = true
  return opts
end

local function wait_ready()
  local response
  for _ = 1, 20 do
    response = http.get_json(request_opts("/inventory"))
    if response.ok then
      return response
    end
    os.execute("sleep 0.1")
  end
  return response
end

local server = assert(vectis.server.new({
  app_name = "lua-downstream-api-example",
  bind = bind,
  port = port,
}))

assert(server:json({
  path = "/inventory",
  body = '{"ok":true,"items":[{"sku":"VX-100","qty":7}]}\n',
  cache_control = "no-store",
}) == true)
assert(server:json({
  path = "/events",
  method = "POST",
  status = 202,
  body = '{"ok":true,"accepted":true}\n',
}) == true)

assert(server:start() == true)

local inventory = wait_ready()
assert(inventory.ok == true, inventory.error and inventory.error.message)
assert(inventory.transport_ok == true)
assert(inventory.status == 200)
assert(inventory.json.ok == true)
assert(inventory.json.items[1].sku == "VX-100")
assert(inventory.json.items[1].qty == 7)

local event = http.post_json(request_opts("/events"))
assert(event.ok == true, event.error and event.error.message)
assert(event.transport_ok == true)
assert(event.status == 202)
assert(event.json.accepted == true)

local missing = http.get_json(request_opts("/missing"))
assert(missing.ok == false)
assert(missing.transport_ok == true)
assert(missing.error.kind == "http_status")
assert(missing.error.http_status == 404)

if serve_forever then
  print("lua downstream API example listening on " .. base_url)
  while true do
    os.execute("sleep 3600")
  end
else
  assert(server:stop() == true)
  server:close()
  print("lua downstream API example ok")
end
