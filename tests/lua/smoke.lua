local auth = require("vectis.auth")
local cert = require("vectis.cert")
local embedded = require("vectis.embedded")
local server_module = require("vectis.server")
local ssh = require("vectis.ssh")
local vectis = require("vectis")
local lockdc = require("lockdc")
local lonejson = require("lonejson")
local cai = require("cai")
local lql = require("lql")
local pslog = require("pslog")
local libmdf = require("libmdf")
local softline = require("softline")
local curl = require("curl")
local openssl = require("openssl")
local zlib = require("zlib")
local opcua = require("opcua")
local audio = require("audio")
local sus = require("sus")
local status = require("vectis.status")
local log = require("vectis.log")
local rest = require("vectis.rest")
local terminal = require("vectis.terminal")
local webdav = require("vectis.webdav")
local mqtt = require("vectis.mqtt")
local dsv = require("vectis.dsv")
local xml = require("vectis.xml")

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
assert(type(vectis.libs) == "table")
assert(vectis.libs.lockdc == lockdc)
assert(vectis.libs.lonejson == lonejson)
assert(vectis.libs.cai == cai)
assert(vectis.libs.lql == lql)
assert(vectis.libs.pslog == pslog)
assert(vectis.libs.libmdf == libmdf)
assert(vectis.libs.softline == softline)
assert(vectis.libs.curl == curl)
assert(vectis.libs.openssl == openssl)
assert(vectis.libs.zlib == zlib)
assert(vectis.libs.opcua == opcua)
assert(vectis.libs.audio == audio)
assert(vectis.libs.sus == sus)
assert(vectis.status_string(vectis.OK) == "ok")
assert(vectis.status_string(vectis.ERR_INVALID) == "invalid")
assert(vectis.status_string(vectis.ERR_NOMEM) == "nomem")
assert(vectis.status_string(vectis.ERR_STATE) == "state")
assert(vectis.status_string(vectis.ERR_CONFLICT) == "conflict")
assert(vectis.status_string(vectis.ERR_NOT_IMPLEMENTED) == "not_implemented")
assert(vectis.status_string(vectis.ERR_TIMEOUT) == "timeout")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_VECTIS) == "vectis")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_LIBSSH2) == "libssh2")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_CPKT) == "cpkt")
assert(status.status_string(status.ERR_INVALID) == "invalid")
assert(status.error_source_string(status.ERROR_SOURCE_CURL) == "curl")
assert(status.error_source_string(status.ERROR_SOURCE_CPKT) == "cpkt")
assert(vectis.status == status)
assert(vectis.auth == auth)
assert(vectis.cert == cert)
assert(vectis.embedded == embedded)
assert(vectis.server == server_module)
assert(vectis.ssh == ssh)
assert(vectis.log == log)
assert(type(ssh) == "table")
assert(type(ssh.exec) == "function")
assert(type(ssh.sftp_upload_file) == "function")
assert(type(ssh.sftp_download_file) == "function")
assert(type(ssh.sftp_open) == "function")
assert(type(ssh.scp_upload_file) == "function")
assert(type(ssh.scp_download_file) == "function")
assert(type(ssh.sftp_stat) == "function")
assert(type(ssh.sftp_mkdir) == "function")
assert(type(ssh.sftp_remove) == "function")
assert(type(ssh.sftp_rmdir) == "function")
assert(type(ssh.sftp_rename) == "function")
assert(type(ssh.sftp_chmod) == "function")
local smtp = require("vectis.smtp")
assert(type(smtp.send) == "function")
assert(arg[0]:match("smoke%.lua$"))
assert(arg[1] == "first")
assert(arg[2] == "second")
assert(package.loaded.luarocks == nil)
assert(not package.path:lower():find("luarocks", 1, true))
assert(not package.cpath:lower():find("luarocks", 1, true))
assert(package.loaded.vectis == vectis)
assert(package.loaded.lockdc == lockdc)
assert(package.loaded.lonejson == lonejson)
assert(package.loaded.cai == cai)
assert(package.loaded.lql == lql)
assert(package.loaded["lql.core"] == lql.core)
assert(package.loaded.pslog == pslog)
assert(package.loaded["vectis.log"] == log)
assert(package.loaded.libmdf == libmdf)
assert(package.loaded.softline == softline)
assert(package.loaded.curl == curl)
assert(package.loaded.openssl == openssl)
assert(package.loaded.zlib == zlib)
assert(package.loaded.opcua == opcua)
assert(package.loaded.audio == audio)
assert(package.loaded.sus == sus)
assert(package.loaded["vectis.auth"] == auth)
assert(package.loaded["vectis.cert"] == cert)
assert(package.loaded["vectis.embedded"] == embedded)
assert(package.loaded["vectis.server"] == server_module)
assert(package.loaded["vectis.ssh"] == ssh)
assert(package.loaded["vectis.status"] == status)
assert(package.loaded["vectis.rest"] == rest)
assert(vectis.rest == rest)
assert(package.loaded["vectis.terminal"] == terminal)
assert(vectis.terminal == terminal)
assert(package.loaded["vectis.webdav"] == webdav)
assert(vectis.webdav == webdav)
assert(package.loaded["vectis.mqtt"] == mqtt)
assert(vectis.mqtt == mqtt)
assert(package.loaded["vectis.smtp"] == smtp)
assert(vectis.smtp == smtp)
assert(package.loaded["vectis.dsv"] == dsv)
assert(vectis.dsv == dsv)
assert(package.loaded["vectis.xml"] == xml)
assert(vectis.xml == xml)

