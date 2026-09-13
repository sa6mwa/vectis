include("${CMAKE_CURRENT_LIST_DIR}/port_retry.cmake")

set(cache_dir "${WORK_DIR}/vectis-webdav-server-cache")
set(root_dir "${WORK_DIR}/vectis-webdav-server-root")
set(auth_pouch_root "${WORK_DIR}/vectis-webdav-server-auth-pouch")
set(auth_lockd_endpoint "pouch://${auth_pouch_root}?single_writer=false")
set(script "${WORK_DIR}/vectis-webdav-server.lua")

file(REMOVE_RECURSE "${cache_dir}")
file(REMOVE_RECURSE "${root_dir}")
file(REMOVE_RECURSE "${auth_pouch_root}")

execute_process(
  COMMAND "${VECTIS_BIN}" -a users --lockd-endpoint "${auth_lockd_endpoint}"
          --add "dav-user" --password "dav-password"
  RESULT_VARIABLE auth_user_result
  OUTPUT_VARIABLE auth_user_output
  ERROR_VARIABLE auth_user_error)
if(NOT auth_user_result EQUAL 0)
  message(FATAL_ERROR "WebDAV auth user provisioning failed: ${auth_user_error}")
endif()
execute_process(
  COMMAND "${VECTIS_BIN}" -a users --lockd-endpoint "${auth_lockd_endpoint}"
          --webdav-key "dav-user" --password "dav-password"
  RESULT_VARIABLE auth_key_result
  OUTPUT_VARIABLE auth_key_output
  ERROR_VARIABLE auth_key_error)
if(NOT auth_key_result EQUAL 0)
  message(FATAL_ERROR "WebDAV auth key provisioning failed: ${auth_key_error}")
endif()
string(REGEX MATCH "client_id=([^\n]+)" auth_client_id_match
       "${auth_key_output}")
if(NOT auth_client_id_match)
  message(FATAL_ERROR "WebDAV auth key did not include client_id")
endif()
set(auth_client_id "${CMAKE_MATCH_1}")
string(REGEX MATCH "client_secret=([^\n]+)" auth_client_secret_match
       "${auth_key_output}")
if(NOT auth_client_secret_match)
  message(FATAL_ERROR "WebDAV auth key did not include client_secret")
endif()
set(auth_client_secret "${CMAKE_MATCH_1}")

