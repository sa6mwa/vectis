local curl = require("curl")
local vstatus = require("vectis.status")

local M = {}

local function curl_status(result)
  if result and (result.code == 28 or
      result.code_name == "CURLE_OPERATION_TIMEDOUT") then
    return vstatus.ERR_TIMEOUT
  end
  return vstatus.ERR_STATE
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

local function opts_from(first, second)
  if type(first) == "table" then
    return copy_table(first)
  end
  local opts = copy_table(second)
  opts.url = first
  return opts
end

function M.error_for(result)
  if type(result) ~= "table" then
    return vstatus.error({
      kind = "invalid_result",
      message = "MQTT result is invalid",
      status = vstatus.ERR_INVALID,
      source_code = vstatus.ERROR_SOURCE_VECTIS,
    })
  end
  if result.ok ~= true then
    return vstatus.error({
      kind = "transport",
      code = result.code,
      code_name = result.code_name,
      message = result.error or result.code_name or "curl MQTT transfer failed",
      attempts = result.attempts,
      status = curl_status(result),
      source_code = vstatus.ERROR_SOURCE_CURL,
      dependency_code = result.code,
    })
  end
  return nil
end

function M.normalize(result)
  local transport_ok = type(result) == "table" and result.ok == true
  local error_info = M.error_for(result)

  if type(result) ~= "table" then
    result = {}
  end
  result.transport_ok = transport_ok
  if error_info ~= nil then
    if type(result.error) == "string" then
      result.error_message = result.error
    end
    result.ok = false
    result.error = error_info
  else
    result.ok = true
    result.error = nil
  end
  return result
end

function M.publish(first, second)
  local opts = opts_from(first, second)
  if opts.url == nil then
    error("vectis.mqtt.publish requires url", 2)
  end
  if opts.body == nil and opts.payload ~= nil then
    opts.body = opts.payload
  end
  if opts.body == nil and opts.message ~= nil then
    opts.body = opts.message
  end
  if opts.body == nil and opts.body_path == nil and opts.upload_path == nil then
    error("vectis.mqtt.publish requires body, payload, message, body_path, or upload_path", 2)
  end
  if opts.protocols == nil then
    opts.protocols = "mqtt"
  end
  opts.upload = true
  return M.normalize(curl.perform(opts))
end

return M
