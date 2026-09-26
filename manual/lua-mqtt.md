# Vectis Lua MQTT

`vectis.mqtt` is a small workflow wrapper over the bundled `curl` facade for
MQTT publish operations.

## Publish

`vectis.mqtt.publish(opts)` requires:

- `url`, such as `mqtt://broker.example.test/topic/name`
- one in-memory string payload source: `body`, `payload`, or `message`

The helper sets `protocols = "mqtt"` when no protocol allowlist is supplied and
selects libcurl's POSTFIELDS publishing mode. Payloads are explicitly buffered;
libcurl constructs the MQTT packet in memory. File sources (`body_path` and
`upload_path`) are rejected before transport setup because libcurl cannot stream
MQTT file publication through its upload callbacks. Empty strings and binary
payloads are supported.

Results use transport-only normalization:

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
as credentials, TLS, proxy, timeout, and retry. The helper selects POST publishing
regardless of supplied `method` or `upload` values.
