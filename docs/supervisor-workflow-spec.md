# Supervisor and Workflow Dispatch Specification

Status: implementation authority.

This specification extends the established Vectis T2 supervisor runtime into a
reusable background-execution model. It also defines the narrow liblockdc
additions needed for immediate, durable workflow dispatch and a safe
dependency-native Lua experience.

It supersedes the future-facing parts of the current statement that the T2
control channel is permanently private. Existing lifecycle safety rules remain
in force: Kore is forked from a thread-clean process; supervisor services
materialize only after that fork boundary; and no foreign thread enters an
arbitrary Lua state.

## Goals

- Keep one supervisor host process per Vectis application runtime by default.
- Allow any number of independently named logical supervisors within that host.
- Make an outbox handler one simple Lua declaration, with a lazily created
  default supervisor when none is selected.
- Make supervisor execution usable for more than transactional outboxes:
  Lockd consumers, timers, native event receivers, reconciliation work, CAI,
  polling integrations, and future service adapters.
- Provide bounded, explicit, process-safe IPC for low-latency local handoff.
- Preserve Lockd/Pouch as the authority for durable work. IPC never becomes a
  second queue, a persistence format, or a replacement retry mechanism.
- Permit applications that use only liblockdc to run correct Lua workflow
  dispatchers without requiring Vectis.
- Support future concurrency without sharing one Lua state across threads.

## Non-goals

- Do not start one operating-system process for every registered source or
  handler.
- Do not turn the supervisor into an outbox-specific abstraction.
- Do not call a Lua closure from a liblockdc, Kore, or dependency worker thread.
- Do not transfer Lua closures, userdata, request pointers, or borrowed native
  handles through IPC.
- Do not claim unbounded work merely because it is ready.
- Do not make application request latency depend on effect execution, retry
  backoff, workflow reconciliation, or a remote wake request.

## Terms

### Supervisor host

The one Vectis supervisor-domain process in a T2 runtime. It already exists to
own app background services after the Kore child fork. In a T3 service-only
runtime it is the current process. A host owns a process-local Lua state and
may own service threads, native clients, and a public bounded IPC router.

### Logical supervisor

An independently named lifecycle scope inside a supervisor host. It owns source
registrations, limits, failure policy, a message endpoint, and an optional Lua
execution lane. Creating multiple logical supervisors does not create multiple
processes by default.

### Source adapter

A typed producer of work for a logical supervisor. Examples are a liblockdc
outbox, a Lockd queue consumer, a timer, a C receiver, a file/watch adapter, or
a future CAI integration. The adapter defines its acknowledgement and retry
semantics; the supervisor does not flatten them into one generic acknowledgement.

### Lua lane

One Lua state and its sole owner thread. A lane executes callbacks serially.
The default supervisor lane is the declaration/supervisor Lua state retained by
the parent after the Kore child fork. An isolated lane is explicitly configured,
loads module entry points, and never receives an arbitrary closure from another
Lua state.

## Existing Lua ownership model

Vectis does not create one Lua VM per route or per request.

1. The Lua application is evaluated once in the declaration process.
2. In T2, Vectis forks the Kore child while the process is thread-clean. The
   child receives a copy of that Lua state for routes; the parent retains its
   own supervisor-state copy.
3. Kore worker processes inherit independent copies of the child route state.
   All routes in one worker share that worker's Lua VM; distinct workers do not
   share globals, userdata, or mutable module state.
4. The supervisor process owns a distinct Lua state. It may execute registered
   supervisor closures on its owner thread, but it may not access a Kore
   worker's Lua state.

Consequently a normal supervisor declaration may retain an inline closure. It
is retained in the supervisor's original Lua state before fork and invoked only
there. An isolated or parallel lane cannot receive that closure; it uses an
explicit module and entry name instead.

## Vectis architecture

### One host, many logical supervisors

The application runtime owns one `vectis_supervisor_host` for the default
topology. Its identity is the app runtime, not an outbox namespace. The host
contains zero or more `vectis_supervisor` objects:

```text
Vectis app runtime
  └─ one supervisor host process
       ├─ default logical supervisor
       │    ├─ orders outbox
       │    └─ cache invalidation receiver
       ├─ integrations logical supervisor
       │    ├─ partner queue consumer
       │    └─ OPC UA event receiver
       └─ maintenance logical supervisor
            └─ timer and reconciliation jobs
```

Logical supervisors have independent names, source registrations, quotas,
health, failure policy, start/stop state, and message endpoints. They share the
host process and its lifecycle monitor. The default policy is one host process
per app because it is cheaper, easier to observe, and avoids needless process
trees.

