# Lua supervisors and workflows

`require("vectis.supervisor")` and `require("vectis.workflow")` compose the
native `vectis.app` userdata. The same tables are available as
`require("vectis").supervisor` and `.workflow`. Direct `lockdc` access remains available.
App-owned methods require the native embedded `vectis.app` runtime. The
standalone Lua source rock carries these Lua modules, but does not supply the
native app binding or embedded runtime.

## Runtime and lifecycle

Declare named scopes, handlers, and timers on the main Lua caller before app
start. One host runs all callbacks serially on that caller. Route workers inherit
producer channels but never run supervisor effects. Clients and dispatchers open
after the applicable fork boundary. `app:wait()` dispatches channel readiness
and timer deadlines; `app:pump(timeout_ms)` explicitly performs one bounded
owner dispatch pass. Lua accepts a nonnegative millisecond timeout, default zero;
it returns `true` or `nil,error`. Zero is a nonblocking probe, not a request to
wait for every durable effect to complete. Do not invoke start, close, stop, or
nested pump from a callback.

```lua
local app = assert(require("vectis").app.new({tls={mode="disabled"}}))
local effects = app:supervisor({name="effects"})
-- Declare sources here, then app:start()/app:wait(), or app:run().
```

Scopes expose `start()`, `stop(deadline_ms)`, `wait()`, `state()` and `last_error()`.
Failure diagnostics copy status/message, persist across stop and clear on a successful
restart; owned native logs include the scope. Retained handles fail after app close.
States are `declared`, `materializing`, `running`, `stopping`, `stopped`, or
`failed`. Scopes default to starting with the app; `start=false` declares a
dormant scope. Stop disables intake and closes its sources; wait succeeds once
stopped. App restart recreates runtime resources for requested scopes. App close
invalidates retained native scope handles. `failure_policy="fail_app"` is the
default; `"stop"` cleans up the failed scope while other scopes continue.
One callback cannot be preempted while inside a blocking dependency operation.
Isolated Lua lanes, module entry points, and automatic restart policies are
future work; only the serial default is delivered.

### Scope options

| Option | Default and accepted behavior |
| --- | --- |
| `name` | `"default"`; unique nonempty, NUL-free string within the app. |
| `start` | `true`; false records a dormant declaration. |
| `failure_policy` | `"fail_app"`; `"stop"` stops only the failed scope. |
| `lua_lanes` | Only omission or `1` is supported. |
| `channel.capacity` | `64` frames; integer from 1 to 4096. |
| `channel.max_frame_size` | `4096` bytes; integer from 1 to 1048576, also subject to the total inbox cap. |
| `channel.signal_kinds` | Empty; dense array of at most 64 unique nonempty, NUL-free strings. |

Unknown options are rejected. `module`/`entry` are rejected until isolated lanes
exist. Avoid names beginning `@workflow:`; workflow-owned scopes use that prefix.
`app:ipc()` resolves the lazy `default` scope; `app:ipc(name)` requires a scope
already declared with that name. Getting a channel during declaration is valid;
sending before app startup returns `nil,"closed"` because IPC is prepared at start.
Lifecycle calls return `true` or `nil,error`; `scope:wait()` is a stopped-state
check, not a background loop. `last_error()` returns nil before failure or after
a successful restart, otherwise a copied status/source/message table.

## Channels, receivers, and timers

```lua
local app = assert(require("vectis").app.new({tls={mode="disabled"}}))
local scope = app:supervisor({name="events", channel={
  capacity=16, max_frame_size=4096, signal_kinds={"repair"},
}})
scope:receiver({kind="ready", handler=function(event)
  assert(event.type == "message")
  print(event.payload)
end})
scope:receiver({kind="repair", handler=function(event)
  assert(event.type == "signal")
end})
local channel = scope:channel() -- also app:ipc("events")
scope:timer({name="maintenance", every="1h", handler=function(timer)
  -- Return false/"disable", or call timer:disable(), to stop delivery.
end})
assert(app:start())
assert(channel:send("ready", "copied bytes"))
assert(channel:signal("repair"))
assert(app:pump(0))
assert(app:stop())
app:close()
```

