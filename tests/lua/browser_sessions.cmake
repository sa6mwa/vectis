include("${CMAKE_CURRENT_LIST_DIR}/port_retry.cmake")

set(pouch_dir "${WORK_DIR}/vectis-browser-session-pouch")
set(script "${WORK_DIR}/vectis-browser-session.lua")

file(REMOVE_RECURSE "${pouch_dir}")
file(MAKE_DIRECTORY "${pouch_dir}")
vectis_pick_test_port(port)

execute_process(
  COMMAND "${VECTIS_BIN}" -a users
          --lockd-endpoint "pouch://${pouch_dir}?single_writer=false"
          --add "lua-session-user" --password "lua-session-password"
          --email "lua-session-user@example.test"
  RESULT_VARIABLE seed_user_result
  OUTPUT_VARIABLE seed_user_output
  ERROR_VARIABLE seed_user_error)
if(NOT seed_user_result EQUAL 0)
  message(FATAL_ERROR "failed to seed browser-session user: ${seed_user_error}")
endif()
execute_process(
  COMMAND "${VECTIS_BIN}" -a users
          --lockd-endpoint "pouch://${pouch_dir}?single_writer=false"
          --add "lua-flow-user" --password "lua-flow-password"
          --totp-secret "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
  RESULT_VARIABLE seed_flow_user_result
  OUTPUT_VARIABLE seed_flow_user_output
  ERROR_VARIABLE seed_flow_user_error)
if(NOT seed_flow_user_result EQUAL 0)
  message(FATAL_ERROR "failed to seed browser-flow user: ${seed_flow_user_error}")
endif()

