local M = {}

M.OK = 0
M.ERR_INVALID = 1
M.ERR_NOMEM = 2
M.ERR_STATE = 3
M.ERR_CONFLICT = 4
M.ERR_NOT_IMPLEMENTED = 5
M.ERR_TIMEOUT = 6

M.ERROR_SOURCE_NONE = 0
M.ERROR_SOURCE_VECTIS = 1
M.ERROR_SOURCE_KORE = 2
M.ERROR_SOURCE_LOCKDC = 3
M.ERROR_SOURCE_LONEJSON = 4
M.ERROR_SOURCE_PSLOG = 5
M.ERROR_SOURCE_CURL = 6
M.ERROR_SOURCE_OPENSSL = 7
M.ERROR_SOURCE_LIBSSH2 = 8
M.ERROR_SOURCE_CPKT = 9
M.ERROR_SOURCE_CAI = 10

local status_names = {
  [M.OK] = "ok",
  [M.ERR_INVALID] = "invalid",
  [M.ERR_NOMEM] = "nomem",
  [M.ERR_STATE] = "state",
  [M.ERR_CONFLICT] = "conflict",
  [M.ERR_NOT_IMPLEMENTED] = "not_implemented",
  [M.ERR_TIMEOUT] = "timeout",
}

local source_names = {
  [M.ERROR_SOURCE_NONE] = "none",
  [M.ERROR_SOURCE_VECTIS] = "vectis",
  [M.ERROR_SOURCE_KORE] = "kore",
  [M.ERROR_SOURCE_LOCKDC] = "lockdc",
  [M.ERROR_SOURCE_LONEJSON] = "lonejson",
  [M.ERROR_SOURCE_PSLOG] = "pslog",
  [M.ERROR_SOURCE_CURL] = "curl",
  [M.ERROR_SOURCE_OPENSSL] = "openssl",
  [M.ERROR_SOURCE_LIBSSH2] = "libssh2",
  [M.ERROR_SOURCE_CPKT] = "cpkt",
  [M.ERROR_SOURCE_CAI] = "cai",
}

function M.status_string(status)
  return status_names[status] or "unknown"
end

function M.error_source_string(source)
  return source_names[source] or "unknown"
end

function M.decorate_error(err, defaults)
  defaults = defaults or {}
  if type(err) ~= "table" then
    err = { message = tostring(err or defaults.message or "vectis error") }
  end

  local code = err.status or defaults.status or M.ERR_STATE
  local source = err.source_code or defaults.source_code or
                 M.ERROR_SOURCE_VECTIS
  err.status = code
  err.status_string = err.status_string or M.status_string(code)
  err.source_code = source
  err.source = M.error_source_string(source)
  if err.dependency_code == nil and defaults.dependency_code ~= nil then
    err.dependency_code = defaults.dependency_code
  end
  if err.http_status == nil and defaults.http_status ~= nil then
    err.http_status = defaults.http_status
  end
  if err.detail == nil and defaults.detail ~= nil then
    err.detail = defaults.detail
  end
  return err
end

function M.error(opts)
  return M.decorate_error(opts, opts)
end

return M
