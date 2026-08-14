set(script "${WORK_DIR}/facade-contracts.lua")

file(WRITE "${script}" [[
local vectis = require("vectis")
local dsv = require("vectis.dsv")
local http = require("vectis.http")
local rest = require("vectis.rest")
local status = require("vectis.status")
local lonejson = require("lonejson")
local mqtt = require("vectis.mqtt")
local smtp = require("vectis.smtp")
local webdav = require("vectis.webdav")
local lockd = require("vectis.lockd")
local xml = require("vectis.xml")

local work_dir = assert(arg[1], "work directory argument is required")
local auth_store = work_dir .. "/facade-auth-credentials.json"
local auth_state = work_dir .. "/facade-auth-state.json"
os.remove(auth_store)
os.remove(auth_store .. ".lock")
os.remove(auth_state)
os.remove(auth_state .. ".lock")

local function assert_status_error(err, status, message_fragment)
  assert(type(err) == "table")
  assert(err.status == status)
  assert(err.status_string == vectis.status_string(status))
  assert(type(err.message) == "string")
  assert(type(err.source) == "string")
  assert(type(err.source_code) == "number")
  assert(err.source == vectis.error_source_string(err.source_code))
  if message_fragment ~= nil then
    assert(err.message:find(message_fragment, 1, true), err.message)
  end
end

assert(vectis.status_string(vectis.ERR_INVALID) == "invalid")
assert(vectis.status_string(vectis.ERR_STATE) == "state")
assert(vectis.status_string(vectis.ERR_TIMEOUT) == "timeout")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_NONE) == "none")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_VECTIS) == "vectis")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_LOCKDC) == "lockdc")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_LONEJSON) == "lonejson")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_LIBSSH2) == "libssh2")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_CPKT) == "cpkt")
assert(vectis.status == status)
assert(status.status_string(status.ERR_TIMEOUT) == "timeout")
assert(status.error_source_string(status.ERROR_SOURCE_CURL) == "curl")
assert(status.error_source_string(status.ERROR_SOURCE_CPKT) == "cpkt")

assert(vectis.auth.store_init({
  credentials_path = auth_store,
  state_path = auth_state,
}) == true)

local user = assert(vectis.auth.user_add({
  credentials_path = auth_store,
  username = "facade-admin@example.com",
  password = "facade-password",
  totp_secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ",
  totp_label = "Vectis:facade-admin@example.com",
  issuer = "Vectis",
}))
assert(user.username == "facade-admin@example.com")
assert(user.totp_secret == "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ")
assert(type(user.totp_uri) == "string")
assert(user.totp_uri:find("otpauth://totp/", 1, true))
assert(type(user.totp_qr) == "string")

local missing_totp = assert(vectis.auth.user_login({
  credentials_path = auth_store,
  username = "facade-admin@example.com",
  password = "facade-password",
}))
assert(missing_totp.authenticated == false)

local login = assert(vectis.auth.user_login({
  credentials_path = auth_store,
  username = "facade-admin@example.com",
  password = "facade-password",
  totp_code = "287082",
  time = 59,
  window = 0,
}))
assert(login.authenticated == true)

local webdav_key = assert(vectis.auth.webdav_key({
  credentials_path = auth_store,
  username = "facade-admin@example.com",
  password = "facade-password",
  totp_code = "287082",
  time = 59,
  window = 0,
}))
assert(type(webdav_key.client_id) == "string")
assert(type(webdav_key.client_secret) == "string")
assert(webdav_key.api_key == nil)
assert(type(webdav_key.claim_json) == "string")

local authorization = assert(vectis.auth.basic_authorization(webdav_key))
assert(authorization:find("Basic ", 1, true) == 1)

local verified = assert(vectis.auth.verify({
  credentials_path = auth_store,
  authorization = authorization,
  allowed_modes = { "basic" },
}))
assert(verified.authenticated == true)
assert(verified.auth_mode == "basic")
assert(verified.client_id == webdav_key.client_id)

