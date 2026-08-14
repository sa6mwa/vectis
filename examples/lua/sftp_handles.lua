local vectis = require("vectis")

local function env_or_default(name, fallback)
  local value = os.getenv(name)
  if value == nil or value == "" then
    return fallback
  end
  return value
end

local function env_port_or_default(name, fallback)
  local value = os.getenv(name)
  if value == nil or value == "" then
    return fallback
  end
  local parsed = tonumber(value)
  if parsed == nil or parsed <= 0 or parsed > 65535 then
    return fallback
  end
  return parsed
end

local function dirname(path)
  local dir = path:match("^(.*)/[^/]+$")
  if dir == nil or dir == "" then
    return "/"
  end
  return dir
end

local function basename(path)
  return path:match("([^/]+)$") or path
end

local host = env_or_default("VECTIS_LUA_SFTP_HOST", "127.0.0.1")
local port = env_port_or_default("VECTIS_LUA_SFTP_PORT", 29222)
local username = env_or_default("VECTIS_LUA_SFTP_USERNAME", "vectis")
local password = env_or_default("VECTIS_LUA_SFTP_PASSWORD", "vectispass")
local known_hosts = os.getenv("VECTIS_LUA_SFTP_KNOWN_HOSTS")
local private_key_path = os.getenv("VECTIS_LUA_SFTP_PRIVATE_KEY")
local remote_path = env_or_default("VECTIS_LUA_SFTP_REMOTE_FILE", "/config/lua-sftp-handles.txt")
local moved_path = remote_path .. ".moved"
local payload = env_or_default("VECTIS_LUA_SFTP_PAYLOAD", "vectis lua stateful sftp e2e\n")

local session_opts = {
  host = host,
  port = port,
  username = username,
  password = password,
  known_hosts_path = known_hosts,
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

local writer = assert(session:open_file({
  remote_path = remote_path,
  mode = "w",
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
local dir = assert(session:open_dir({remote_path = dirname(remote_path)}))
while true do
  local entry, read_err = dir:read()
  assert(entry ~= nil or read_err == nil, read_err and read_err.message)
  if entry == nil then
    break
  end
  if entry.name == basename(remote_path) then
    listed = true
    assert(entry.stat.has_size == true)
    assert(entry.stat.size == #payload)
  end
end
assert(dir:close() == true)
assert(listed == true)

local reader = assert(session:open_file({
  remote_path = remote_path,
  mode = "r",
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
