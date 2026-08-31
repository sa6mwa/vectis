local cai = require("vectis.cai")
local status = require("vectis.status")

local M = {}

local function copy_table(source)
  local target = {}
  if source ~= nil then
    for key, value in pairs(source) do
      target[key] = value
    end
  end
  return target
end

local function close(handle)
  if handle ~= nil and type(handle.close) == "function" then
    handle:close()
  end
end

local function decorate(err, message)
  return status.decorate_error(err, {
    status = status.ERR_STATE,
    source_code = status.ERROR_SOURCE_CAI,
    message = message,
  })
end

-- Normalize the non-TUI Smith surface. CAI owns the agent loop, durable
-- journal semantics, steering, queued turns, and provider streaming. Vectis
-- deliberately keeps presentation out of this facade.
function M.config(config)
  if config == nil then
    config = {}
  end
  if type(config) ~= "table" then
    error("vectis.smith.config requires a table", 2)
  end

  local normalized = copy_table(config)
  normalized.client_config = cai.config(normalized.client_config or normalized)
  normalized.client = config.client
  normalized.runtime = copy_table(config.runtime)
  if normalized.runtime.workspace_directory == nil then
    normalized.runtime.workspace_directory = config.workspace_directory
  end
  if type(normalized.runtime.workspace_directory) ~= "string" or
      normalized.runtime.workspace_directory == "" then
    error("vectis.smith.config requires runtime.workspace_directory", 2)
  end
  normalized.runtime.preset = nil
  return normalized
end

function M.open(config)
  local normalized = M.config(config)
  local client = normalized.client
  local owns_client = false
  if client == nil then
    local open_err
    client, open_err = cai.open(normalized.client_config)
    if client == nil then
      return nil, decorate(open_err, "Smith client open failed")
    end
    owns_client = true
  end

  local runtime, runtime_err = client:new_smith_runtime(normalized.runtime)
  if runtime == nil then
    if owns_client then
      close(client)
    end
    return nil, decorate(runtime_err, "Smith runtime open failed")
  end

  local handle = {
    runtime = runtime,
    client = client,
    _owns_client = owns_client,
  }

  function handle:submit(text)
    return self.runtime:submit(text)
  end

  function handle:steer(text)
    return self.runtime:submit_steering(text)
  end

  function handle:queue(text)
    return self.runtime:submit_queued(text)
  end

  function handle:pump(timeout_ms)
    return self.runtime:pump(timeout_ms or 0)
  end

  function handle:state()
    return self.runtime:state()
  end

  function handle:session_id()
    return self.runtime:session_id()
  end

  function handle:export_markdown()
    return self.runtime:export_markdown()
  end

  function handle:close()
    if self.runtime ~= nil then
      close(self.runtime)
      self.runtime = nil
    end
    if self._owns_client and self.client ~= nil then
      close(self.client)
    end
    self.client = nil
  end

  return handle
end

function M.with_runtime(config, handler)
  if type(handler) ~= "function" then
    error("vectis.smith.with_runtime requires a function", 2)
  end
  local runtime, open_err = M.open(config)
  if runtime == nil then
    return nil, open_err
  end
  local ok, result, handler_err = pcall(handler, runtime)
  runtime:close()
  if not ok then
    error(result)
  end
  if handler_err ~= nil then
    return nil, decorate(handler_err, "Smith runtime handler failed")
  end
  return result == nil and true or result
end

return M
