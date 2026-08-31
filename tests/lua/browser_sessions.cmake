include("${CMAKE_CURRENT_LIST_DIR}/port_retry.cmake")

set(auth_path "${WORK_DIR}/vectis-browser-session-auth.json")
set(pouch_dir "${WORK_DIR}/vectis-browser-session-pouch")
set(script "${WORK_DIR}/vectis-browser-session.lua")

file(REMOVE "${auth_path}" "${auth_path}.lock")
file(REMOVE_RECURSE "${pouch_dir}")
file(MAKE_DIRECTORY "${pouch_dir}")
vectis_pick_test_port(port)

string(CONFIGURE [=[
local curl = require("curl")
local vectis = require("vectis")

local port_text = assert(arg[1], "port is required")
local port = tonumber(port_text)
local auth_path = assert(arg[2], "auth path is required")
local pouch_dir = assert(arg[3], "pouch directory is required")
local cert_path = pouch_dir .. "/browser-session.pem"
local base = "https://localhost:" .. tostring(port)
local session = {
  mode = "m2m_and_browser",
  cookie_name = "lua_browser_session",
  purpose = "lua-browser",
  state_key = "lua.browser-session",
  ttl_seconds = 60,
}
local flow_session = {
  mode = "m2m_and_browser",
  cookie_name = "lua_flow_session",
  purpose = "lua-flow-browser",
  state_key = "lua.flow-browser-session",
  ttl_seconds = 60,
}

local function request(path, method, body, headers)
  return curl.perform({
    url = base .. path,
    method = method,
    body = body,
    headers = headers,
    protocols = "https",
    timeout_ms = 2000,
    connect_timeout_ms = 500,
    verify_peer = false,
    verify_host = false,
    no_signal = true,
  })
end

assert(vectis.auth.store_init({credentials_path = auth_path}) == true)
assert(vectis.auth.user_add({
  credentials_path = auth_path,
  username = "lua-session-user",
  password = "lua-session-password",
}).username == "lua-session-user")
assert(vectis.auth.user_add({
  credentials_path = auth_path,
  username = "lua-flow-user",
  password = "lua-flow-password",
  totp_secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ",
}).username == "lua-flow-user")
assert(vectis.cert.generate_bundle({
  common_name = "localhost",
  ip_addresses = "127.0.0.1",
  output_bundle_path = cert_path,
  key_bits = 2048,
  valid_days = 1,
}) == true)

local server = assert(vectis.app.new({
  bind = "127.0.0.1",
  port = port,
  tls = {
    mode = "manual",
    cert_key_bundle_path = cert_path,
    domain = "localhost",
  },
  lockd = {
    endpoints = {"pouch://" .. pouch_dir .. "?single_writer=false"},
  },
}))
assert(server:auth_routes({
  path_prefix = "/auth",
  credentials_path = auth_path,
  browser_session = session,
}) == true)
assert(server:auth_routes({
  path_prefix = "/flow",
  credentials_path = auth_path,
  steps = {"password", "totp"},
  browser_session = flow_session,
}) == true)
assert(server:auth_routes({
  path_prefix = "/flow/admin",
  credentials_path = auth_path,
  steps = {"password", "totp"},
  browser_session = flow_session,
}) == true)
assert(server:auth_json({
  path = "/callback-protected",
  body = '{"session":"accepted"}\n',
  auth = {
    kind = "callback",
    browser_session = session,
    callback = function(provider_request)
      if provider_request.cookie ~= nil then
        return {
          action = "deny",
          status_code = 500,
          body = "cookie leaked to Lua\n",
        }
      end
      return {
        action = "required",
        status_code = 401,
        www_authenticate = 'Bearer realm="lua-browser"',
        body = "callback invoked\n",
      }
    end,
  },
}) == true)
assert(server:start() == true)

local ready
for _ = 1, 30 do
  ready = request("/auth/login", "GET")
  if ready.ok then break end
  assert(vectis.sleep_ms(100) == true)
end
assert(ready.ok == true, ready.error)
assert(ready.status == 200)

local form = "username=lua-session-user&password=lua-session-password"
local m2m_login = request("/auth/continue", "POST", form, {
  ["Content-Type"] = "application/x-www-form-urlencoded",
  ["Accept"] = "text/html",
})
assert(m2m_login.ok == true, m2m_login.error)
assert(m2m_login.status == 403)
assert(m2m_login.headers:lower():find("set-cookie:", 1, true) == nil)

local browser_login = request("/auth/continue", "POST", form, {
  ["Content-Type"] = "application/x-www-form-urlencoded",
  ["Accept"] = "text/html",
  ["Sec-Fetch-Mode"] = "navigate",
  ["Sec-Fetch-Dest"] = "document",
  ["Sec-Fetch-Site"] = "same-origin",
})
assert(browser_login.ok == true, browser_login.error)
assert(browser_login.status == 303)
local set_cookie = assert(browser_login.headers:match(
    "[Ss]et%-[Cc]ookie:%s*([^\r\n]+)"), browser_login.headers)
assert(set_cookie:find("HttpOnly", 1, true))
assert(set_cookie:find("Secure", 1, true))
assert(set_cookie:find("SameSite=Strict", 1, true))
local cookie = assert(set_cookie:match("^([^;]+)"))

local function workflow_cookie(response)
  local header = assert(response.headers:match(
      "[Ss]et%-[Cc]ookie:%s*([^\r\n]+)"), response.headers)
  local value = assert(header:match("^([^;]+)"))
  local name = assert(value:match("^([^=]+)="))
  assert(name:match("^vectis_auth_flow_[0-9a-f]+$"))
  return value, name
end

local navigation_headers = {
  ["Content-Type"] = "application/x-www-form-urlencoded",
  ["Accept"] = "text/html",
  ["Sec-Fetch-Mode"] = "navigate",
  ["Sec-Fetch-Dest"] = "document",
  ["Sec-Fetch-Site"] = "same-origin",
}
local parent_flow = request("/flow/continue", "POST",
    "username=lua-flow-user&password=lua-flow-password", navigation_headers)
assert(parent_flow.ok == true, parent_flow.error)
assert(parent_flow.status == 303)
local parent_flow_cookie, parent_flow_name = workflow_cookie(parent_flow)
local child_flow = request("/flow/admin/continue", "POST",
    "username=lua-flow-user&password=lua-flow-password", navigation_headers)
assert(child_flow.ok == true, child_flow.error)
assert(child_flow.status == 303)
local child_flow_cookie, child_flow_name = workflow_cookie(child_flow)
assert(parent_flow_name ~= child_flow_name)
local flow_totp = assert(vectis.auth.totp.new(
    "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"))
local child_complete = request("/flow/admin/continue", "POST",
    "totp_code=" .. flow_totp:generate(os.time()), {
      ["Content-Type"] = "application/x-www-form-urlencoded",
      ["Accept"] = "text/html",
      ["Sec-Fetch-Mode"] = "navigate",
      ["Sec-Fetch-Dest"] = "document",
      ["Sec-Fetch-Site"] = "same-origin",
      ["Cookie"] = parent_flow_cookie .. "; " .. child_flow_cookie,
    })
assert(child_complete.ok == true, child_complete.error)
assert(child_complete.status == 303,
       "nested workflow status=" .. tostring(child_complete.status) .. " body=" ..
           tostring(child_complete.body) .. " headers=" ..
           tostring(child_complete.headers))

local session_response = request("/callback-protected", "GET", nil, {
  ["Cookie"] = cookie,
})
assert(session_response.ok == true, session_response.error)
assert(session_response.status == 200)
assert(session_response.body == '{"session":"accepted"}\n')

local bad_cookie = cookie:sub(1, -2) .. (cookie:sub(-1) == "0" and "1" or "0")
local invalid_response = request("/callback-protected", "GET", nil, {
  ["Cookie"] = bad_cookie,
})
assert(invalid_response.ok == true, invalid_response.error)
assert(invalid_response.status == 401)
assert(invalid_response.body == "callback invoked\n")

local logout = request("/auth/logout", "POST", "", {
  ["Content-Type"] = "application/x-www-form-urlencoded",
  ["Cookie"] = cookie,
})
assert(logout.ok == true, logout.error)
assert(logout.status == 200,
       "logout status=" .. tostring(logout.status) .. "\n" ..
           (logout.body or "") .. "\n" .. (logout.headers or ""))
assert(logout.headers:find("lua_browser_session=; Path=/; Max%-Age=0", 1))

local revoked_response = request("/callback-protected", "GET", nil, {
  ["Cookie"] = cookie,
})
assert(revoked_response.ok == true, revoked_response.error)
assert(revoked_response.status == 401)
assert(revoked_response.body == "callback invoked\n")

assert(server:stop() == true)
server:close()
print("vectis-lua-browser-sessions-ok")
]=] script_body @ONLY)
file(WRITE "${script}" "${script_body}")

execute_process(
  COMMAND "${VECTIS_BIN}" "${script}" "${port}" "${auth_path}" "${pouch_dir}"
  RESULT_VARIABLE browser_session_result
  OUTPUT_VARIABLE browser_session_stdout
  ERROR_VARIABLE browser_session_stderr)
if(NOT browser_session_result EQUAL 0)
  message(FATAL_ERROR
    "Lua browser-session test failed: ${browser_session_stdout}${browser_session_stderr}")
endif()
if(NOT browser_session_stdout MATCHES "vectis-lua-browser-sessions-ok")
  message(FATAL_ERROR
    "Lua browser-session test missed success marker: ${browser_session_stdout}${browser_session_stderr}")
endif()
