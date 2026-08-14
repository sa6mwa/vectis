local status = require("vectis.status")

local M = {
  status = status,
}

local status_exports = {
  "OK",
  "ERR_INVALID",
  "ERR_NOMEM",
  "ERR_STATE",
  "ERR_CONFLICT",
  "ERR_NOT_IMPLEMENTED",
  "ERR_TIMEOUT",
  "ERROR_SOURCE_NONE",
  "ERROR_SOURCE_VECTIS",
  "ERROR_SOURCE_KORE",
  "ERROR_SOURCE_LOCKDC",
  "ERROR_SOURCE_LONEJSON",
  "ERROR_SOURCE_PSLOG",
  "ERROR_SOURCE_CURL",
  "ERROR_SOURCE_OPENSSL",
  "ERROR_SOURCE_LIBSSH2",
  "ERROR_SOURCE_CPKT",
}

local dependency_modules = {
  "lockdc",
  "lonejson",
  "pslog",
  "lql",
  "cai",
  "libmdf",
  "softline",
  "curl",
  "opcua",
  "openssl",
  "zlib",
  "audio",
  "sus",
}

local workflow_modules = {
  auth = "vectis.auth",
  cert = "vectis.cert",
  embedded = "vectis.embedded",
  server = "vectis.server",
  ssh = "vectis.ssh",
  log = "vectis.log",
  http = "vectis.http",
  webdav = "vectis.webdav",
  rest = "vectis.rest",
  terminal = "vectis.terminal",
  mqtt = "vectis.mqtt",
  smtp = "vectis.smtp",
  lockd = "vectis.lockd",
  dsv = "vectis.dsv",
  xml = "vectis.xml",
}

local function optional_require(name)
  local ok, module = pcall(require, name)
  if ok then
    return module
  end
  return nil
end

for _, name in ipairs(status_exports) do
  M[name] = status[name]
end

M.status_string = status.status_string
M.error_source_string = status.error_source_string

local version = optional_require("vectis.version")
M.version = type(version) == "string" and version or "0.0.0"

M.libs = {}
for _, name in ipairs(dependency_modules) do
  local module = optional_require(name)
  if module ~= nil then
    M.libs[name] = module
  end
end

for field, module_name in pairs(workflow_modules) do
  local module = optional_require(module_name)
  if module ~= nil then
    M[field] = module
  end
end

function M.has_embedded_lockd_bundle()
  return false
end

function M.embedded_lockd_bundle_size()
  return 0
end

function M.embedded_lockd_bundle_source()
  return nil, status.error({
    status = status.ERR_STATE,
    source_code = status.ERROR_SOURCE_VECTIS,
    message = "no embedded lockd bundle",
  })
end

return M
