-- Restart fixture: use only public Vectis/lockdc APIs and encrypted Pouch.
local vectis = require("vectis")
local lockdc = require("lockdc")
local mode, root, key = arg[1], arg[2], arg[3]
local endpoint = "pouch://" .. root .. "/state"
local function checked(value, err)
  assert(value ~= nil and value ~= false,
         type(err) == "table" and err.message or tostring(err))
  return value
end
if mode == "seed" then
  local count = assert(tonumber(arg[4]))
  local client = checked(lockdc.open({
    endpoints = {endpoint},
    default_namespace = "vectis.metrics",
    pouch = {
      crypto_key = assert(os.getenv("VECTIS_POUCH_CRYPTO_KEY")),
      single_writer = arg[6] ~= "shared",
    },
  }))
  local doc = {
    format = "vectis-metrics-snapshot", version = 1,
    app_name = "metrics-startup-gate",
    http = {requests_total = 0, route_misses = 0, body_rejects = 0,
            status = {["1xx"]=0, ["2xx"]=0, ["3xx"]=0, ["4xx"]=0, ["5xx"]=0}},
    auth = {allowed=0, denied=0, required=0, redirected=0},
    persistence = {writes=0, errors=0},
  }
  for i = 1, count do
    local lease = checked(client:acquire({key=key, owner="vectis", ttl_seconds=30}))
    doc.http.requests_total, doc.http.status["2xx"], doc.persistence.writes = i, i, i
    checked(lease:update(checked(lockdc.encode_json(doc)), {content_type="application/json"}))
    checked(lease:release())
    lease:close()
  end
  if arg[5] == "crash" then
    print("SEEDED")
    io.stdout:flush()
    while true do vectis.sleep(1) end
  end
  client:close()
else
  local app = assert(vectis.app.new({
    app_name = "metrics-startup-gate", bind = "127.0.0.1", port = tonumber(arg[4]),
    tls = {mode="manual", cert_key_bundle_path=root .. "/tls.pem", domain="localhost"},
  }))
  assert(app:route({path="/ready", handler=function() return {status=200, body="ready"} end}))
  assert(app:metrics({path="/.metrics", json_path="/.metrics.json",
                     persistence_enabled=true, storage_endpoint=endpoint}))
  checked(app:start())
  checked(app:wait())
  app:close()
end