Future policy may request an isolated supervisor host process for a selected
logical supervisor. That is an explicit isolation decision, not the default and
not an API redesign.

### C API

The public C surface uses opaque handles, receiver shells, and free-function
equivalents. A receiver shell is useful to embedders that already use the
Vectis receiver style; free functions remain the straightforward C API.

```c
typedef struct vectis_supervisor_host vectis_supervisor_host;
typedef struct vectis_supervisor vectis_supervisor;
typedef struct vectis_supervisor_channel vectis_supervisor_channel;

void vectis_supervisor_config_init(vectis_supervisor_config *config);
vectis_status vectis_supervisor_new(vectis_app *app,
                                    const vectis_supervisor_config *config,
                                    vectis_supervisor **out,
                                    vectis_error *error);
vectis_status vectis_supervisor_start(vectis_supervisor *self,
                                      vectis_error *error);
vectis_status vectis_supervisor_stop(vectis_supervisor *self,
                                     long deadline_ms,
                                     vectis_error *error);
vectis_status vectis_supervisor_wait(vectis_supervisor *self,
                                     long deadline_ms,
                                     vectis_error *error);
void vectis_supervisor_close(vectis_supervisor *self);
```

`new()` declares the logical supervisor while the app is declaring. For a
route-backed app, `start()` before `app->start()` records the requested start;
it does not materialize threads or clients before the fork boundary. Runtime
startup materializes the supervisor after Kore readiness. A running supervisor
may start another already-declared logical supervisor in the host domain.

The receiver shell provides `start`, `stop`, `wait`, `state`, `channel`, and
typed source-registration operations with the same contract. Its public method
table reserves eight `void *` extension slots. Every operation is also available
as a free function so callers never need to depend on vtable layout.

Each logical supervisor has the lifecycle:

```text
declared → materializing → running → stopping → stopped
                    └──────────────────────→ failed
```

Failure policy is explicit: fail the app, stop only this logical supervisor, or
restart it with bounded backoff. A failure never leaves a source silently
claiming durable work.

### Source adapters and outcomes

The supervisor owns scheduling, limits, logging, lane selection, and failure
observation. It does not redefine a source's delivery contract.

| Adapter | Input | Handler terminal outcome |
| --- | --- | --- |
| workflow outbox | claimed `lc_outbox_job` | complete, retry, dead-letter |
| Lockd queue | claimed queue message | ack, nack/retry, dead-letter |
| timer | scheduled tick | success, retry/reschedule, disable |
| IPC receiver | copied bounded message | accept, reply, reject, drop |
| native receiver/watch | copied typed event | adapter-defined success/failure |

All adapters have bounded in-flight capacity. They must not claim or copy more
work than their selected lane can accept. A Lua lane defaults to one in-flight
handler. A C-only adapter may use its own explicitly configured worker count.

### Public IPC

The supervisor host exposes a public application IPC facility. It is not the
existing private lifecycle control bus; the existing control frames remain
reserved and are never visible to application handlers.

Channels are declared before fork and assigned an immutable numeric id. Their
endpoints are inherited by the Kore child and supervisor host. A channel has a
fixed maximum frame size and bounded message capacity. A frame contains only:

```text
channel id, kind, correlation id, flags, copied byte payload
```

The initial API has two deliberately different paths:

- `send()`: reliable only within its bounded capacity; it returns an explicit
  full/closed/error status and never blocks an HTTP route indefinitely.
- `signal()`: a coalescing advisory wake. It may collapse repeated signals and
  is appropriate only when durable state or another authority can repair a lost
  wake.

Optional request/reply is correlation-id based. It has an explicit deadline and
removes its reply slot on success, timeout, send failure, or close. IPC payloads
are copied bytes. Lua table convenience helpers may encode/decode JSON, but
their materialization is explicit. There are no implicit pointers, closures,
userdata, or streaming claims across the process boundary.

The normal outbox fast path is therefore:

```text
route worker commits transaction + outbox effect
  → nonblocking signal containing durable outbox key
  → one supervisor process accepts the signal
  → liblockdc workflow receives direct-key notification and claims the job
  → supervisor Lua lane runs the handler
```

The route responds after the transaction commit; it never waits for any later
arrow. A failed or full signal changes latency only. The durable workflow's
startup and overflow reconciliation repair correctness.

### Lua surface

`app:supervisor(opts)` declares a logical supervisor and returns a scope. The
scope does not create a process at declaration time.

