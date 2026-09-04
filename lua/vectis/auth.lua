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
  "app",
  "namespace",
  "state_key",
  "transient_state_key",
  "max_record_bytes",
  "path_prefix",
  "prefix",
  "realm",
  "login_title",
  "credential_purpose",
  "steps",
  "workflow_state_key",
  "workflow_ttl_seconds",
  "email_code_max_attempts",
  "browser_template_html",
  "browser_template_path",
  "browser_template_embedded_path",
  "max_body_bytes",
  "smtp",
  "email_smtp",
  "browser_session",
  "time",
  "window",
}

local provider_keys = {
  "app",
  "namespace",
  "state_key",
  "transient_state_key",
  "max_record_bytes",
  "purpose",
  "realm",
  "allowed_modes",
  "browser_session",
  "browser_login_path",
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

local workflow = {}
workflow.__index = workflow

function workflow:routes(opts)
  return select_keys(merge_tables(self.config, opts), route_keys)
end

function workflow:provider(opts)
  local config = merge_tables(self.config, opts)
  local provider = select_keys(config, provider_keys)
  local path_prefix = config.path_prefix or config.prefix

  if provider.browser_login_path == nil and path_prefix ~= nil then
    path_prefix = path_prefix:gsub("/+$", "")
    provider.browser_login_path =
        (path_prefix == "" and "" or path_prefix) .. "/login"
  end
  return core.provider_native(provider)
end

function workflow:mount(server, opts)
  local server_type = type(server)
  if (server_type ~= "table" and server_type ~= "userdata") or
      type(server.auth_routes) ~= "function" then
    error("vectis.auth.workflow:mount requires a vectis server", 2)
  end
  return server:auth_routes(self:routes(opts))
end

function M.workflow(opts)
  return setmetatable({ config = copy_table(opts) }, workflow)
end

M.core = core

return M
