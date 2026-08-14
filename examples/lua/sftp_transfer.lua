local http = require("vectis.http")

local base_url = os.getenv("VECTIS_LUA_SFTP_URL") or "sftp://127.0.0.1:29222"
local username = os.getenv("VECTIS_LUA_SFTP_USERNAME") or "vectis"
local password = os.getenv("VECTIS_LUA_SFTP_PASSWORD") or "vectispass"
local remote_path = os.getenv("VECTIS_LUA_SFTP_REMOTE_FILE") or
    "/config/lua-sftp-upload.txt"
local upload_path = os.getenv("VECTIS_LUA_SFTP_UPLOAD_FILE") or
    "lua-sftp-upload.txt"
local download_path = os.getenv("VECTIS_LUA_SFTP_DOWNLOAD_FILE") or
    "lua-sftp-download.txt"
local known_hosts = os.getenv("VECTIS_LUA_SFTP_KNOWN_HOSTS")
local private_key = os.getenv("VECTIS_LUA_SFTP_PRIVATE_KEY")
local public_key = os.getenv("VECTIS_LUA_SFTP_PUBLIC_KEY")
local payload = os.getenv("VECTIS_LUA_SFTP_PAYLOAD") or
    "vectis lua sftp e2e\n"
if remote_path:sub(1, 1) ~= "/" then
  remote_path = "/" .. remote_path
end
if base_url:sub(-1) == "/" then
  base_url = base_url:sub(1, -2)
end

local upload_file = assert(io.open(upload_path, "wb"))
upload_file:write(payload)
upload_file:close()

local uploaded = http.sftp_upload({
  url = base_url .. remote_path,
  username = username,
  password = password,
  ssh_known_hosts = known_hosts,
  ssh_private_key = private_key,
  ssh_public_key = public_key,
  upload_path = upload_path,
  timeout_ms = 10000,
  connect_timeout_ms = 5000,
  no_signal = true,
})
assert(uploaded.ok == true, uploaded.error and uploaded.error.message)
assert(uploaded.transport_ok == true)

local downloaded = http.sftp_download({
  url = base_url .. remote_path,
  username = username,
  password = password,
  ssh_known_hosts = known_hosts,
  ssh_private_key = private_key,
  ssh_public_key = public_key,
  download_path = download_path,
  timeout_ms = 10000,
  connect_timeout_ms = 5000,
  no_signal = true,
})
assert(downloaded.ok == true, downloaded.error and downloaded.error.message)
assert(downloaded.transport_ok == true)
local downloaded_file = assert(io.open(download_path, "rb"))
assert(downloaded_file:read("*a") == payload)
downloaded_file:close()

print("lua sftp transfer example ok")