```lua
local delivery = app:supervisor({ name = "delivery" })
local maintenance = app:supervisor({ name = "maintenance" })

delivery:outbox({
  workflow = orders,
  kind = "order.webhook",
  handler = function(job)
    local result, err = vectis.http.post({
      url = job.destination,
      headers = { ["idempotency-key"] = job.effect_key },
      body_source = job:payload_source(),
    })
    if not result then
      return job:retry(err.message)
    end
    if result.status >= 500 then
      return job:retry("remote 5xx")
    end
    if result.status >= 400 then
      return job:dead_letter("remote rejected payload")
    end
    return job:complete()
  end,
})

maintenance:timer({
  name = "prune-expired-orders",
  every = "1h",
  handler = function()
    return orders:prune_expired()
  end,
})

local reindex = maintenance:receiver({
  name = "reindex",
  handler = function(event)
    return rebuild_index(event.payload)
  end,
})
```

The high-level workflow surface is intentionally simpler than raw lockdc:

```lua
local orders = app:workflow({
  namespace = "myapp.orders",
  max_attempts = 12,
})

-- No supervisor argument: attach to the lazily declared default supervisor.
orders:outbox({
  kind = "order.webhook",
  handler = deliver_webhook,
})

orders:transaction(function(tx)
  tx:update_json(order)
  tx:append_outbox({
    operation_id = "order/" .. order.id,
    effect_id = "webhook",
    effect_key = "order-webhook-" .. order.id,
    payload_digest = payload_digest,
    kind = "order.webhook",
    destination = webhook_url,
    content_type = "application/json",
  }, payload_source)
end)
```

`job:complete()`, `job:retry(...)`, and `job:dead_letter(...)` construct typed
outcomes. They do not directly perform the terminal workflow mutation; Vectis
does that after the handler returns. A handler exception, cancellation, or
deadline becomes a retry unless the adapter's policy says otherwise.

`job:payload_source()` is a handler-scoped streaming source. A convenience
`job:json()` may exist for intentionally materialized small payloads, but it
must document its memory limit and must not be the only payload API.

The default supervisor lane serializes inline closures. Future `lua_lanes > 1`
requires module/entry declarations because arbitrary closures cannot move into
independent Lua states:

```lua
app:supervisor({
  name = "parallel-import",
  lua_lanes = 4,
  module = "myapp.import_worker",
  entry = "handle",
})
```

This creates isolated lanes in the same supervisor host process. It is not an
implicit license to run one Lua state concurrently.

### Workflow dispatch and one local dispatcher

For one app runtime and one workflow namespace, Vectis runs one persistent
workflow dispatcher in the supervisor host, not one dispatcher per Kore route
worker. Route workers use their own post-fork producer clients and commit
transactions concurrently. The supervisor owns a separate post-fork client and
one `lc_workflow` handle.

An application declares that workflow once. Vectis derives the route-side
producer configuration and supervisor-side dispatcher configuration from that
single declaration. They must name the same Lockd root, namespace, outbox
identity, validation policy, and retry policy; a route must not independently
construct a superficially similar workflow configuration. Separate post-fork
clients are an isolation and ownership choice, not separate workflow domains.

The dispatcher claims only up to its lane capacity. It renews a claim while a
long foreign operation is in progress. A successful effect completes the job;
a transient failure returns it to `retry_wait`; a permanent, diagnosed failure
dead-letters it. If the process stops before a terminal action, the claim
expires and liblockdc recovery makes the immutable job eligible again. Effects
are at-least-once and must send the stable `effect_key` to the foreign system as
an idempotency key.

For multiple Vectis runtimes that deliberately share one app/outbox identity,
the active dispatcher is guarded by a Lockd leader lease. A replacement gains
leadership only after the prior lease expires or is released. This is an
availability optimization and does not weaken per-job claims or idempotency.

## liblockdc requirements

liblockdc already owns the durable parts: workflow transactions, immutable
outbox records and payloads, claims, retries, dead letters, a private dispatcher
thread, and recovery. It must remain the sole authority for these semantics.

### Separate workflow producers from dispatchers

The current `lc_client_new_workflow()` contract creates a private dispatcher
thread. That is correct for a long-lived worker, but it is wrong for an HTTP
route worker that only needs to atomically mutate domain state and append an
outbox effect. A route must not start, retain, or repeatedly create a dispatcher
thread merely to produce work.

liblockdc must therefore expose a threadless transactional producer, named here
for discussion:

```c
typedef struct lc_workflow_producer lc_workflow_producer;

int lc_client_new_workflow_producer(lc_client *client,
                                    const lc_workflow_config *config,
                                    lc_workflow_producer **out,
                                    lc_error *error);
```

