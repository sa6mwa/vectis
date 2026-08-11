local http = require("vectis.http")

local function env_or_default(name, fallback)
  local value = os.getenv(name)
  if value == nil or value == "" then
    return fallback
  end
  return value
end

local function join_url(base_url, remote_path)
  if remote_path:sub(1, 1) ~= "/" then
    remote_path = "/" .. remote_path
  end
  if base_url:sub(-1) == "/" then
    return base_url:sub(1, -2) .. remote_path
  end
  return base_url .. remote_path
end

local function write_file(path, body)
  local fp = assert(io.open(path, "wb"))
  fp:write(body)
  fp:close()
end

local function read_file(path)
  local fp = assert(io.open(path, "rb"))
  local body = fp:read("*a")
  fp:close()
  return body
end

local base_url = env_or_default("VECTIS_LUA_SFTP_URL", "sftp://127.0.0.1:29222")
local username = env_or_default("VECTIS_LUA_SFTP_USERNAME", "vectis")
local password = env_or_default("VECTIS_LUA_SFTP_PASSWORD", "vectispass")
local remote_path = env_or_default("VECTIS_LUA_SFTP_REMOTE_FILE", "/config/lua-sftp-upload.txt")
local upload_path = env_or_default("VECTIS_LUA_SFTP_UPLOAD_FILE", "lua-sftp-upload.txt")
local download_path = env_or_default("VECTIS_LUA_SFTP_DOWNLOAD_FILE", "lua-sftp-download.txt")
local known_hosts = os.getenv("VECTIS_LUA_SFTP_KNOWN_HOSTS")
local private_key = os.getenv("VECTIS_LUA_SFTP_PRIVATE_KEY")
local public_key = os.getenv("VECTIS_LUA_SFTP_PUBLIC_KEY")
local payload = env_or_default("VECTIS_LUA_SFTP_PAYLOAD", "vectis lua sftp e2e\n")

local function transfer_opts(extra)
  local opts = {
    url = join_url(base_url, remote_path),
    username = username,
    password = password,
    ssh_known_hosts = known_hosts,
    ssh_private_key = private_key,
    ssh_public_key = public_key,
    timeout_ms = 10000,
    connect_timeout_ms = 5000,
    no_signal = true,
  }
  for key, value in pairs(extra) do
    opts[key] = value
  end
  return opts
end

write_file(upload_path, payload)

local uploaded = http.sftp_upload(transfer_opts({ upload_path = upload_path }))
assert(uploaded.ok == true, uploaded.error and uploaded.error.message)
assert(uploaded.transport_ok == true)

local downloaded = http.sftp_download(transfer_opts({ download_path = download_path }))
assert(downloaded.ok == true, downloaded.error and downloaded.error.message)
assert(downloaded.transport_ok == true)
assert(read_file(download_path) == payload)

print("lua sftp transfer example ok")
