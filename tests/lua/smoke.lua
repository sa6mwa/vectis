local vectis = require("vectis")
local lockdc = require("lockdc")
local lonejson = require("lonejson")
local cai = require("cai")
local pslog = require("pslog")
local libmdf = require("libmdf")
local softline = require("softline")

local function base64_encode(input)
  local alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
  local out = {}
  local index = 1
  for i = 1, #input, 3 do
    local a = input:byte(i)
    local b = input:byte(i + 1)
    local c = input:byte(i + 2)
    local triple = a * 65536 + (b or 0) * 256 + (c or 0)
    local first = math.floor(triple / 262144) % 64 + 1
    local second = math.floor(triple / 4096) % 64 + 1
    local third = math.floor(triple / 64) % 64 + 1
    out[index] = alphabet:sub(first, first)
    out[index + 1] = alphabet:sub(second, second)
    out[index + 2] = b and alphabet:sub(third, third) or "="
    out[index + 3] = c and alphabet:sub(triple % 64 + 1, triple % 64 + 1) or "="
    index = index + 4
  end
  return table.concat(out)
end

assert(type(vectis) == "table")
assert(vectis.version == (os.getenv("VECTIS_EXPECTED_VERSION") or "0.0.0"))
assert(vectis.status_string(vectis.OK) == "ok")
assert(vectis.status_string(vectis.ERR_INVALID) == "invalid")
assert(arg[0]:match("smoke%.lua$"))
assert(arg[1] == "first")
assert(arg[2] == "second")

assert(type(vectis.auth) == "table")
local function oauth_transport(mode)
  return function(request)
    assert(request.method == "POST")
    assert(request.url == "https://idp.example.test/token")
    assert(request.content_type == "application/x-www-form-urlencoded")
    assert(type(request.body) == "string")
    if mode == "code" then
      assert(request.body:find("grant_type=authorization_code", 1, true))
      assert(request.body:find("code=auth-code", 1, true))
      assert(request.body:find("client_id=vectis-client", 1, true))
      assert(request.body:find("code_verifier=", 1, true))
      return {
        status_code = 200,
        content_type = "application/json",
        body = '{"access_token":"browser-token","token_type":"Bearer","refresh_token":"browser-refresh","scope":"openid dav","id_token":"id-token","expires_in":4200}',
      }
    end
    if mode == "client" then
      assert(request.body:find("grant_type=client_credentials", 1, true))
      assert(request.body:find("client_id=vectis-client", 1, true))
      assert(request.body:find("client_secret=vectis-secret", 1, true))
      return {
        status_code = 200,
        content_type = "application/json",
        body = '{"access_token":"m2m-token","token_type":"Bearer","refresh_token":"m2m-refresh","scope":"dav","expires_in":3600}',
      }
    end
    if mode == "fail" then
      assert(request.body:find("grant_type=refresh_token", 1, true))
      assert(request.body:find("refresh_token=lua-refresh-token", 1, true))
      error("mock OAuth2 refresh failed")
    end
    assert(request.body:find("grant_type=refresh_token", 1, true))
    assert(request.body:find("refresh_token=old-refresh", 1, true))
    return {
      status_code = 200,
      content_type = "application/json",
      body = '{"access_token":"refreshed-token","token_type":"Bearer","refresh_token":"new-refresh","scope":"dav","expires_in":7200}',
    }
  end
