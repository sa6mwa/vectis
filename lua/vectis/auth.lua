local core = require("vectis.auth.core")

local M = {}

for key, value in pairs(core) do
  M[key] = value
end

local function copy_table(source)
  local target = {}
  if source then
    for key, value in pairs(source) do
      target[key] = value
    end
  end
  return target
end

local function merge_tables(base, extra)
  local merged = copy_table(base)
  if extra then
    for key, value in pairs(extra) do
      merged[key] = value
    end
  end
  return merged
end

local route_keys = {
  "credentials_path",
  "path",
  "state_path",
  "auth_state_path",
  "max_store_bytes",
  "path_prefix",
  "prefix",
  "realm",
  "login_title",
  "login_template_html",
  "login_template_path",
  "template_path",
  "login_template_embedded_path",
  "template_embedded_path",
  "required_factors",
  "require_email_token",
  "email_token_ttl_seconds",
  "email_token_max_attempts",
  "email_token",
  "smtp",
  "email_smtp",
  "browser_session",
  "time",
  "window",
}

local provider_keys = {
  "credentials_path",
  "path",
  "state_path",
  "auth_state_path",
  "max_store_bytes",
  "purpose",
  "realm",
  "allowed_modes",
  "browser_session",
}

local webdav_key_keys = {
  "credentials_path",
  "path",
  "state_path",
  "auth_state_path",
  "max_store_bytes",
  "username",
  "password",
  "totp_code",
  "time",
  "window",
}

local function select_keys(source, keys)
  local selected = {}
  for _, key in ipairs(keys) do
    if source[key] ~= nil then
      selected[key] = source[key]
    end
  end
  return selected
end

local browser_flow = {}
browser_flow.__index = browser_flow

function browser_flow:routes(opts)
  return select_keys(merge_tables(self.config, opts), route_keys)
end

function browser_flow:provider(opts)
  return core.provider_native(select_keys(merge_tables(self.config, opts),
                                          provider_keys))
end

function browser_flow:mount(server, opts)
  local server_type = type(server)
  if (server_type ~= "table" and server_type ~= "userdata") or
      type(server.auth_routes) ~= "function" then
    error("vectis.auth.browser_flow:mount requires a vectis server", 2)
  end
  return server:auth_routes(self:routes(opts))
end

function browser_flow:webdav_key(opts)
  return core.webdav_key(select_keys(merge_tables(self.config, opts),
                                     webdav_key_keys))
end

function browser_flow:webdav_authorization(opts)
  local key, err = self:webdav_key(opts)
  if key == nil then
    return nil, err
  end
  return core.basic_authorization(key)
end

function M.browser_flow(opts)
  return setmetatable({ config = copy_table(opts) }, browser_flow)
end

M.core = core

return M
