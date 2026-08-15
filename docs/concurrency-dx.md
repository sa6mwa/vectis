# Vectis Concurrency DX

Vectis service composition is C-owned and Lua-pumped. Kore HTTP/API/WebDAV,
liblockdc consumer services, OPC UA clients/servers, curl transfers, and other
dependency-backed services may run in one process, but Lua state entry is never a
background-thread side effect.

The DX layer provides a small mailbox primitive for cross-service handoff:

- C services publish bounded, copied events into a `vectis_mailbox`.
- C services consume events from a `vectis_mailbox` without entering Lua.
- Lua may publish and drain mailbox events, but Lua callbacks run only while the
  owner `lua_State` explicitly pumps the mailbox.
- Request/reply flows use correlation ids; `vectis_mailbox_broker` owns the
  per-request reply mailbox lifecycle for route-local waits.
- Durable work remains a liblockdc queue concern. The mailbox is the in-process
  handoff primitive and must not be presented as durable storage.

## Runtime Contract

Lua callbacks are owner-state callbacks. A Lua-created handler belongs to the
`lua_State` that registered it, and Vectis invokes it only from an explicit pump
on that state. Worker threads, liblockdc consumer callbacks, OPC UA background
callbacks, and Kore worker code must not call arbitrary Lua callbacks directly.

The safe cross-service path is:

1. A service receives work in C.
2. The service publishes a copied event into a mailbox or another C-owned shared
   resource.
3. The target service consumes that event in C, or the owning Lua state drains
   it with a bounded pump.
4. Results are returned through another mailbox, a lockd queue, an HTTP response
   path, an OPC UA response path, or another explicit receiver.

No API named as a callback bridge may hide full-message buffering or cross-thread
Lua entry. Bounded mailbox storage is allowed because it is the explicit product
semantics of the in-process handoff.

## Mailbox Semantics

A mailbox is a bounded FIFO of copied events. Each event has:

- `kind`: an optional short event name.
- `payload`: optional binary bytes owned by the dequeued event.
- `correlation_id`: zero for ordinary events, nonzero for request/reply flows.
- `expects_reply`: advisory metadata for receivers that should produce a reply.

`vectis_mailbox_publish()` copies the event into mailbox-owned storage. It fails
with `VECTIS_ERR_CONFLICT` when the queue is full, `VECTIS_ERR_INVALID` when the
event is malformed, and `VECTIS_ERR_STATE` when the mailbox is closed.

`vectis_mailbox_wait_next()` transfers ownership of the next copied event to the
caller. The caller releases it with `vectis_mailbox_event_cleanup()`. Timeouts
return `VECTIS_ERR_TIMEOUT` without modifying the output event.

`vectis_mailbox_publish_request()` assigns or preserves a correlation id, marks
the queued event as expecting a reply, and returns the correlation id to the
caller. `vectis_mailbox_reply()` publishes a non-request event with the supplied
correlation id.

`vectis_mailbox_broker` is the route-local helper over these primitives. It
borrows the worker request mailbox, creates a private one-slot reply mailbox for
each `broker->request()` call, removes that pending reply mailbox on success,
timeout, publish failure, or close, and exposes `broker->reply()` as the worker
side routing entry point. Late replies after timeout fail with
`VECTIS_ERR_TIMEOUT` instead of being delivered to a stale route wait.

`vectis_mailbox_stats_get()` returns a thread-safe snapshot with capacity,
current depth, high-water depth, publish/drain counts, full/closed/timeout
failure counts, and request/reply correlation counters. The stats surface is
diagnostic only and must not be used as an exclusive synchronization primitive.

## Performance Model

The intended performance hierarchy is:

1. C receiver to C receiver: near ordinary hand-written C integration.
2. C receiver plus mailbox handoff: one bounded copy, one mutex/condition path,
   and receiver dispatch.
3. C service to mailbox to Lua pump to C facade: one mailbox handoff plus Lua
   payload materialization and callback invocation.
4. Durable local request/reply through liblockdc: correct for durability,
   retry, and audit, but not the low-latency in-process RPC path.

High-rate telemetry and tight control loops should stay in C receivers and batch
before crossing into Lua. Lua is the coordination and application-policy layer;
it is not the shared concurrent callback runtime.

## Integration Patterns

OPC UA event to lockd enqueue:

- Fast path: OPC UA C receiver publishes directly to lockd or to a C mailbox
  consumed by a lockd worker.
- Lua path: OPC UA event publishes to a mailbox; Lua pumps, applies policy, and
  calls the `lockdc` facade.

