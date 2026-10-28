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
  "ERROR_SOURCE_CAI",
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
  pdf = "vectis.pdf",
  png = "vectis.png",
  compression = "vectis.compression",
  sql = "vectis.sql",
  sqlite = "vectis.sqlite",
  pg = "vectis.pg",
  digest = "vectis.digest",
  buffer = "vectis.buffer",
  auth = "vectis.auth",
  cert = "vectis.cert",
  embedded = "vectis.embedded",
  app = "vectis.app",
  mailbox = "vectis.mailbox",
  curl_worker = "vectis.curl_worker",
  cai_worker = "vectis.cai_worker",
  audio_worker = "vectis.audio_worker",
  sus_worker = "vectis.sus_worker",
  ssh = "vectis.ssh",
  log = "vectis.log",
  http = "vectis.http",
  cai = "vectis.cai",
  cli = "vectis.cli",
  smith = "vectis.smith",
  webdav = "vectis.webdav",
  rest = "vectis.rest",
  terminal = "vectis.terminal",
  mqtt = "vectis.mqtt",
  smtp = "vectis.smtp",
  lockd = "vectis.lockd",
  supervisor = "vectis.supervisor",
  workflow = "vectis.workflow",
  storage = "vectis.storage",
  dsv = "vectis.dsv",
  xml = "vectis.xml",
  kore = "vectis.kore",
}

-- Only exact root absence or a known native core absent from a source-only
-- rock is optional. Preserve loader exceptions, including table identities.
local optional_native = {
  ["vectis.pg"] = { ["vectis.pg.core"] = true, ["vectis.pg.connection.core"] = true },
  lockdc = { ["lockdc.core"] = true }, lonejson = { ["lonejson.core"] = true },
  ["lockdc.postgres"] = { ["lockdc.core"] = true },
  pslog = { ["pslog.core"] = true }, lql = { ["lql.core"] = true },
  cai = { ["cai.core"] = true }, libmdf = { ["libmdf.core"] = true },
  softline = { ["softline.core"] = true },
  curl = { ["curl.core"] = true },
  ["vectis.auth"] = { ["vectis.auth.core"] = true },
  ["vectis.dsv"] = { ["vectis.dsv.core"] = true },
  ["vectis.xml"] = { ["vectis.xml.core"] = true },
  ["vectis.digest"] = { ["vectis.digest.core"] = true },
  ["vectis.sql"] = { ["vectis.sqlite.core"] = true },
  ["vectis.sqlite"] = { ["vectis.sqlite.core"] = true },
  ["vectis.pdf"] = { ["vectis.pdf.core"] = true },
  ["vectis.png"] = { ["vectis.png.core"] = true },
  ["vectis.compression"] = { ["vectis.compression.core"] = true },
  ["vectis.buffer"] = { ["vectis.buffer.core"] = true },
}
local helper_dependencies = {
  ["vectis.lockd"]={"lockdc"}, ["vectis.workflow"]={"lockdc"},
  ["vectis.log"]={"pslog"}, ["vectis.http"]={"curl"},
  ["vectis.webdav"]={"curl"}, ["vectis.rest"]={"curl","lonejson"},
  ["vectis.smtp"]={"curl"}, ["vectis.mqtt"]={"curl"},
  ["vectis.cai"]={"cai"}, ["vectis.smith"]={"cai"},
  ["vectis.terminal"]={"libmdf","softline"}, ["vectis.dsv"]={"lonejson"},
}
for helper, dependencies in pairs(helper_dependencies) do
  optional_native[helper] = optional_native[helper] or {}
  for _, dependency in ipairs(dependencies) do
    optional_native[helper][dependency] = true
    for core in pairs(optional_native[dependency] or {}) do optional_native[helper][core] = true end
  end
end
local function optional_require(name)
  local ok, module = pcall(require, name)
  if ok then
    return module
  end
  local missing = type(module) == "string" and (module:match("^module '([^']+)' not found:")
    or module:match("^.-:%d+: module '([^']+)' not found:"))
  local search_failure = missing and module:find("\n\tno field package.preload[", 1, true)
  if search_failure and (missing == name or (optional_native[name] and optional_native[name][missing])) then
    return nil
  end
  error(module, 0)
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
M.libs.lockdc_postgres = optional_require("lockdc.postgres")

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
