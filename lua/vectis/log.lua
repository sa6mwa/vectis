local pslog = require("pslog")
local vstatus = require("vectis.status")

local unpack_values = table.unpack or unpack

local M = {
  native = pslog,
}

local reserved_error_fields = {
  dependency_code = true,
  dependency_message = true,
  error_kind = true,
  error_message = true,
  http_status = true,
  source = true,
  source_code = true,
  status = true,
  status_string = true,
}

local function copy_table(source)
  local target = {}
  if source then
    for key, value in pairs(source) do
      target[key] = value
    end
  end
  return target
end

local function log_status_error(message, status, source_code)
  return nil, vstatus.error({
    kind = "log",
    message = tostring(message or "logging helper failed"),
    status = status or vstatus.ERR_STATE,
    source_code = source_code or vstatus.ERROR_SOURCE_PSLOG,
  })
end

local function append_pair(fields, key, value)
  if value ~= nil then
    fields[#fields + 1] = key
    fields[#fields + 1] = value
  end
end

local function append_map_fields(fields, map, skip)
  local keys = {}
  if map == nil then
    return
  end
  if type(map) ~= "table" then
    error("vectis.log fields must be a table", 3)
  end
  for key, _ in pairs(map) do
    if type(key) ~= "string" then
      error("vectis.log field names must be strings", 3)
    end
    if not (skip and skip[key]) then
      keys[#keys + 1] = key
    end
  end
  table.sort(keys)
  for _, key in ipairs(keys) do
    append_pair(fields, key, map[key])
  end
end

local function scoped_logger(logger, fields)
  local scoped
  local ok
  local close_ok

  if fields == nil then
    return logger
  end
  local field_args = {}
  append_map_fields(field_args, fields)
  if #field_args == 0 then
    return logger
  end
  ok, scoped = pcall(logger.with, logger, unpack_values(field_args))
  if not ok then
    if type(logger.close) == "function" then
      close_ok = pcall(logger.close, logger)
      (function(_) end)(close_ok)
    end
    return log_status_error(scoped, vstatus.ERR_INVALID)
  end
  return scoped
end

local function take_default_fields(opts)
  local fields = opts.fields or opts.default_fields
  opts.fields = nil
  opts.default_fields = nil
  return fields
end

local function new_with(factory, opts)
  local logger
  local ok
  local fields

  if opts ~= nil and type(opts) ~= "table" then
    error("vectis.log.new requires an options table", 3)
  end
  opts = copy_table(opts)
  fields = take_default_fields(opts)
  ok, logger = pcall(factory, opts)
  if not ok then
    return log_status_error(logger, vstatus.ERR_INVALID)
  end
  if logger == nil then
    return log_status_error("failed to create pslog logger")
  end
  return scoped_logger(logger, fields)
end

function M.new(opts)
  return new_with(pslog.new_json, opts)
end

function M.from_env(prefix_or_opts, maybe_opts)
  local opts
  local prefix
  local fields
  local logger
  local ok

  if type(prefix_or_opts) == "table" and maybe_opts == nil then
    opts = copy_table(prefix_or_opts)
    fields = take_default_fields(opts)
    ok, logger = pcall(pslog.from_env, opts)
  else
    if maybe_opts ~= nil and type(maybe_opts) ~= "table" then
      error("vectis.log.from_env requires an options table", 2)
    end
    prefix = prefix_or_opts
    opts = copy_table(maybe_opts)
    fields = take_default_fields(opts)
    ok, logger = pcall(pslog.from_env, prefix, opts)
  end
  if not ok then
    return log_status_error(logger, vstatus.ERR_INVALID)
  end
  if logger == nil then
    return log_status_error("failed to create pslog logger")
  end
  return scoped_logger(logger, fields)
end

function M.error_fields(err)
  local fields = {}

  if type(err) == "table" then
    append_pair(fields, "error_kind", err.kind)
    append_pair(fields, "error_message", err.message)
    append_pair(fields, "status", err.status)
    append_pair(fields, "status_string", err.status_string)
    append_pair(fields, "source", err.source)
    append_pair(fields, "source_code", err.source_code)
    append_pair(fields, "dependency_code", err.dependency_code)
    append_pair(fields, "dependency_message", err.dependency_message)
    append_pair(fields, "http_status", err.http_status)
  elseif err ~= nil then
    append_pair(fields, "error_message", tostring(err))
  end
  return fields
end

function M.log_error(logger, level, message, err, fields)
  if logger == nil then
    error("vectis.log.log_error requires a pslog logger", 2)
  end
  level = level or "error"
  if type(level) ~= "string" or type(logger[level]) ~= "function" then
    return log_status_error("unknown pslog level: " .. tostring(level),
                            vstatus.ERR_INVALID, vstatus.ERROR_SOURCE_VECTIS)
  end
  if type(message) ~= "string" then
    error("vectis.log.log_error requires a message", 2)
  end

  local field_args = M.error_fields(err)
  append_map_fields(field_args, fields, reserved_error_fields)
  local ok, result = pcall(logger[level], logger, message,
                           unpack_values(field_args))
  if not ok then
    return log_status_error(result, vstatus.ERR_STATE)
  end
  (function(_) end)(result)
  return true
end

return M
