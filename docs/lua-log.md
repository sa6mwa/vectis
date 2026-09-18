# Lua Logging

`vectis.log` is a small logging DX layer over the dependency-native `pslog` Lua
module. It does not replace `pslog`; direct access remains available as
`vectis.log.native`.

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

## Seeded Vectis Logging

`vectis.log.configure(opts)` configures the logger that `vectis.app` and its
native managed services inherit automatically. It is a Vectis convenience
layer over pslog, not a replacement for direct `pslog` configuration.

Call it before the first `vectis.app.new()` in a Lua runtime:

```lua
local log = require("vectis.log")

assert(log.configure({
  sys = "checkout", -- defaults to "vectis"
  fields = {
    environment = "production",
    region = "se-1",
  },
  subs = {
    lockdc = false, -- silence only Vectis-owned lockdc clients
    cai = true,
    curl = true,
    http = true, -- structured Kore access records
  },
}))
```

The seed starts with JSON output and reads `LOG_` environment configuration by
default, exactly as pslog does. Set `env = false` to ignore the environment,
or set `prefix = "MYAPP_LOG_"` to use a different environment prefix. Other
pslog creation options are passed through unchanged.

Every propagated record has these structured fields:

- `sys`: the configured system identifier; the default is `vectis`.
- `app`: the `app_name` passed to `vectis.app.new()`.
- `sub`: the Vectis subsystem producing the record.

`fields` adds stable application fields. It may not replace `sys`, `app`, or
`sub`; those fields establish the Vectis record identity. `subs` is an optional
boolean policy table. Its complete set of names is `vectis`, `lockdc`, `cai`,
`opcua`, `curl`, `audio`, `sus`, and `http`; omitted names remain enabled.
`http` controls Vectis's default structured Kore access sink. Setting a
name to `false` disables logging only for that subsystem. A service-level
`logger_disabled = true` remains a stronger, local opt-out.

The seed is immutable after the first `vectis.app.new()` because native apps
and workers can retain the logger on lifecycle threads. `configure()` then
returns `nil, err` with `ERR_STATE`. The seed intentionally rejects a Lua
function as `output`: function sinks are Lua-state-bound and cannot safely
receive records from Vectis native lifecycle threads. Use a pslog file or
standard-output sink for propagated logging; direct `pslog` and `log.new()`
remain available for Lua-only function sinks.
