local vectis = require("vectis")
local webdav = require("vectis.webdav")

local port = tonumber(assert(os.getenv("VECTIS_LUA_WEBDAV_EXAMPLE_PORT")))
local cache_dir = assert(os.getenv("VECTIS_LUA_WEBDAV_EXAMPLE_CACHE_DIR"))
local base_url = "http://127.0.0.1:" .. tostring(port)
local site_id = "example-" .. tostring(port)
local collection_path = "/dav/public-" .. tostring(port)
local file_path = collection_path .. "/readme.txt"

local server = assert(vectis.server.new({
  bind = "127.0.0.1",
  port = port,
}))
assert(server:webdav({
  path_prefix = "/dav",
  cache_dir = cache_dir,
  site_id = site_id,
  auth_required = false,
}) == true)
assert(server:start() == true)

local ready
for _ = 1, 20 do
  ready = webdav.propfind({
    url = base_url .. "/dav",
    depth = 0,
    timeout_ms = 2000,
    connect_timeout_ms = 1000,
    retry = false,
    no_signal = true,
  })
  if ready.transport_ok then
    break
  end
  os.execute("sleep 0.1")
end
assert(ready.transport_ok == true, ready.error and ready.error.message)

local mkcol = webdav.mkcol({
  url = base_url .. collection_path,
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  retry = false,
  no_signal = true,
})
assert(mkcol.ok == true, mkcol.error and mkcol.error.message)
assert(mkcol.status == 201)

local put = webdav.put({
  url = base_url .. file_path,
  body = "mutable webdav file\n",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  retry = false,
  no_signal = true,
})
assert(put.ok == true, put.error and put.error.message)
assert(put.status == 201 or put.status == 204)

local get = webdav.get({
  url = base_url .. file_path,
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  retry = false,
  no_signal = true,
})
assert(get.ok == true, get.error and get.error.message)
assert(get.body == "mutable webdav file\n")

local listing = webdav.propfind({
  url = base_url .. collection_path,
  depth = 1,
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  retry = false,
  no_signal = true,
})
assert(listing.ok == true, listing.error and listing.error.message)
assert(listing.body:find("readme.txt", 1, true))

local delete = webdav.delete({
  url = base_url .. file_path,
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  retry = false,
  no_signal = true,
})
assert(delete.ok == true, delete.error and delete.error.message)
assert(delete.status == 204)

assert(server:stop() == true)
server:close()

print("lua webdav fileserver example ok")
