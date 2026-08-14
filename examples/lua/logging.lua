local vectis = require("vectis")
local log = require("vectis.log")
local pslog = require("pslog")
local status = require("vectis.status")

assert(vectis.log == log)
assert(log.raw == pslog)

local chunks = {}
local logger = assert(log.new({
  output = function(chunk)
    chunks[#chunks + 1] = chunk
  end,
  disable_timestamp = true,
  no_color = true,
  fields = {
    service = "vectis-logging-example",
  },
}))

logger:info("logger ready", "component", "lua")

local _, helper_err = nil, status.error({
  kind = "http",
  message = "downstream failed",
  status = status.ERR_TIMEOUT,
  source_code = status.ERROR_SOURCE_CURL,
  dependency_code = 28,
  http_status = 504,
})

assert(log.log_error(logger, "warn", "request failed", helper_err, {
  route = "/health",
}))
logger:close()

local payload = table.concat(chunks)
assert(payload:find('"msg":"logger ready"', 1, true))
assert(payload:find('"service":"vectis-logging-example"', 1, true))
assert(payload:find('"component":"lua"', 1, true))
assert(payload:find('"msg":"request failed"', 1, true))
assert(payload:find('"status_string":"timeout"', 1, true))
assert(payload:find('"source":"curl"', 1, true))
assert(payload:find('"dependency_code":28', 1, true))
assert(payload:find('"http_status":504', 1, true))
assert(payload:find('"route":"/health"', 1, true))

local fields = log.error_fields(helper_err)
assert(#fields >= 10)
assert(fields[1] == "error_kind")
assert(fields[2] == "http")

local ok, err = log.log_error(logger, "verbose", "bad level", helper_err)
assert(ok == nil)
assert(err.status == vectis.ERR_INVALID)
assert(err.source_code == vectis.ERROR_SOURCE_VECTIS)
assert(err.message:find("unknown pslog level", 1, true))

print("lua logging example ok")