`send(kind, bytes[, opts])` copies a Lua string into a bounded process-shared frame
ring. It returns `true`, or `nil, "full"`, `nil, "closed"`, or an explicit
error. Contention may return full rather than blocking a producer. Signal kinds
are a finite declaration separate from frame capacity; repeated pending signals
coalesce, even when the ring is full. Unknown signal kinds are errors. Receivers
alternate pending frames with signals, using round-robin signal selection so
continuous frame production cannot starve repair.
Receivers without a kind catch all events. Neither messages nor signals are durable.
The total inbox allocation is limited to 16 MiB. Receive/start/stop never wait
for a producer-held mutex; receive preserves readiness on contention. Stop closes
ingress before joining workers; mappings survive until producer quiescence.
C callers must join their own external producer threads before app destruction
or restart. Both wake descriptors are close-on-exec; fork producers close receiving ends.
C callers use `vectis/supervisor.h`, including frame correlation/flags and the
readiness FD. Lua send options accept only `correlation_id` and `flags`, exact
uint32 integers (default zero); received events expose those copied values.
This is one-way transport metadata. Managed request/reply routing and permits
remain future work. Public C receive requires the channel's owner process and
thread. The VRC1 lifecycle protocol is not a public channel format.

`scope:receiver({kind=..., handler=...})` accepts `name` as a kind alias;
if both are given they must agree. Omitting both registers one catch-all.
The receiver returns an acceptance value or no values; `false` or `nil,error`
fails the scope according to its policy. Events expose `type`, `kind`,
`channel_id`, `correlation_id`, `flags` and copied `payload` bytes for messages.
Signals have no payload field; the kind identifies the advisory wake.

`scope:timer({name=..., every=..., handler=...})` requires a unique name and a
positive interval: integer milliseconds or a string such as `"250ms"`, `"1s"`,
`"5m"` or `"1h"`, bounded to 2147483647 milliseconds. `every_ms` is the numeric
alias; `every` takes precedence if both are supplied. The callback receives its
timer handle. Returning false/"disable" or calling `timer:disable()` disables
it; `timer:enable()` starts the next interval from the current monotonic time.
An exception follows scope failure policy. Timers do not start service threads.

## Workflow transactions and outbox handlers

```lua
local vectis = require("vectis")
local app = assert(vectis.app.new({app_name="orders-example", tls={mode="disabled"}}))
local effects = app:supervisor({name="effects"})
local workflow = app:workflow({namespace="orders"}) -- inherits app storage/key
effects:outbox({workflow=workflow, kind="mail", handler=function(job)
  assert(job:write_payload({write=function(chunk)
    -- Send each chunk to a synchronous bounded sink.
    return true
  end}))
  return job:complete({delivery_reference="accepted"})
end})
assert(app:start())
local result = assert(workflow:transaction(function(tx)
  -- State participants acquired through tx:acquire(...) commit together.
  return tx:append_outbox({
    operation_id="order-42", effect_id="receipt", effect_key="order-42-mail",
    kind="mail", destination="local", content_type="text/plain",
    payload_digest=require("openssl").sha256("hello"),
  }, "hello")
end))
assert(#result.outbox_receipts == 1)
assert(app:pump(0))
assert(app:stop())
app:close()
```

### Workflow options

| Option | Default and accepted behavior |
| --- | --- |
| `namespace` | Required nonempty, NUL-free identity; one workflow per namespace per app. |
| `client` | Captured copy of the app's `lockd` selection; an explicit table supplies the workflow selection. |
| `with_state` | `false`; true enables short-lived checkpoint access. |
| `shared_runtime` | Omitted or false; true is rejected. |
| `ipc_capacity` | `64` copied receipt-key frames; same bounds as a scope channel. |
| `ipc_max_frame_size` | `4096` bytes; same bounds as a scope channel. |
| `recovery_probe_ms` | `100`; positive integer up to 2147483647. Probes indexed candidates, not namespace discovery. |
| `recovery_interval_seconds` | `30`; explicit zero disables periodic durable discovery. Startup/explicit reconciliation still applies. |

The following fields pass to both upstream outboxes with upstream validation
and defaults; unknown fields are rejected:

- identity: `owner`;
- lease durations in seconds: `transaction_ttl_seconds`, `claim_ttl_seconds`;
- retry policy: `max_attempts`, `retry_initial_delay_seconds`,
  `retry_max_delay_seconds`, `host_retry_delay_max_seconds`;
- dispatcher limits: `notification_capacity`, `shutdown_timeout_ms`;
- dead-letter policy: `replay_dead_letters_on_startup`,
  `dead_letter_retention_seconds`, `dead_letter_max_count`, `dead_letter_max_bytes`.

`client` uses the supported workflow client fields and canonical Pouch options
in [Lua lockd](lua-lockd.md). Producer and owner clients are separate
post-fork handles; their endpoint, encryption and workflow policy describe the
same durable namespace.

### Commit from an HTTP route

This complete service accepts a plain-text operation ID and commits a `print`
effect. Run it as a Lua script, then POST the ID to `/effects`. The `202` confirms
the durable commit; effect output runs in the supervisor's owner state. It uses
the app's encrypted default storage; configure `lockd` to select another root.

