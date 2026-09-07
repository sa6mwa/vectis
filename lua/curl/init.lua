local core = require("curl.core")
local lonejson = require("lonejson")
local M = {}

local function copy_table(source)
  local target = {}
  if source then
    for key, value in pairs(source) do
      target[key] = value
    end
  end
  return target
end

local function default_header(headers, name, value)
  for key in pairs(headers) do
    if type(key) == "string" and key:lower() == name then
      return
    end
  end
  headers[name] = value
end

function M.version()
  return core.version()
end

function M.perform(opts)
  return core.perform(opts)
end

function M.json(opts)
  opts = copy_table(opts)
  opts.headers = copy_table(opts.headers)
  default_header(opts.headers, "content-type", "application/json")
  default_header(opts.headers, "accept", "application/json")

  if opts.body_json ~= nil then
    opts.body = lonejson.encode_value(opts.body_json)
    opts.body_json = nil
  elseif opts.json ~= nil then
    opts.body = lonejson.encode_value(opts.json)
    opts.json = nil
  end

  local result = core.perform(opts)
  if result.body and result.body ~= "" then
    local ok, value = pcall(lonejson.decode_value, result.body)
    if ok then
      result.json = value
      result.response_json = value
    end
  end
  return result
end

function M.stream_json(opts)
  opts = copy_table(opts)
  opts.headers = copy_table(opts.headers)
  default_header(opts.headers, "content-type", "application/json")
  default_header(opts.headers, "accept", "application/json")

  if opts.request ~= nil then
    opts.request_schema = opts.request.schema
    opts.body_json = opts.request.value
    opts.request = nil
  end
  if opts.response ~= nil then
    opts.response_schema = opts.response.schema
    opts.response = nil
  end
  return core.perform(opts)
end

M.core = core

return M
