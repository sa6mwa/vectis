local vectis = require("vectis")

local bind = os.getenv("VECTIS_LUA_SITE_BIND") or "127.0.0.1"
local port = tonumber(os.getenv("VECTIS_LUA_SITE_PORT") or "28630")
local credentials_path = assert(os.getenv("VECTIS_LUA_SITE_CREDENTIALS"))
local auth_state_path = assert(os.getenv("VECTIS_LUA_SITE_AUTH_STATE"))
local asset_root = assert(os.getenv("VECTIS_LUA_SITE_ASSET_ROOT"))
local content_root = assert(os.getenv("VECTIS_LUA_SITE_CONTENT_ROOT"))
local cache_dir = assert(os.getenv("VECTIS_LUA_SITE_CACHE"))

assert(vectis.mkdir_p(content_root) == true)
assert(vectis.auth.store_init({
  credentials_path = credentials_path,
  auth_state_path = auth_state_path,
}) == true)
assert(vectis.auth.user_add({
  credentials_path = credentials_path,
  username = "site-admin",
  password = "site-password",
}).username == "site-admin")

local native_auth = {
  kind = "native",
  credentials_path = credentials_path,
  realm = "lua-site-example",
  purpose = "webdav",
}

local app = assert(vectis.app.new({
  app_name = "lua-site-example",
  bind = bind,
  port = port,
  tls = { mode = "disabled" },
}))

assert(app:static_directory({
  path_prefix = "/assets",
  root_dir = asset_root,
}) == true)
assert(app:static_directory({
  path_prefix = "/published",
  root_dir = content_root,
  content_type = "text/plain; charset=utf-8",
}) == true)
assert(app:auth_routes({
  path_prefix = "/auth",
  credentials_path = credentials_path,
  auth_state_path = auth_state_path,
  realm = "lua-site-example",
  login_title = "Lua Site Login",
  required_factors = {"password"},
}) == true)

assert(app:route({
  path = "/",
  handler = function()
    return {
      content_type = "text/html; charset=utf-8",
      body = [[<!doctype html>
<html><head><title>Lua Site</title><link rel="stylesheet" href="/assets/site.css"></head>
<body><main id="site-home"><h1>Lua Site</h1>
<form id="contact-form" action="/contact" method="post">
<label>Name <input name="name"></label><label>Message <textarea name="message"></textarea></label>
<button type="submit">Send</button></form></main></body></html>
]],
    }
  end,
}) == true)

assert(app:route({
  path = "/contact",
  methods = {"POST"},
  body = {
    mode = "buffered",
    max_bytes = 256,
  },
  handler = function(request)
    local body = request.body or ""
    local name = body:match("^name=([^&]*)") or body:match("&name=([^&]*)")
    local message = body:match("^message=([^&]*)") or
        body:match("&message=([^&]*)")
    if name ~= nil then
      name = name:gsub("+", " "):gsub("%%(%x%x)", function(hex)
        return string.char(tonumber(hex, 16))
      end)
    end
    if message ~= nil then
      message = message:gsub("+", " "):gsub("%%(%x%x)", function(hex)
        return string.char(tonumber(hex, 16))
      end)
    end
    if name == nil or name == "" or message == nil or message == "" then
      return {
        status = 400,
        content_type = "text/plain; charset=utf-8",
        body = "name and message are required\n",
      }
    end
    if #name > 64 or #message > 160 then
      return {
        status = 422,
        content_type = "text/plain; charset=utf-8",
        body = "message is too long\n",
      }
    end
    return {
      status = 201,
      content_type = "text/html; charset=utf-8",
      headers = { ["cache-control"] = "no-store" },
      body = "<!doctype html><html><body><main id=\"contact-received\">" ..
          "Thanks for contacting Lua Site.</main></body></html>\n",
    }
  end,
}) == true)

assert(app:route({
  path = "/admin",
  auth = native_auth,
  handler = function(request)
    return {
      content_type = "text/html; charset=utf-8",
      body = "<!doctype html><html><body><main id=\"site-admin\">" ..
          "Authenticated editor: " .. (request.principal or "unknown") ..
          "</main></body></html>\n",
    }
  end,
}) == true)

local webdav_mounted, webdav_mount_error = app:webdav({
  path_prefix = "/content",
  cache_dir = cache_dir,
  site_id = "lua-site-example",
  root_dir = content_root,
  max_file_bytes = 1024,
  max_total_bytes = 4096,
  max_resources = 16,
  conceal_unauthorized = false,
  auth = native_auth,
})
assert(webdav_mounted == true,
       webdav_mount_error and webdav_mount_error.message or "WebDAV mount failed")

assert(app:start() == true)
print("lua site example listening on http://" .. bind .. ":" .. tostring(port))
assert(app:wait() == true)
app:close()