string(CONFIGURE [=[
local curl = require("curl")
local vectis = require("vectis")

local port_text = assert(arg[1], "port is required")
local port = tonumber(port_text)
local pouch_dir = assert(arg[2], "pouch directory is required")
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

assert(vectis.cert.generate_bundle({
  common_name = "localhost",
  ip_addresses = "127.0.0.1",
  output_bundle_path = cert_path,
  key_bits = 2048,
  valid_days = 1,
}) == true)

local server = assert(vectis.app.new({
  profile = "production_webserver",
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
local covered_cookie_route, covered_cookie_route_err = server:auth_routes({
  path_prefix = "/app/auth",
  state_key = "auth/v1/store",
  browser_session = {
    mode = "m2m_and_browser",
    cookie_path = "/app",
    purpose = "lua-covered-browser",
    state_key = "lua.covered-browser-session",
  },
})
assert(covered_cookie_route == true, covered_cookie_route_err and
    covered_cookie_route_err.message)
local invalid_cookie_route, invalid_cookie_route_err = server:auth_routes({
  path_prefix = "/invalid-cookie-path",
  state_key = "auth/v1/store",
  browser_session = {
    mode = "m2m_and_browser",
    cookie_path = "/app",
    purpose = "lua-invalid-browser",
    state_key = "lua.invalid-browser-session",
  },
})
assert(invalid_cookie_route == nil)
assert(type(invalid_cookie_route_err) == "table")
assert(invalid_cookie_route_err.status == vectis.ERR_INVALID)
assert(invalid_cookie_route_err.message:find("cookie_path", 1, true))
assert(server:auth_routes({
  path_prefix = "/auth",
  state_key = "auth/v1/store",
  browser_session = session,
}) == true)
assert(server:auth_routes({
  path_prefix = "/flow",
  state_key = "auth/v1/store",
  steps = {"password", "totp"},
  browser_session = flow_session,
}) == true)
assert(server:auth_routes({
  path_prefix = "/flow/admin",
  state_key = "auth/v1/store",
  steps = {"password", "totp"},
  browser_session = flow_session,
}) == true)
assert(server:auth_routes({
  path_prefix = "/email-flow",
  state_key = "auth/v1/store",
  steps = {"email_code", "password", "totp"},
  email_smtp = {
    url = "smtp://127.0.0.1:1",
    mail_from = "login@example.test",
  },
  browser_session = flow_session,
}) == true)
local redirect_flow = assert(vectis.auth.workflow({
  state_key = "auth/v1/store",
  path_prefix = "/redirect-flow",
  purpose = "stats",
  steps = {"password"},
  browser_session = flow_session,
}))
assert(redirect_flow:mount(server) == true)
local stats_route, stats_route_err = server:auth_json({
  path = "/.stats",
  body = '{"stats":true}\n',
  auth = redirect_flow:provider({app = server}),
})
assert(stats_route == true,
       stats_route_err and stats_route_err.message or "stats route failed")
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
local start_ok, start_err = server:start()
assert(start_ok == true, start_err and start_err.message or "server start failed")

local ready
for _ = 1, 30 do
  ready = request("/auth/login", "GET")
  if ready.ok then break end
  assert(vectis.sleep_ms(100) == true)
end
assert(ready.ok == true, ready.error)
assert(ready.status == 200)

local email_flow_page = request("/email-flow/login", "GET")
assert(email_flow_page.ok == true, email_flow_page.error)
assert(email_flow_page.status == 200)
assert(email_flow_page.body:find("Enter your email address", 1, true),
       email_flow_page.body)
assert(email_flow_page.body:find("Email address", 1, true))
assert(email_flow_page.body:find("Step ", 1, true) == nil)
assert(email_flow_page.body:find("email code", 1, true) == nil)
assert(email_flow_page.body:find("password", 1, true) == nil)
assert(email_flow_page.body:find("authenticator code", 1, true) == nil)

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
assert(browser_login.headers:lower():find("location: /", 1, true))
local set_cookie = assert(browser_login.headers:match(
    "[Ss]et%-[Cc]ookie:%s*(lua_browser_session=[^\r\n]+)"), browser_login.headers)
local cleared_workflow = assert(browser_login.headers:match(
    "[Ss]et%-[Cc]ookie:%s*(vectis_auth_flow_[^\r\n]+)"), browser_login.headers)
assert(cleared_workflow:find("Max-Age=0", 1, true), cleared_workflow)
assert(set_cookie:find("HttpOnly", 1, true))
assert(set_cookie:find("Secure", 1, true))
assert(set_cookie:find("SameSite=Strict", 1, true))
local cookie = assert(set_cookie:match("^([^;]+)"))

local navigation_headers = {
  ["Content-Type"] = "application/x-www-form-urlencoded",
  ["Accept"] = "text/html",
  ["Sec-Fetch-Mode"] = "navigate",
  ["Sec-Fetch-Dest"] = "document",
  ["Sec-Fetch-Site"] = "same-origin",
}
local protected_redirect = request("/.stats", "GET", nil, {
  ["Accept"] = "text/html",
})
assert(protected_redirect.ok == true, protected_redirect.error)
assert(protected_redirect.status == 303)
assert(protected_redirect.headers:lower():find(
    "location: /redirect-flow/login?return=/.stats", 1, true),
    protected_redirect.headers)
local protected_query_redirect = request("/.stats?view=detail&tag=x", "GET", nil, {
  ["Accept"] = "text/html",
})
assert(protected_query_redirect.ok == true, protected_query_redirect.error)
assert(protected_query_redirect.status == 303)
assert(protected_query_redirect.headers:lower():find(
    "location: /redirect-flow/login?return=/.stats%3f%2576%2569%2565%2577%3d%2564%2565%2574%2561%2569%256c%26%2574%2561%2567%3d%2578", 1, true),
    protected_query_redirect.headers)
local escaped_query_redirect = request("/.stats?tag=a%26b%23c%2Bd", "GET", nil, {
  ["Accept"] = "text/html",
})
assert(escaped_query_redirect.status == 303)
assert(escaped_query_redirect.headers:lower():find(
    "return=/.stats%3f%2574%2561%2567%3d%2561%2526%2562%2523%2563%252b%2564", 1, true),
    escaped_query_redirect.headers)
local redirect_complete = request("/redirect-flow/continue", "POST",
    "username=lua-session-user&password=lua-session-password&return=/.stats",
    navigation_headers)
assert(redirect_complete.ok == true, redirect_complete.error)
assert(redirect_complete.status == 303)
assert(redirect_complete.headers:lower():find("location: /.stats", 1, true),
       redirect_complete.headers)
local redirect_session_cookie = assert(redirect_complete.headers:match(
    "[Ss]et%-[Cc]ookie:%s*(lua_flow_session=[^;]+)"),
    redirect_complete.headers)
local protected_page = request("/.stats", "GET", nil, {
  ["Cookie"] = redirect_session_cookie,
})
assert(protected_page.ok == true, protected_page.error)
assert(protected_page.status == 200)
assert(protected_page.body == '{"stats":true}\n')

local function workflow_cookie(response)
  local header = assert(response.headers:match(
    "[Ss]et%-[Cc]ookie:%s*([^\r\n]+)"), response.headers)
  local value = assert(header:match("^([^;]+)"))
  local name = assert(value:match("^([^=]+)="))
  assert(name:match("^vectis_auth_flow_[0-9a-f]+$"))
  return value, name
end

local function assert_six_cell_code_input(page, field_name, character_pattern)
  local cell_count

  assert(page.body:find('name="' .. field_name .. '" data%-otp%-value'))
  assert(page.body:find('class="otp%-cells"', 1))
  assert(page.body:find('class="otp%-cell" type="password"', 1))
  _, cell_count = page.body:gsub('class="otp%-cell"', '')
  assert(cell_count == 6, "expected six code cells, got " .. cell_count)
  assert(page.body:find("allowed=/" .. character_pattern .. "/", 1, true))
  assert(page.body:find('clipboardData', 1, true))
  assert(page.body:find('form.requestSubmit()', 1, true))
end

local email_flow_start = request("/email-flow/continue", "POST",
    "email=lua-session-user%40example.test", navigation_headers)
assert(email_flow_start.ok == true, email_flow_start.error)
assert(email_flow_start.status == 303)
local email_flow_cookie = workflow_cookie(email_flow_start)
local email_code_page = request("/email-flow/login", "GET", nil, {
  ["Cookie"] = email_flow_cookie,
})
assert(email_code_page.ok == true, email_code_page.error)
assert(email_code_page.status == 200)
assert(email_code_page.body:find("Enter your email code", 1, true))
assert_six_cell_code_input(email_code_page, "email_token", "[A-Za-z0-9]")

local parent_flow = request("/flow/continue", "POST",
    "username=lua-flow-user&password=lua-flow-password", navigation_headers)
assert(parent_flow.ok == true, parent_flow.error)
assert(parent_flow.status == 303)
local parent_flow_cookie, parent_flow_name = workflow_cookie(parent_flow)
local totp_code_page = request("/flow/login", "GET", nil, {
  ["Cookie"] = parent_flow_cookie,
})
assert(totp_code_page.ok == true, totp_code_page.error)
assert(totp_code_page.status == 200)
assert(totp_code_page.body:find("Enter your authenticator code", 1, true))
assert_six_cell_code_input(totp_code_page, "totp_code", "[0-9]")
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

-- Failures retain the generic current-step page and count toward the default
-- production HTTP 403 limit (10), including the navigation rejection above.
local function rejected(path, body, headers)
  local result = request(path, "POST", body, headers or navigation_headers)
  assert(result.ok == true, result.error)
  assert(result.status == 403, tostring(result.status) .. " " .. result.body)
  assert(result.body:find("Those details could not be verified. Try again.", 1, true))
  assert(result.body:find("Step ", 1, true) == nil)
  return result
end
local wrong_password = rejected("/auth/continue",
    "username=lua-session-user&password=wrong")
local unknown_user = rejected("/auth/continue",
    "username=nonexistent-user&password=wrong")
assert(wrong_password.body == unknown_user.body)
local totp_headers = {}
for key, value in pairs(navigation_headers) do totp_headers[key] = value end
totp_headers.Cookie = parent_flow_cookie
local wrong_totp = rejected("/flow/continue", "totp_code=invalid", totp_headers)
assert(wrong_totp.body:find("Enter your authenticator code", 1, true))
for _ = 1, 6 do
  rejected("/auth/continue", "username=lua-session-user&password=wrong")
end
local blocked = request("/auth/continue", "POST", form, navigation_headers)
assert(blocked.ok == false, "autoblock must reject even valid credentials after the limit")

assert(server:stop() == true)
server:close()
print("vectis-lua-browser-sessions-ok")
]=] script_body @ONLY)
file(WRITE "${script}" "${script_body}")

execute_process(
  COMMAND "${VECTIS_BIN}" "${script}" "${port}" "${pouch_dir}"
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
