local cai = require("cai")
local vstatus = require("vectis.status")

local M = {
  native = cai,
  load_dotenv_api_key = cai.load_dotenv_api_key,
  model_info = cai.model_info,
  model_can_estimate_usage_usd = cai.model_can_estimate_usage_usd,
  response_params = cai.response_params,
  conversation_items_params = cai.conversation_items_params,
  tool_schema = cai.tool_schema,
  tool_registry = cai.tool_registry,
  mcp_handler = cai.mcp_handler,
  mcp_client = cai.mcp_client,
  chatgpt_auth_default_path = cai.chatgpt_auth_default_path,
  chatgpt_auth = cai.chatgpt_auth,
  chatgpt_login = cai.chatgpt_login,
  chatgpt_login_browser_command = cai.chatgpt_login_browser_command,
  chatgpt_login_open_browser = cai.chatgpt_login_open_browser,
}

local function copy_table(source)
  local target = {}
  if source ~= nil then
    for key, value in pairs(source) do
      target[key] = value
    end
  end
  return target
end

local function close_handle(handle)
  if type(handle) == "userdata" or type(handle) == "table" then
    local close = handle.close
    if type(close) == "function" then
      close(handle)
    end
  end
end

local function owns_client(config)
  return not (type(config) == "table" and config.client ~= nil)
end

local function normalize_usage_limits(limits)
  if limits == nil then
    return nil
  end
  if type(limits) ~= "table" then
    error("vectis.cai.config usage_limits must be a table", 3)
  end
  return copy_table(limits)
end

function M.error(err, message)
  return vstatus.decorate_error(err, {
    status = vstatus.ERR_STATE,
    source_code = vstatus.ERROR_SOURCE_CAI,
    message = message or "cai workflow failed",
  })
end

function M.config(config)
  if config == nil then
    config = {}
  end
  if type(config) ~= "table" then
    error("vectis.cai.config requires a table", 2)
  end

  local normalized = copy_table(config)
  normalized.client = nil
  normalized.agent = nil
  normalized.session = nil
  normalized.usage_limits = normalize_usage_limits(normalized.usage_limits)

  if normalized.provider == "openrouter" then
    normalized.openrouter = true
    normalized.provider = nil
  elseif normalized.provider == "openai" then
    normalized.provider = nil
  elseif normalized.provider ~= nil then
    error("vectis.cai.config provider must be 'openai' or 'openrouter'", 2)
  end

  if normalized.auth_json_path ~= nil and normalized.chatgpt_auth_json == nil then
    normalized.chatgpt_auth_json = normalized.auth_json_path
    normalized.auth_json_path = nil
  end

  return normalized
end

function M.open(config)
  local normalized, config_err = M.config(config)
  if normalized == nil then
    return nil, config_err
  end

  local client, open_err = cai.open(normalized)
  if client == nil then
    return nil, M.error(open_err, "cai open failed")
  end
  return client
end

function M.with_client(config, handler)
  if type(handler) ~= "function" then
    error("vectis.cai.with_client requires a handler", 2)
  end

  local client = type(config) == "table" and config.client or nil
  local owned = false
  local err
  if client == nil then
    client, err = M.open(config)
    owned = true
  end
  if client == nil then
    return nil, err
  end

  local ok, result, handler_err = pcall(handler, client)
  if owned then
    close_handle(client)
  end
  if not ok then
    error(result)
  end
  if handler_err ~= nil then
    return nil, vstatus.decorate_error(handler_err, {
      status = vstatus.ERR_STATE,
      source_code = vstatus.ERROR_SOURCE_VECTIS,
      message = "cai handler failed",
    })
  end
  if result == nil then
    return true
  end
  return result
end

function M.new_agent(config, agent_config)
  if type(config) == "table" and config.agent ~= nil then
    return config.agent, false
  end

  local client = type(config) == "table" and config.client or nil
  if client ~= nil then
    local agent, agent_err = client:new_agent(agent_config or config.agent_config)
    if agent == nil then
      return nil, M.error(agent_err, "cai new_agent failed")
    end
    return agent, true
  end

  local client_config = M.config(config)
  local created_client, open_err = M.open(client_config)
  if created_client == nil then
    return nil, open_err
  end

  local agent, agent_err =
      created_client:new_agent(agent_config or client_config.agent_config)
  if agent == nil then
    close_handle(created_client)
    return nil, M.error(agent_err, "cai new_agent failed")
  end
  return agent, true, created_client
end

function M.with_agent(config, handler)
  if type(handler) ~= "function" then
    error("vectis.cai.with_agent requires a handler", 2)
  end

  local agent, owned_agent, owned_client = M.new_agent(config)
  if agent == nil then
    return nil, owned_agent
  end

  local ok, result, handler_err = pcall(handler, agent)
  if owned_agent then
    close_handle(agent)
  end
  if owned_client ~= nil and owns_client(config) then
    close_handle(owned_client)
  end
  if not ok then
    error(result)
  end
  if handler_err ~= nil then
    return nil, vstatus.decorate_error(handler_err, {
      status = vstatus.ERR_STATE,
      source_code = vstatus.ERROR_SOURCE_VECTIS,
      message = "cai agent handler failed",
    })
  end
  if result == nil then
    return true
  end
  return result
end

function M.send_text(config, text, agent_config)
  if type(text) ~= "string" then
    error("vectis.cai.send_text requires text", 2)
  end

  return M.with_agent(config, function(agent)
    local output, run_err = agent:send_text(text, agent_config)
    if output == nil then
      return nil, M.error(run_err, "cai send_text failed")
    end
    return output
  end)
end

return M
