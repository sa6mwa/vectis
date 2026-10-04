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
    if normalized.client_bundle_source == nil then
      normalized.client_bundle_source = { path = normalized.client_bundle }
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
  if (type(handle) == "table" or type(handle) == "userdata") and
      type(handle.close) == "function" then
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
    return lease:read_json()
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
    return nil, lockdc_error(meta_or_err, "lockdc read_json failed")
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

-- Public batch-one adapter. Client terminal methods leave the batch handle
-- intact so explicit close releases both the native message and client retain.
function M._dequeue_one(client, req)
  local request = copy_table(req)
  request.page_size = 1
  local batch, err = client:dequeue_batch(request)
  if not batch then
    if err and err.code == lockdc.ERR_TIMEOUT then return nil end
    return nil, err
  end
  local raw = batch[1]
  if not raw then return nil end
  local active = true
  local delivery = {}
  local function live() assert(active, "queue delivery is outside its handler lifetime") end
  function delivery:close()
    if active then active = false; raw:close() end
  end
  function delivery:is_open() return active end
  local function terminal(action, request)
    live()
    local ok, result, failure = pcall(function()
      if action == "ack" then return client:queue_ack(raw) end
      local args = copy_table(request); args.message = raw
      return client:queue_nack(args)
    end)
    delivery:close()
    if not ok then error(result, 0) end
    return result, failure
  end
  function delivery:ack() return terminal("ack") end
  function delivery:nack(request) return terminal("nack", request) end
  for _, name in ipairs({"info", "extend", "state", "rewind_payload", "write_payload", "read_payload", "read_payload_json"}) do
    delivery[name] = function(_, ...) live(); return raw[name](raw, ...) end
  end
  return delivery
end

function M.with_dequeued_json(config, req, handler)
  if type(handler) ~= "function" then
    error("vectis.lockd.with_dequeued_json requires a handler", 2)
  end
  local client, err = M.open(config)
  if not client then return nil, err end
  local message
  local ok, result, failure = pcall(function()
    local dequeue_err
    message, dequeue_err = M._dequeue_one(client, req)
    if not message then
      if dequeue_err then return nil, lockdc_error(dequeue_err, "lockdc dequeue failed") end
      return nil -- empty is ordinary no-work, never a handler invocation
    end
    local payload, payload_err = message:read_payload_json()
    if payload == nil then return nil, lockdc_error(payload_err, "lockdc read_payload_json failed") end
    local value, handler_err = handler(payload, message, client, payload_err)
    if handler_err ~= nil then return nil, handler_error(handler_err) end
    if value == nil then return true end
    return value
  end)
  if message then message:close() end
  client:close()
  if not ok then error(result, 0) end
  return result, failure
end

function M.pouch_endpoint_build(root, options)
  return require("vectis.lockd.core").pouch_endpoint_build(root, options)
end
function M.pouch_endpoint_has_option(endpoint, name)
  return require("vectis.lockd.core").pouch_endpoint_has_option(endpoint, name)
end
return M
