local pslog = require("pslog")
local vstatus = require("vectis.status")

local unpack_values = table.unpack or unpack

local M = {
  native = pslog,
}

local seeded = {
  logger = nil,
  locked = false,
  subs = {
    vectis = true,
    lockdc = true,
    cai = true,
    opcua = true,
    curl = true,
    audio = true,
    sus = true,
  },
}

local known_subs = {
  vectis = true,
  lockdc = true,
  cai = true,
  opcua = true,
  curl = true,
  audio = true,
  sus = true,
}

local reserved_seed_fields = {
  sys = true,
  app = true,
  sub = true,
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

-- Configure the logger Vectis attaches to native app and managed-service
-- lifecycles. Direct pslog use stays independent: only this convenience seed
-- receives automatic propagation into Vectis-owned components.
function M.configure(opts)
  local config
  local fields
  local subs
  local use_env
  local prefix
  local sys
  local root
  local logger
  local scoped_err
  local ok

  if seeded.locked then
    return log_status_error(
      "vectis.log.configure must run before vectis.app.new", vstatus.ERR_STATE)
  end
  if opts ~= nil and type(opts) ~= "table" then
    error("vectis.log.configure requires an options table", 2)
  end
  opts = copy_table(opts)
  config = copy_table(opts)
  fields = config.fields or config.default_fields
  config.fields = nil
  config.default_fields = nil
  subs = config.subs
  config.subs = nil
  use_env = config.env
  config.env = nil
  prefix = config.prefix
  config.prefix = nil
  sys = config.sys
  config.sys = nil

  if use_env == nil then
    use_env = true
  elseif type(use_env) ~= "boolean" then
    error("vectis.log.configure env must be a boolean", 2)
  end
  if prefix == nil then
    prefix = "LOG_"
  elseif type(prefix) ~= "string" or prefix == "" then
    error("vectis.log.configure prefix must be a non-empty string", 2)
  end
  if sys == nil then
    sys = "vectis"
  elseif type(sys) ~= "string" or sys == "" then
    error("vectis.log.configure sys must be a non-empty string", 2)
  end
  if type(config.output) == "function" then
    return log_status_error(
      "vectis.log.configure does not accept function output; Vectis loggers "
        .. "may write from native lifecycle threads", vstatus.ERR_INVALID)
  end
  if fields ~= nil and type(fields) ~= "table" then
    error("vectis.log.configure fields must be a table", 2)
  end
  if fields then
    for key, _ in pairs(fields) do
      if reserved_seed_fields[key] then
        return log_status_error(
          "vectis.log.configure fields may not override " .. key,
          vstatus.ERR_INVALID)
      end
    end
  end
  if subs ~= nil and type(subs) ~= "table" then
    error("vectis.log.configure subs must be a table", 2)
  end
  seeded.subs = {
    vectis = true,
    lockdc = true,
    cai = true,
    opcua = true,
    curl = true,
    audio = true,
    sus = true,
  }
  if subs then
    for key, value in pairs(subs) do
      if not known_subs[key] then
        return log_status_error(
          "vectis.log.configure has unknown sub: " .. tostring(key),
          vstatus.ERR_INVALID)
      end
      if type(value) ~= "boolean" then
        error("vectis.log.configure subs values must be booleans", 2)
      end
      seeded.subs[key] = value
    end
  end
  if config.mode == nil then
    config.mode = "json"
  end

  if use_env then
    ok, root = pcall(pslog.from_env, prefix, config)
  else
    ok, root = pcall(pslog.new, config)
  end
  if not ok or root == nil then
    return log_status_error(root or "failed to create pslog logger",
                            vstatus.ERR_INVALID)
  end
  logger, scoped_err = scoped_logger(root, fields)
  if logger == nil then
    return nil, scoped_err
  end
  ok, logger = pcall(logger.with, logger, "sys", sys)
  root:close()
  if not ok or logger == nil then
    return log_status_error(logger or "failed to derive Vectis logger",
                            vstatus.ERR_INVALID)
  end
  if seeded.logger ~= nil then
    seeded.logger:close()
  end
  seeded.logger = logger
  return true
end

-- Internal native-app bridge. Do not use from application code.
function M._acquire_seed()
  local ok, err

  if seeded.logger == nil then
    ok, err = M.configure({})
    if not ok then
      return nil, err
    end
  end
  seeded.locked = true
  return seeded.logger, copy_table(seeded.subs)
end

return M
