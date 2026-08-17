# Vectis Lua Kore

`vectis.kore` exposes the embedded Kore runtime contract to Lua code without
creating a second server API. Use `vectis.server` to construct servers, routes,
WebDAV mounts, metrics endpoints, and managed services.

The embedded `vectis` binary preloads a C-owned `vectis.kore` module:

```lua
local kore = require("vectis.kore")

assert(kore.runtime_available == true)
assert(kore.runtime_model == "embedded")
assert(kore.websocket.TEXT == require("vectis").websocket.TEXT)
```

The standalone Lua rock ships a pure-Lua fallback with the same constants and
`runtime_available = false`, because an ordinary Lua process does not contain
the Vectis-owned Kore runtime. A `vectis` binary built with
`VECTIS_WITH_KORE_RUNTIME=OFF` also reports `runtime_available = false`, with
`runtime_model = "none"`.

## Fields

- `runtime_available`: true when the current process contains the embedded
  Vectis/Kore runtime.
- `runtime_model`: `"embedded"` in the `vectis` binary and `"external"` in the
  standalone Lua fallback.
- `MAX_WORKER_COUNT`: maximum explicit worker count accepted by Vectis server
  config.
- `MAX_KORE_CURL_TIMEOUT_SECONDS`: maximum explicit timeout accepted for
  Kore-owned curl operations such as ACME.
- `DEFAULT_WEBSOCKET_MAX_FRAME_BYTES`: default Vectis WebSocket frame ceiling.
- `DEFAULT_WEBSOCKET_TIMEOUT_MS`: default Vectis WebSocket idle timeout.
- `WORKER_DEATH_RESTART` and `WORKER_DEATH_TERMINATE`: worker death policy
  constants matching `server.new({ worker_death_policy = ... })` semantics.
- `websocket`: WebSocket opcode constants.