local native_provider = assert(vectis.auth.provider_native({
  credentials_path = auth_store,
  purpose = "webdav",
  realm = "facade",
  allowed_modes = { vectis.auth.BASIC },
}))
assert(native_provider.kind == "native")
local required = assert(native_provider:authenticate({
  resource = "/dav",
  allowed_modes = "basic",
}))
assert(required.action == "required")
assert(required.status_code == 401)
assert(type(required.www_authenticate) == "string")
assert(required.www_authenticate:find("Basic", 1, true))

local allowed = assert(native_provider:authenticate({
  authorization = authorization,
  resource = "/dav",
  allowed_modes = "basic",
}))
assert(allowed.action == "allow")
assert(allowed.result.authenticated == true)
assert(allowed.result.auth_mode == "basic")

local callback_seen = false
local callback_provider = vectis.auth.provider_callback(function(request)
  callback_seen = request.resource == "/admin" and
                  request.authorization == "Bearer callback"
  return {
    action = "redirect",
    status_code = 302,
    location = "/_vectis/auth/login",
    content_type = "text/plain",
    body = "login required",
    principal = "facade",
  }
end)
assert(callback_provider.kind == "callback")
local redirect = assert(callback_provider:authenticate({
  authorization = "Bearer callback",
  resource = "/admin",
}))
assert(callback_seen == true)
assert(redirect.action == "redirect")
assert(redirect.status_code == 302)
assert(redirect.location == "/_vectis/auth/login")
assert(redirect.content_type == "text/plain")
assert(redirect.body == "login required")
assert(redirect.principal == "facade")

local invalid_callback = vectis.auth.provider_callback(function()
  return { action = "challenge" }
end)
local invalid_response, invalid_response_err = invalid_callback:authenticate({})
assert(invalid_response == nil)
assert_status_error(invalid_response_err, vectis.ERR_INVALID,
                    "auth callback response action")

local email_token = assert(vectis.auth.email_token_issue({
  credentials_path = auth_store,
  state_path = auth_state,
  username = "email-user@example.com",
  realm = "facade",
  email = "email-user@example.com",
  transaction_id = "email-tx-1",
  token = "123456",
  now = 1000,
  ttl_seconds = 300,
}))
assert(email_token.transaction_id == "email-tx-1")
assert(email_token.token == "123456")
assert(email_token.expires_at == 1300)

local wrong_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_store,
  state_path = auth_state,
  transaction_id = "email-tx-1",
  username = "email-user@example.com",
  realm = "facade",
  token = "000000",
  now = 1100,
}))
assert(wrong_email_token.verified == false)
assert(wrong_email_token.expired == false)
assert(wrong_email_token.failed_attempts == 1)

local good_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_store,
  state_path = auth_state,
  transaction_id = "email-tx-1",
  username = "email-user@example.com",
  realm = "facade",
  token = "123456",
  now = 1100,
}))
assert(good_email_token.verified == true)
assert(good_email_token.expired == false)
assert(good_email_token.username == "email-user@example.com")
assert(good_email_token.realm == "facade")
assert(good_email_token.email == "email-user@example.com")

local replayed_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_store,
  state_path = auth_state,
  transaction_id = "email-tx-1",
  username = "email-user@example.com",
  realm = "facade",
  token = "123456",
  now = 1100,
}))
assert(replayed_email_token.verified == false)
assert(replayed_email_token.expired == false)

assert(vectis.auth.email_token_issue({
  credentials_path = auth_store,
  state_path = auth_state,
  username = "email-user@example.com",
  realm = "facade",
  email = "email-user@example.com",
  transaction_id = "email-tx-limited",
  token = "limited",
  now = 1300,
  ttl_seconds = 300,
  max_attempts = 2,
}))

local limited_wrong_one = assert(vectis.auth.email_token_verify({
  credentials_path = auth_store,
  state_path = auth_state,
  transaction_id = "email-tx-limited",
  username = "email-user@example.com",
  realm = "facade",
  token = "wrong-one",
  now = 1310,
}))
assert(limited_wrong_one.verified == false)
assert(limited_wrong_one.failed_attempts == 1)
assert(limited_wrong_one.max_attempts == 2)

local limited_wrong_two = assert(vectis.auth.email_token_verify({
  credentials_path = auth_store,
  state_path = auth_state,
  transaction_id = "email-tx-limited",
  username = "email-user@example.com",
  realm = "facade",
  token = "wrong-two",
  now = 1310,
}))
assert(limited_wrong_two.verified == false)
assert(limited_wrong_two.failed_attempts == 2)
assert(limited_wrong_two.max_attempts == 2)

