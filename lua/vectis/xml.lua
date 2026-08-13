local core = require("vectis.xml.core")

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
  opts.xml = first
  return opts
end

function M.parse(first, second)
  return core.parse(opts_from(first, second))
end

function M.parse_record(first, second)
  return core.parse_record(opts_from(first, second))
end

M.core = core

return M