end
local oidc = assert(vectis.auth.oidc_authorization({
  authorization_endpoint = "https://idp.example.test/authorize",
  client_id = "vectis-client",
  redirect_uri = "http://127.0.0.1/callback",
  scope = "openid dav",
  state = "lua-state",
  nonce = "lua-nonce",
  code_verifier = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~abc",
}))
assert(oidc.authorization_url:find("https://idp.example.test/authorize?", 1, true) == 1)
assert(oidc.authorization_url:find("response_type=code", 1, true))
assert(oidc.authorization_url:find("code_challenge_method=S256", 1, true))
assert(oidc.code_verifier == "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~abc")
assert(type(oidc.code_challenge) == "string" and #oidc.code_challenge > 0)
assert(oidc.state == "lua-state")
assert(oidc.nonce == "lua-nonce")
local exchanged = assert(vectis.auth.oidc_exchange_callback({
  transport = oauth_transport("code"),
  token_endpoint = "https://idp.example.test/token",
  client_id = "vectis-client",
  client_secret = "vectis-secret",
  redirect_uri = "http://127.0.0.1/callback",
  code_verifier = oidc.code_verifier,
  callback_query = "?code=auth-code&state=lua-state",
  expected_state = "lua-state",
  now = 1000,
}))
assert(exchanged.code == "auth-code")
assert(exchanged.state == "lua-state")
assert(exchanged.token.access_token == "browser-token")
assert(exchanged.token.id_token == "id-token")
assert(exchanged.flow.access_token == "browser-token")
assert(exchanged.flow.expires_at == 5200)
local m2m = assert(vectis.auth.oauth2_client_credentials({
  transport = oauth_transport("client"),
  token_endpoint = "https://idp.example.test/token",
  client_id = "vectis-client",
  client_secret = "vectis-secret",
  scope = "dav",
}))
assert(m2m.access_token == "m2m-token")
assert(m2m.refresh_token == "m2m-refresh")
assert(m2m.expires_in == 3600)
local ensured = assert(vectis.auth.oauth2_flow_ensure({
  transport = oauth_transport("refresh"),
  token_endpoint = "https://idp.example.test/token",
  client_id = "vectis-client",
  client_secret = "vectis-secret",
  now = 1000,
  flow = {
    access_token = "old-token",
    token_type = "Bearer",
    refresh_token = "old-refresh",
    scope = "dav",
    expires_at = 900,
    has_expires_at = true,
  },
}))
assert(ensured.result.state == "refreshed")
assert(ensured.result.refreshed == true)
assert(ensured.flow.access_token == "refreshed-token")
assert(ensured.flow.refresh_token == "new-refresh")
assert(ensured.flow.expires_at == 8200)
local auth_path = os.tmpname()
os.remove(auth_path)
assert(vectis.auth.store_init({ credentials_path = auth_path }))
assert(vectis.auth.oauth2_flow_upsert({
  credentials_path = auth_path,
  flow_id = "lua-flow",
  subject = "lua-oidc@example.com",
  flow = {
    access_token = "lua-access-token",
    token_type = "Bearer",
    refresh_token = "lua-refresh-token",
    scope = "openid dav",
    id_token = "lua-id-token",
    expires_at = 5200,
    has_expires_at = true,
  },
}))
local loaded_flow = assert(vectis.auth.oauth2_flow_load({
  credentials_path = auth_path,
  flow_id = "lua-flow",
}))
assert(loaded_flow.found == true)
assert(loaded_flow.flow_id == "lua-flow")
assert(loaded_flow.subject == "lua-oidc@example.com")
assert(loaded_flow.flow.access_token == "lua-access-token")
assert(loaded_flow.flow.refresh_token == "lua-refresh-token")
assert(loaded_flow.flow.expires_at == 5200)
assert(loaded_flow.flow.has_expires_at == true)
local oauth_webdav_key = assert(vectis.auth.oauth2_webdav_key({
  credentials_path = auth_path,
  flow_id = "lua-flow",
  subject = "lua-oidc@example.com",
}))
assert(type(oauth_webdav_key.client_id) == "string")
assert(type(oauth_webdav_key.client_secret) == "string")
assert(oauth_webdav_key.claim_json:match('"oauth2_flow_id":"lua%-flow"'))
local oauth_webdav_verified = assert(vectis.auth.verify({
  credentials_path = auth_path,
  authorization = "Basic " .. base64_encode(
    oauth_webdav_key.client_id .. ":" .. oauth_webdav_key.client_secret),
  allowed_modes = { "basic" },
}))
assert(oauth_webdav_verified.authenticated == true)
assert(oauth_webdav_verified.claim_json:match('"oauth2_flow_id":"lua%-flow"'))
local stored_ensure, stored_ensure_error = vectis.auth.oauth2_stored_flow_ensure({
  credentials_path = auth_path,
  flow_id = "lua-flow",
  transport = oauth_transport("fail"),
  token_endpoint = "https://idp.example.test/token",
  client_id = "vectis-client",
  client_secret = "vectis-secret",
  now = 6000,
})
assert(stored_ensure == nil)
assert(type(stored_ensure_error) == "table")
local oauth_webdav_revoked = assert(vectis.auth.verify({
  credentials_path = auth_path,
  authorization = "Basic " .. base64_encode(
    oauth_webdav_key.client_id .. ":" .. oauth_webdav_key.client_secret),
  allowed_modes = { "basic" },
}))
assert(oauth_webdav_revoked.authenticated == false)
local issued = assert(vectis.auth.issue({
  credentials_path = auth_path,
  subject = "lua@example.com",
  purpose = "webdav",
  modes = { "bearer" },
}))
assert(type(issued.client_id) == "string")
assert(type(issued.api_key) == "string")
assert(issued.client_secret == nil)
local verified = assert(vectis.auth.verify({
  credentials_path = auth_path,
  authorization = "Bearer " .. issued.api_key,
  allowed_modes = { "bearer" },
}))
assert(verified.authenticated == true)
assert(verified.auth_mode == "bearer")
assert(verified.claim_json:match('"purpose":"webdav"'))
local native_provider = assert(vectis.auth.provider_native({
  credentials_path = auth_path,
  purpose = "webdav",
  realm = "lua",
  allowed_modes = { "bearer" },
}))
local native_allowed = assert(native_provider:authenticate({
  authorization = "Bearer " .. issued.api_key,
}))
assert(native_allowed.action == "allow")
assert(native_allowed.principal == "lua@example.com")
local native_required = assert(native_provider:authenticate({}))
assert(native_required.action == "required")
assert(native_required.www_authenticate == "Bearer")
local callback_provider = assert(vectis.auth.provider_callback(function(request)
  return { action = "allow", principal = request.resource }
end))
local callback_allowed = assert(callback_provider:authenticate({ resource = "/lua" }))
assert(callback_allowed.action == "allow")
assert(callback_allowed.status_code == 0)
assert(callback_allowed.principal == "/lua")
local callback_required_provider = assert(vectis.auth.provider_callback(function(request)
  return {
    action = "required",
    status_code = 401,
    www_authenticate = 'Basic realm="' .. request.resource .. '"',
  }
end))
local callback_required = assert(callback_required_provider:authenticate({ resource = "lua" }))
assert(callback_required.action == "required")
assert(callback_required.status_code == 401)
assert(callback_required.www_authenticate == 'Basic realm="lua"')
local callback_redirect_provider = assert(vectis.auth.provider_callback(function()
  return {
    action = "redirect",
    status_code = 303,
    location = "/auth/login?next=/dav",
    content_type = "text/plain",
    body = "login required",
  }
end))
local callback_redirect = assert(callback_redirect_provider:authenticate({}))
assert(callback_redirect.action == "redirect")
assert(callback_redirect.status_code == 303)
assert(callback_redirect.location == "/auth/login?next=/dav")
assert(callback_redirect.content_type == "text/plain")
assert(callback_redirect.body == "login required")
local callback_deny_provider = assert(vectis.auth.provider_callback(function()
  return {}
end))
local callback_deny = assert(callback_deny_provider:authenticate({}))
assert(callback_deny.action == "deny")
local callback_invalid_provider = assert(vectis.auth.provider_callback(function()
  return { action = "maybe" }
end))
local callback_invalid, callback_invalid_error = callback_invalid_provider:authenticate({})
assert(callback_invalid == nil)
assert(callback_invalid_error.status_string == "invalid")
assert(callback_invalid_error.message:match("action"))
local user = assert(vectis.auth.user_add({
  credentials_path = auth_path,
  username = "lua-user@example.com",
  password = "lua-password",
  totp_secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ",
  totp_label = "Vectis:lua-user@example.com",
  issuer = "Vectis",
}))
assert(user.username == "lua-user@example.com")
assert(user.totp_secret == "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ")
assert(user.totp_uri:find("otpauth://totp/", 1, true) == 1)
assert(user.totp_qr:find("\226\150\136", 1, true))
local missing_totp = assert(vectis.auth.user_login({
  credentials_path = auth_path,
  username = "lua-user@example.com",
  password = "lua-password",
}))
assert(missing_totp.authenticated == false)
local logged_in = assert(vectis.auth.user_login({
  credentials_path = auth_path,
  username = "lua-user@example.com",
  password = "lua-password",
  totp_code = "287082",
  time = 59,
  window = 0,
}))
assert(logged_in.authenticated == true)
local webdav_key = assert(vectis.auth.webdav_key({
  credentials_path = auth_path,
  username = "lua-user@example.com",
  password = "lua-password",
  totp_code = "287082",
  time = 59,
  window = 0,
}))
assert(type(webdav_key.client_id) == "string")
assert(type(webdav_key.client_secret) == "string")
local totp = assert(vectis.auth.totp.new("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"))
assert(totp:secret() == "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ")
assert(totp:generate(59) == "287082")
assert(totp:validate("287082", 59, 0))
assert(not totp:validate("287083", 59, 0))
assert(totp:uri("Vectis:auth", "Vectis") ==
  "otpauth://totp/Vectis%3Aauth?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ&issuer=Vectis")
assert(totp:qr("Vectis:auth", "Vectis"):find("\226\150\136", 1, true))
local qr = assert(vectis.auth.qr.new("vectis"))
assert(qr:size() > 0)
assert(qr:ansi():find("\226\150\136", 1, true))
assert(vectis.auth.revoke({ credentials_path = auth_path, client_id = issued.client_id }))
local revoked = assert(vectis.auth.verify({
  credentials_path = auth_path,
  authorization = "Bearer " .. issued.api_key,
  allowed_modes = { "bearer" },
}))
assert(revoked.authenticated == false)
os.remove(auth_path)
os.remove(auth_path .. ".lock")

assert(type(lonejson) == "table")
assert(lonejson.encode_json(lonejson.json_null) == "null")

assert(type(lockdc) == "table")
assert(type(lockdc.open) == "function")
assert(type(lockdc.version_string()) == "string")
assert(lockdc.encode_json({ ok = true }) == '{"ok":true}')

assert(type(cai) == "table")
assert(type(cai.open) == "function")
assert(type(cai.mcp_handler) == "function")
assert(type(cai.MODEL_DEFAULT_RESPONSES) == "string")
assert(type(cai.MCP_PROTOCOL_VERSION) == "string")

assert(type(pslog) == "table")
assert(type(pslog.new_json) == "function")
assert(type(pslog.version()) == "string")
local log_chunks = {}
local log = assert(pslog.new_json(function(chunk)
  log_chunks[#log_chunks + 1] = chunk
end, { timestamps = false }))
log:info("lua smoke", "component", "vectis")
log:close()
local log_payload = table.concat(log_chunks)
assert(log_payload:match('"msg":"lua smoke"'))
assert(log_payload:match('"component":"vectis"'))

assert(type(libmdf) == "table")
assert(type(libmdf.render) == "function")
assert(type(libmdf.render_stream) == "function")
assert(libmdf.version == "0.6.0")
local rendered_markdown = libmdf.render("# Vectis\n\n**ok**", { format = "html" })
assert(rendered_markdown:match("Vectis"))
assert(rendered_markdown:match("ok"))

assert(type(softline) == "table")
assert(type(softline.new) == "function")
local line = assert(softline.new({ line_max_len = 32 }))
assert(line:set_buffer("draft"))
assert(line:insert("++"))
assert(line:buffer():match("%+%+"))
line:close()

local encoded = lonejson.encode_json({
  b = true,
  a = lonejson.json_array({ "first", lonejson.json_null, 3 }),
})
assert(encoded == '{"a":["first",null,3],"b":true}')

local decoded = lonejson.decode_json(encoded)
assert(decoded.a[1] == "first")
assert(decoded.a[2] == lonejson.json_null)
assert(decoded.a[3] == 3)
assert(decoded.b == true)

local chunks = {}
lonejson.encode_json_to_sink({ z = "sink", a = lonejson.json_array({ true, false }) }, function(chunk)
  chunks[#chunks + 1] = chunk
end)
assert(table.concat(chunks) == '{"a":[true,false],"z":"sink"}')
