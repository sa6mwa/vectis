local vectis = require("vectis")

local host = os.getenv("VECTIS_LUA_SFTP_HOST") or "127.0.0.1"
local port = tonumber(os.getenv("VECTIS_LUA_SFTP_PORT") or "") or 29222
if port <= 0 or port > 65535 then
  port = 29222
end
local username = os.getenv("VECTIS_LUA_SFTP_USERNAME") or "vectis"
local password = os.getenv("VECTIS_LUA_SFTP_PASSWORD") or "vectispass"
local known_hosts = os.getenv("VECTIS_LUA_SFTP_KNOWN_HOSTS")
local private_key_path = os.getenv("VECTIS_LUA_SFTP_PRIVATE_KEY")
local remote_path = os.getenv("VECTIS_LUA_SFTP_REMOTE_FILE") or
    "/config/lua-sftp-handles.txt"
local moved_path = remote_path .. ".moved"
local payload = os.getenv("VECTIS_LUA_SFTP_PAYLOAD") or
    "vectis lua stateful sftp e2e\n"
local remote_dir = remote_path:match("^(.*)/[^/]+$") or "/"
if remote_dir == "" then
  remote_dir = "/"
end
local remote_name = remote_path:match("([^/]+)$") or remote_path

local session_opts = {
  host = host,
  port = port,
  username = username,
  password = password,
  known_hosts_path = known_hosts,
  host_key_sha256 = os.getenv("VECTIS_LUA_SFTP_HOST_KEY_SHA256"),
  timeout_ms = 10000,
}
if private_key_path ~= nil and private_key_path ~= "" then
  session_opts.private_key_path = private_key_path
  if os.getenv("VECTIS_LUA_SFTP_PASSWORD") == nil then
    session_opts.password = nil
  end
end

local session, session_err = vectis.ssh.sftp_open(session_opts)
assert(session, session_err and session_err.message)

session:remove({remote_path = remote_path})
session:remove({remote_path = moved_path})

local write_flags = vectis.ssh.SFTP_OPEN_WRITE | vectis.ssh.SFTP_OPEN_CREATE |
                    vectis.ssh.SFTP_OPEN_TRUNCATE
local writer = assert(session:open_file({
  remote_path = remote_path,
  flags = write_flags,
  permissions = 420,
}))
assert(writer:write(payload) == #payload)
local written_stat = assert(writer:stat())
assert(written_stat.has_size == true)
assert(written_stat.size == #payload)
assert(writer:close() == true)

local stat = assert(session:stat({remote_path = remote_path}))
assert(stat.has_size == true)
assert(stat.size == #payload)

local listed = false
local dir = assert(session:open_dir({remote_path = remote_dir}))
while true do
  local entry, read_err = dir:read()
  assert(entry ~= nil or read_err == nil, read_err and read_err.message)
  if entry == nil then
    break
  end
  if entry.name == remote_name then
    listed = true
    assert(entry.stat.has_size == true)
    assert(entry.stat.size == #payload)
  end
end
assert(dir:close() == true)
assert(listed == true)

local reader = assert(session:open_file({
  remote_path = remote_path,
  flags = vectis.ssh.SFTP_OPEN_READ,
}))
local chunks = {}
while true do
  local chunk = assert(reader:read(7))
  if #chunk == 0 then
    break
  end
  chunks[#chunks + 1] = chunk
end
assert(reader:close() == true)
assert(table.concat(chunks) == payload)

assert(session:rename({old_path = remote_path, new_path = moved_path}) == true)
local moved = assert(session:open_file({remote_path = moved_path, mode = "r"}))
assert(moved:read(#payload + 8) == payload)
assert(moved:close() == true)
assert(session:remove({remote_path = moved_path}) == true)
assert(session:close() == true)

print("lua stateful sftp handles example ok")
