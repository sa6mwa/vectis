# Vectis Lua Lockd

`require("vectis").lockd` is a thin Vectis-owned workflow layer over the raw
`lockdc` Lua module. It keeps the upstream module available as
`vectis.lockd.raw` while adding Vectis-specific config normalization for packed
lockd bundle sources.

## Entry Points

- `vectis.lockd.config(opts)` normalizes a lockd config table.
- `vectis.lockd.open(opts)` normalizes the config and delegates to
  `lockdc.open`.
- `vectis.lockd.with_client(opts, handler)` opens a client, calls `handler`, and
  closes the client after the handler returns or raises.
- `vectis.lockd.load_json(opts, req)` opens a client, acquires a state lease,
  reads JSON with `lease:get_json()`, releases the lease after a successful
  read or no-content response, closes the client, and returns `value, meta`.
- `vectis.lockd.save_json(opts, req, value)` opens a client, acquires a state
  lease, writes JSON with `lease:update_json(value)`, releases the lease after a
  successful write, and closes the client.
- `vectis.lockd.enqueue_json(opts, req, value)` opens a client, JSON-encodes
  `value`, sets `content_type = "application/json"` when absent, enqueues the
  payload, and closes the client.
- `vectis.lockd.with_acquired_lease(opts, req, handler)` opens a client,
  acquires a lease, calls `handler(lease, client)`, closes the lease handle, and
  closes the client. It does not implicitly release the remote lease; call
  `lease:release()` in the handler when that is the intended workflow.
- `vectis.lockd.encode_json(value)` delegates to `lockdc.encode_json`.
- `vectis.lockd.decode_json(json)` delegates to `lockdc.decode_json`.
- `vectis.lockd.json_null` is the raw lockdc JSON null sentinel.
- `vectis.lockd.raw` is the raw `lockdc` module for complete upstream access.

## Config Normalization

`config()` accepts the raw `lockdc.open` fields and adds these Vectis
conveniences:

- `namespace` is copied to `default_namespace` and then removed.
- `client_bundle = "embedded"` uses the packed in-memory lockd bundle source.
- `client_bundle = "/path/to/client.pem"` is copied to
  `client_bundle_path`.

When no packed lockd bundle exists, `client_bundle = "embedded"` returns
`nil, err`; `err.status`, `err.status_string`, and `err.message` follow the
Vectis Lua facade conventions.

## Example

```lua
local vectis = require("vectis")

local client = assert(vectis.lockd.open({
  endpoints = {"https://127.0.0.1:8443"},
  client_bundle = "embedded",
  namespace = "app",
}))

local payload = vectis.lockd.encode_json({
  type = "order.created",
  id = "1001",
})

local ok, err = client:enqueue({
  queue = "orders",
  content_type = "application/json",
}, payload)

client:close()
assert(ok, err and err.message or "enqueue failed")
```

For one-shot JSON state save/load:

```lua
assert(vectis.lockd.save_json({
  endpoints = {"https://127.0.0.1:8443"},
  client_bundle = "embedded",
  namespace = "app",
}, {
  key = "accounts/1001",
  owner = "accounts-api",
  ttl_seconds = 30,
}, {
  status = "active",
}))

local state = assert(vectis.lockd.load_json({
  endpoints = {"https://127.0.0.1:8443"},
  client_bundle = "embedded",
  namespace = "app",
}, {
  key = "accounts/1001",
  owner = "accounts-api",
  ttl_seconds = 30,
}))
assert(state.status == "active")
```

For one-shot JSON queue publishing:

```lua
assert(vectis.lockd.enqueue_json({
  endpoints = {"https://127.0.0.1:8443"},
  client_bundle = "embedded",
  namespace = "app",
}, {
  queue = "orders",
  visibility_timeout_seconds = 30,
}, {
  type = "order.created",
  id = "1001",
}))
```
