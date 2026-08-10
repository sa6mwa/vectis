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

function M.version()
  return core.version()
end

function M.perform(opts)
  return core.perform(opts)
end

function M.json(opts)
  opts = copy_table(opts)
  opts.headers = copy_table(opts.headers)
  opts.headers["content-type"] =
      opts.headers["content-type"] or "application/json"
  opts.headers.accept = opts.headers.accept or "application/json"

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
  opts.headers["content-type"] =
      opts.headers["content-type"] or "application/json"
  opts.headers.accept = opts.headers.accept or "application/json"

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
