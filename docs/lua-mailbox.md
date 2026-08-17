# Lua Mailbox

`require("vectis.mailbox")` exposes the Vectis mailbox DX layer for bounded
in-process handoff between C-owned services and Lua application code.

The mailbox is not durable storage. Use `lockdc` queues or pouch-backed storage
when work must survive process exit. Use mailboxes when services in the same
runtime domain need a bounded copied event channel or a low-latency request/reply
path.

## Ownership Model

Lua callbacks are owner-state callbacks. A mailbox created in Lua belongs to the
`lua_State` that created it, and Lua handlers run only when that same state calls
`box:pump(...)`.

Background services such as liblockdc consumers, OPC UA monitor callbacks, CAI
workers, curl workers, audio capture/playback, and SUS workers must not call Lua
callbacks directly. They can publish copied events into a `vectis_mailbox`; Lua
can then drain those events explicitly from the owning state.

## API

```lua
local mailbox = require("vectis.mailbox")

local box = assert(mailbox.new({
  capacity = 32,
  max_payload_bytes = 65536,
}))
```

`box:publish(event)` queues a copied event and returns `true`, or
`nil, err`.

`box:request(event)` queues a copied event with `expects_reply = true` and
returns the correlation id.

`box:reply(correlation_id, event)` queues a copied reply event.

`box:next(timeout_ms)` returns the next event table or `nil, err`.

`box:pump(handler, opts)` drains up to `opts.max` events and invokes
`handler(event)` on the owning Lua state. `opts.timeout_ms` controls the wait
for each event. The pump returns the number of events handled, or `nil, err` if
the handler fails or the mailbox operation fails.

`box:stats()` returns mailbox counters plus Lua pump counters:
`pump_calls`, `pump_events`, and `pump_callback_failures`.

`box:close()` closes the mailbox and wakes waiters.

Events are Lua tables:

```lua
{
  kind = "worker.opcua",
  payload = "read ns=2;s=Temperature",
  correlation_id = 0,
  expects_reply = false,
}
```

`kind` and `payload` are optional. `payload` is a Lua string and may contain
binary data. `correlation_id` is zero for ordinary events and nonzero for
request/reply flows.

## Broker

`mailbox.broker(opts)` creates a request/reply broker over a borrowed request
mailbox:

```lua
local requests = assert(mailbox.new({capacity = 8}))
local broker = assert(mailbox.broker({
  requests = requests,
  reply = {max_payload_bytes = 65536},
  max_pending = 8,
}))
```

`broker:request(event, {timeout_ms = 1000})` publishes a correlated request,
waits for a reply, and returns `reply_event, correlation_id`.

`broker:reply(correlation_id, event)` routes a worker reply to the pending
request. Late replies after timeout fail instead of being delivered to stale
route state.

`broker:close()` wakes pending request waiters. The borrowed request mailbox is
not closed by the broker.

## Runtime Lifecycle

In supervised route runtimes, ordinary Lua mailbox objects are process-local.
They are valid in the domain that created them and are not a cross-process
contract between the Kore child and the supervisor.

For route-to-worker handoff in one process, use a mailbox or broker directly.
For supervised Kore child to supervisor communication, use durable `lockdc`
queues/pouch storage today. The private Vectis runtime control bus is currently
reserved for lifecycle control frames and is not a public Lua request/reply API.

## Example

```lua
local mailbox = require("vectis.mailbox")

local box = assert(mailbox.new({capacity = 4, max_payload_bytes = 1024}))

assert(box:publish({kind = "policy.check", payload = "hello"}))

local handled = assert(box:pump(function(event)
  assert(event.kind == "policy.check")
  assert(event.payload == "hello")
end, {max = 1, timeout_ms = 0}))

assert(handled == 1)
```

The executable contract lives in `tests/lua/smoke.lua` and
`examples/lua/mailbox_pipeline.lua`.
