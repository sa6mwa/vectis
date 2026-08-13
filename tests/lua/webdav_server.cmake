include("${CMAKE_CURRENT_LIST_DIR}/port_retry.cmake")

set(cache_dir "${WORK_DIR}/vectis-webdav-server-cache")
set(auth_path "${WORK_DIR}/vectis-webdav-server-auth.json")
set(script "${WORK_DIR}/vectis-webdav-server.lua")

file(REMOVE_RECURSE "${cache_dir}")
file(REMOVE "${auth_path}" "${auth_path}.lock")

string(CONFIGURE [=[
local vectis = require("vectis")
local webdav = require("vectis.webdav")

local port = tonumber(assert(os.getenv("VECTIS_WEBDAV_SERVER_PORT")))
local cache_dir = [[@cache_dir@]]
local auth_path = [[@auth_path@]]
local base = "http://127.0.0.1:" .. tostring(port)

local function base64(data)
  local chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
  local out = {}
  for i = 1, #data, 3 do
    local a = data:byte(i) or 0
    local b = data:byte(i + 1) or 0
    local c = data:byte(i + 2) or 0
    local n = a * 65536 + b * 256 + c
    local pad = (#data - i == 0) and 2 or ((#data - i == 1) and 1 or 0)
    out[#out + 1] = chars:sub(math.floor(n / 262144) % 64 + 1, math.floor(n / 262144) % 64 + 1)
    out[#out + 1] = chars:sub(math.floor(n / 4096) % 64 + 1, math.floor(n / 4096) % 64 + 1)
    out[#out + 1] = pad >= 2 and "=" or chars:sub(math.floor(n / 64) % 64 + 1, math.floor(n / 64) % 64 + 1)
    out[#out + 1] = pad >= 1 and "=" or chars:sub(n % 64 + 1, n % 64 + 1)
  end
  return table.concat(out)
end

local function request_opts(path, extra)
  local opts = {
    url = base .. path,
    timeout_ms = 2000,
    connect_timeout_ms = 1000,
    no_signal = true,
  }
  if extra ~= nil then
    for key, value in pairs(extra) do
      opts[key] = value
    end
  end
  return opts
end

assert(vectis.auth.store_init({credentials_path = auth_path}) == true)
assert(vectis.auth.user_add({
  credentials_path = auth_path,
  username = "dav-user",
  password = "dav-password",
}).username == "dav-user")
local key = assert(vectis.auth.webdav_key({
  credentials_path = auth_path,
  username = "dav-user",
  password = "dav-password",
}))
local basic_auth = "Basic " .. base64(key.client_id .. ":" .. key.client_secret)

local callback_provider = assert(vectis.auth.provider_callback(function(request)
  if request.authorization == "Bearer callback-dav" and
      request.resource == "/allowed.txt" then
    return {action = "allow", principal = "callback-user"}
  end
  return {
    action = "required",
    status_code = 401,
    www_authenticate = 'Bearer realm="callback-dav"',
    content_type = "text/plain; charset=utf-8",
    body = "callback login required\n",
  }
end))

local server = assert(vectis.server.new({
  bind = "127.0.0.1",
  port = port,
}))
assert(server:webdav({
  path_prefix = "/open",
  cache_dir = cache_dir,
  site_id = "open",
  auth_required = false,
}) == true)
assert(server:webdav({
  path_prefix = "/native",
  cache_dir = cache_dir,
  site_id = "native",
  conceal_unauthorized = false,
  auth = {
    kind = "native",
    credentials_path = auth_path,
    realm = "native-dav",
    purpose = "webdav",
  },
}) == true)
assert(server:webdav({
  path_prefix = "/callback",
  cache_dir = cache_dir,
  site_id = "callback",
  conceal_unauthorized = false,
  auth = {
    provider = callback_provider,
    purpose = "webdav",
    allowed_modes = {"bearer"},
  },
}) == true)
assert(server:start() == true)

local ready
for _ = 1, 20 do
  ready = webdav.propfind(request_opts("/open", {depth = 0}))
  if ready.transport_ok then break end
  os.execute("sleep 0.1")
end
assert(ready.transport_ok == true, ready.error and ready.error.message)

local open_mkcol = webdav.mkcol(request_opts("/open/public"))
assert(open_mkcol.ok == true, open_mkcol.error and open_mkcol.error.message)
assert(open_mkcol.status == 201)
local open_put = webdav.put(request_opts("/open/public/readme.txt", {
  body = "open webdav\n",
}))
assert(open_put.ok == true, open_put.error and open_put.error.message)
local open_read = webdav.get(request_opts("/open/public/readme.txt"))
assert(open_read.ok == true, open_read.error and open_read.error.message)
assert(open_read.body == "open webdav\n")

local native_required = webdav.get(request_opts("/native/protected.txt"))
assert(native_required.ok == false)
assert(native_required.transport_ok == true)
assert(native_required.status == 401)
assert(native_required.headers:lower():find('www-authenticate: basic realm="native-dav"', 1, true))
local native_put = webdav.put(request_opts("/native/protected.txt", {
  body = "native protected\n",
  authorization = basic_auth,
}))
assert(native_put.ok == true, native_put.error and native_put.error.message)
local native_read = webdav.get(request_opts("/native/protected.txt", {
  authorization = basic_auth,
}))
assert(native_read.ok == true, native_read.error and native_read.error.message)
assert(native_read.body == "native protected\n")

local callback_required = webdav.get(request_opts("/callback/allowed.txt"))
assert(callback_required.ok == false)
assert(callback_required.transport_ok == true)
assert(callback_required.status == 401)
assert(callback_required.body == "callback login required\n")
assert(callback_required.headers:lower():find('www-authenticate: bearer realm="callback-dav"', 1, true))
local callback_put = webdav.put(request_opts("/callback/allowed.txt", {
  body = "callback protected\n",
  authorization = "Bearer callback-dav",
}))
assert(callback_put.ok == true, callback_put.error and callback_put.error.message)
local callback_read = webdav.get(request_opts("/callback/allowed.txt", {
  authorization = "Bearer callback-dav",
}))
assert(callback_read.ok == true, callback_read.error and callback_read.error.message)
assert(callback_read.body == "callback protected\n")

assert(server:stop() == true)
server:close()
print("vectis-webdav-server-ok")
]=] script_body @ONLY)
file(WRITE "${script}" "${script_body}")

vectis_run_command_with_port(
  LABEL "vectis Lua WebDAV server"
  PORT_ENV "VECTIS_WEBDAV_SERVER_PORT"
  SUCCESS_MARKER "vectis-webdav-server-ok"
  COMMAND "${VECTIS_BIN}" "${script}")