local limited_consumed = assert(vectis.auth.email_token_verify({
  credentials_path = auth_store,
  state_path = auth_state,
  transaction_id = "email-tx-limited",
  username = "email-user@example.com",
  realm = "facade",
  token = "limited",
  now = 1310,
}))
assert(limited_consumed.verified == false)
assert(limited_consumed.expired == false)

assert(vectis.auth.email_token_issue({
  credentials_path = auth_store,
  state_path = auth_state,
  username = "email-user@example.com",
  realm = "facade",
  email = "email-user@example.com",
  pending_transaction_id = "pending-email-1",
  transaction_id = "email-tx-scoped",
  token = "abcdef",
  now = 1200,
  ttl_seconds = 300,
}))

local wrong_pending = assert(vectis.auth.email_token_verify({
  credentials_path = auth_store,
  state_path = auth_state,
  transaction_id = "email-tx-scoped",
  username = "email-user@example.com",
  realm = "facade",
  pending_transaction_id = "pending-email-other",
  token = "abcdef",
  now = 1210,
}))
assert(wrong_pending.verified == false)
assert(wrong_pending.expired == false)

local scoped_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_store,
  state_path = auth_state,
  transaction_id = "email-tx-scoped",
  username = "email-user@example.com",
  realm = "facade",
  pending_transaction_id = "pending-email-1",
  token = "abcdef",
  now = 1210,
}))
assert(scoped_email_token.verified == true)
assert(scoped_email_token.pending_transaction_id == "pending-email-1")

assert(vectis.auth.email_token_issue({
  credentials_path = auth_store,
  state_path = auth_state,
  username = "email-user@example.com",
  realm = "facade",
  email = "email-user@example.com",
  transaction_id = "email-tx-expired",
  token = "654321",
  now = 2000,
  ttl_seconds = 60,
}))

local expired_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_store,
  state_path = auth_state,
  transaction_id = "email-tx-expired",
  username = "email-user@example.com",
  realm = "facade",
  token = "654321",
  now = 2061,
}))
assert(expired_email_token.verified == false)
assert(expired_email_token.expired == true)

local consumed_expired_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_store,
  state_path = auth_state,
  transaction_id = "email-tx-expired",
  username = "email-user@example.com",
  realm = "facade",
  token = "654321",
  now = 2061,
}))
assert(consumed_expired_token.verified == false)
assert(consumed_expired_token.expired == false)

local verifier = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~facade"
local oidc = assert(vectis.auth.oidc_authorization({
  authorization_endpoint = "https://idp.example.test/authorize",
  client_id = "facade-client",
  redirect_uri = "http://127.0.0.1/callback",
  scope = "openid dav",
  state = "facade-state",
  nonce = "facade-nonce",
  code_verifier = verifier,
}))
assert(oidc.authorization_url:find("https://idp.example.test/authorize?", 1,
                                   true) == 1)