```lua
local vectis = require("vectis")
local lockdc, openssl = require("lockdc"), require("openssl")
local app = assert(vectis.app.new({app_name="effects-example",
  bind="127.0.0.1", port=8080, worker_count=2, tls={mode="disabled"}}))
local workflow = app:workflow({namespace="example.effects"})
workflow:outbox({kind="print", handler=function(job)
  assert(job:write_payload({write=function(chunk)
    io.stdout:write(chunk); return true
  end}))
  io.stdout:write("\n"); io.stdout:flush()
  return job:complete()
end})
assert(app:route({path="/effects", method="POST", body="buffered",
  max_body_bytes=128, handler=function(request)
    local id = request.body
    if id == "" then return {status=400, body="operation ID required\n"} end
    local result, err = workflow:transaction(function(tx)
      return tx:append_outbox({operation_id=id, effect_id="print", effect_key=id,
        kind="print", destination="stdout", content_type="text/plain",
        payload_digest=openssl.sha256(id)}, id)
    end)
    if not result then return {status=500, body=lockdc.encode_json(err)} end
    return {status=202, content_type="application/json",
      body=lockdc.encode_json({receipts=#(result.outbox_receipts or {})})}
  end}))
assert(app:start())
print("listening on 127.0.0.1:8080"); io.stdout:flush()
assert(app:wait())
app:close()
```

Declare handlers before start. Route code only stages/commits work; it neither
pumps a dispatcher nor forwards receipt keys manually. `app:wait()` handles the
FD wake, not a sleep or recovery scan. A repeated completed effect with the same
stable identity does not run again. Effects remain at-least-once across crashes:
an external side effect can succeed before its durable completion is recorded.
Use `job.effect_key` as the external system's idempotency key.
Duplicate commits can omit `outbox_receipts`; this example reports zero new
receipts while still accepting the already committed operation.

`workflow:outbox(opts)` selects the lazy default scope unless
`opts.supervisor` is supplied. Registrations are immutable, unique by workflow
identity and kind, and must precede start. Workflow namespace identifies one
workflow per app; duplicate identity declarations are rejected. Producer and
dispatcher use copies of one captured client/outbox configuration. Omitted client
settings inherit app `lockd` settings, including bundle aliases and typed Pouch
options. Local storage defaults resolve through Vectis after start; the
`VECTIS_POUCH_CRYPTO_KEY` environment override follows app policy. Configured
outbox TTL/retry/retention options propagate to both outboxes. Endpoint arrays
must be dense and bounded; strings crossing native configuration boundaries
reject NUL and boolean options reject truthy substitutes. Unknown client-root
options fail explicitly; canonical bundle names take precedence over aliases,
matching native app configuration. Public Pouch endpoint grammar stays upstream-owned.

The transaction callback exposes `append_outbox` plus upstream state participant
operations. Commit/rollback/close belong to the workflow. An unregistered kind
or failed append poisons the transaction even if the callback catches the error;
subsequent valid appends cannot restore commit eligibility. The return value is
the actual upstream commit result. Callback `false` or `nil,error` rolls back;
implicit `nil,nil` succeeds. The transaction must still contain an upstream
committable state/outbox change. App lifecycle changes, nested pumping and
scope start/stop are rejected during transaction and private-client callbacks;
the app and its borrowed native loggers remain alive until the protected call
returns. Endpoint query strings are retained independently of mutable caller
tables for the duration of client open. Committed receipt keys automatically enter a
bounded workflow channel and wake the owner FD. Overflow schedules one separately
coalesced reconcile signal; it never changes an already successful commit.

One persistent dispatcher per workflow routes every registered kind. Startup
explicitly reconciles durable work. Direct keys are notified and pumped with
zero wait, without a recovery scan. Because upstream asynchronous recovery has
no public readiness FD, an owner timer probes existing candidates (default
100 ms, `recovery_probe_ms` override). This is separate from direct-key wakeup.
Durable discovery defaults to `recovery_interval_seconds=30`; explicit zero
opts out. A ready-candidate probe never initiates discovery.
`workflow:stats()` exposes `forwarded_keys`, `reconcile_wakes`, `closed_wakes`,
`failed_wakes`, `received_keys`, `received_reconciles`, `handled` and
`dispatcher_starts`, plus owner-side `dispatcher` stats when materialized. `workflow:reconcile()` explicitly schedules repair and returns the channel
signal result; repair completes asynchronously.
Forwarding evidence is local to the calling process; received-key and handled
counts belong to the owner. Producer counters are not aggregated across forks.

