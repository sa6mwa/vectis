if(NOT DEFINED VECTIS_HOST_LUA)
  message(FATAL_ERROR "VECTIS_HOST_LUA is required")
endif()
if(NOT DEFINED VECTIS_SOURCE_DIR)
  message(FATAL_ERROR "VECTIS_SOURCE_DIR is required")
endif()
if(NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "WORK_DIR is required")
endif()

set(script "${WORK_DIR}/vectis-top-level-module.lua")
file(WRITE "${script}" [=[
local lua_dir = assert(arg[1])
package.path = lua_dir .. "/?.lua;" .. lua_dir .. "/?/init.lua;" ..
               package.path

local dependency_names = {
  "lockdc",
  "lonejson",
  "pslog",
  "lql",
  "cai",
  "libmdf",
  "softline",
  "curl",
  "opcua",
  "openssl",
  "zlib",
  "audio",
  "sus",
}

for _, name in ipairs(dependency_names) do
  local module = { __name = name }
  if name == "lonejson" then
    module.encode_json = function() return "{}" end
    module.decode_value = function() return {} end
  end
  package.preload[name] = function()
    return module
  end
end

package.preload["vectis.auth.core"] = function()
  return {
    __name = "vectis.auth.core",
    BASIC = 1,
    provider_native = function(opts)
      return {
        kind = "native",
        config = opts,
        credentials_path = opts.credentials_path,
        state_path = opts.state_path,
        purpose = opts.purpose,
        realm = opts.realm,
        allowed_modes = opts.allowed_modes,
      }
    end,
    webdav_key = function(opts)
      return {
        client_id = opts.username,
        client_secret = opts.password,
      }
    end,
    basic_authorization = function(key)
      return "Basic " .. key.client_id .. ":" .. key.client_secret
    end,
  }
end

local c_owned = {
  "vectis.cert",
  "vectis.embedded",
  "vectis.server",
  "vectis.curl_worker",
  "vectis.cai_worker",
  "vectis.audio_worker",
  "vectis.sus_worker",
  "vectis.ssh",
  "vectis.dsv.core",
  "vectis.xml.core",
}

for _, name in ipairs(c_owned) do
  local module = { __name = name }
  package.preload[name] = function()
    return module
  end
end

local vectis = require("vectis")
local status = require("vectis.status")

assert(package.loaded["vectis"] == vectis)
assert(package.loaded["vectis.status"] == status)
assert(vectis.status == status)
assert(vectis.version == "0.0.0")
assert(vectis.OK == status.OK)
assert(vectis.ERR_STATE == status.ERR_STATE)
assert(vectis.ERROR_SOURCE_VECTIS == status.ERROR_SOURCE_VECTIS)
assert(vectis.status_string(vectis.OK) == "ok")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_VECTIS) == "vectis")

for _, name in ipairs(dependency_names) do
  assert(vectis.libs[name] == require(name), name)
end

assert(vectis.auth == require("vectis.auth"))
assert(vectis.auth.core == require("vectis.auth.core"))
assert(vectis.cert == require("vectis.cert"))
assert(vectis.embedded == require("vectis.embedded"))
assert(vectis.server == require("vectis.server"))
assert(vectis.curl_worker == require("vectis.curl_worker"))
assert(vectis.cai_worker == require("vectis.cai_worker"))
assert(vectis.audio_worker == require("vectis.audio_worker"))
assert(vectis.sus_worker == require("vectis.sus_worker"))
assert(vectis.ssh == require("vectis.ssh"))
assert(vectis.log == require("vectis.log"))
assert(vectis.http == require("vectis.http"))
assert(vectis.webdav == require("vectis.webdav"))
assert(vectis.rest == require("vectis.rest"))
assert(vectis.terminal == require("vectis.terminal"))
assert(vectis.mqtt == require("vectis.mqtt"))
assert(vectis.smtp == require("vectis.smtp"))
assert(vectis.lockd == require("vectis.lockd"))
assert(vectis.dsv == require("vectis.dsv"))
assert(vectis.xml == require("vectis.xml"))

local flow = vectis.auth.browser_flow({
  credentials_path = "credentials.json",
  state_path = "state.json",
  path_prefix = "/_vectis/auth",
  realm = "admin",
  purpose = "webdav",
  allowed_modes = { vectis.auth.BASIC },
})
local routes = flow:routes({ login_title = "Admin Login" })
assert(routes.credentials_path == "credentials.json")
assert(routes.state_path == "state.json")
assert(routes.path_prefix == "/_vectis/auth")
assert(routes.realm == "admin")
assert(routes.login_title == "Admin Login")
assert(routes.purpose == nil)
local provider = assert(flow:provider())
assert(provider.kind == "native")
assert(provider.credentials_path == "credentials.json")
assert(provider.state_path == "state.json")
assert(provider.purpose == "webdav")
assert(provider.realm == "admin")
local authorization = assert(flow:webdav_authorization({
  username = "admin",
  password = "secret",
}))
assert(authorization == "Basic admin:secret")
local mounted
local server = {
  auth_routes = function(self, opts)
    assert(self ~= nil)
    mounted = opts
    return true
  end
}
assert(flow:mount(server, { path_prefix = "/login" }) == true)
assert(mounted.path_prefix == "/login")

assert(vectis.has_embedded_lockd_bundle() == false)
assert(vectis.embedded_lockd_bundle_size() == 0)
local source, err = vectis.embedded_lockd_bundle_source()
assert(source == nil)
assert(err.status == vectis.ERR_STATE)
assert(err.source_code == vectis.ERROR_SOURCE_VECTIS)
assert(err.message == "no embedded lockd bundle")

print("vectis top-level Lua module ok")
]=])

execute_process(
  COMMAND "${VECTIS_HOST_LUA}" "${script}" "${VECTIS_SOURCE_DIR}/lua"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "vectis top-level Lua module failed: ${stdout}${stderr}")
endif()
if(NOT stdout MATCHES "vectis top-level Lua module ok")
  message(FATAL_ERROR "vectis top-level Lua module did not report success")
endif()
