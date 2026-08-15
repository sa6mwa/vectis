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
- Request/reply flows use correlation ids and, when needed, a per-request or
  route-owned reply mailbox.
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
correlation id. Higher-level APIs may wrap these primitives for route-local
request/reply flows.

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

- Low-latency local flow: route publishes a correlated mailbox request and waits
  on a route-owned or app-owned reply mailbox with a timeout.
- Durable flow: route enqueues through liblockdc, returns accepted/pending, or
  waits only when the product explicitly wants durable queue latency.

## Scenario Coverage

`examples/concurrency/mailbox_request_reply.c` is the C contract example for the
mailbox DX layer. It covers a route-style correlated request/reply handoff to a
worker thread, plus an OPC UA/lockd-style direct C handoff where one receiver
drains a mailbox event and publishes the next C-owned work item without entering
Lua.

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

Lua mailbox events are tables with `kind`, `payload`, `correlation_id`, and
`expects_reply`. The facade does not invoke handlers from background service
threads.

## Open Follow-Ups

- Add typed event adapters for common route, lockd, OPC UA, and WebDAV events
  when concrete scenario code proves that generic byte payloads are too noisy.
- Add route-local helper APIs for request/reply with deadline and automatic reply
  mailbox cleanup.
