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
local rest = require("vectis.rest")

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
assert(type(vectis.http.options) == "function")
assert(type(vectis.http.form) == "function")
assert(type(vectis.http.form_encode) == "function")
assert(type(vectis.http.multipart) == "function")
assert(type(vectis.http.client) == "function")
assert(type(vectis.http.request_json) == "function")
assert(type(vectis.http.get_json) == "function")
assert(type(vectis.http.post_json) == "function")
assert(type(vectis.http.options_json) == "function")
assert(type(vectis.http.download) == "function")
assert(type(vectis.http.download_file) == "function")
assert(type(vectis.http.upload) == "function")
assert(type(vectis.http.upload_file) == "function")
assert(type(vectis.http.sftp_download) == "function")
assert(type(vectis.http.sftp_download_file) == "function")
assert(type(vectis.http.sftp_upload) == "function")
assert(type(vectis.http.sftp_upload_file) == "function")
assert(require("vectis.http") == vectis.http)
assert(vectis.rest == rest)
assert(type(rest.route) == "function")
assert(type(rest.group) == "function")
assert(type(rest.client) == "function")
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
assert(type(file_client.options_json) == "function")
assert(type(file_client.download_file) == "function")
assert(type(file_client.upload_file) == "function")
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
local download_file_path = download_path .. ".file-helper"
os.remove(download_file_path)
local downloaded_file = vectis.http.download_file(download_url, download_file_path, {
  protocols = "file",
})
assert(downloaded_file.ok == true,
       downloaded_file.error and downloaded_file.error.message)
assert(downloaded_file.body == "")
do
  local fp = assert(io.open(download_file_path, "rb"))
  local body = fp:read("*a")
  fp:close()
  assert(body == "downloaded through curl file sink\n")
end
local client_download_path = download_path .. ".client-helper"
os.remove(client_download_path)
local client_downloaded_file =
    file_client.download_file(download_url, client_download_path)
assert(client_downloaded_file.ok == true,
       client_downloaded_file.error and client_downloaded_file.error.message)
do
  local fp = assert(io.open(client_download_path, "rb"))
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
local upload_file_target = arg[5] .. ".file-helper"
os.remove(upload_file_target)
local uploaded_file = vectis.http.upload_file("file://" .. upload_file_target,
                                              upload_path, {
  protocols = "file",
})
assert(uploaded_file.ok == true,
       uploaded_file.error and uploaded_file.error.message)
do
  local fp = assert(io.open(upload_file_target, "rb"))
  local body = fp:read("*a")
  fp:close()
  assert(body == "uploaded through curl file source\n")
end
local client_upload_file_target = arg[5] .. ".client-helper"
os.remove(client_upload_file_target)
local client_uploaded_file =
    file_client.upload_file("file://" .. client_upload_file_target, upload_path)
assert(client_uploaded_file.ok == true,
       client_uploaded_file.error and client_uploaded_file.error.message)
do
  local fp = assert(io.open(client_upload_file_target, "rb"))
  local body = fp:read("*a")
  fp:close()
  assert(body == "uploaded through curl file source\n")
end

