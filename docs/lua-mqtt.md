# Vectis Lua MQTT

`vectis.mqtt` is a small workflow wrapper over the bundled `curl` facade for
MQTT publish operations.

## Publish

`vectis.mqtt.publish(opts)` requires:

- `url`, such as `mqtt://broker.example.test/topic/name`
- one payload source: `body`, `payload`, `message`, `body_path`, or
  `upload_path`

The helper sets `protocols = "mqtt"` when no protocol allowlist is supplied and
enables libcurl upload mode. Results use transport-only normalization:

- `ok = true` means libcurl completed the publish transfer.
- `transport_ok` mirrors libcurl transfer success.
- failures return `ok = false` and `error.kind = "transport"`.
- transport errors carry Vectis status/source metadata with `source = "curl"`
  and the CURLcode in `error.dependency_code` when available.

```lua
local vectis = require("vectis")

local result = vectis.mqtt.publish({
  url = "mqtt://broker.local/events/build",
  message = "{\"ok\":true}\n",
  username = "publisher",
  password = "secret",
})
assert(result.ok, result.error and result.error.message)
```

For advanced MQTT options, pass through supported `curl.perform` options such
as credentials, TLS, proxy, timeout, retry, and upload path fields.
