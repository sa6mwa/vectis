include("${CMAKE_CURRENT_LIST_DIR}/port_retry.cmake")

set(site_dir "${WORK_DIR}/vectis-webdav-site")
set(cache_dir "${WORK_DIR}/vectis-webdav-cache")
set(download_path "${WORK_DIR}/vectis-webdav-download.txt")
set(upload_path "${WORK_DIR}/vectis-webdav-upload.txt")
set(script "${WORK_DIR}/vectis-webdav-client.lua")
set(output "${WORK_DIR}/vectis-webdav-client")

file(REMOVE_RECURSE "${site_dir}" "${cache_dir}")
file(REMOVE "${download_path}" "${upload_path}" "${output}")
file(MAKE_DIRECTORY "${site_dir}/assets")
file(WRITE "${site_dir}/index.html" "<!doctype html><title>Vectis WebDAV</title>\n")
file(WRITE "${site_dir}/assets/source.txt" "embedded webdav source\n")
file(WRITE "${upload_path}" "file backed upload\n")

string(CONFIGURE [=[
local vectis = require("vectis")
local webdav = require("vectis.webdav")

local port = tonumber(assert(os.getenv("VECTIS_WEBDAV_PORT")))
local cache_dir = [[@cache_dir@]]
local download_path = [[@download_path@]]
local upload_path = [[@upload_path@]]
local base = "http://127.0.0.1:" .. tostring(port)

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
  local fp = assert(io.open(path, "rb"))
  local body = fp:read("*a")
  fp:close()
  return body
end

assert(require("vectis.webdav") == webdav)
assert(vectis.webdav == webdav)

local server = assert(vectis.server.new({
  bind = "127.0.0.1",
  port = port,
}))
assert(server:webdav_embedded_site({
  path_prefix = "/dav",
  cache_dir = cache_dir,
  site_id = "webdav-client",
  auth_required = false,
  extract_policy = "repair",
}) == true)
assert(server:start() == true)

local ready
for _ = 1, 20 do
  ready = webdav.get(request_opts("/dav/assets/source.txt"))
  if ready.ok then break end
  os.execute("sleep 0.1")
end
assert(ready.ok == true, ready.error and ready.error.message)
assert(ready.status == 200)
assert(ready.body == "embedded webdav source\n")

local missing = webdav.get(request_opts("/dav/assets/missing.txt"))
assert(missing.ok == false)
assert(missing.transport_ok == true)
assert(missing.error.kind == "http_status")
assert(missing.error.status == vectis.ERR_STATE)
assert(missing.error.status_string == "state")
assert(missing.error.source == "curl")
assert(missing.error.http_status == 404)

local listed = webdav.propfind(request_opts("/dav/assets", {depth = 1}))
assert(listed.ok == true, listed.error and listed.error.message)
assert(listed.status == 207)
assert(listed.body:find("/dav/assets/source.txt", 1, true), listed.body)

local made = webdav.mkcol(request_opts("/dav/user"))
assert(made.ok == true, made.error and made.error.message)
assert(made.status == 201)

local put_body = webdav.put(request_opts("/dav/user/body.txt", {
  body = "body upload\n",
}))
assert(put_body.ok == true, put_body.error and put_body.error.message)
assert(put_body.status == 201 or put_body.status == 204)

local uploaded = webdav.upload(request_opts("/dav/user/file.txt", {
  upload_path = upload_path,
}))
assert(uploaded.ok == true, uploaded.error and uploaded.error.message)
assert(uploaded.status == 201 or uploaded.status == 204)

local downloaded = webdav.download(request_opts("/dav/user/file.txt", {
  download_path = download_path,
}))
assert(downloaded.ok == true, downloaded.error and downloaded.error.message)
assert(downloaded.status == 200)
assert(downloaded.body == "")
assert(read_file(download_path) == "file backed upload\n")

local copied = webdav.copy(request_opts("/dav/user/file.txt", {
  destination = base .. "/dav/user/copied.txt",
}))
assert(copied.ok == true, copied.error and copied.error.message)
assert(copied.status == 201 or copied.status == 204)

local copy_read = webdav.get(request_opts("/dav/user/copied.txt"))
assert(copy_read.ok == true, copy_read.error and copy_read.error.message)
assert(copy_read.body == "file backed upload\n")

local moved = webdav.move(request_opts("/dav/user/copied.txt", {
  destination = base .. "/dav/user/moved.txt",
}))
assert(moved.ok == true, moved.error and moved.error.message)
assert(moved.status == 201 or moved.status == 204)

local old_copy = webdav.get(request_opts("/dav/user/copied.txt"))
assert(old_copy.ok == false)
assert(old_copy.error.kind == "http_status")
assert(old_copy.error.status == vectis.ERR_STATE)
assert(old_copy.error.http_status == 404)

local moved_read = webdav.get(request_opts("/dav/user/moved.txt"))
assert(moved_read.ok == true, moved_read.error and moved_read.error.message)
assert(moved_read.body == "file backed upload\n")

local deleted = webdav.delete(request_opts("/dav/user/moved.txt"))
assert(deleted.ok == true, deleted.error and deleted.error.message)
assert(deleted.status == 204 or deleted.status == 200)

local deleted_read = webdav.get(request_opts("/dav/user/moved.txt"))
assert(deleted_read.ok == false)
assert(deleted_read.error.status == vectis.ERR_STATE)
assert(deleted_read.error.http_status == 404)

local bad_copy_ok, bad_copy_err = pcall(function()
  webdav.copy(request_opts("/dav/user/file.txt"))
end)
assert(bad_copy_ok == false)
assert(tostring(bad_copy_err):find("destination", 1, true))

local bad_put_ok, bad_put_err = pcall(function()
  webdav.put(request_opts("/dav/user/empty.txt"))
end)
assert(bad_put_ok == false)
assert(tostring(bad_put_err):find("body", 1, true))

assert(server:stop() == true)
server:close()
print("vectis-webdav-client-ok")
]=] script_body @ONLY)
file(WRITE "${script}" "${script_body}")

execute_process(COMMAND "${VECTIS_BIN}" -a pack --script "${script}"
                        --output "${output}" --asset-dir "/:${site_dir}"
                RESULT_VARIABLE pack_result
                OUTPUT_VARIABLE pack_stdout
                ERROR_VARIABLE pack_stderr)
if(NOT pack_result EQUAL 0)
  message(FATAL_ERROR "vectis webdav client pack failed: ${pack_stdout}${pack_stderr}")
endif()

vectis_run_command_with_port(
  LABEL "vectis Lua WebDAV client"
  PORT_ENV "VECTIS_WEBDAV_PORT"
  SUCCESS_MARKER "vectis-webdav-client-ok"
  COMMAND "${output}")