assert(oidc.authorization_url:find("client_id=facade-client", 1, true))
assert(oidc.authorization_url:find("state=facade-state", 1, true))
assert(oidc.authorization_url:find("nonce=facade-nonce", 1, true))
assert(oidc.code_verifier == verifier)
assert(type(oidc.code_challenge) == "string")
assert(#oidc.code_challenge > 0)

local oauth_transport_calls = {}
local function oauth_transport(mode)
  return function(request)
    assert(request.method == "POST")
    assert(request.url == "https://idp.example.test/token")
    assert(request.content_type == "application/x-www-form-urlencoded")
    assert(type(request.body) == "string")
    oauth_transport_calls[#oauth_transport_calls + 1] = mode
    if mode == "code" then
      assert(request.body:find("grant_type=authorization_code", 1, true))
      assert(request.body:find("code=facade-code", 1, true))
      assert(request.body:find("client_id=facade-client", 1, true))
      assert(request.body:find("code_verifier=", 1, true))
      return {
        status_code = 200,
        content_type = "application/json",
        body = "{\"access_token\":\"browser-token\",\"token_type\":\"Bearer\",\"refresh_token\":\"browser-refresh\",\"scope\":\"openid dav\",\"id_token\":\"id-token\",\"expires_in\":4200}",
      }
    elseif mode == "client" then
      assert(request.body:find("grant_type=client_credentials", 1, true))
      assert(request.body:find("client_id=facade-client", 1, true))
      assert(request.body:find("client_secret=facade-secret", 1, true))
      return {
        status_code = 200,
        content_type = "application/json",
        body = "{\"access_token\":\"m2m-token\",\"token_type\":\"Bearer\",\"refresh_token\":\"m2m-refresh\",\"scope\":\"dav\",\"expires_in\":3600}",
      }
    elseif mode == "refresh" then
      assert(request.body:find("grant_type=refresh_token", 1, true))
      assert(request.body:find("refresh_token=old-refresh", 1, true))
      return {
        status_code = 200,
        content_type = "application/json",
        body = "{\"access_token\":\"refreshed-token\",\"token_type\":\"Bearer\",\"refresh_token\":\"new-refresh\",\"scope\":\"dav\",\"expires_in\":7200}",
      }
    end
    error("unexpected OAuth transport mode")
  end
end

local exchange = assert(vectis.auth.oidc_exchange_callback({
  token_endpoint = "https://idp.example.test/token",
  client_id = "facade-client",
  client_secret = "facade-secret",
  redirect_uri = "http://127.0.0.1/callback",
  code_verifier = verifier,
  callback_query = "?code=facade-code&state=facade-state",
  expected_state = "facade-state",
  now = 1000,
  transport = oauth_transport("code"),
}))
assert(exchange.code == "facade-code")
assert(exchange.state == "facade-state")
assert(exchange.token.access_token == "browser-token")
assert(exchange.token.id_token == "id-token")
assert(exchange.token.has_expires_in == true)
assert(exchange.token.expires_in == 4200)
assert(exchange.flow.access_token == "browser-token")
assert(exchange.flow.refresh_token == "browser-refresh")
assert(exchange.flow.expires_at == 5200)

local client_token = assert(vectis.auth.oauth2_client_credentials({
  token_endpoint = "https://idp.example.test/token",
  client_id = "facade-client",
  client_secret = "facade-secret",
  scope = "dav",
  transport = oauth_transport("client"),
}))
assert(client_token.access_token == "m2m-token")
assert(client_token.refresh_token == "m2m-refresh")
assert(client_token.has_expires_in == true)
assert(client_token.expires_in == 3600)

local refreshed = assert(vectis.auth.oauth2_flow_ensure({
  flow = {
    access_token = "expired-token",
    token_type = "Bearer",
    refresh_token = "old-refresh",
    scope = "dav",
    expires_at = 10,
  },
  token_endpoint = "https://idp.example.test/token",
  client_id = "facade-client",
  client_secret = "facade-secret",
  scope = "dav",
  now = 1000,
  disable_retry = true,
  transport = oauth_transport("refresh"),
}))
assert(refreshed.result.state == "refreshed")
assert(refreshed.result.refreshed == true)
assert(refreshed.flow.access_token == "refreshed-token")
assert(refreshed.flow.refresh_token == "new-refresh")
assert(refreshed.flow.expires_at == 8200)

assert(vectis.auth.oauth2_flow_upsert({
  credentials_path = auth_store,
  flow_id = "facade-flow",
  subject = "facade-oidc@example.com",
  flow = exchange.flow,
}) == true)

local loaded_flow = assert(vectis.auth.oauth2_flow_load({
  credentials_path = auth_store,
  flow_id = "facade-flow",
}))
assert(loaded_flow.found == true)
assert(loaded_flow.flow_id == "facade-flow")
assert(loaded_flow.subject == "facade-oidc@example.com")
assert(loaded_flow.flow.access_token == "browser-token")
assert(loaded_flow.flow.refresh_token == "browser-refresh")

local stored_ready = assert(vectis.auth.oauth2_stored_flow_ensure({
  credentials_path = auth_store,
  flow_id = "facade-flow",
  now = 1000,
}))
assert(stored_ready.found == true)
assert(stored_ready.result.state == "ready")
assert(stored_ready.result.refreshed == false)
assert(stored_ready.flow.access_token == "browser-token")

local oauth_webdav_key = assert(vectis.auth.oauth2_webdav_key({
  credentials_path = auth_store,
  flow_id = "facade-flow",
  subject = "facade-oidc@example.com",
}))
assert(type(oauth_webdav_key.client_id) == "string")
assert(type(oauth_webdav_key.client_secret) == "string")
assert(oauth_webdav_key.claim_json:find("\"oauth2_flow_id\":\"facade-flow\"", 1,
                                        true))
local oauth_authorization =
    assert(vectis.auth.basic_authorization(oauth_webdav_key))
local oauth_verified = assert(vectis.auth.verify({
  credentials_path = auth_store,
  authorization = oauth_authorization,
  allowed_modes = "basic",
}))
assert(oauth_verified.authenticated == true)
assert(oauth_verified.claim_json:find("\"oauth2_flow_id\":\"facade-flow\"", 1,
                                      true))
assert(#oauth_transport_calls == 3)

local stopped, stopped_err = dsv.each({
  data = "id\nalpha\n",
  on_row = function()
    return false
  end,
})
assert(stopped == nil)
assert_status_error(stopped_err, vectis.ERR_STATE, "callback stopped")
assert(stopped_err.source == "vectis")
assert(stopped_err.source_code == vectis.ERROR_SOURCE_VECTIS)

local bad_each_ok, bad_each_err = pcall(function()
  dsv.each({ data = "id\nalpha\n" })
end)
assert(bad_each_ok == false)
assert(tostring(bad_each_err):find("on_row callback", 1, true))

local missing_bundle, missing_bundle_err = lockd.config({
  client_bundle = "embedded",
})
assert(missing_bundle == nil)
assert_status_error(missing_bundle_err, vectis.ERR_STATE,
                    "no embedded lockd bundle")
assert(missing_bundle_err.source == "vectis")

local xml_schema = lonejson.schema("contract", {
  lonejson.field("id", lonejson.string({required = true})),
})
local bad_xml, bad_xml_err = xml.parse({
  schema = xml_schema,
  xml = "<wrong id=\"x\"/>",
  root_element = "contract",
})
assert(bad_xml == nil)
assert_status_error(bad_xml_err, vectis.ERR_INVALID, "root element")
assert(bad_xml_err.source == "vectis")
assert(bad_xml_err.source_code == vectis.ERROR_SOURCE_VECTIS)

local invalid_http = http.normalize(nil)
assert(invalid_http.ok == false)
assert(invalid_http.transport_ok == false)
assert(invalid_http.error.kind == "invalid_result")
assert(invalid_http.error.message == "HTTP result is invalid")
assert_status_error(invalid_http.error, vectis.ERR_INVALID,
                    "HTTP result is invalid")
assert(invalid_http.error.source == "vectis")

local transport_http = http.normalize({
  ok = false,
  error = "connection refused",
  code = 7,
  code_name = "CURLE_COULDNT_CONNECT",
  attempts = 2,
})
assert(transport_http.ok == false)
assert(transport_http.transport_ok == false)
assert(transport_http.error.kind == "transport")
assert(transport_http.error.code == 7)
assert(transport_http.error.attempts == 2)
assert_status_error(transport_http.error, vectis.ERR_STATE,
                    "connection refused")
assert(transport_http.error.source == "curl")
assert(transport_http.error.source_code == vectis.ERROR_SOURCE_CURL)
assert(transport_http.error.dependency_code == 7)
assert(transport_http.error_message == "connection refused")

local timeout_http = http.normalize({
  ok = false,
  error = "timeout",
  code = 28,
  code_name = "CURLE_OPERATION_TIMEDOUT",
  attempts = 1,
})
assert(timeout_http.error.kind == "transport")
assert_status_error(timeout_http.error, vectis.ERR_TIMEOUT, "timeout")

local status_http = http.normalize({
  ok = true,
  status = 404,
  body = "missing",
  attempts = 1,
})
assert(status_http.ok == false)
assert(status_http.transport_ok == true)
assert(status_http.error.kind == "http_status")
assert_status_error(status_http.error, vectis.ERR_STATE,
                    "HTTP request failed")
assert(status_http.error.http_status == 404)
assert(status_http.error.source == "curl")
assert(status_http.error.body == "missing")

local rest_response = assert(rest.json({
  ok = true,
  surface = "rest",
}, {
  status = 202,
  headers = {
    ["x-vectis-rest"] = "contract",
  },
}))
assert(rest_response.status == 202)
assert(rest_response.content_type == rest.JSON_CONTENT_TYPE)
assert(rest_response.headers["x-vectis-rest"] == "contract")
local rest_decoded = assert(lonejson.decode_json(rest_response.body))
assert(rest_decoded.ok == true)
assert(rest_decoded.surface == "rest")

local bad_rest_json, bad_rest_json_err = rest.json({
  unsupported = function()
    return true
  end,
})
assert(bad_rest_json == nil)
assert(bad_rest_json_err.kind == "json_encode")
assert_status_error(bad_rest_json_err, vectis.ERR_INVALID)
assert(bad_rest_json_err.source == "lonejson")
assert(bad_rest_json_err.source_code == vectis.ERROR_SOURCE_LONEJSON)

local rest_error_response = rest.error_response({
  kind = "validation",
  message = "rest contract failed",
  status = vectis.ERR_INVALID,
}, {
  headers = {
    ["x-vectis-error"] = "contract",
  },
})
assert(rest_error_response.status == 400)
assert(rest_error_response.content_type == rest.JSON_CONTENT_TYPE)
assert(rest_error_response.headers["x-vectis-error"] == "contract")
local rest_error_body = assert(lonejson.decode_json(rest_error_response.body))
assert(rest_error_body.ok == false)
assert(rest_error_body.error.kind == "validation")
assert_status_error(rest_error_body.error, vectis.ERR_INVALID,
                    "rest contract failed")
assert(rest_error_body.error.source == "vectis")
assert(rest_error_body.error.source_code == vectis.ERROR_SOURCE_VECTIS)

local mqtt_transport = mqtt.normalize({
  ok = false,
  error = "mqtt failed",
  attempts = 3,
})
assert(mqtt_transport.ok == false)
assert(mqtt_transport.transport_ok == false)
assert(mqtt_transport.error.kind == "transport")
assert(mqtt_transport.error.attempts == 3)
assert_status_error(mqtt_transport.error, vectis.ERR_STATE, "mqtt failed")
assert(mqtt_transport.error.source == "curl")

local smtp_transport = smtp.normalize({
  ok = false,
  error = "smtp failed",
  attempts = 4,
})
assert(smtp_transport.ok == false)
assert(smtp_transport.transport_ok == false)
assert(smtp_transport.error.kind == "transport")
assert(smtp_transport.error.attempts == 4)
assert_status_error(smtp_transport.error, vectis.ERR_STATE, "smtp failed")
assert(smtp_transport.error.source == "curl")

local webdav_conflict = webdav.normalize({
  ok = true,
  status = 409,
  body = "conflict",
})
assert(webdav_conflict.error.kind == "http_status")
assert(webdav_conflict.error.http_status == 409)
assert_status_error(webdav_conflict.error, vectis.ERR_STATE,
                    "HTTP request failed")

local bad_webdav_ok, bad_webdav_err = pcall(function()
  webdav.copy({ url = "https://example.test/source" })
end)
assert(bad_webdav_ok == false)
assert(tostring(bad_webdav_err):find("destination", 1, true))

print("vectis-lua-facade-contracts-ok")
]])

execute_process(COMMAND "${VECTIS_BIN}" "${script}" "${WORK_DIR}"
                RESULT_VARIABLE facade_contracts_result
                OUTPUT_VARIABLE facade_contracts_stdout
                ERROR_VARIABLE facade_contracts_stderr)
if(NOT facade_contracts_result EQUAL 0)
  message(FATAL_ERROR
    "vectis Lua facade contracts failed: ${facade_contracts_stdout}${facade_contracts_stderr}")
endif()
if(NOT facade_contracts_stdout MATCHES "vectis-lua-facade-contracts-ok")
  message(FATAL_ERROR
    "vectis Lua facade contracts did not report success: ${facade_contracts_stdout}")
endif()