assert(type(dsv.parse) == "function")
assert(type(dsv.parse_json) == "function")
assert(type(dsv.each) == "function")
assert(type(dsv.to_string) == "function")
assert(type(xml.parse) == "function")
assert(type(xml.parse_record) == "function")
assert(type(rest.route) == "function")
assert(type(rest.client) == "function")
local smoke_rest_client = rest.client({base_url = "http://127.0.0.1"})
assert(type(smoke_rest_client.head) == "function")
assert(type(smoke_rest_client.options) == "function")
local smoke_http = require("vectis.http")
assert(type(smoke_http.options) == "function")
assert(type(smoke_http.options_json) == "function")
assert(type(smoke_http.client({}).options) == "function")
assert(type(smoke_http.client({}).options_json) == "function")
assert(type(terminal.markdown) == "function")
assert(type(terminal.markdown_stream) == "function")
assert(type(terminal.editor) == "function")
assert(type(webdav.request) == "function")
assert(type(webdav.propfind) == "function")
assert(type(webdav.mkcol) == "function")
assert(type(webdav.copy) == "function")
assert(type(webdav.move) == "function")
assert(type(mqtt.publish) == "function")
assert(type(openssl.version) == "function")
assert(type(openssl.sha256_hex) == "function")
assert(type(openssl.hmac_sha256_hex) == "function")
assert(type(openssl.digest) == "function")
assert(type(openssl.digest_hex) == "function")
assert(type(openssl.hmac) == "function")
assert(type(openssl.hmac_hex) == "function")
assert(type(openssl.random_bytes) == "function")
assert(type(zlib.version) == "function")
assert(type(zlib.deflate) == "function")
assert(type(zlib.inflate) == "function")
assert(type(zlib.compress) == "function")
assert(type(zlib.decompress) == "function")
assert(type(zlib.gzip) == "function")
assert(type(zlib.gunzip) == "function")
assert(type(zlib.deflate_file) == "function")
assert(type(zlib.inflate_file) == "function")
assert(type(zlib.compress_file) == "function")
assert(type(zlib.decompress_file) == "function")
assert(type(zlib.gzip_file) == "function")
assert(type(zlib.gunzip_file) == "function")
local xml_schema = lonejson.schema("invoice", {
  lonejson.field("id", lonejson.string({required = true})),
  lonejson.field("amount", lonejson.object({
    required = true,
    fields = {
      lonejson.field("currency", lonejson.string({required = true})),
      lonejson.field("text", lonejson.f64({required = true})),
    },
  })),
  lonejson.field("line", lonejson.object_array({
    fields = {
      lonejson.field("sku", lonejson.string({required = true})),
      lonejson.field("quantity", lonejson.i64({required = true})),
    },
  })),
  lonejson.field("tag", lonejson.string_array()),
  lonejson.field("active", lonejson.boolean({required = true})),
})
local xml_payload = table.concat({
  "<invoice id=\"inv-1\">",
  "<amount currency=\"EUR\">12.50</amount>",
  "<line><sku>A-1</sku><quantity>2</quantity></line>",
  "<line><sku>B-2</sku><quantity>5</quantity></line>",
  "<tag>paid</tag><tag>priority</tag>",
  "<active>true</active>",
  "</invoice>",
})
local xml_doc, xml_err = xml.parse({
  schema = xml_schema,
  xml = xml_payload,
  root_element = "invoice",
  trim_text = true,
})
assert(xml_doc, xml_err and xml_err.message)
assert(xml_doc.id == "inv-1")
assert(xml_doc.amount.currency == "EUR")
assert(xml_doc.amount.text == 12.5)
assert(#xml_doc.line == 2)
assert(xml_doc.line[1].sku == "A-1")
assert(xml_doc.line[2].quantity == 5)
assert(xml_doc.tag[2] == "priority")
assert(xml_doc.active == true)
local xml_path = os.tmpname()
local xml_file = assert(io.open(xml_path, "wb"))
xml_file:write(xml_payload)
xml_file:close()
local xml_record = assert(xml.parse_record({
  schema = xml_schema,
  path = xml_path,
  root_element = "invoice",
  trim_text = true,
}))
os.remove(xml_path)
local xml_record_table = xml_record:to_table()
assert(xml_record_table.line[2].sku == "B-2")
local bad_xml_doc, bad_xml_err = xml.parse({
  schema = xml_schema,
  xml = "<wrong/>",
  root_element = "invoice",
})
assert(bad_xml_doc == nil)
assert(type(bad_xml_err) == "table")
assert(bad_xml_err.message:match("root"))

assert(type(opcua.open62541_version()) == "string")
assert(#opcua.open62541_version() > 0)
assert(type(opcua.facade_version()) == "string")
assert(#opcua.facade_version() > 0)
assert(type(opcua.result_string(opcua.OK)) == "string")
assert(type(opcua.status_name(0)) == "string")
local bad_node, bad_node_err = opcua.node_id_parse("not-a-node-id")
assert(bad_node == nil)
assert(bad_node_err.status == vectis.ERR_INVALID)
assert(bad_node_err.status_string == "invalid")
assert(bad_node_err.source == "cpkt")
assert(bad_node_err.source_code == vectis.ERROR_SOURCE_CPKT)
assert(bad_node_err.dependency == "opcua")
assert(bad_node_err.dependency_code == bad_node_err.result)
assert(bad_node_err.opcua_status ~= nil)
assert(bad_node_err.status_name == nil)
assert(type(audio.result_string(audio.OK)) == "string")
assert(audio.can_decode("wav") == true)
assert(audio.can_encode("wav") == true)
assert(type(audio.capture.open_default) == "function")
assert(type(audio.playback.open_default) == "function")
assert(type(sus.facade_version()) == "string")
assert(sus.model_catalog_count() > 0)
local sus_registry = debug.getregistry()
assert(type(sus_registry["sus.model"].__index.create_transcriber) == "function")
assert(type(sus_registry["sus.transcriber"].__index.transcribe_f32_mono_16k) == "function")
assert(type(sus_registry["sus.transcriber"].__index.transcribe_f32_mono_16k_text) == "function")
assert(type(sus_registry["sus.transcriber"].__index.transcribe_audio_decoder_segmented) == "function")
assert(type(sus_registry["sus.transcriber"].__index.transcribe_audio_decoder_segmented_text) == "function")
assert(type(sus_registry["sus.transcriber"].__index.transcribe_audio_vox_segment) == "function")
assert(type(sus_registry["sus.transcriber"].__index.revised_text) == "function")
local numeric_node = opcua.node_id_numeric(2, 1234)
assert(numeric_node:type() == opcua.NODE_ID_NUMERIC)
assert(numeric_node:namespace() == 2)
assert(opcua.node_id_parse(tostring(numeric_node)) == numeric_node)
local string_node = opcua.node_id_string(3, "temperature")
assert(string_node:type() == opcua.NODE_ID_STRING)
assert(opcua.node_id(tostring(string_node)) == string_node)
local guid_node = opcua.node_id_guid(1, "01234567-89ab-cdef-0123-456789abcdef")
assert(guid_node:type() == opcua.NODE_ID_GUID)
local boolean_value = opcua.value_boolean(true)
assert(boolean_value:type() == opcua.VALUE_BOOLEAN)
assert(boolean_value:get() == true)
local string_value = opcua.value_string("hello")
assert(string_value:type() == opcua.VALUE_STRING)
assert(string_value:get() == "hello")
local client = assert(opcua.client())
assert(type(client.connect) == "function")
assert(type(client.read) == "function")
assert(client:close() == true)

local server = assert(vectis.server.new({ app_name = "lua-smoke", port = 18080 }))
assert(type(server.static_directory) == "function")
assert(type(server.webdav) == "function")
assert(type(server.webdav_embedded) == "function")
assert(type(server.route) == "function")
assert(type(server.dsv) == "function")
assert(type(server.json) == "function")
assert(type(server.text) == "function")
assert(type(server.redirect) == "function")
assert(type(server.auth_json) == "function")
local route_auth_path = os.tmpname()
os.remove(route_auth_path)
local route_auth_state_path = os.tmpname()
os.remove(route_auth_state_path)
assert(vectis.auth.store_init({
  credentials_path = route_auth_path,
  auth_state_path = route_auth_state_path,
}))
assert(server:auth_routes({
  path_prefix = "/_auth-state",
  credentials_path = route_auth_path,
  auth_state_path = route_auth_state_path,
  realm = "lua-route-state",
}) == true)
local consumer_service, consumer_service_error = server:consumer_service({
  queue = "lua-smoke",
  on_message = function() end,
})
assert(consumer_service == nil)
assert(type(consumer_service_error) == "table")
assert(consumer_service_error.status == vectis.ERR_INVALID)
assert(consumer_service_error.status_string == "invalid")
assert(consumer_service_error.message:match("direct Lua callbacks"))
consumer_service, consumer_service_error = server:consumer_service({
  queue = "lua-smoke",
  handler = {
    kind = "missing_receiver",
  },
})
assert(consumer_service == nil)
assert(type(consumer_service_error) == "table")
assert(consumer_service_error.status == vectis.ERR_INVALID)
assert(consumer_service_error.message:match("receiver_kind"))
server:close()

local tls_bundle_pem_server = assert(vectis.server.new({
  app_name = "lua-manual-tls-bundle-pem",
  port = 18170,
  tls = {
    mode = "manual",
    cert_key_bundle_pem = "-----BEGIN CERTIFICATE-----\nplaceholder\n" ..
        "-----END CERTIFICATE-----\n-----BEGIN PRIVATE KEY-----\n" ..
        "placeholder\n-----END PRIVATE KEY-----\n",
  },
}))
tls_bundle_pem_server:close()

local tls_split_pem_server = assert(vectis.server.new({
  app_name = "lua-manual-tls-split-pem",
  port = 18171,
  tls = {
    mode = "manual",
    certificate_pem = "-----BEGIN CERTIFICATE-----\nplaceholder\n" ..
        "-----END CERTIFICATE-----\n",
    private_key_pem = "-----BEGIN PRIVATE KEY-----\nplaceholder\n" ..
        "-----END PRIVATE KEY-----\n",
    ca_bundle_pem = "-----BEGIN CERTIFICATE-----\nca\n" ..
        "-----END CERTIFICATE-----\n",
  },
}))
tls_split_pem_server:close()

local tls_client_ca_pem_server = assert(vectis.server.new({
  app_name = "lua-manual-tls-client-ca-pem",
  port = 18172,
  tls = {
    mode = "manual",
    cert_key_bundle_pem = "-----BEGIN CERTIFICATE-----\nplaceholder\n" ..
        "-----END CERTIFICATE-----\n-----BEGIN PRIVATE KEY-----\n" ..
        "placeholder\n-----END PRIVATE KEY-----\n",
    require_client_certificate = true,
    client_ca_bundle_pem = "-----BEGIN CERTIFICATE-----\nclient-ca\n" ..
        "-----END CERTIFICATE-----\n",
  },
}))
tls_client_ca_pem_server:close()

local acme_auth_path = os.tmpname()
os.remove(acme_auth_path)
assert(vectis.auth.store_init({ credentials_path = acme_auth_path }))

local acme_missing_domain = assert(vectis.server.new({
  app_name = "lua-acme-missing-domain",
  port = 18180,
  tls = {
    mode = "acme",
    acme_email = "ops@example.com",
  },
}))
assert(acme_missing_domain:auth_json({
  path = "/probe",
  auth = { kind = "native", credentials_path = acme_auth_path },
}) == true)
local acme_started, acme_error = acme_missing_domain:start()
assert(acme_started == nil)
assert(type(acme_error) == "table")
assert(acme_error.status == vectis.ERR_INVALID)
assert(acme_error.message:match("tls%.domains"))
acme_missing_domain:close()

local acme_duplicate, acme_duplicate_error = vectis.server.new({
  app_name = "lua-acme-duplicate-domain",
  port = 18181,
  tls = {
    mode = "acme",
    domains = { "api.example.com", "api.example.com" },
    email = "ops@example.com",
  },
})
assert(acme_duplicate == nil)
assert(type(acme_duplicate_error) == "table")
assert(acme_duplicate_error.status == vectis.ERR_INVALID)
assert(acme_duplicate_error.message:match("duplicate"))

local acme_missing_email = assert(vectis.server.new({
  app_name = "lua-acme-missing-email",
  port = 18182,
  tls = {
    mode = "acme",
    domains = { "api.example.com", "www.example.com" },
    provider = "https://acme.example.test/directory",
    cache_dir = "/tmp/vectis-lua-acme-cache",
  },
}))
assert(acme_missing_email:auth_json({
  path = "/probe",
  auth = { kind = "native", credentials_path = acme_auth_path },
}) == true)
acme_started, acme_error = acme_missing_email:start()
assert(acme_started == nil)
assert(type(acme_error) == "table")
assert(acme_error.status == vectis.ERR_INVALID)
assert(acme_error.message:match("acme_email"))
acme_missing_email:close()

local acme_empty_cache = assert(vectis.server.new({
  app_name = "lua-acme-empty-cache",
  port = 18183,
  tls = {
    mode = "acme",
    domains = { "api.example.com" },
    email = "ops@example.com",
    provider = "https://acme.example.test/directory",
    cache_dir = "",
  },
}))
assert(acme_empty_cache:auth_json({
  path = "/probe",
  auth = { kind = "native", credentials_path = acme_auth_path },
}) == true)
acme_started, acme_error = acme_empty_cache:start()
assert(acme_started == nil)
assert(type(acme_error) == "table")
assert(acme_error.status == vectis.ERR_INVALID)
assert(acme_error.message:match("acme_state_dir"))
acme_empty_cache:close()

local acme_missing_cache = assert(vectis.server.new({
  app_name = "lua-acme-missing-cache",
  port = 18184,
  tls = {
    mode = "acme",
    domains = { "api.example.com" },
    email = "ops@example.com",
    provider = "https://acme.example.test/directory",
  },
}))
assert(acme_missing_cache:auth_json({
  path = "/probe",
  auth = { kind = "native", credentials_path = acme_auth_path },
}) == true)
acme_started, acme_error = acme_missing_cache:start()
assert(acme_started == nil)
assert(type(acme_error) == "table")
assert(acme_error.status == vectis.ERR_INVALID)
assert(acme_error.message:match("acme_state_dir"))
acme_missing_cache:close()

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
local auth_state_path = os.tmpname()
os.remove(auth_state_path)
local function file_contains(path, text)
  local file = io.open(path, "rb")
  if not file then
    return false
  end
  local body = file:read("*a")
  file:close()
  return body:find(text, 1, true) ~= nil
end
assert(vectis.auth.store_init({
  credentials_path = auth_path,
  state_path = auth_state_path,
}))
assert(vectis.auth.oauth2_flow_upsert({
  credentials_path = auth_path,
  flow_id = "lua-browser-flow",
  subject = "lua-browser-oidc@example.com",
  flow = exchanged.flow,
}))
local browser_flow_key = assert(vectis.auth.oauth2_webdav_key({
  credentials_path = auth_path,
  flow_id = "lua-browser-flow",
  subject = "lua-browser-oidc@example.com",
}))
assert(type(browser_flow_key.client_id) == "string")
assert(type(browser_flow_key.client_secret) == "string")
assert(browser_flow_key.claim_json:match('"oauth2_flow_id":"lua%-browser%-flow"'))
local browser_flow_verified = assert(vectis.auth.verify({
  credentials_path = auth_path,
  authorization = "Basic " .. base64_encode(
    browser_flow_key.client_id .. ":" .. browser_flow_key.client_secret),
  allowed_modes = { "basic" },
}))
assert(browser_flow_verified.authenticated == true)
assert(browser_flow_verified.claim_json:match('"oauth2_flow_id":"lua%-browser%-flow"'))
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
local oauth_webdav_retained_key = assert(vectis.auth.oauth2_webdav_key({
  credentials_path = auth_path,
  flow_id = "lua-flow",
  subject = "lua-oidc@example.com",
}))
local retained_ensure, retained_ensure_error =
  vectis.auth.oauth2_stored_flow_ensure({
    credentials_path = auth_path,
    flow_id = "lua-flow",
    transport = oauth_transport("fail"),
    token_endpoint = "https://idp.example.test/token",
    client_id = "vectis-client",
    client_secret = "vectis-secret",
    now = 6000,
    revoke_webdav_keys_on_failure = false,
  })
assert(retained_ensure == nil)
assert(type(retained_ensure_error) == "table")
local oauth_webdav_retained = assert(vectis.auth.verify({
  credentials_path = auth_path,
  authorization = "Basic " .. base64_encode(
    oauth_webdav_retained_key.client_id .. ":" ..
      oauth_webdav_retained_key.client_secret),
  allowed_modes = { "basic" },
}))
assert(oauth_webdav_retained.authenticated == true)
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
local email_token = assert(vectis.auth.email_token_issue({
  credentials_path = auth_path,
  state_path = auth_state_path,
  username = "lua-user@example.com",
  realm = "lua",
  email = "lua-user@example.com",
  pending_transaction_id = "lua-pending-1",
  transaction_id = "lua-email-tx-1",
  token = "123456",
  now = 1000,
  ttl_seconds = 300,
  max_attempts = 2,
}))
assert(file_contains(auth_state_path, "lua-email-tx-1"))
assert(not file_contains(auth_path, "lua-email-tx-1"))
assert(email_token.transaction_id == "lua-email-tx-1")
assert(email_token.token == "123456")
assert(email_token.expires_at == 1300)
local wrong_pending_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_path,
  state_path = auth_state_path,
  transaction_id = "lua-email-tx-1",
  username = "lua-user@example.com",
  realm = "lua",
  pending_transaction_id = "lua-pending-other",
  token = "123456",
  now = 1100,
}))
assert(wrong_pending_email_token.verified == false)
assert(wrong_pending_email_token.expired == false)
assert(wrong_pending_email_token.failed_attempts == 0)
assert(wrong_pending_email_token.max_attempts == 0)
local wrong_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_path,
  state_path = auth_state_path,
  transaction_id = "lua-email-tx-1",
  username = "lua-user@example.com",
  realm = "lua",
  pending_transaction_id = "lua-pending-1",
  token = "000000",
  now = 1100,
}))
assert(wrong_email_token.verified == false)
assert(wrong_email_token.expired == false)
assert(wrong_email_token.pending_transaction_id == "lua-pending-1")
assert(wrong_email_token.failed_attempts == 1)
assert(wrong_email_token.max_attempts == 2)
local verified_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_path,
  state_path = auth_state_path,
  transaction_id = "lua-email-tx-1",
  username = "lua-user@example.com",
  realm = "lua",
  pending_transaction_id = "lua-pending-1",
  token = "123456",
  now = 1100,
}))
assert(verified_email_token.verified == true)
assert(verified_email_token.expired == false)
assert(verified_email_token.username == "lua-user@example.com")
assert(verified_email_token.realm == "lua")
assert(verified_email_token.email == "lua-user@example.com")
assert(verified_email_token.pending_transaction_id == "lua-pending-1")
assert(verified_email_token.failed_attempts == 1)
assert(verified_email_token.max_attempts == 2)
local replayed_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_path,
  state_path = auth_state_path,
  transaction_id = "lua-email-tx-1",
  username = "lua-user@example.com",
  realm = "lua",
  token = "123456",
  now = 1100,
}))
assert(replayed_email_token.verified == false)
assert(replayed_email_token.expired == false)
local expiring_email_token = assert(vectis.auth.email_token_issue({
  credentials_path = auth_path,
  state_path = auth_state_path,
  username = "lua-user@example.com",
  realm = "lua",
  email = "lua-user@example.com",
  transaction_id = "lua-email-tx-2",
  token = "654321",
  now = 1000,
  ttl_seconds = 300,
}))
assert(expiring_email_token.expires_at == 1300)
local expired_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_path,
  state_path = auth_state_path,
  transaction_id = "lua-email-tx-2",
  username = "lua-user@example.com",
  realm = "lua",
  token = "654321",
  now = 1400,
}))
assert(expired_email_token.verified == false)
assert(expired_email_token.expired == true)
local expired_replay_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_path,
  state_path = auth_state_path,
  transaction_id = "lua-email-tx-2",
  username = "lua-user@example.com",
  realm = "lua",
  token = "654321",
  now = 1400,
}))
assert(expired_replay_email_token.verified == false)
assert(expired_replay_email_token.expired == false)
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
local webdav_authorization = assert(vectis.auth.basic_authorization(webdav_key))
assert(webdav_authorization == "Basic " .. base64_encode(
  webdav_key.client_id .. ":" .. webdav_key.client_secret))
