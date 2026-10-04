# Vectis Lua Lockd

`require("vectis").lockd` is a thin Vectis-owned workflow layer over the
dependency-native `lockdc` Lua module. It keeps the upstream module available as
`vectis.lockd.native` while adding Vectis-specific config normalization for packed
lockd bundle sources.

For app-owned transactions with automatic bounded receipt forwarding, one
persistent owner dispatcher, and typed outbox handlers, see
[Lua supervisors and workflows](lua-supervisor.md). Direct lockdc access remains
available; a raw notification method alone does not connect route workers to
the supervisor.

## Entry Points

- `vectis.lockd.config(opts)` normalizes a lockd config table.
- `vectis.lockd.open(opts)` normalizes the config and delegates to
  `lockdc.open`.
- `vectis.lockd.with_client(opts, handler)` opens a client, calls `handler`, and
  closes the client after the handler returns or raises.
- `vectis.lockd.load_json(opts, req)` opens a client, acquires a state lease,
  reads JSON with `lease:read_json()`, releases the lease after a successful
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
  dequeues one message, reads `message:read_payload_json()`, calls
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
- `client_bundle = "/path/to/client.pem"` becomes a file-backed
  `client_bundle_source`.

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

## Canonical Pouch endpoints and owned workflow clients

`vectis.lockd.pouch_endpoint_build(absolute_root, options)` binds the public
native builder. `options` is an ordered array of `{name=...,value=...}`;
omitting value emits a bare option, while `value=""` emits an empty value.
The builder percent-encodes reserved root/name/value bytes and frees its native
allocation after copying the result. `pouch_endpoint_has_option(endpoint,name)`
requires the runner/native `vectis.lockd.core` companion; source-only rocks
ship no native implementation. The inspector checks decoded names using the native parser, including duplicates, bare options
and fragments. Native parse errors return `nil,error`; NUL strings are rejected.
Query encryption names are `crypto_key` and `crypto_key_file`; native app
configuration fields use `pouch_crypto_*`.

Both workflow and scope-consumer clients resolve the actual app endpoint/key
after startup/fork. Flat app crypto options become typed upstream Pouch options;
endpoint queries remain untouched for the native parser. A scoped owning-thread
bridge bypasses only upstream Lua's query prohibition, restoring the exact
original endpoints at public `lc_client_open`. It injects the app's native
borrowed lockd logger or explicit mute and closes owned children/clients before
logger cleanup. Direct `lockdc.open` and standalone `vectis.lockd.open` retain
their upstream behavior. Independent temporary logger injection is unsupported
because native child handles can outlive Lua client close.

Native Pouch single-writer policy is preserved: explicitly use
`single_writer=false` when the app and its workflow clients share an opened root.
The app default selects shared-writer storage and encryption consistently.

App-level `lockd` settings use Vectis's native configuration: `endpoints`,
`unix_socket_path`, `client_bundle="embedded"` (or `bundle`),
`client_bundle_path` (or `bundle_path`), `default_namespace` (or `namespace`),
`timeout_ms`, flat `pouch_crypto_key`, `pouch_crypto_key_file`,
`pouch_crypto_generate_key_file`, `pouch_compression`, and `logger_disabled`.
Unknown fields, malformed endpoint arrays and NUL strings are rejected.
Use official endpoint query options for additional native app Pouch settings.
Nested `pouch` tables and upstream-only transport controls belong in an explicit
workflow/consumer `client` configuration; the app rejects them rather than
opening storage with different settings. `logger_disabled=true` mutes native
app clients and inherited private clients independently of the log seed.

`with_dequeued_json(config, request, handler)` uses public bounded batch-one
intake. Empty dequeue returns `nil, nil` without calling the handler. Its delivery
adapter preserves `info`, payload readers/writers, `extend`, and explicit
`ack`/`nack`; terminal operations use public client methods followed by explicit
handle close. Delivery access after callback return is rejected. All error paths
close the message and client. A handler throw retains its error identity;
`nil, error` returns a structured error; a successful handler with no result
returns `true`. Raw `lockdc` access is unchanged.