local api_server = assert(vectis.server.new({
  bind = "127.0.0.1",
  port = 28484,
}))
assert(type(api_server.group) == "function")
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
  path = "/options-status",
  method = "OPTIONS",
  body = '{"ok":true,"surface":"options"}\n',
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
  if request.authorization == "Bearer route-ok" and
      request.resource == "/dynamic-guarded" then
    return {action = "allow", principal = "route-user"}
  end
  if request.authorization == "Bearer route-ok" and
      request.resource == "/dynamic-dsv" then
    return {action = "allow", principal = "route-user"}
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
assert(api_server:route({
  path = "/dynamic-spooled",
  handler = function()
    local chunks = {"spooled ", "source ", "response\n"}
    local index = 1
    return {
      status = 203,
      content_type = "text/plain",
      headers = {
        ["x-vectis-spooled"] = "callback",
      },
      spooled_source = {
        reset = function()
          index = 1
          return true
        end,
        read = function(max_bytes)
          assert(type(max_bytes) == "number")
          assert(max_bytes > 0)
          local chunk = chunks[index]
          index = index + 1
          return chunk
        end,
        close = function()
          local fp = assert(io.open(static_dir .. "/spooled-close.txt", "wb"))
          fp:write("spooled source closed\n")
          fp:close()
        end,
      },
    }
  end,
}) == true)
assert(api_server:route({
  path = "/dynamic-spooled-single-pass",
  handler = function()
    local done = false
    return {
      status = 200,
      content_type = "text/plain",
      spooled_source = {
        read = function()
          if done then
            return nil
          end
          done = true
          return "single pass spooled response\n"
        end,
      },
    }
  end,
}) == true)
assert(api_server:route({
  path = "/dynamic-spooled-bad",
  handler = function()
    return {
      status = 200,
      spooled_source = {
        read = function()
          return {}
        end,
      },
    }
  end,
}) == true)
assert(api_server:route({
  path = "/dynamic-stream",
  handler = function()
    local chunks = {"live ", "stream ", "response\n"}
    local index = 1
    local closed = false
    return {
      status = 202,
      content_type = "text/plain",
      headers = {
        ["x-vectis-stream"] = "callback",
      },
      stream_source = {
        read = function(max_bytes)
          assert(type(max_bytes) == "number")
          assert(max_bytes > 0)
          assert(closed == false)
          local chunk = chunks[index]
          index = index + 1
          return chunk
        end,
        close = function()
          closed = true
        end,
      },
    }
  end,
}) == true)
assert(api_server:sse({
  path = "/dynamic-sse",
  open = function(request)
    assert(request.path == "/dynamic-sse")
    return { index = 1 }
  end,
  read = function(state, max_bytes)
    assert(type(max_bytes) == "number")
    assert(max_bytes > 0)
    if state.index == 1 then
      state.index = 2
      return {
        id = "1",
        event = "ready",
        data = "alpha\nbeta",
      }
    end
    if state.index == 2 then
      state.index = 3
      return { comment = "heartbeat" }
    end
    if state.index == 3 then
      state.index = 4
      return "done"
    end
    return nil
  end,
  close = function(state)
    local fp = assert(io.open(static_dir .. "/sse-close.txt", "wb"))
    fp:write("sse closed at " .. state.index .. "\n")
    fp:close()
  end,
}) == true)
assert(api_server:route({
  path = "/dynamic-guarded",
  auth = {
    provider = callback_provider,
    purpose = "callback",
    allowed_modes = {"bearer"},
  },
  handler = function(request)
    assert(request.principal == "route-user")
    return {
      status = 200,
      content_type = "application/json",
      body = '{"ok":true,"principal":"' .. request.principal .. '"}\n',
    }
  end,
}) == true)
local bad_route_group, bad_route_group_err = api_server:group({
  prefix = "/bad-group",
  routes = {
    {
      path = "bad",
      before = "not-a-function",
      handler = function()
        return "bad\n"
      end,
    },
  },
})
assert(bad_route_group == nil)
assert(bad_route_group_err.status == vectis.ERR_INVALID)
assert(bad_route_group_err.message:find("before hook", 1, true))
assert(api_server:group({
  prefix = "/group",
  methods = {"GET", "POST"},
  body = {
    mode = "buffered",
    max_bytes = 1024,
  },
  before = function(request)
    assert(request.path:find("/group/", 1, true) == 1)
    if request.header("x-block") == "1" then
      return {
        status = 418,
        content_type = "text/plain; charset=utf-8",
        body = "blocked by before\n",
      }
    end
    return true
  end,
  after = function(request, response)
    response.headers = response.headers or {}
    response.headers["x-vectis-group"] = request.method
    response.body = response.body .. "after=" .. request.method .. "\n"
    return response
  end,
  routes = {
    {
      path = "hello/:name",
      handler = function(request)
        return {
          status = request.method == "POST" and 201 or 200,
          content_type = "text/plain; charset=utf-8",
          body = request.param("name") .. ":" ..
              (request.query("suffix") or "") .. ":" ..
              tostring(request.body or "") .. "\n",
        }
      end,
    },
    {
      path = "/short-circuit",
      handler = function()
        error("before hook should short-circuit")
      end,
    },
  },
}) == true)
assert(rest.route(api_server, {
  path = "/rest/echo/:id",
  method = "POST",
  max_body_bytes = 1024,
  validate = function(json)
    if type(json) ~= "table" or type(json.name) ~= "string" then
      return false, {
        kind = "validation",
        message = "name is required",
        status = vectis.ERR_INVALID,
      }
    end
    return true
  end,
  handler = function(request)
    return {
      id = request.param("id"),
      name = request.json.name,
      suffix = request.query("suffix"),
    }, {
      status = 201,
      headers = {
        ["x-vectis-rest"] = "echo",
      },
    }
  end,
}) == true)
assert(rest.group(api_server, {
  prefix = "/rest-group",
  decode_json = false,
  routes = {
    {
      path = "status",
      handler = function()
        return {
          ok = true,
          grouped = true,
        }
      end,
    },
  },
}) == true)
local dsv_route_schema = lonejson.schema("lua-http-dsv-route-row", {
  lonejson.field("id", lonejson.string({required = true})),
  lonejson.field("count", lonejson.i64({required = true})),
})
local dsv_route_rows = {}
local dsv_route_sum = 0
assert(api_server:dsv({
  path = "/dynamic-dsv",
  schema = dsv_route_schema,
  auth = {
    provider = callback_provider,
    purpose = "callback",
    allowed_modes = {"bearer"},
  },
  on_row = function(row_number, row, request)
    assert(request.method == "POST")
    assert(request.body == nil)
    assert(request.principal == "route-user")
    dsv_route_rows[#dsv_route_rows + 1] = row_number .. ":" .. row.id
    dsv_route_sum = dsv_route_sum + row.count
  end,
  on_complete = function(request, summary)
    assert(request.path == "/dynamic-dsv")
    assert(summary.rows == 2)
    return {
      status = 200,
      content_type = "application/json",
      body = '{"rows":' .. tostring(summary.rows) ..
             ',"sum":' .. tostring(dsv_route_sum) ..
             ',"ids":"' .. table.concat(dsv_route_rows, ",") .. '"}\n',
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
local dynamic_spooled = vectis.http.get("http://127.0.0.1:28484/dynamic-spooled", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(dynamic_spooled.ok == true,
       dynamic_spooled.error and dynamic_spooled.error.message)
assert(dynamic_spooled.status == 203)
assert(dynamic_spooled.body == "spooled source response\n")
assert(dynamic_spooled.headers:lower():find("x-vectis-spooled: callback", 1, true))
local dynamic_spooled_closed = vectis.http.get(
    "http://127.0.0.1:28484/files/spooled-close.txt", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(dynamic_spooled_closed.ok == true,
       dynamic_spooled_closed.error and dynamic_spooled_closed.error.message)
assert(dynamic_spooled_closed.body == "spooled source closed\n")
local dynamic_spooled_single = vectis.http.get(
    "http://127.0.0.1:28484/dynamic-spooled-single-pass", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(dynamic_spooled_single.ok == true,
       dynamic_spooled_single.error and dynamic_spooled_single.error.message)
assert(dynamic_spooled_single.status == 200)
assert(dynamic_spooled_single.body == "single pass spooled response\n")
local dynamic_spooled_bad = vectis.http.get(
    "http://127.0.0.1:28484/dynamic-spooled-bad", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(dynamic_spooled_bad.ok == false)
assert(dynamic_spooled_bad.status == 500)
local dynamic_stream = vectis.http.get(
    "http://127.0.0.1:28484/dynamic-stream", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(dynamic_stream.ok == true,
       dynamic_stream.error and dynamic_stream.error.message)
assert(dynamic_stream.status == 202)
assert(dynamic_stream.body == "live stream response\n")
assert(dynamic_stream.headers:lower():find("x-vectis-stream: callback", 1, true))
assert(dynamic_stream.headers:lower():find("transfer-encoding: chunked", 1, true))
local dynamic_sse = vectis.http.get(
    "http://127.0.0.1:28484/dynamic-sse", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(dynamic_sse.ok == true,
       dynamic_sse.error and dynamic_sse.error.message)
assert(dynamic_sse.status == 200)
assert(dynamic_sse.body ==
       "id: 1\nevent: ready\ndata: alpha\ndata: beta\n\n" ..
       ": heartbeat\n\n" ..
       "data: done\n\n")
assert(dynamic_sse.headers:lower():find("content-type: text/event-stream", 1, true))
assert(dynamic_sse.headers:lower():find("cache-control: no-cache", 1, true))
assert(dynamic_sse.headers:lower():find("x-accel-buffering: no", 1, true))
assert(dynamic_sse.headers:lower():find("transfer-encoding: chunked", 1, true))
local dynamic_sse_closed = vectis.http.get(
    "http://127.0.0.1:28484/files/sse-close.txt", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(dynamic_sse_closed.ok == true,
       dynamic_sse_closed.error and dynamic_sse_closed.error.message)
assert(dynamic_sse_closed.body == "sse closed at 4\n")
local dynamic_guarded_required = vectis.http.get("http://127.0.0.1:28484/dynamic-guarded", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(dynamic_guarded_required.ok == false)
assert(dynamic_guarded_required.error.http_status == 401)
assert(dynamic_guarded_required.error.message:find("HTTP request failed", 1, true))
assert(dynamic_guarded_required.error.body == "callback login required\n")
assert(dynamic_guarded_required.error.http_status == 401)
local dynamic_guarded_allowed = vectis.http.get("http://127.0.0.1:28484/dynamic-guarded", {
  headers = {
    Authorization = "Bearer route-ok",
  },
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(dynamic_guarded_allowed.ok == true,
       dynamic_guarded_allowed.error and dynamic_guarded_allowed.error.message)
assert(dynamic_guarded_allowed.status == 200)
assert(dynamic_guarded_allowed.body == '{"ok":true,"principal":"route-user"}\n')
assert(dynamic_guarded_allowed.headers:lower():find("cache-control: no-store", 1, true))
local group_get = vectis.http.get("http://127.0.0.1:28484/group/hello/alice?suffix=ok", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(group_get.ok == true, group_get.error and group_get.error.message)
assert(group_get.status == 200)
assert(group_get.body == "alice:ok:\nafter=GET\n")
assert(group_get.headers:lower():find("x-vectis-group: get", 1, true))
local group_post = vectis.http.post("http://127.0.0.1:28484/group/hello/bob?suffix=ok", {
  body = "payload=1",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(group_post.ok == true, group_post.error and group_post.error.message)
assert(group_post.status == 201)
assert(group_post.body == "bob:ok:payload=1\nafter=POST\n")
assert(group_post.headers:lower():find("x-vectis-group: post", 1, true))
local group_blocked = vectis.http.get("http://127.0.0.1:28484/group/short-circuit", {
  headers = {
    ["x-block"] = "1",
  },
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(group_blocked.ok == false)
assert(group_blocked.status == 418)
assert(group_blocked.error.http_status == 418)
assert(group_blocked.error.body == "blocked by before\n")
local rest_client = rest.client({
  base_url = "http://127.0.0.1:28484",
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
local rest_echo = rest_client.post("/rest/echo/42?suffix=ok", {
  json = {name = "alice"},
})
assert(rest_echo.ok == true, rest_echo.error and rest_echo.error.message)
assert(rest_echo.status == 201)
assert(rest_echo.json.id == "42")
assert(rest_echo.json.name == "alice")
assert(rest_echo.json.suffix == "ok")
assert(rest_echo.headers:lower():find("x-vectis-rest: echo", 1, true))
local rest_validation = rest_client.post("/rest/echo/42", {
  json = {},
})
assert(rest_validation.ok == false)
assert(rest_validation.status == 400)
assert(rest_validation.error.http_status == 400)
assert(rest_validation.json.error.kind == "validation")
assert(rest_validation.json.error.status == vectis.ERR_INVALID)
local rest_invalid_json = rest_client.post("/rest/echo/42", {
  body = "{bad",
})
assert(rest_invalid_json.ok == false)
assert(rest_invalid_json.status == 400)
assert(rest_invalid_json.json.error.kind == "json_parse")
assert(rest_invalid_json.json.error.source == "lonejson")
local rest_group = rest_client.get("/rest-group/status")
assert(rest_group.ok == true, rest_group.error and rest_group.error.message)
assert(rest_group.status == 200)
assert(rest_group.json.ok == true)
assert(rest_group.json.grouped == true)
local rest_head = rest_client.head("/head-status")
assert(rest_head.ok == true, rest_head.error and rest_head.error.message)
assert(rest_head.status == 200)
assert(rest_head.body == "")
assert(rest_head.headers:lower():find("cache-control: no-store", 1, true))
local rest_options = rest_client.options("/options-status")
assert(rest_options.ok == true, rest_options.error and rest_options.error.message)
assert(rest_options.status == 200)
assert(rest_options.json.ok == true)
assert(rest_options.json.surface == "options")
local dynamic_dsv_required = vectis.http.post("http://127.0.0.1:28484/dynamic-dsv", {
  body = "id,count\nalpha,2\nbeta,3\n",
  headers = {
    ["content-type"] = "text/csv",
  },
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(dynamic_dsv_required.ok == false)
assert(dynamic_dsv_required.error.http_status == 401)
local dynamic_dsv_allowed = vectis.http.post("http://127.0.0.1:28484/dynamic-dsv", {
  body = "id,count\nalpha,2\nbeta,3\n",
  headers = {
    ["content-type"] = "text/csv",
    Authorization = "Bearer route-ok",
  },
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(dynamic_dsv_allowed.ok == true,
       dynamic_dsv_allowed.error and dynamic_dsv_allowed.error.message)
assert(dynamic_dsv_allowed.status == 200)
assert(dynamic_dsv_allowed.body == '{"rows":2,"sum":5,"ids":"1:alpha,2:beta"}\n')
assert(dynamic_dsv_allowed.headers:lower():find("cache-control: no-store", 1, true))
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
local simple_options = vectis.http.options("http://127.0.0.1:28484/options-status", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(simple_options.ok == true,
       simple_options.error and simple_options.error.message)
assert(simple_options.status == 200)
assert(simple_options.body == '{"ok":true,"surface":"options"}\n')
local simple_options_json = vectis.http.options_json("http://127.0.0.1:28484/options-status", {
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(simple_options_json.ok == true,
       simple_options_json.error and simple_options_json.error.message)
assert(simple_options_json.status == 200)
assert(simple_options_json.json.ok == true)
assert(simple_options_json.json.surface == "options")
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
local client_options = authenticated_client.options(
    "http://127.0.0.1:28484/options-status")
assert(client_options.ok == true,
       client_options.error and client_options.error.message)
assert(client_options.status == 200)
assert(client_options.body == '{"ok":true,"surface":"options"}\n')
local client_options_json = authenticated_client.options_json(
    "http://127.0.0.1:28484/options-status")
assert(client_options_json.ok == true,
       client_options_json.error and client_options_json.error.message)
assert(client_options_json.status == 200)
assert(client_options_json.json.ok == true)
assert(client_options_json.json.surface == "options")
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