Lockd consumer to OPC UA action:

- Fast path: the C consumer receiver calls the OPC UA C facade directly.
- Lua path: the consumer receiver publishes to the mailbox and the owner Lua
  state later performs the OPC UA operation during pump.

Kore API request to worker result:

- Low-latency local flow: route calls `vectis_mailbox_broker::request()` with a
  deadline; the worker drains the request mailbox and calls
  `vectis_mailbox_broker::reply()` with the request correlation id.
- Typed route flow: route builds a `vectis.route` JSON event through
  `vectis_route_event_from_request()` or calls
  `vectis_route_mailbox_request()` to build, dispatch, wait, and map the worker
  reply to the HTTP response.
- Durable flow: route enqueues through liblockdc, returns accepted/pending, or
  waits only when the product explicitly wants durable queue latency.

## Typed Route Adapter

The route adapter is C-side and explicit. `vectis_route_event_config` selects
which path parameters, query values, and request headers are copied into the
event. It never enumerates Kore internals and it never reads a request body
unless `include_body` is set.

When `include_body` is set, the body must already be available through
`vectis_request_body_bytes()` and must fit under `max_body_bytes`. Reader-backed
uploads, streaming upload readers, and bodies over the configured bound fail
closed instead of being secretly materialized.

The event payload is JSON with `type`, `method`, `path`, `path_params`, `query`,
`headers`, and `body` fields. `vectis_route_mailbox_request()` sends that event
through a `vectis_mailbox_broker`, maps successful worker reply bytes to the
configured success response, maps timeout to the configured timeout response,
and maps other broker failures through Vectis JSON error responses.

## Typed Lockd Consumer Adapter

The lockd consumer adapter is C-side and plugs into the existing
`vectis_consumer_receiver` registry as the built-in `mailbox` receiver kind.
`vectis_lockd_consumer_event_from_message()` projects a managed
`lc_consumer_message` into a JSON mailbox payload with delivery metadata and an
optional copied payload.

Payload copying is disabled by default. When `include_payload` is set, the
adapter copies through a bounded `lc_sink`; payloads over `max_payload_bytes`
fail during the copy and the managed consumer receives a normal liblockdc
callback error. This is materialized payload projection, not a streaming bridge.

`vectis_lockd_consumer_mailbox_receiver_config` supports two target modes:
`mailbox` for fire-and-forget delivery into a mailbox, and `broker` for
request/reply delivery. Broker mode takes precedence and returns success only
after a reply arrives before `reply_timeout_ms`; failures propagate to the
managed consumer callback so liblockdc can use its documented nack/failure
semantics.

## Scenario Coverage

`examples/concurrency/mailbox_request_reply.c` is the C contract example for the
mailbox DX layer. It covers a route-style `vectis_mailbox_broker` request/reply
handoff to a worker thread with automatic reply mailbox cleanup, plus an OPC
UA/lockd-style direct C handoff where one receiver drains a mailbox event and
publishes the next C-owned work item without entering Lua.

`examples/lua/mailbox_pipeline.lua` is the Lua contract example. It covers
publishing a request, owner-state `pump()` dispatch, correlated reply
publication, reply draining, and stats inspection without reusable helper
functions that would hide the facade shape.

## Lua Surface

The Lua facade mirrors the C mailbox:

- `vectis.mailbox.new(opts)` creates a bounded mailbox.
- `box:publish(event)` copies a Lua string payload into the mailbox.
- `box:request(event)` publishes an event with `expects_reply = true` and returns
  the correlation id.
- `box:reply(correlation_id, event)` publishes a reply event.
- `box:next(timeout_ms)` returns the next event table or `nil, error`.
- `box:pump(handler, opts)` drains up to a bounded count and calls `handler(event)`
  on the owner Lua state.
- `box:stats()` returns the core mailbox stats plus Lua-owned `pump_calls`,
  `pump_events`, and `pump_callback_failures`.
- `vectis.mailbox.broker(opts)` creates a request/reply broker over a borrowed
  request mailbox.
- `broker:request(event, opts)` publishes a correlated request, waits for a
  reply using `opts.timeout_ms`, and returns `reply_event, correlation_id`.
- `broker:reply(correlation_id, event)` routes a worker reply to the matching
  pending request.

Lua mailbox events are tables with `kind`, `payload`, `correlation_id`, and
`expects_reply`. The facade does not invoke handlers from background service
threads.

## Open Follow-Ups

- Add typed event adapters for common route, lockd, OPC UA, and WebDAV events
  when concrete scenario code proves that generic byte payloads are too noisy.
