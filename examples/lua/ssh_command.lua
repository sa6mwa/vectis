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

local opts = {
  host = env_or_default("VECTIS_LUA_SSH_HOST", "127.0.0.1"),
  port = env_port_or_default("VECTIS_LUA_SSH_PORT", 29222),
  username = env_or_default("VECTIS_LUA_SSH_USERNAME", "vectis"),
  password = env_or_default("VECTIS_LUA_SSH_PASSWORD", "vectispass"),
  known_hosts = os.getenv("VECTIS_LUA_SSH_KNOWN_HOSTS"),
  command = env_or_default("VECTIS_LUA_SSH_COMMAND", "printf vectis-lua-ssh-ok"),
  timeout_ms = tonumber(env_or_default("VECTIS_LUA_SSH_TIMEOUT_MS", "10000")),
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

print("lua ssh command example ok")
