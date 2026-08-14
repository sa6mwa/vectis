set(json_file "${WORK_DIR}/vectis-http-response.json")
set(download_source "${WORK_DIR}/vectis-http-download-source.txt")
set(download_target "${WORK_DIR}/vectis-http-download-target.txt")
set(upload_source "${WORK_DIR}/vectis-http-upload-source.txt")
set(upload_target "${WORK_DIR}/vectis-http-upload-target.txt")
set(auth_path "${WORK_DIR}/vectis-http-auth.json")
set(static_dir "${WORK_DIR}/vectis-http-static")
set(script "${WORK_DIR}/vectis-http-smoke.lua")

file(WRITE "${json_file}" "{\"ok\":true,\"message\":\"vectis-http\"}\n")
file(WRITE "${download_source}" "downloaded through curl file sink\n")
file(WRITE "${upload_source}" "uploaded through curl file source\n")
file(MAKE_DIRECTORY "${static_dir}/assets")
file(WRITE "${static_dir}/index.html" "static directory index\n")
file(WRITE "${static_dir}/assets/app.txt" "static directory asset\n")
file(REMOVE "${download_target}" "${upload_target}" "${auth_path}"
            "${auth_path}.lock")

file(WRITE "${script}" [[
local vectis = require("vectis")
local lonejson = require("lonejson")

local json_url = "file://" .. assert(arg[1])
local download_url = "file://" .. assert(arg[2])
local download_path = assert(arg[3])
local upload_path = assert(arg[4])
local upload_url = "file://" .. assert(arg[5])
local auth_path = assert(arg[6])
local static_dir = assert(arg[7])

local b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local function base64(data)
  local out = {}
  for i = 1, #data, 3 do
    local a = data:byte(i) or 0
    local b = data:byte(i + 1) or 0
    local c = data:byte(i + 2) or 0
    local n = a * 65536 + b * 256 + c
    local pad = (#data - i == 0) and 2 or ((#data - i == 1) and 1 or 0)
    out[#out + 1] = b64chars:sub(math.floor(n / 262144) % 64 + 1, math.floor(n / 262144) % 64 + 1)
    out[#out + 1] = b64chars:sub(math.floor(n / 4096) % 64 + 1, math.floor(n / 4096) % 64 + 1)
    out[#out + 1] = pad >= 2 and "=" or b64chars:sub(math.floor(n / 64) % 64 + 1, math.floor(n / 64) % 64 + 1)
    out[#out + 1] = pad >= 1 and "=" or b64chars:sub(n % 64 + 1, n % 64 + 1)
  end
  return table.concat(out)
end

assert(type(vectis.http) == "table")
assert(type(vectis.http.request) == "function")
assert(type(vectis.http.get) == "function")
assert(type(vectis.http.post) == "function")
assert(type(vectis.http.put) == "function")
assert(type(vectis.http.patch) == "function")
assert(type(vectis.http.delete) == "function")
assert(type(vectis.http.head) == "function")
assert(type(vectis.http.form) == "function")
assert(type(vectis.http.form_encode) == "function")
assert(type(vectis.http.multipart) == "function")
assert(type(vectis.http.client) == "function")
assert(type(vectis.http.request_json) == "function")
assert(type(vectis.http.get_json) == "function")
assert(type(vectis.http.post_json) == "function")
assert(type(vectis.http.download) == "function")
assert(type(vectis.http.upload) == "function")
assert(type(vectis.http.sftp_download) == "function")
assert(type(vectis.http.sftp_upload) == "function")
assert(require("vectis.http") == vectis.http)
assert(vectis.http.form_encode({
  q = "hello world",
  tag = {"a+b", "c/d"},
  empty = "",
}) == "empty=&q=hello%20world&tag=a%2Bb&tag=c%2Fd")

local decoded = vectis.http.get_json({
  url = json_url,
  protocols = "file",
})
assert(decoded.ok == true, decoded.error and decoded.error.message)
assert(decoded.transport_ok == true)
assert(decoded.json.message == "vectis-http")
assert(decoded.error == nil)

local file_client = vectis.http.client({
  protocols = "file",
  retry = false,
})
assert(type(file_client.get_json) == "function")
assert(type(file_client.form) == "function")
assert(type(file_client.multipart) == "function")
local client_decoded = file_client.get_json(json_url)
assert(client_decoded.ok == true,
       client_decoded.error and client_decoded.error.message)
assert(client_decoded.json.message == "vectis-http")

local retry_client = vectis.http.client({
  protocols = "http",
  timeout_ms = 200,
  connect_timeout_ms = 50,
  no_signal = true,
  retry = {
    max_attempts = 2,
    initial_delay_ms = 0,
    max_delay_ms = 0,
    conditions = {"transport"},
  },
})
local client_failed = retry_client.get("http://127.0.0.1:1/")
assert(client_failed.ok == false)
assert(client_failed.error.kind == "transport")
assert(client_failed.attempts == 2)

local failed = vectis.http.request({
  url = "http://127.0.0.1:1/",
  protocols = "http",
  timeout_ms = 200,
  connect_timeout_ms = 50,
  no_signal = true,
  retry = {
    max_attempts = 2,
    initial_delay_ms = 0,
    max_delay_ms = 0,
    conditions = {"transport"},
  },
})
assert(failed.ok == false)
assert(failed.transport_ok == false)
assert(failed.attempts == 2)
assert(failed.error.kind == "transport")
assert(failed.error.attempts == 2)
assert(type(failed.error.message) == "string")
assert(type(failed.error_message) == "string")

local downloaded = vectis.http.download({
  url = download_url,
  protocols = "file",
  download_path = download_path,
})
assert(downloaded.ok == true, downloaded.error and downloaded.error.message)
assert(downloaded.body == "")
do
  local fp = assert(io.open(download_path, "rb"))
  local body = fp:read("*a")
  fp:close()
  assert(body == "downloaded through curl file sink\n")
end

local uploaded = vectis.http.upload({
  url = upload_url,
  protocols = "file",
  upload_path = upload_path,
})
assert(uploaded.ok == true, uploaded.error and uploaded.error.message)
do
  local fp = assert(io.open(arg[5], "rb"))
  local body = fp:read("*a")
  fp:close()
  assert(body == "uploaded through curl file source\n")
end

local api_server = assert(vectis.server.new({
  bind = "127.0.0.1",
  port = 28484,
}))
assert(vectis.auth.store_init({credentials_path = auth_path}) == true)
assert(vectis.auth.user_add({
  credentials_path = auth_path,
  username = "http-user",
  password = "http-password",
}).username == "http-user")
local http_key = assert(vectis.auth.webdav_key({
  credentials_path = auth_path,
  username = "http-user",
  password = "http-password",
}))
local http_basic_auth = "Basic " .. base64(http_key.client_id .. ":" .. http_key.client_secret)
assert(api_server:json({
  path = "/status",
  body = '{"ok":true,"surface":"json"}\n',
  cache_control = "no-store",
}) == true)
assert(api_server:json({
  path = "/head-status",
  method = "HEAD",
  body = '{"ok":true,"surface":"head"}\n',
  cache_control = "no-store",
}) == true)
assert(api_server:json({
  path = "/created",
  method = "POST",
  status = 201,
  body = '{"ok":true,"created":true}\n',
}) == true)
assert(api_server:text({
  path = "/plain",
  body = "plain text route\n",
  cache_control = "max-age=7",
}) == true)
assert(api_server:redirect({
  path = "/go",
  location = "/status",
  status = 303,
  body = "see status\n",
  cache_control = "no-store",
}) == true)
assert(api_server:auth_json({
  path = "/guarded-created",
  method = "POST",
  status_code = 202,
  auth = {
    kind = "native",
    credentials_path = auth_path,
    realm = "http",
    purpose = "webdav",
  },
  body = '{"ok":true,"guarded":true}\n',
}) == true)
local native_provider = assert(vectis.auth.provider_native({
  credentials_path = auth_path,
  realm = "http-provider",
  purpose = "webdav",
}))
assert(api_server:auth_json({
  path = "/provider-native-guarded",
  auth = native_provider,
  body = '{"ok":true,"provider":"native"}\n',
}) == true)
local callback_provider = assert(vectis.auth.provider_callback(function(request)
  if request.authorization == "Bearer callback-ok" and
      request.resource == "/callback-guarded" then
    return {action = "allow", principal = "callback-user"}
  end
  if request.authorization == "Bearer callback-direct" and
      request.resource == "/callback-direct" then
    return {action = "allow", principal = "callback-direct-user"}
  end
  return {
    action = "required",
    status_code = 401,
    www_authenticate = 'Bearer realm="callback"',
    content_type = "text/plain; charset=utf-8",
    body = "callback login required\n",
  }
end))
assert(api_server:auth_json({
  path = "/callback-guarded",
  auth = {
    provider = callback_provider,
    purpose = "callback",
    allowed_modes = {"bearer"},
  },
  body = '{"ok":true,"provider":"callback"}\n',
}) == true)
assert(api_server:auth_json({
  path = "/callback-direct",
  auth = callback_provider,
  body = '{"ok":true,"provider":"callback-direct"}\n',
}) == true)
assert(api_server:route({
  path = "/dynamic/:name",
  methods = {"GET", "POST"},
  body = {
    mode = "buffered",
    max_bytes = 4096,
  },
  handler = function(request)
    local name = assert(request.param("name"))
    local suffix = request.query("suffix") or ""
    local trace = request.header("x-vectis-trace") or "none"
    if request.method == "POST" then
      assert(request.body == "payload=1")
      assert(request.body_size == 9)
      return {
        status = 201,
        content_type = "text/plain; charset=utf-8",
        headers = {
          ["x-vectis-handler"] = "dynamic",
        },
        body = name .. suffix .. ":" .. trace .. ":" .. request.body .. "\n",
      }
    end
    return {
      status = 200,
      headers = {
        ["x-vectis-handler"] = "dynamic",
      },
      body = name .. suffix .. ":" .. trace .. "\n",
    }
  end,
}) == true)
assert(api_server:route({
  path = "/dynamic-file",
  handler = function()
    return {
      status = 200,
      content_type = "text/plain",
      file_path = static_dir .. "/index.html",
    }
  end,
}) == true)
assert(api_server:static_directory({
  path_prefix = "/files",
  root_dir = static_dir,
  content_type = "text/plain",
  index_file = "index.html",
}) == true)
assert(api_server:start() == true)
local api_response
for _ = 1, 20 do
  api_response = vectis.http.request({
    url = "http://127.0.0.1:28484/status",
    protocols = "http",
    timeout_ms = 2000,
    connect_timeout_ms = 1000,
    no_signal = true,
  })
  if api_response.ok then break end
  os.execute("sleep 0.1")
end
assert(api_response.ok == true, api_response.error and api_response.error.message)
assert(api_response.status == 200)
assert(api_response.body == '{"ok":true,"surface":"json"}\n')
assert(api_response.headers:lower():find("cache-control: no-store", 1, true))
local text_response = vectis.http.get("http://127.0.0.1:28484/plain", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(text_response.ok == true, text_response.error and text_response.error.message)
assert(text_response.status == 200)
assert(text_response.body == "plain text route\n")
assert(text_response.headers:lower():find("content-type: text/plain; charset=utf-8", 1, true))
assert(text_response.headers:lower():find("cache-control: max-age=7", 1, true))
local redirect_response = vectis.http.get("http://127.0.0.1:28484/go", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(redirect_response.ok == true,
       redirect_response.error and redirect_response.error.message)
assert(redirect_response.status == 303)
assert(redirect_response.body == "see status\n")
assert(redirect_response.headers:lower():find("location: /status", 1, true))
assert(redirect_response.headers:lower():find("cache-control: no-store", 1, true))
local dynamic_get = vectis.http.get("http://127.0.0.1:28484/dynamic/alice?suffix=-ok", {
  headers = {
    ["x-vectis-trace"] = "trace-get",
  },
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(dynamic_get.ok == true,
       dynamic_get.error and dynamic_get.error.message)
assert(dynamic_get.status == 200)
assert(dynamic_get.body == "alice-ok:trace-get\n")
assert(dynamic_get.headers:lower():find("x-vectis-handler: dynamic", 1, true))
local dynamic_post = vectis.http.post("http://127.0.0.1:28484/dynamic/bob?suffix=-ok", {
  body = "payload=1",
  headers = {
    ["x-vectis-trace"] = "trace-post",
  },
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(dynamic_post.ok == true,
       dynamic_post.error and dynamic_post.error.message)
assert(dynamic_post.status == 201)
assert(dynamic_post.body == "bob-ok:trace-post:payload=1\n")
assert(dynamic_post.headers:lower():find("x-vectis-handler: dynamic", 1, true))
local dynamic_file = vectis.http.get("http://127.0.0.1:28484/dynamic-file", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(dynamic_file.ok == true,
       dynamic_file.error and dynamic_file.error.message)
assert(dynamic_file.status == 200)
assert(dynamic_file.body == "static directory index\n")
local simple_get = vectis.http.get("http://127.0.0.1:28484/status", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(simple_get.ok == true, simple_get.error and simple_get.error.message)
assert(simple_get.status == 200)
local simple_head = vectis.http.head("http://127.0.0.1:28484/head-status", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(simple_head.ok == true, simple_head.error and simple_head.error.message)
assert(simple_head.status == 200)
assert(simple_head.body == "")
local created_response = vectis.http.request({
  url = "http://127.0.0.1:28484/created",
  method = "POST",
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(created_response.ok == true,
       created_response.error and created_response.error.message)
assert(created_response.status == 201)
assert(created_response.body == '{"ok":true,"created":true}\n')
local simple_post = vectis.http.post("http://127.0.0.1:28484/created", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(simple_post.ok == true, simple_post.error and simple_post.error.message)
assert(simple_post.status == 201)
local form_post = vectis.http.form({
  url = "http://127.0.0.1:1/form",
  form = {
    username = "form user",
    token = "a+b",
  },
  timeout_ms = 200,
  connect_timeout_ms = 50,
  no_signal = true,
  retry = false,
})
assert(form_post.ok == false)
assert(form_post.error.kind == "transport")
local invalid_multipart_ok, invalid_multipart_err = pcall(vectis.http.multipart, {
  url = "http://127.0.0.1:28484/created",
  parts = {
    {name = "bad-file", path = ""},
  },
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(invalid_multipart_ok == false)
assert(tostring(invalid_multipart_err):find("multipart file path", 1, true))
local multipart_post = vectis.http.multipart({
  url = "http://127.0.0.1:28484/created",
  parts = {
    description = "multipart text field",
    upload = {
      path = upload_path,
      filename = "upload.txt",
      content_type = "text/plain",
    },
  },
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(multipart_post.transport_ok == true,
       multipart_post.error and multipart_post.error.message)
assert(multipart_post.ok == false)
assert(multipart_post.status == 413)
assert(multipart_post.error.kind == "http_status")
local anonymous_guarded = vectis.http.request({
  url = "http://127.0.0.1:28484/guarded-created",
  method = "POST",
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(anonymous_guarded.status == 401)
assert(anonymous_guarded.headers:lower():find('www-authenticate: basic realm="http"', 1, true))
local guarded_created = vectis.http.request({
  url = "http://127.0.0.1:28484/guarded-created",
  method = "POST",
  headers = {Authorization = http_basic_auth},
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(guarded_created.ok == true,
       guarded_created.error and guarded_created.error.message)
assert(guarded_created.status == 202)
assert(guarded_created.body == '{"ok":true,"guarded":true}\n')
assert(guarded_created.headers:lower():find("cache-control: no-store", 1, true))
local provider_native_required = vectis.http.request({
  url = "http://127.0.0.1:28484/provider-native-guarded",
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(provider_native_required.status == 401)
assert(provider_native_required.headers:lower():find('www-authenticate: basic realm="http-provider"', 1, true))
local provider_native_allowed = vectis.http.request({
  url = "http://127.0.0.1:28484/provider-native-guarded",
  headers = {Authorization = http_basic_auth},
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(provider_native_allowed.ok == true,
       provider_native_allowed.error and provider_native_allowed.error.message)
assert(provider_native_allowed.status == 200)
assert(provider_native_allowed.body == '{"ok":true,"provider":"native"}\n')
local authenticated_client = vectis.http.client({
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
  headers = {Authorization = http_basic_auth},
})
local client_provider_native_allowed =
    authenticated_client.get("http://127.0.0.1:28484/provider-native-guarded")
assert(client_provider_native_allowed.ok == true,
       client_provider_native_allowed.error and
       client_provider_native_allowed.error.message)
assert(client_provider_native_allowed.status == 200)
local client_head = authenticated_client.head(
    "http://127.0.0.1:28484/head-status")
assert(client_head.ok == true, client_head.error and client_head.error.message)
assert(client_head.status == 200)
assert(client_head.body == "")
local client_callback_allowed = authenticated_client.get(
    "http://127.0.0.1:28484/callback-guarded",
    {headers = {Authorization = "Bearer callback-ok"}})
assert(client_callback_allowed.ok == true,
       client_callback_allowed.error and client_callback_allowed.error.message)
assert(client_callback_allowed.status == 200)
local callback_required = vectis.http.request({
  url = "http://127.0.0.1:28484/callback-guarded",
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(callback_required.status == 401)
assert(callback_required.body == "callback login required\n")
assert(callback_required.headers:lower():find('www-authenticate: bearer realm="callback"', 1, true))
local callback_allowed = vectis.http.request({
  url = "http://127.0.0.1:28484/callback-guarded",
  headers = {Authorization = "Bearer callback-ok"},
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(callback_allowed.ok == true,
       callback_allowed.error and callback_allowed.error.message)
assert(callback_allowed.status == 200)
assert(callback_allowed.body == '{"ok":true,"provider":"callback"}\n')
assert(callback_allowed.headers:lower():find("cache-control: no-store", 1, true))
local callback_direct_allowed = vectis.http.request({
  url = "http://127.0.0.1:28484/callback-direct",
  headers = {Authorization = "Bearer callback-direct"},
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(callback_direct_allowed.ok == true,
       callback_direct_allowed.error and callback_direct_allowed.error.message)
assert(callback_direct_allowed.status == 200)
assert(callback_direct_allowed.body == '{"ok":true,"provider":"callback-direct"}\n')
local static_index = vectis.http.request({
  url = "http://127.0.0.1:28484/files",
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(static_index.ok == true,
       static_index.error and static_index.error.message)
assert(static_index.status == 200)
assert(static_index.body == "static directory index\n")
local static_asset = vectis.http.request({
  url = "http://127.0.0.1:28484/files/assets/app.txt",
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(static_asset.ok == true,
       static_asset.error and static_asset.error.message)
assert(static_asset.status == 200)
assert(static_asset.body == "static directory asset\n")
local static_traversal = vectis.http.request({
  url = "http://127.0.0.1:28484/files/../vectis-http-response.json",
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(static_traversal.status == 404)
assert(api_server:stop() == true)
api_server:close()

local response_schema = lonejson.schema("vectis-http-response", {
  lonejson.field("ok", lonejson.boolean()),
  lonejson.field("message", lonejson.string()),
})
local streamed = vectis.http.stream_json({
  url = json_url,
  protocols = "file",
  response = {schema = response_schema},
})
assert(streamed.ok == true, streamed.error and streamed.error.message)
assert(streamed.json.ok == true)
assert(streamed.response_json.message == "vectis-http")
]])

execute_process(COMMAND "${VECTIS_BIN}" "${script}" "${json_file}"
                        "${download_source}" "${download_target}"
                        "${upload_source}" "${upload_target}" "${auth_path}"
                        "${static_dir}"
                RESULT_VARIABLE http_result
                OUTPUT_VARIABLE http_stdout
                ERROR_VARIABLE http_stderr)
if(NOT http_result EQUAL 0)
  message(FATAL_ERROR "vectis HTTP Lua smoke failed: ${http_stdout}${http_stderr}")
endif()
