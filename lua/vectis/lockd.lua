local lockdc = require("lockdc")

local M = {
  raw = lockdc,
  encode_json = lockdc.encode_json,
  decode_json = lockdc.decode_json,
  json_null = lockdc.json_null,
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

local function vectis_module()
  local loaded = package.loaded.vectis
  if type(loaded) == "table" then
    return loaded
  end
  return require("vectis")
end

local function vectis_error(status_name, message)
  local vectis = vectis_module()
  local status = vectis[status_name]
  return {
    status = status,
    status_string = vectis.status_string(status),
    message = message,
  }
end

function M.config(config)
  if config == nil then
    config = {}
  end
  if type(config) ~= "table" then
    error("vectis.lockd.config requires a table", 2)
  end

  local normalized = copy_table(config)
  if normalized.default_namespace == nil and normalized.namespace ~= nil then
    normalized.default_namespace = normalized.namespace
  end
  normalized.namespace = nil

  if normalized.client_bundle == "embedded" then
    if normalized.client_bundle_source == nil then
      local source, err = vectis_module().embedded_lockd_bundle_source()
      if source == nil then
        return nil, vectis_error("ERR_STATE",
                                 err or "no embedded lockd bundle")
      end
      normalized.client_bundle_source = source
    end
    normalized.client_bundle = nil
  elseif type(normalized.client_bundle) == "string" and
      normalized.client_bundle ~= "" then
    if normalized.client_bundle_path == nil then
      normalized.client_bundle_path = normalized.client_bundle
    end
    normalized.client_bundle = nil
  end

  return normalized
end

function M.open(config)
  local normalized, err = M.config(config)
  if normalized == nil then
    return nil, err
  end
  return lockdc.open(normalized)
end

function M.with_client(config, handler)
  if type(handler) ~= "function" then
    error("vectis.lockd.with_client requires a handler", 2)
  end

  local client, err = M.open(config)
  if client == nil then
    return nil, err
  end

  local ok, result, handler_err = pcall(handler, client)
  client:close()
  if not ok then
    error(result)
  end
  if handler_err ~= nil then
    return nil, handler_err
  end
  if result == nil then
    return true
  end
  return result
end

return M