Handlers return the unique token selected by `job:complete(metadata)`,
`job:retry({diagnostic=..., delay_seconds=...})`, or
`job:dead_letter(diagnostic)`. `complete` metadata accepts upstream
`delivery_reference` and `response_digest`; retry also accepts a diagnostic string
shorthand. Terminalization occurs only after callback
return. Exceptions, absent outcomes, and returning another value become retries
through upstream pump policy. Duplicate durable effects do not run twice after
completion. Job payload and renewal methods become invalid after the handler;
`job:state()` returns nil in stateless mode. `with_state=true` supports short
checkpoint read/update/completion; its borrowed checkpoint cannot be released,
closed or used after handler return. Renewal in stateful mode is explicitly
rejected because upstream deferred completion fails its state version
precondition after successful renewal and checkpoint update. See the audit and
executable upstream-contract probe.

`job:write_payload(sink)` is genuine push streaming from upstream storage;
the facade does not buffer or spool the full payload. `job:renew(ttl_seconds)`
is explicit for stateless handlers. No initial renewal is injected.
**No `job:payload_source()` pull API or automatic renewal throughout blocking
handlers is delivered.** Those require additional public upstream facilities;
see the [implementation audit](../docs/supervisor-workflow-audit.md).
Until automatic renewal is available, keep handlers within the claim interval
and explicitly renew stateless claims between bounded operations. Stateful
handlers must finish within their initial claim interval.

Stopping a kind's scope pauses intake for its whole workflow, since the public
dispatcher cannot filter claims by kind. Other workflow identities continue.
Cross-runtime leader/configuration guards are not implemented;
`shared_runtime=true` is rejected. Do not deploy independent Vectis runtimes
claiming the same workflow as a supported leader-election arrangement.

## Queue consumers

`scope:consumer({client=..., request=..., handler=..., every_ms=100})` opens a
client after fork and performs public `dequeue_batch` with page size one and zero
wait on the owner. Explicit pumps and timer ticks probe this source. The handler
receives scoped `info()`, streaming `write_payload(sink)`, `extend(request)`,
and typed `ack()` / `nack(request)` tokens. Only the returned selected token is
applied after callback return. Exception/missing outcome requests a failure
nack. Thrown errors, `nil, error`, rejection and missing tokens record a bounded
512-byte message in `last_error_json` on the public failure nack. The returned
source exposes `stats()` with failure count and the latest copied diagnostic.
Retry is ordinary source behavior; it need not fail the whole scope. Terminal
operation errors do propagate through the scope failure policy. Handles close
after terminalization, including errors. Direct probes also reject app/scope
lifecycle reentry while private native clients are active. Upstream queue
policy controls retry/dead-letter transitions; there is no public forced
dead-letter method to wrap.

Consumer requests accept the upstream `namespace`, `queue`, `owner`, `txn_id`,
`start_after`, and `visibility_timeout_seconds` fields. `wait_seconds` must be
zero and `page_size` one when supplied. Unknown fields, including stateful
dequeue options, are rejected. Nack requests accept only `delay_seconds`,
`intent`, and `last_error_json`.

The runnable [workflow example](../examples/lua/supervisor_workflow.lua) and
normal/packed behavioral tests exercise the delivered default. The
[specification](../docs/supervisor-workflow-spec.md) also describes stronger
contracts and future lanes; their presence in the specification is not evidence
of implementation.

Disabling a timer removes its native poll deadline; enabling schedules the next
interval from the current monotonic time. Timer names are unique and immutable.
Meaningful unsupported lane/restart/health-policy/quota options are rejected;
current logical health consists of state plus copied last error, with finite
channel bounds and serial dispatch. Full quota scheduling remains an intended
specification requirement. `scope:sus_logs(opts)` is an explicit owner safe-point
adapter; see [SUS logging ownership](lua-sus.md).

Stopping and restarting an individual scope keeps queued frames and coalesced
signals in its existing channel. They are delivered after restart; they are not
silently discarded. A complete app runtime restart prepares fresh channels.
Producers must be quiesced before restarting or destroying their channel.

App-owned Pouch clients default to shared writer mode, including flat app
`lockd.endpoints` plus `pouch_crypto_key` configuration. Explicit endpoint query
settings remain under the native parser's authority; explicit exclusive mode
cannot coexist with the app's additional workflow/consumer clients and fails
startup. An explicit typed `pouch.single_writer=true` is rejected. Selected typed
settings retain upstream precedence over query settings for that client.
Workflow transactions and scoped client construction protect the entire native
call: app close/stop/pump and both public and native scope lifecycle operations
are rejected until it returns, including rollback and callback-error paths.
