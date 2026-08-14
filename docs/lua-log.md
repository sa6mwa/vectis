# Lua Logging

`vectis.log` is a small logging DX layer over the dependency-native `pslog` Lua
module. It does not replace `pslog`; direct access remains available as
`vectis.log.raw`.

## Logger Creation

```lua
local log = require("vectis.log")

local logger = assert(log.new({
  output = function(chunk)
    io.stderr:write(chunk)
  end,
  fields = {
    service = "orders",
  },
}))

logger:info("service started", "bind", "127.0.0.1")
logger:close()
```

`log.new(opts)` creates a JSON `pslog` logger. `opts.fields` or
`opts.default_fields` are applied with `logger:with(...)` and then removed from
the pslog option table. Other options are passed through to
`pslog.new_json(opts)`.

`log.from_env(prefix[, opts])` wraps `pslog.from_env(...)` with the same
default-field behavior and structured Vectis errors. Use it when process
environment should be allowed to override the pslog configuration while
Vectis-owned default fields still apply.

## Error Logging

```lua
local status = require("vectis.status")
local log = require("vectis.log")

local logger = assert(log.new())
local _, err = nil, status.error({
  kind = "http",
  message = "downstream timed out",
  status = status.ERR_TIMEOUT,
  source_code = status.ERROR_SOURCE_CURL,
  dependency_code = 28,
})

assert(log.log_error(logger, "warn", "request failed", err, {
  route = "/orders",
}))
logger:close()
```

`log.log_error(logger, level, message, err[, fields])` writes Vectis structured
error metadata as pslog fields. Unknown levels return `nil, err` with Vectis
status/source metadata; programmer misuse raises Lua errors.

`log.error_fields(err)` returns a key/value array suitable for advanced
pslog calls when an application needs complete control over the final log call.
