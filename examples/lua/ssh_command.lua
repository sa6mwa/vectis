local vectis = require("vectis")

local host = os.getenv("VECTIS_LUA_SSH_HOST") or "127.0.0.1"
local port = tonumber(os.getenv("VECTIS_LUA_SSH_PORT") or "") or 29222
if port <= 0 or port > 65535 then
  port = 29222
end
local username = os.getenv("VECTIS_LUA_SSH_USERNAME") or "vectis"
local password = os.getenv("VECTIS_LUA_SSH_PASSWORD") or "vectispass"
local command = os.getenv("VECTIS_LUA_SSH_COMMAND") or
    "printf vectis-lua-ssh-ok"
local timeout_ms = tonumber(os.getenv("VECTIS_LUA_SSH_TIMEOUT_MS") or "") or
    10000
local opts = {
  host = host,
  port = port,
  username = username,
  password = password,
  known_hosts = os.getenv("VECTIS_LUA_SSH_KNOWN_HOSTS"),
  host_key_sha256 = os.getenv("VECTIS_LUA_SSH_HOST_KEY_SHA256"),
  command = command,
  timeout_ms = timeout_ms,
}

local private_key_path = os.getenv("VECTIS_LUA_SSH_PRIVATE_KEY")
if private_key_path ~= nil and private_key_path ~= "" then
  opts.private_key_path = private_key_path
end

local private_key = os.getenv("VECTIS_LUA_SSH_PRIVATE_KEY_PEM")
if private_key ~= nil and private_key ~= "" then
  opts.private_key = private_key
  if os.getenv("VECTIS_LUA_SSH_PASSWORD") == nil then
    opts.password = nil
  end
end

local result, err = vectis.ssh.exec(opts)
if result == nil then
  error(err and err.message or "SSH command failed")
end
assert(result.exit_status == 0)
if opts.command == "printf vectis-lua-ssh-ok" then
  assert(result.stdout == "vectis-lua-ssh-ok")
end

local session = assert(vectis.ssh.open(opts))
local session_result = assert(session:exec(command))
assert(session_result.exit_status == 0)
if command == "printf vectis-lua-ssh-ok" then
  assert(session_result.stdout == "vectis-lua-ssh-ok")
end
assert(type(session.sftp_open) == "function")
assert(type(session.sftp_upload_file) == "function")
assert(type(session.scp_download_file) == "function")
assert(session:close() == true)
local closed_result, closed_err = session:exec(command)
assert(closed_result == nil)
assert(type(closed_err) == "table")
assert(closed_err.message:find("closed", 1, true))

print("lua ssh command example ok")
