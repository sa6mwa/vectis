local core = require("vectis.dsv.core")
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

local function opts_from(first, second)
  if type(first) == "table" then
    return copy_table(first)
  end
  local opts = copy_table(second)
  opts.data = first
  return opts
end

local function decode_generated(json)
  local ok, value = pcall(lonejson.decode_value, json)
  if ok then
    return value
  end
  return nil, {
    message = tostring(value),
  }
end

function M.parse_json(first, second)
  local opts = opts_from(first, second)
  opts.schema = nil
  return core.parse_json(opts)
end

function M.parse(first, second)
  local opts = opts_from(first, second)
  if opts.schema ~= nil then
    return core.parse_typed(opts)
  end
  local json, err = core.parse_json(opts)
  if json == nil then
    return nil, err
  end
  local rows, decode_err = decode_generated(json)
  if rows == nil then
    return nil, decode_err
  end
  return rows
end

function M.parse_spill(first, second)
  local opts = opts_from(first, second)
  opts.schema = nil
  return core.parse_spill(opts)
end

function M.each(first, second)
  local opts = opts_from(first, second)
  local callback = opts.on_row or opts.callback
  if type(callback) ~= "function" then
    error("vectis.dsv.each requires on_row callback", 2)
  end
  if opts.schema ~= nil then
    return core.each(opts)
  end
  local rows, err = M.parse(opts)
  if rows == nil then return nil, err end
  for index, row in ipairs(rows) do
    if callback(index, row) == false then
      return nil, {
        message = "DSV row callback stopped",
      }
    end
  end
  return true
end

function M.to_string(first, second)
  return core.to_string(opts_from(first, second))
end

M.core = core

return M