assert(vectis.auth.basic_authorization(
  webdav_key.client_id, webdav_key.client_secret) == webdav_authorization)
local missing_basic, missing_basic_err =
  vectis.auth.basic_authorization({client_id = webdav_key.client_id})
assert(missing_basic == nil)
assert(missing_basic_err.status == vectis.ERR_INVALID)
assert(missing_basic_err.message:find("client_secret", 1, true))
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

assert(type(lql) == "table")
assert(type(lql.core) == "table")
assert(lql.core == require("lql.core"))
assert(type(lql.version()) == "string")
assert(type(lql.new) == "function")

assert(type(cai) == "table")
assert(type(cai.open) == "function")
assert(type(cai.mcp_handler) == "function")
assert(type(cai.MODEL_DEFAULT_RESPONSES) == "string")
assert(type(cai.MCP_PROTOCOL_VERSION) == "string")

assert(type(curl) == "table")
assert(type(curl.perform) == "function")
assert(type(curl.json) == "function")
assert(type(curl.stream_json) == "function")
assert(type(curl.version()) == "string")

assert(type(zlib) == "table")
assert(type(zlib.version()) == "string")
local zlib_payload = ("vectis zlib payload\n"):rep(32)
local deflated = assert(zlib.deflate(zlib_payload, { level = 9 }))
assert(deflated ~= zlib_payload)
assert(assert(zlib.inflate(deflated)) == zlib_payload)
assert(assert(zlib.decompress(deflated)) == zlib_payload)
assert(assert(zlib.decompress(assert(zlib.compress(zlib_payload)))) == zlib_payload)
local gzipped = assert(zlib.gzip(zlib_payload))
assert(assert(zlib.gunzip(gzipped)) == zlib_payload)
assert(assert(zlib.decompress(gzipped)) == zlib_payload)
local limited, limited_err = zlib.inflate(deflated, { max_output_bytes = 8 })
assert(limited == nil)
assert(limited_err.status == vectis.ERR_INVALID)
assert(limited_err.message:find("max_output_bytes", 1, true))
local invalid_zlib, invalid_zlib_err = zlib.decompress("not compressed")
assert(invalid_zlib == nil)
assert(invalid_zlib_err.status == vectis.ERR_INVALID)
local zlib_file_in = os.tmpname()
local zlib_file_gz = os.tmpname()
local zlib_file_out = os.tmpname()
local zlib_file_limited = os.tmpname()
local zlib_file_fp = assert(io.open(zlib_file_in, "wb"))
zlib_file_fp:write(zlib_payload)
zlib_file_fp:close()
local gzip_file_result = assert(zlib.gzip_file({
  input_path = zlib_file_in,
  output_path = zlib_file_gz,
  level = 9,
}))
assert(gzip_file_result.ok == true)
assert(gzip_file_result.input_bytes == #zlib_payload)
assert(gzip_file_result.output_bytes > 0)
local gunzip_file_result = assert(zlib.gunzip_file({
  input_path = zlib_file_gz,
  output_path = zlib_file_out,
}))
assert(gunzip_file_result.ok == true)
assert(gunzip_file_result.output_bytes == #zlib_payload)
zlib_file_fp = assert(io.open(zlib_file_out, "rb"))
assert(zlib_file_fp:read("*a") == zlib_payload)
zlib_file_fp:close()
local limited_file, limited_file_err = zlib.decompress_file({
  input_path = zlib_file_gz,
  output_path = zlib_file_limited,
  max_output_bytes = 8,
})
assert(limited_file == nil)
assert(limited_file_err.status == vectis.ERR_INVALID)
assert(limited_file_err.message:find("max_output_bytes", 1, true))
os.remove(zlib_file_in)
os.remove(zlib_file_gz)
os.remove(zlib_file_out)
os.remove(zlib_file_limited)

assert(type(vectis.http) == "table")
assert(type(vectis.http.get) == "function")
assert(type(vectis.http.post) == "function")
assert(type(vectis.http.put) == "function")
assert(type(vectis.http.patch) == "function")
assert(type(vectis.http.delete) == "function")
assert(type(vectis.http.form) == "function")
assert(type(vectis.http.form_encode) == "function")
assert(type(vectis.http.multipart) == "function")
assert(type(vectis.http.client) == "function")
assert(type(vectis.http.request_json) == "function")
assert(type(vectis.http.download) == "function")

assert(type(vectis.lockd) == "table")
assert(type(vectis.lockd.config) == "function")
assert(type(vectis.lockd.open) == "function")
assert(type(vectis.lockd.with_client) == "function")
assert(type(vectis.lockd.enqueue_json) == "function")
assert(type(vectis.lockd.load_json) == "function")
assert(type(vectis.lockd.save_json) == "function")
assert(type(vectis.lockd.with_acquired_lease) == "function")
assert(vectis.lockd.raw == lockdc)
assert(vectis.lockd.encode_json({ ok = true }) == '{"ok":true}')
assert(vectis.lockd.decode_json('{"ok":true}').ok == true)
assert(vectis.lockd.json_null == lockdc.json_null)
local vectis_lockd_required = require("vectis.lockd")
assert(vectis_lockd_required == vectis.lockd)
local normalized_lockd = assert(vectis.lockd.config({
  endpoints = { "https://127.0.0.1:1" },
  namespace = "lua-lockd",
  client_bundle = "/tmp/vectis-lockd-client.pem",
}))
assert(normalized_lockd.default_namespace == "lua-lockd")
assert(normalized_lockd.namespace == nil)
assert(normalized_lockd.client_bundle == nil)
assert(normalized_lockd.client_bundle_path == "/tmp/vectis-lockd-client.pem")
local embedded_lockd_config, embedded_lockd_err =
    vectis.lockd.config({ client_bundle = "embedded" })
assert(embedded_lockd_config == nil)
assert(type(embedded_lockd_err) == "table")
assert(embedded_lockd_err.status == vectis.ERR_STATE)
assert(embedded_lockd_err.status_string == "state")
assert(embedded_lockd_err.source == "vectis")
assert(embedded_lockd_err.source_code == vectis.ERROR_SOURCE_VECTIS)
assert(embedded_lockd_err.message == "no embedded lockd bundle")

assert(type(vectis.cert) == "table")
assert(type(vectis.cert.generate_bundle) == "function")
assert(type(vectis.cert.generate_private_key) == "function")
assert(type(vectis.cert.generate_csr) == "function")
assert(type(vectis.cert.inspect_bundle) == "function")
assert(type(vectis.cert.validate_bundle) == "function")
assert(type(vectis.cert.validate_pair) == "function")
assert(type(vectis.auth.basic_authorization) == "function")

assert(type(pslog) == "table")
assert(type(pslog.new_json) == "function")
assert(type(pslog.version()) == "string")
local log_chunks = {}
local raw_log = assert(pslog.new_json(function(chunk)
  log_chunks[#log_chunks + 1] = chunk
end, { timestamps = false }))
raw_log:info("lua smoke", "component", "vectis")
raw_log:close()
local log_payload = table.concat(log_chunks)
assert(log_payload:match('"msg":"lua smoke"'))
assert(log_payload:match('"component":"vectis"'))
assert(log.raw == pslog)
local vectis_log_chunks = {}
local vectis_logger = assert(log.new({
  output = function(chunk)
    vectis_log_chunks[#vectis_log_chunks + 1] = chunk
  end,
  disable_timestamp = true,
  no_color = true,
  fields = { service = "smoke" },
}))
local _, log_status_err = nil, status.error({
  kind = "smoke",
  message = "expected",
  status = status.ERR_INVALID,
  source_code = status.ERROR_SOURCE_VECTIS,
})
assert(log.log_error(vectis_logger, "error", "structured smoke",
                     log_status_err, { path = "/smoke" }))
vectis_logger:close()
local vectis_log_payload = table.concat(vectis_log_chunks)
assert(vectis_log_payload:match('"service":"smoke"'))
assert(vectis_log_payload:match('"status_string":"invalid"'))
assert(vectis_log_payload:match('"source":"vectis"'))
assert(vectis_log_payload:match('"path":"/smoke"'))
local level_ok, level_err =
    log.log_error(vectis_logger, "verbose", "bad level", log_status_err)
assert(level_ok == nil)
assert(level_err.status == vectis.ERR_INVALID)
assert(level_err.source_code == vectis.ERROR_SOURCE_VECTIS)

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
