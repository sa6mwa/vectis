local vectis = require("vectis")
local rest = require("vectis.rest")

local bind = os.getenv("VECTIS_LUA_DOWNSTREAM_EXAMPLE_BIND") or "127.0.0.1"
local port = tonumber(os.getenv("VECTIS_LUA_DOWNSTREAM_EXAMPLE_PORT") or "28588")
local serve_forever = os.getenv("VECTIS_LUA_DOWNSTREAM_EXAMPLE_SERVE") == "1"

local base_url = "http://" .. bind .. ":" .. tostring(port)

local api = rest.client({
  base_url = base_url,
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})

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

local inventory
for _ = 1, 20 do
  inventory = api.get("/inventory")
  if inventory.ok then
    break
  end
  os.execute("sleep 0.1")
end
assert(inventory.ok == true, inventory.error and inventory.error.message)
assert(inventory.transport_ok == true)
assert(inventory.status == 200)
assert(inventory.json.ok == true)
assert(inventory.json.items[1].sku == "VX-100")
assert(inventory.json.items[1].qty == 7)
assert(inventory.headers:lower():find("cache-control: no-store", 1, true))

local event = api.post("/events")
assert(event.ok == true, event.error and event.error.message)
assert(event.transport_ok == true)
assert(event.status == 202)
assert(event.json.accepted == true)

local missing = api.get("/missing")
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
