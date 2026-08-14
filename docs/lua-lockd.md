# Vectis Lua Lockd

`require("vectis").lockd` is a thin Vectis-owned workflow layer over the
dependency-native `lockdc` Lua module. It keeps the upstream module available as
`vectis.lockd.native` while adding Vectis-specific config normalization for packed
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
- `vectis.lockd.with_dequeued_json(opts, req, handler)` opens a client,
  dequeues one message, reads `message:payload_json()`, calls
  `handler(payload, message, client, payload_bytes)`, closes the message handle,
  and closes the client. It does not implicitly ack or nack; call
  `message:ack()` or `message:nack()` in the handler.
- `vectis.lockd.encode_json(value)` delegates to `lockdc.encode_json`.
- `vectis.lockd.decode_json(json)` delegates to `lockdc.decode_json`.
- `vectis.lockd.json_null` is the direct lockdc JSON null sentinel.
- `vectis.lockd.native` is the direct `lockdc` module for complete upstream access.

Use direct `require("lockdc")` or `vectis.lockd.native` for complete lockd API
coverage: query output, namespace configuration, queue ack/nack/extend,
dequeue batch/state variants, lease metadata/mutate/remove/attachment helpers,
and message ack/nack/extend/state/payload controls. Vectis-owned helpers stay
narrow and only wrap workflows that combine packed bundle config, JSON defaults,
or deterministic client/handle cleanup.

## Config Normalization

`config()` accepts the direct `lockdc.open` fields and adds these Vectis
conveniences:

- `namespace` is copied to `default_namespace` and then removed.
- `client_bundle = "embedded"` uses the packed in-memory lockd bundle source.
- `client_bundle = "/path/to/client.pem"` is copied to
  `client_bundle_path`.

When no packed lockd bundle exists, `client_bundle = "embedded"` returns
`nil, err`; `err.status`, `err.status_string`, and `err.message` follow the
Vectis Lua facade conventions.

## Errors

Expected workflow failures return `nil, err` with the standard Vectis status
fields. Underlying `lockdc` open, acquire, update, release, enqueue, dequeue,
and payload failures use `err.source = "lockdc"` and
`err.source_code = vectis.ERROR_SOURCE_LOCKDC`. User callback failures returned
as `nil, err` from `with_client`, `with_acquired_lease`, or
`with_dequeued_json` use `err.source = "vectis"` and
`err.source_code = vectis.ERROR_SOURCE_VECTIS` unless the callback already
returned a structured source.

## Example

```lua
local vectis = require("vectis")

assert(vectis.lockd.enqueue_json({
  endpoints = {"https://127.0.0.1:8443"},
  client_bundle = "embedded",
  namespace = "app",
}, {
  queue = "orders",
}, {
  type = "order.created",
  id = "1001",
}))
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

For one-shot JSON dequeue handling with explicit ack/nack:

```lua
assert(vectis.lockd.with_dequeued_json({
  endpoints = {"https://127.0.0.1:8443"},
  client_bundle = "embedded",
  namespace = "app",
}, {
  queue = "orders",
  owner = "orders-worker",
  visibility_timeout_seconds = 30,
}, function(payload, message)
  assert(payload.type == "order.created")
  return message:ack()
end))
```