The producer owns the workflow transaction and append/receipt operations,
including `begin`, participant acquisition, `append_outbox`, inbox/command
acceptance, commit, and rollback. It does not own a dispatcher thread, claim
jobs, expose `next()`, or run recovery. Its configuration shares namespace,
transaction, validation, and retry policy types with `lc_workflow` where those
values are meaningful.

The producer has an explicit close operation and may be retained for the
lifetime of its owning request-worker client. Destroying it rolls back any
uncommitted transaction; no producer action may invoke an effect handler or
foreign side effect. A receipt is returned only after the transaction commits:
failed or rolled-back transactions expose no wakeable `outbox_key`.

The existing `lc_client_new_workflow()` remains the thread-owning persistent
dispatcher constructor. A producer receipt contains the committed `outbox_key`;
the host may use that receipt for its optional local wake path only after commit
succeeds. This separation is required for Vectis Kore routes and equally useful
to any other web host using liblockdc directly.

### Direct-key workflow notification

The required core addition is a public direct-key injection operation, named
here for discussion:

```c
int lc_workflow_notify_outbox_key(lc_workflow *workflow,
                                  const char *outbox_key,
                                  lc_error *error);
```

It accepts a known committed `lc_outbox_receipt.outbox_key`, deduplicates it in
the workflow's existing bounded direct-notification queue, and wakes the private
dispatcher. The dispatcher performs a targeted claim/read path for that key; it
does not run a namespace scan merely because a Vectis route committed an effect.

The function is safe to call only in the process that owns `workflow`. Vectis
delivers the key to that process through its local IPC signal. The operation
does not execute a foreign effect and does not call user code.

Capacity overflow is visible through `lc_workflow_stats`, increments a durable
repair counter, and schedules indexed reconciliation. It never makes a committed
effect disappear. Startup reconciliation and claim-expiry recovery remain
mandatory.

### Cross-host notification

Process-local IPC is the normal low-latency path for one Vectis runtime. It
cannot wake a supervisor on another host. For remote Lockd and intentionally
distributed dispatchers, liblockdc should add a server-backed workflow change
subscription or long-poll notification mechanism. Pouch should use an efficient
cross-process root notification facility where available. Both mechanisms only
say "reconcile this durable namespace/key now"; correctness continues to depend
on durable reconciliation after loss, overflow, restart, or partition.

No route should synchronously call that remote notification mechanism after a
transaction commits. It is a supervisor-side latency optimization.

### Dependency-native Lua dispatcher

liblockdc's Lua module needs a safe workflow dispatcher facade, but it must not
invent Vectis's generic service process, IPC router, or multi-source scope.
liblockdc is the correct owner only of workflow dispatch.

The Lua module mirrors the producer/dispatcher separation:

```lua
-- Request or route domain: threadless transactional production only.
local producer = assert(client:new_workflow_producer({
  namespace_name = "myapp.orders",
}))

producer:transaction(function(tx)
  tx:append_outbox(entry, payload_source)
end)

-- Dedicated worker process: persistent dispatcher and Lua effect handlers.
local workflow = assert(client:new_workflow({
  namespace_name = "myapp.orders",
}))
local dispatcher = workflow:dispatcher({ handlers = handlers })
assert(dispatcher:run())
```

`new_workflow_producer()` is safe for a host to use synchronously in a route
when the host otherwise permits its Lockd client operation. It never creates
liblockdc dispatcher threads. `new_workflow()` and `workflow:dispatcher()` are
long-lived worker constructs, not request constructs.

The producer facade supplies transactional operations and close/garbage-
collection cleanup only. It deliberately accepts no `handlers` table and has no
`run`, `pump`, `next`, claim, retry, or terminal-job methods. Conversely, the
dispatcher facade consumes jobs but does not expose route-side transaction
production as a shortcut. The two APIs make ownership and latency boundaries
obvious in Lua as well as C.

The direct Lua API wraps a persistent workflow with an owner-state dispatch
loop:

```lua
-- worker.lua: run as a dedicated process under the application's service manager
local lockdc = require("lockdc")

local client = assert(lockdc.open({ url = os.getenv("LOCKD_URL") }))
local workflow = assert(client:new_workflow({
  namespace_name = "myapp.orders",
  max_attempts = 12,
}))

local dispatcher = workflow:dispatcher({
  handlers = {
    ["order.webhook"] = function(job)
      local ok, err = deliver(job)
      if not ok then
        return job:retry({ diagnostic = err })
      end
      return job:complete()
    end,
  },
})

assert(dispatcher:run()) -- blocking; owns this Lua state and handles retries
```