string(CONFIGURE [=[
local vectis = require("vectis")
local webdav = require("vectis.webdav")

local port = tonumber(assert(os.getenv("VECTIS_WEBDAV_SERVER_PORT")))
local cache_dir = [[@cache_dir@]]
local root_dir = [[@root_dir@]]
local auth_lockd_endpoint = [[@auth_lockd_endpoint@]]
local auth_client_id = [[@auth_client_id@]]
local auth_client_secret = [[@auth_client_secret@]]
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

local function read_file(path)
  local file = assert(io.open(path, "rb"))
  local body = file:read("*a")
  file:close()
  return body
end

local basic_auth = "Basic " .. base64(auth_client_id .. ":" .. auth_client_secret)

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

local server = assert(vectis.app.new({
  bind = "127.0.0.1",
  port = port,
  lockd = { endpoints = {auth_lockd_endpoint} },
}))
assert(server:webdav({
  path_prefix = "/open",
  cache_dir = cache_dir,
  site_id = "open",
  auth_required = false,
}) == true)
assert(server:webdav({
  path_prefix = "/disk",
  cache_dir = cache_dir,
  site_id = "disk",
  root_dir = root_dir,
  auth_required = false,
}) == true)
assert(server:webdav({
  path_prefix = "/native",
  cache_dir = cache_dir,
  site_id = "native",
  conceal_unauthorized = false,
  auth = {
    kind = "native",
    state_key = "auth/v1/store",
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
local child_provider = assert(vectis.auth.provider_callback(function(request)
  if request.resource:find("secret", 1, true) or request.resource == "/restricted/visible.txt" then
    return {action = "deny", status_code = 404}
  end
  return {action = "allow", principal = "child-test"}
end))
for _, site in ipairs({"open", "disk"}) do
  assert(server:webdav({path_prefix = "/guard-" .. site, cache_dir = cache_dir,
    site_id = site, root_dir = site == "disk" and root_dir or nil,
    auth = {provider = child_provider}}))
end
assert(server:start() == true)

local ready
for _ = 1, 20 do
  ready = webdav.propfind(request_opts("/open", {depth = 0}))
  if ready.transport_ok then break end
  assert(vectis.sleep_ms(100) == true)
end
assert(ready.transport_ok == true, ready.error and ready.error.message)

for _, mount in ipairs({"/open", "/disk"}) do
  local path = mount .. "/conditional.txt"
  local function put(headers, body)
    return webdav.put(request_opts(path, {headers = headers, body = body or "bad"}))
  end
  assert(put({["If-Match"] = "*"}).status == 412)
  assert(put({["If-None-Match"] = "*"}, "original").ok)
  local original = webdav.get(request_opts(path))
  local etag = assert(original.headers:match('etag: ([^\r\n]+)'))
  assert(etag:match('^"%x+"$'))
  assert(put({["If-None-Match"] = "*"}).status == 412)
  assert(put({["If-Match"] = '"wrong"'}).status == 412)
  assert(put({["If-Match"] = "W/" .. etag}).status == 412)
  assert(put({["If-None-Match"] = "W/" .. etag}).status == 412)
  assert(put({["If-Match"] = etag .. ','}).status == 400)
  assert(webdav.get(request_opts(path)).body == "original")
  assert(put({["If-Match"] = '"wrong", ' .. etag}, "updated").ok)
  assert(put({["If-Match"] = etag}).status == 412)
  assert(webdav.get(request_opts(path)).body == "updated")
  assert(webdav.delete(request_opts(path)).ok)
end

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
local open_propfind_with_body = webdav.propfind(request_opts("/open/public", {
  depth = 0,
  body = [[<?xml version="1.0" encoding="utf-8"?><D:propfind xmlns:D="DAV:"><D:allprop/></D:propfind>]],
  headers = {["Content-Type"] = "application/xml; charset=utf-8"},
}))
assert(open_propfind_with_body.ok == true, open_propfind_with_body.error and open_propfind_with_body.error.message)
assert(open_propfind_with_body.status == 207)
assert(open_propfind_with_body.body:find("/open/public", 1, true) ~= nil)
local open_after_propfind_body = webdav.get(request_opts("/open/public/readme.txt"))
assert(open_after_propfind_body.ok == true, open_after_propfind_body.error and open_after_propfind_body.error.message)
assert(open_after_propfind_body.body == "open webdav\n")

local disk_mkcol = webdav.mkcol(request_opts("/disk/public"))
assert(disk_mkcol.ok == true, disk_mkcol.error and disk_mkcol.error.message)
assert(disk_mkcol.status == 201)
local disk_put = webdav.put(request_opts("/disk/public/readme.txt", {
  body = "direct disk webdav\n",
}))
assert(disk_put.ok == true, disk_put.error and disk_put.error.message)
assert(read_file(root_dir .. "/public/readme.txt") == "direct disk webdav\n")
local disk_read = webdav.get(request_opts("/disk/public/readme.txt"))
assert(disk_read.ok == true, disk_read.error and disk_read.error.message)
assert(disk_read.body == "direct disk webdav\n")
local disk_copy = webdav.copy(request_opts("/disk/public/readme.txt", {
  destination = base .. "/disk/public/copied.txt",
}))
assert(disk_copy.ok == true, disk_copy.error and disk_copy.error.message)
assert(read_file(root_dir .. "/public/copied.txt") == "direct disk webdav\n")
local disk_move = webdav.move(request_opts("/disk/public/copied.txt", {
  destination = base .. "/disk/public/moved.txt",
}))
assert(disk_move.ok == true, disk_move.error and disk_move.error.message)
assert(read_file(root_dir .. "/public/moved.txt") == "direct disk webdav\n")
local disk_delete = webdav.delete(request_opts("/disk/public/moved.txt"))
assert(disk_delete.ok == true, disk_delete.error and disk_delete.error.message)
assert(io.open(root_dir .. "/public/moved.txt", "rb") == nil)
for _, prefix in ipairs({"/open", "/disk"}) do
  local source = prefix .. "/public/readme.txt"
  local destination = prefix .. "/public/target.txt"
  assert(webdav.put(request_opts(destination, {body = "keep destination"})).ok)
  local original = webdav.get(request_opts(source)).body
  local etag = assert(webdav.get(request_opts(source)).headers:match('[Ee][Tt][Aa][Gg]:%s*([^\r\n]+)'))
  for _, method in ipairs({"GET", "HEAD"}) do
    for _, case in ipairs({
      {{["If-None-Match"] = "*"}, 304},
      {{["If-None-Match"] = etag}, 304},
      {{["If-None-Match"] = "W/" .. etag}, 304},
      {{["If-None-Match"] = '"other,tag", ' .. etag}, 304},
      {{["If-Match"] = '"wrong"'}, 412},
      {{["If-Match"] = "W/" .. etag}, 412},
      {{["If-Match"] = '"wrong"', ["If-None-Match"] = "*"}, 412},
      {{["If-Match"] = etag}, 200},
      {{["If-Match"] = "*"}, 200},
      {{["If-None-Match"] = '"other"'}, 200},
      {{["If-Match"] = etag .. ","}, 400},
      {{["If-None-Match"] = etag .. ","}, 400},
    }) do
      local result = webdav.request(request_opts(source, {method = method, headers = case[1]}))
      assert(result.status == case[2], tostring(result.status) .. " expected " .. case[2])
      assert(result.headers:find(etag, 1, true))
      if method == "HEAD" or case[2] ~= 200 then assert(result.body == "")
      else assert(result.body == original) end
    end
  end
  assert(webdav.mkcol(request_opts(prefix .. "/depth-move")).ok)
  assert(webdav.put(request_opts(prefix .. "/depth-move/child", {body = "child"})).ok)
  assert(webdav.mkcol(request_opts(prefix .. "/depth-target")).ok)
  assert(webdav.put(request_opts(prefix .. "/depth-target/keep", {body = "keep"})).ok)
  for _, depth in ipairs({"0", "1", "garbage"}) do
    local result = webdav.move(request_opts(prefix .. "/depth-move", {
      depth = depth, destination = base .. prefix .. "/depth-target",
    }))
    assert(result.status == 400)
    assert(webdav.get(request_opts(prefix .. "/depth-move/child")).body == "child")
    assert(webdav.get(request_opts(prefix .. "/depth-target/keep")).body == "keep")
  end
  assert(webdav.move(request_opts(prefix .. "/depth-move", {
    depth = "infinity", destination = base .. prefix .. "/depth-target",
  })).ok)
  assert(webdav.get(request_opts(prefix .. "/depth-target/child")).body == "child")
  assert(webdav.move(request_opts(prefix .. "/depth-target", {
    destination = base .. prefix .. "/depth-move",
  })).ok)
  assert(webdav.move(request_opts(prefix .. "/depth-move/child", {
    depth = 0, destination = base .. prefix .. "/depth-move/file",
  })).ok)
  for _, operation in ipairs({webdav.copy, webdav.move}) do
    for _, value in ipairs({"false", "true", "f", "t", "0", "1", "T, F", "FF"}) do
      local result = operation(request_opts(source, {
        destination = base .. destination, headers = {Overwrite = value},
      }))
      assert(result.status == 400, value .. ": " .. tostring(result.status))
      assert(webdav.get(request_opts(source)).body == original)
      assert(webdav.get(request_opts(destination)).body == "keep destination")
    end
    local refused = operation(request_opts(source, {
      destination = base .. destination, headers = {Overwrite = "F"},
    }))
    assert(refused.status == 412)
    assert(webdav.get(request_opts(source)).body == original)
    assert(webdav.get(request_opts(destination)).body == "keep destination")
    for _, headers in ipairs({{["If-Match"] = '"stale"'}, {["If-None-Match"] = "*"}}) do
      local result = operation(request_opts(source, {
        destination = base .. destination, headers = headers,
      }))
      assert(result.status == 412)
      assert(webdav.get(request_opts(source)).body == original)
      assert(webdav.get(request_opts(destination)).body == "keep destination")
    end
  end
  assert(webdav.copy(request_opts(source, {destination = base .. destination,
    headers = {["If-Match"] = "*"}})).status == 201)
  assert(webdav.get(request_opts(destination)).body == original)
  assert(webdav.move(request_opts(destination, {destination = base .. prefix .. "/public/matched.txt",
    headers = {["If-Match"] = "*"}})).status == 201)
  for _, operation in ipairs({webdav.copy, webdav.move}) do
    for _, headers in ipairs({{}, {Overwrite = "T"}}) do
      local src = prefix .. "/public/overwrite-source"
      local dst = prefix .. "/public/overwrite-target"
      assert(webdav.put(request_opts(src, {body = "replacement"})).ok)
      assert(webdav.put(request_opts(dst, {body = "original"})).ok)
      assert(operation(request_opts(src, {destination = base .. dst, headers = headers})).status == 201)
      assert(webdav.get(request_opts(dst)).body == "replacement")
      local remaining = webdav.get(request_opts(src))
      if operation == webdav.move then assert(remaining.status == 404)
      else assert(remaining.body == "replacement") end
    end
  end
end

for _, site in ipairs({"open", "disk"}) do
  local raw = "/" .. site
  local guard = "/guard-" .. site
  for _, path in ipairs({"/tree", "/tree/nested", "/plain", "/occupied", "/occupied/nested"}) do
    assert(webdav.mkcol(request_opts(raw .. path)).ok)
  end
  for _, path in ipairs({"/tree/nested/secret.txt", "/tree/nested/visible.txt",
      "/plain/visible.txt", "/occupied/nested/secret.txt"}) do
    assert(webdav.put(request_opts(raw .. path, {body = path})).ok)
  end
  assert(webdav.get(request_opts(guard .. "/tree/nested/secret.txt")).status == 404)
  local listing = webdav.propfind(request_opts(guard .. "/tree/nested", {depth = 1}))
  assert(listing.status == 207)
  assert(not listing.body:find("secret.txt", 1, true))
  assert(listing.body:find("visible.txt", 1, true))
  assert(webdav.delete(request_opts(guard .. "/tree")).status == 404)
  assert(webdav.get(request_opts(raw .. "/tree/nested/secret.txt")).body == "/tree/nested/secret.txt")
  for _, operation in ipairs({webdav.copy, webdav.move}) do
    for _, pair in ipairs({{"/tree", "/plain"}, {"/plain", "/restricted"}, {"/plain", "/occupied"}}) do
      local result = operation(request_opts(guard .. pair[1], {destination = base .. guard .. pair[2]}))
      assert(result.status == 404, tostring(result.status))
      assert(webdav.get(request_opts(raw .. "/tree/nested/secret.txt")).body == "/tree/nested/secret.txt")
      assert(webdav.get(request_opts(raw .. "/plain/visible.txt")).body == "/plain/visible.txt")
      assert(webdav.get(request_opts(raw .. "/occupied/nested/secret.txt")).body == "/occupied/nested/secret.txt")
      assert(webdav.propfind(request_opts(raw .. "/restricted", {depth = 0})).status == 404)
    end
  end
  assert(webdav.copy(request_opts(guard .. "/plain", {
    destination = base .. guard .. "/occupied", depth = 0})).status == 404)
  assert(webdav.copy(request_opts(guard .. "/tree", {
    destination = base .. guard .. "/shallow-auth", depth = 0})).status == 201)
  assert(webdav.get(request_opts(raw .. "/shallow-auth/nested/secret.txt")).status == 404)
  assert(webdav.copy(request_opts(guard .. "/plain", {destination = base .. guard .. "/allowed-copy"})).status == 201)
  assert(webdav.move(request_opts(guard .. "/allowed-copy", {destination = base .. guard .. "/allowed-move"})).status == 201)
  assert(webdav.delete(request_opts(guard .. "/allowed-move")).status == 204)
  for i, hidden in ipairs({".vectis-tmp-secret", ".vectis-txn-secret", ".vectis-tmp-dir/secret.txt", ".vectis-txn-dir/secret.txt"}) do
    local src = "/hidden-source-" .. i
    local dst = "/hidden-target-" .. i
    assert(webdav.mkcol(request_opts(raw .. src)).ok)
    assert(webdav.mkcol(request_opts(raw .. dst)).ok)
    local directory = hidden:match("^(.+)/")
    if directory then assert(webdav.mkcol(request_opts(raw .. src .. "/" .. directory)).ok) end
    assert(webdav.put(request_opts(raw .. src .. "/" .. hidden, {body = "protected"})).ok)
    assert(webdav.put(request_opts(raw .. dst .. "/keep.txt", {body = "keep"})).ok)
    assert(webdav.delete(request_opts(guard .. src .. "/" .. hidden)).status == 404)
    assert(webdav.delete(request_opts(guard .. src)).status == 404)
    for _, operation in ipairs({webdav.copy, webdav.move}) do
      assert(operation(request_opts(guard .. src, {destination = base .. guard .. dst})).status == 404)
      -- Also protect hidden children removed by replacing the destination.
      assert(operation(request_opts(guard .. dst, {destination = base .. guard .. src})).status == 404)
      assert(webdav.get(request_opts(raw .. src .. "/" .. hidden)).body == "protected")
      assert(webdav.get(request_opts(raw .. dst .. "/keep.txt")).body == "keep")
    end
    assert(webdav.copy(request_opts(guard .. dst, {destination = base .. guard .. src, depth = 0})).status == 404)
    assert(webdav.copy(request_opts(guard .. src, {destination = base .. guard .. dst, depth = 0})).status == 201)
    assert(webdav.get(request_opts(raw .. src .. "/" .. hidden)).body == "protected")
    local listed = webdav.propfind(request_opts(raw .. src, {depth = 1}))
    assert(listed.status == 207 and not listed.body:find(".vectis-", 1, true))
  end
  for i, content_type in ipairs({"application/x-unknown", "application/xml"}) do
    local path = raw .. "/body-mkcol-" .. i
    assert(webdav.mkcol(request_opts(path, {body = "unsupported", headers = {["Content-Type"] = content_type}})).status == 415)
    assert(webdav.propfind(request_opts(path, {depth = 0})).status == 404)
    assert(webdav.mkcol(request_opts(path, {body = "", headers = {["Content-Type"] = content_type}})).status == 201)
  end
end

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
