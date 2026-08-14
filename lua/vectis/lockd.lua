local lockdc = require("lockdc")
local vstatus = require("vectis.status")

local M = {
  native = lockdc,
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
  local status = vstatus[status_name]
  return vstatus.error({
    status = status,
    source_code = vstatus.ERROR_SOURCE_VECTIS,
    message = message,
  })
end

local function lockdc_error(err, message)
  return vstatus.decorate_error(err, {
    status = vstatus.ERR_STATE,
    source_code = vstatus.ERROR_SOURCE_LOCKDC,
    message = message or "lockdc workflow failed",
  })
end

local function handler_error(err)
  return vstatus.decorate_error(err, {
    status = vstatus.ERR_STATE,
    source_code = vstatus.ERROR_SOURCE_VECTIS,
    message = "lockd handler failed",
  })
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
        if type(err) == "table" then
          return nil, vstatus.decorate_error(err, {
            status = vstatus.ERR_STATE,
            source_code = vstatus.ERROR_SOURCE_VECTIS,
            message = "no embedded lockd bundle",
          })
        end
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
  local client, open_err = lockdc.open(normalized)
  if client == nil then
    return nil, lockdc_error(open_err, "lockdc open failed")
  end
  return client
end

local function json_request(req)
  local next_req = copy_table(req)
  if next_req.content_type == nil then
    next_req.content_type = "application/json"
  end
  return next_req
end

local function close_handle(handle)
  if type(handle) == "table" and type(handle.close) == "function" then
    handle:close()
  end
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
    return nil, handler_error(handler_err)
  end
  if result == nil then
    return true
  end
  return result
end

function M.load_json(config, req)
  local client, err = M.open(config)
  if client == nil then
    return nil, err
  end

  local lease, acquire_err = client:acquire(req)
  if lease == nil then
    client:close()
    return nil, lockdc_error(acquire_err, "lockdc acquire failed")
  end

  local ok_get, value, meta_or_err = pcall(function()
    return lease:get_json()
  end)
  if not ok_get then
    close_handle(lease)
    client:close()
    error(value)
  end
  if value == nil and not (type(meta_or_err) == "table" and
      meta_or_err.no_content) then
    close_handle(lease)
    client:close()
    return nil, lockdc_error(meta_or_err, "lockdc get_json failed")
  end

  local ok_release, released, release_err = pcall(function()
    return lease:release()
  end)
  if not ok_release then
    close_handle(lease)
    client:close()
    error(released)
  end
  if released == nil then
    close_handle(lease)
    client:close()
    return nil, lockdc_error(release_err, "lockdc release failed")
  end
  client:close()
  return value, meta_or_err
end

function M.save_json(config, req, value)
  return M.with_client(config, function(client)
    local lease, acquire_err = client:acquire(req)
    if lease == nil then
      return nil, lockdc_error(acquire_err, "lockdc acquire failed")
    end

    local ok_update, updated, update_err = pcall(function()
      return lease:update_json(value)
    end)
    if not ok_update then
      close_handle(lease)
      error(updated)
    end
    if updated == nil then
      close_handle(lease)
      return nil, lockdc_error(update_err, "lockdc update_json failed")
    end

    local ok_release, released, release_err = pcall(function()
      return lease:release()
    end)
    if not ok_release then
      close_handle(lease)
      error(released)
    end
    if released == nil then
      close_handle(lease)
      return nil, lockdc_error(release_err, "lockdc release failed")
    end
    return updated
  end)
end

function M.enqueue_json(config, req, value)
  return M.with_client(config, function(client)
    local enqueued, enqueue_err =
        client:enqueue(json_request(req), M.encode_json(value))
    if enqueued == nil then
      return nil, lockdc_error(enqueue_err, "lockdc enqueue failed")
    end
    return enqueued
  end)
end

function M.with_acquired_lease(config, req, handler)
  if type(handler) ~= "function" then
    error("vectis.lockd.with_acquired_lease requires a handler", 2)
  end

  return M.with_client(config, function(client)
    local lease, acquire_err = client:acquire(req)
    if lease == nil then
      return nil, lockdc_error(acquire_err, "lockdc acquire failed")
    end

    local ok, result, handler_err = pcall(handler, lease, client)
    close_handle(lease)
    if not ok then
      error(result)
    end
    if handler_err ~= nil then
      return nil, handler_error(handler_err)
    end
    if result == nil then
      return true
    end
    return result
  end)
end

function M.with_dequeued_json(config, req, handler)
  if type(handler) ~= "function" then
    error("vectis.lockd.with_dequeued_json requires a handler", 2)
  end

  return M.with_client(config, function(client)
    local message, dequeue_err = client:dequeue(req)
    if message == nil then
      return nil, lockdc_error(dequeue_err, "lockdc dequeue failed")
    end

    local ok_payload, payload, payload_written_or_err = pcall(function()
      return message:payload_json()
    end)
    if not ok_payload then
      close_handle(message)
      error(payload)
    end
    if payload == nil then
      close_handle(message)
      return nil, lockdc_error(payload_written_or_err,
                               "lockdc payload_json failed")
    end

    local ok, result, handler_err =
        pcall(handler, payload, message, client, payload_written_or_err)
    close_handle(message)
    if not ok then
      error(result)
    end
    if handler_err ~= nil then
      return nil, handler_error(handler_err)
    end
    if result == nil then
      return true
    end
    return result
  end)
end

return M