The facade also offers `start()`, bounded `pump(opts)`, `stop()`, `wait()`, and
`stats()`. `pump()` integrates with a host that already owns an event loop;
`run()` is the simplest correct choice for a dedicated worker process. It calls
Lua only on the calling owner state. It must never invoke a handler on
liblockdc's private dispatcher thread.

#### Lua host-placement contract

This distinction must be explicit in liblockdc documentation and examples:

- `dispatcher:run()` is a blocking service loop. It belongs in a dedicated
  worker process or an equivalent service-only owner-state runtime, never in an
  HTTP route handler.
- `dispatcher:pump()` runs handler code synchronously on the calling Lua state.
  A zero-timeout call is Lua-thread-safe inside a route callback, but it is not
  an acceptable route-dispatch architecture: it performs foreign effects on
  request latency and still requires the thread-owning workflow object.
- liblockdc does not know whether a generic Lua host is Kore, Vectis, nginx, or
  a custom event loop. It must not attempt to infer host topology or enter a
  Lua closure from its dispatcher thread.
- Vectis owns the host-specific policy: its `app:workflow()` route producer
  facade uses `new_workflow_producer()`, while `app:supervisor()` owns the sole
  persistent `new_workflow()` dispatcher and its handler closure.

The direct `require("lockdc")` module remains intentionally available in a
Vectis script. Its long-lived dispatcher use inside a route is documented as
unsupported; Vectis's owned workflow facade is the supported route path. The
dependency-native module must not acquire a Vectis dependency merely to police
that host rule.

The facade keeps raw `workflow:next()` and raw job terminal operations available
for advanced users. Its value is correct lifecycle, handler outcome mapping,
claim renewal, error-to-retry policy, and safe Lua ownership without a manual
`while next()` loop.

When an application uses only liblockdc, the ordinary deployment is one web
producer process plus one dedicated Lua worker process running `dispatcher:run`.
That remains simple, process-safe, and durable. If a host wants several worker
processes, Lockd claims safely distribute jobs; it is not a promise of one
global dispatcher. One global dispatcher across hosts requires host election or
the optional distributed notification path above.

## Required verification

### Vectis

- Multiple Kore workers concurrently append effects to one workflow; one
  supervisor claims and dispatches each effect exactly once while live.
- The request path performs no effect I/O and does not wait for dispatcher
  scheduling, handler execution, or reconciliation.
- A route-to-supervisor signal reaches a ready handler without periodic timer
  polling; signal overflow, supervisor death, and restart recover all effects.
- Inline Lua closures execute only in the supervisor Lua state; no worker or
  native background thread enters that state.
- Two logical supervisors run independently in one host process, including
  independent stop/failure policies and channel capacity handling.
- Isolated Lua lanes reject closure registration and accept only module/entry
  declarations; no one state is entered concurrently.
- IPC rejects oversized frames, reports full/closed endpoints, cleans timed-out
  replies, and cannot carry borrowed pointers.
- Graceful shutdown stops ingress, stops claims, completes or releases active
  work according to deadline, joins services, and closes every client after the
  fork-safe lifecycle order.

### liblockdc

- `notify_outbox_key` dispatches a committed key without a namespace scan.
- Duplicate notifications are coalesced; bounded overflow repairs through
  indexed reconciliation.
- A failed handler retries with the stable job identity and configured policy;
  terminal-operation failures retain the active claim until retry or expiry.
- Lua `dispatcher:run()` never enters a Lua state from the private dispatcher
  thread and handles stop, handler exception, renewal, retry, and dead-letter
  paths deterministically.
- Multiple independent Lua worker processes safely compete for one namespace.

## Implementation order

1. Add and test liblockdc direct-key notification plus the dependency-native
   Lua workflow producer/dispatcher facades.
2. Promote a narrowly scoped, declared public IPC channel above Vectis's private
   lifecycle control bus without exposing control frames.
3. Add the generic Vectis supervisor host/logical-supervisor C lifecycle and
   receiver shell.
4. Add the default supervisor Lua facade and generic timer, receiver, and
   consumer adapters.
5. Add the workflow/outbox Lua convenience surface and replace the periodic
   auth-email workflow open/drain/close implementation with one persistent
   supervisor-owned workflow.
6. Migrate existing C-owned worker descriptors onto supervisor scopes without
   changing their native semantics.
7. Add isolated Lua lanes only after the serial default, lifecycle, and IPC
   invariants are proven end to end.
