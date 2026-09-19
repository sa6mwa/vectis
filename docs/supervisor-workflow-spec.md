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
vectis_status vectis_app_supervisor_host(vectis_app *app,
                                         vectis_supervisor_host **out,
                                         vectis_error *error);
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
vectis_status vectis_supervisor_channel_get(vectis_supervisor *self,
                                            vectis_supervisor_channel **out,
                                            vectis_error *error);
vectis_status vectis_supervisor_channel_send(
    vectis_supervisor_channel *channel,
    uint32_t kind,
    const void *payload,
    size_t payload_size,
    vectis_error *error);
vectis_status vectis_supervisor_channel_signal(
    vectis_supervisor_channel *channel,
    uint32_t kind,
    vectis_error *error);
void vectis_supervisor_close(vectis_supervisor *self);
```

`new()` declares the logical supervisor while the app is declaring. For a
route-backed app, `start()` before `app->start()` records the requested start;
it does not materialize threads or clients before the fork boundary. Runtime
startup materializes the supervisor after Kore readiness. A running supervisor
may start another already-declared logical supervisor in the host domain.

`vectis_app_supervisor_host()` returns the app-owned host and never transfers
ownership. Each logical supervisor has one declared inbox channel;
`vectis_supervisor_channel_get()` returns that borrowed endpoint. Its channel
configuration, including frame capacity and allowed signal kinds, belongs to the
supervisor declaration and cannot change after the fork boundary. The C config
uses immutable `uint32_t` signal-kind values; omission declares no application
signal kinds.

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
fixed maximum frame size, bounded message capacity, and a finite declared set
of signal kinds. A frame contains only:

```text
channel id, kind, correlation id, flags, copied byte payload
```

Channel descriptors are created close-on-exec and are inherited only across the
intentional T2 fork. Each post-fork domain closes endpoints it does not own; no
spawned helper, application `exec`, or unrelated child receives an IPC endpoint.

The initial API has two deliberately different paths:

- `send()`: reliable only within its bounded capacity; it returns an explicit
  full/closed/error status and never blocks an HTTP route indefinitely.
- `signal()`: a coalescing advisory wake. It may collapse repeated signals and
  carries no authoritative per-event payload. One pending wake is retained per
  `(channel, kind)` without consuming the channel's data-frame capacity; it is
  appropriate only when durable state or another authority can repair a lost
  wake. Undeclared signal kinds are rejected, keeping pending signal state
  bounded.

A later request/reply extension must be correlation-id based and have its own
bounded pending-reply capacity. The runtime must allocate opaque correlation ids
and reject a new request when that capacity is full. It must remove a reply slot
on success, timeout, send failure, or close; a late or unknown reply is dropped.
IPC payloads are copied bytes. Lua table convenience helpers may encode/decode
JSON, but their materialization is explicit. There are no implicit pointers,
closures, userdata, or streaming claims across the process boundary.

The normal outbox fast path is therefore:

```text
route worker commits transaction + outbox effect
  → nonblocking channel send containing durable outbox key
  → or, if full, coalesced signal(OUTBOX_RECONCILE)
  → one supervisor process receives the key or reconcile wake
  → liblockdc workflow receives direct-key notification and claims the job
  → supervisor Lua lane runs the handler
```

The route responds after the transaction commit; it never waits for any later
arrow. A full send changes neither the committed result nor the response
outcome: the route emits the payload-free `OUTBOX_RECONCILE` signal, and the
supervisor schedules indexed reconciliation. A closed send or signal is
repaired by durable startup recovery. Generic coalesced signals are not used
for per-outbox-key delivery.

### Lua surface

`app:supervisor(opts)` declares a logical supervisor and returns a scope. The
scope does not create a process at declaration time.

`scope:channel()` returns its declared bounded inbox channel.
`channel:send(kind, bytes)` and `channel:signal(kind)` are nonblocking and
return `true` or `nil, status`, including `"full"` and `"closed"`. `send()`
accepts bytes only; a `send_json()` convenience may encode explicitly, subject
to the same frame bound. `signal()` accepts no payload and only declared signal
kinds. Lua declares signal-kind labels as strings; Vectis maps those labels to
immutable numeric ids before fork. Duplicate labels or C numeric values are
rejected during declaration.

```lua
local delivery = app:supervisor({ name = "delivery" })
local maintenance = app:supervisor({
  name = "maintenance",
  channel = { signal_kinds = { "reindex" } },
})

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

One dispatcher owns exactly one handler registration for each `(workflow,
kind)` pair. Registering the same pair twice, including through different
logical supervisors, fails at declaration time; different kinds from one
workflow may use different logical supervisors.

The one native dispatcher is a source router, not a Lua lane. It maps each
claimed job's registered `kind` to its logical supervisor and enqueues it only
when that supervisor's selected lane has capacity. Vectis's high-level
`tx:append_outbox()` rejects an unregistered kind before commit, so it cannot
create an orphaned Vectis job with no terminal-outcome owner; that rejection
marks the enclosing transaction failed, making commit impossible and rolling
back all of its staged state. Raw liblockdc remains available for deliberately
decoupled producers that need another unhandled-job policy.

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
worker. Route workers use their own post-fork workflow clients for transaction
production and commit transactions concurrently. The supervisor owns a
separate post-fork client and one workflow dispatcher.

Each declared `app:workflow()` also owns one host-level worker-to-dispatcher key
channel. It is created before fork from the same bounded IPC primitive, but it
is not a logical supervisor's public inbox: it carries only committed outbox
keys or the payload-free `OUTBOX_RECONCILE` wake, its sole declared signal kind.
This lets one native dispatcher route different kinds to several logical
supervisors without coupling the fast path to any one of their channels.

After a successful high-level transaction commit, Vectis sends every committed
outbox receipt key to that workflow channel. On the first full send it emits one
`OUTBOX_RECONCILE` wake instead of attempting to queue more keys; durable
reconciliation covers the whole committed set. Application route code never
manually sends workflow receipt keys.

An application declares that workflow once. Vectis derives both route-side
workflow handles and the supervisor-side dispatcher from that one canonical
workflow configuration. They must name the same Lockd root, namespace, owner,
claim and recovery policy, notification capacity, and retry policy; a route must
not independently construct a superficially similar workflow configuration.
Separate post-fork clients are an isolation and ownership choice, not separate
workflow domains.

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
Every such runtime must use the same canonical workflow configuration;
configuration mismatch fails before leader election rather than allowing two
processes to apply different dispatch policy to one durable namespace.

During shutdown, the dispatcher first stops accepting new IPC wakes and claims,
then either reaches a terminal outcome for each active job within the shared
shutdown deadline or releases it for liblockdc claim-expiry recovery. A route
transaction admitted before Kore ingress closes remains a successful durable
commit even if its wake is no longer accepted; it is reconciled after restart.
Shutdown never reports such a committed effect as failed merely because it was
not dispatched before the deadline.

## liblockdc requirements

liblockdc already owns the durable parts: workflow transactions, immutable
outbox records and payloads, claims, retries, dead letters, a private dispatcher
thread, and recovery. It must remain the sole authority for these semantics.

### Decouple workflow construction from dispatchers

`lc_client_new_workflow()` must construct a logical, threadless workflow. It
owns workflow transaction and append/receipt operations, including `begin`,
participant acquisition, `append_outbox`, inbox/command acceptance, commit,
and rollback. It does not start a dispatcher thread, claim jobs, execute
handlers, or run recovery. A route can therefore use an ordinary workflow
handle to atomically mutate domain state and append an outbox effect without
starting, retaining, or repeatedly creating a dispatcher thread.

This is a clean pre-1.0 semantic cutover: liblockdc does not retain an implicit
thread-owning workflow mode or a parallel legacy constructor. Existing hosts
that need consumption explicitly acquire a dispatcher.

Dispatcher acquisition is a separate operation, named here for discussion:

```c
typedef struct lc_workflow_dispatcher lc_workflow_dispatcher;

int lc_client_new_workflow(lc_client *client,
                           const lc_workflow_config *config,
                           lc_workflow **out,
                           lc_error *error);

int lc_client_new_workflow_with_dispatcher(
    lc_client *client,
    const lc_workflow_config *config,
    lc_workflow_dispatcher *dispatcher,
    lc_workflow **out,
    lc_error *error);

int lc_workflow_dispatcher_get_or_start(
    lc_workflow *workflow,
    lc_workflow_dispatcher **out,
    lc_error *error);

int lc_workflow_dispatcher_next(lc_workflow_dispatcher *dispatcher,
                                long timeout_ms,
                                lc_outbox_job **out,
                                lc_error *error);
int lc_workflow_dispatcher_stop(lc_workflow_dispatcher *dispatcher,
                                long deadline_ms,
                                lc_error *error);
int lc_workflow_dispatcher_wait(lc_workflow_dispatcher *dispatcher,
                                long deadline_ms,
                                lc_error *error);
int lc_workflow_dispatcher_get_stats(
    lc_workflow_dispatcher *dispatcher,
    lc_workflow_stats *out,
    lc_error *error);
int lc_workflow_dispatcher_reconcile(lc_workflow_dispatcher *dispatcher,
                                     lc_error *error);
int lc_workflow_dispatcher_replay_dead_letter(
    lc_workflow_dispatcher *dispatcher,
    const char *outbox_key,
    lc_error *error);
void lc_workflow_dispatcher_close(lc_workflow_dispatcher *dispatcher);
```

`get_or_start()` resolves the process-local dispatcher registry for the calling
client. It returns an already-running compatible dispatcher, or creates and
starts exactly one lazily. Compatibility requires the same client/root and the
same canonical `lc_workflow_config`, including its namespace, owner, claim and
recovery policy, notification capacity, timeout, and retry policy. A mismatch
fails explicitly; it must not silently attach a workflow to a dispatcher with
different durable semantics. Different `lc_client` instances do not share a
dispatcher because they may have different connection, identity, or ownership
properties.

Workflow construction validates, canonicalizes, and retains this configuration
snapshot; registry comparison never depends on caller-owned config pointers or
string storage after `new_workflow()` returns.

Acquisition is linearizable with dispatcher stop: concurrent compatible
acquirers receive the same live dispatcher, while an acquirer racing shutdown
receives a closed/busy result until registry cleanup permits one replacement.
It must never transiently create two dispatchers for one client and canonical
workflow configuration.

A stopped or failed dispatcher is never returned as running. After its shutdown
and registry cleanup complete, a later acquisition creates a replacement; it
does not revive a partially stopped dispatcher or reuse its old handler sink.

An existing compatible dispatcher may be supplied as an optional workflow-open
attachment. That is a convenience for a worker that already owns the dispatcher;
it is not required for transactional production. Otherwise a workflow resolves
dispatch only when its caller explicitly acquires a dispatcher. The Lua form is:

```lua
local workflow = client:new_workflow(config, { dispatcher = dispatcher })
```

Attachment requires the same client and compatible workflow identity.
Acquisition and attachment retain the dispatcher. Closing a workflow releases
only its attachment; it must not stop a shared dispatcher. A dispatcher has its
own explicit stop/close lifecycle and remains alive while acquired, until its
owning client closes, or until its host stops it. Client close stops registered
dispatchers before releasing its registry; outstanding workflow and dispatcher
handles become closed handles and never retain a usable client pointer.

Acquisition may start the dispatcher's private notification and recovery
infrastructure, but it must not claim or deliver a user effect until a single
consumer sink is ready. In Lua, `dispatcher:run()` or `dispatcher:pump()` binds
that owner-state sink and makes the dispatcher claim-ready. A second concurrent
sink, a different Lua owner state, or an incompatible handler registration fails
explicitly. Direct-key notifications received before readiness remain bounded
wakes; durable reconciliation remains responsible for repair.

Destroying a workflow rolls back any uncommitted transaction; no transaction or
append operation may invoke an effect handler or foreign side effect. A receipt
contains the committed `outbox_key` only after the transaction commits; failed
or rolled-back transactions expose no wakeable key. This separation is required
for Vectis Kore routes and equally useful to any other web host using liblockdc
directly.

### Direct-key workflow notification

The required core addition is a public direct-key injection operation, named
here for discussion:

```c
int lc_workflow_dispatcher_notify_outbox_key(
    lc_workflow_dispatcher *dispatcher,
    const char *outbox_key,
    lc_error *error);
```

It accepts a known committed `lc_outbox_receipt.outbox_key`, deduplicates it in
the dispatcher's existing bounded direct-notification queue, and wakes the
private dispatcher. The dispatcher performs a targeted claim/read path for that
key; it does not run a namespace scan merely because a Vectis route committed
an effect.

The function is safe to call only in the process that owns `dispatcher`.
Vectis delivers the key to that process through its local IPC signal. The
operation does not execute a foreign effect and does not call user code.

Dispatcher direct-notification capacity overflow is visible through
`lc_workflow_dispatcher_get_stats()`. It increments a durable repair counter
and schedules indexed reconciliation; it never makes a committed effect
disappear. Startup reconciliation and claim-expiry recovery remain mandatory.

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

The Lua module mirrors the workflow/dispatcher separation:

```lua
-- Request or route domain: threadless transactional production only.
local workflow = assert(client:new_workflow({
  namespace_name = "myapp.orders",
}))

workflow:transaction(function(tx)
  tx:append_outbox(entry, payload_source)
end)

-- Dedicated worker/service domain: get the shared dispatcher or start it.
local dispatcher = workflow:dispatcher()
assert(dispatcher:run({ handlers = handlers }))
```

`new_workflow()` is safe for a host to use synchronously in a route when the
host otherwise permits its Lockd client operation. It never creates liblockdc
dispatcher threads. `workflow:dispatcher()` is the explicit acquire-or-start
operation; it returns the compatible dispatcher already registered for that
client or starts one lazily. It takes no configuration argument: all dispatch
policy comes from the workflow's canonical `lc_workflow_config`.

The workflow facade supplies transactional operations and close/garbage-
collection cleanup. It has no `run`, `pump`, `next`, claim, retry, or terminal-
job methods. Its optional `dispatcher()` acquisition method does not itself
accept a Lua handler. The dispatcher facade alone accepts `handlers` and
consumes jobs, reconciles, and replays dead letters; it does not expose route-
side transaction production as a shortcut. The two handles make ownership and
latency boundaries obvious in Lua as well as C.

The direct Lua API wraps a dispatcher acquired from a workflow with an
owner-state dispatch loop:

```lua
-- worker.lua: run as a dedicated process under the application's service manager
local lockdc = require("lockdc")

local client = assert(lockdc.open({ url = os.getenv("LOCKD_URL") }))
local workflow = assert(client:new_workflow({
  namespace_name = "myapp.orders",
  max_attempts = 12,
}))

local dispatcher = workflow:dispatcher()
assert(dispatcher:run({
  handlers = {
    ["order.webhook"] = function(job)
      local ok, err = deliver(job)
      if not ok then
        return job:retry({ diagnostic = err })
      end
      return job:complete()
    end,
  },
})) -- blocking; owns this Lua state and handles retries
```

The facade also offers bounded `pump(opts)`, `stop()`, `wait()`, `stats()`,
`reconcile()`, and `replay_dead_letter(outbox_key)`. `pump()` integrates with a
host that already owns an event loop; `run()` is the simplest correct choice for
a dedicated worker process. It calls Lua only on the calling owner state. It
must never invoke a handler on liblockdc's private dispatcher thread.

#### Lua host-placement contract

This distinction must be explicit in liblockdc documentation and examples:

- `dispatcher:run()` is a blocking service loop. It belongs in a dedicated
  worker process or an equivalent service-only owner-state runtime, never in an
  HTTP route handler.
- `dispatcher:pump()` runs handler code synchronously on the calling Lua state.
  A zero-timeout call is Lua-thread-safe inside a route callback, but it is not
  an acceptable route-dispatch architecture: it performs foreign effects on
  request latency and still requires the thread-owning dispatcher object.
- liblockdc does not know whether a generic Lua host is Kore, Vectis, nginx, or
  a custom event loop. It must not attempt to infer host topology or enter a
  Lua closure from its dispatcher thread.
- Vectis owns the host-specific policy: its `app:workflow()` route facade uses
  an ordinary threadless `new_workflow()`, while `app:supervisor()` explicitly
  acquires the sole persistent dispatcher and owns its handler closure.

The direct `require("lockdc")` module remains intentionally available in a
Vectis script. Its long-lived dispatcher use inside a route is documented as
unsupported; Vectis's owned workflow facade is the supported route path. The
dependency-native module must not acquire a Vectis dependency merely to police
that host rule.

The facade keeps raw `dispatcher:next()` and job terminal operations available
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
- Duplicate `(workflow, kind)` handler declarations fail before runtime; distinct
  kinds may be assigned to independent logical supervisors and are routed only
  to their selected lane when it has capacity.
- An unregistered Vectis outbox kind is rejected before commit and creates no
  durable job or staged state change.
- The request path performs no effect I/O and does not wait for dispatcher
  scheduling, handler execution, or reconciliation.
- A route-to-supervisor key send reaches a ready handler without periodic timer
  polling; channel overflow, supervisor death, and restart recover all effects.
- A full outbox-key channel emits `OUTBOX_RECONCILE`, drops no durable state,
  and schedules reconciliation without requiring a per-key signal payload.
- A transaction committing several effects sends its returned receipt keys until
  full, then one reconcile wake recovers every remaining key.
- Inline Lua closures execute only in the supervisor Lua state; no worker or
  native background thread enters that state.
- Two logical supervisors run independently in one host process, including
  independent stop/failure policies and channel capacity handling.
- Isolated Lua lanes reject closure registration and accept only module/entry
  declarations; no one state is entered concurrently.
- IPC rejects oversized frames, reports full/closed endpoints, rejects
  undeclared signal kinds, and cannot carry borrowed pointers. The later
  request/reply extension cleans timed-out and late replies.
- IPC endpoints reach only their declared post-fork domains, are close-on-exec,
  and do not keep a channel or runtime alive through an unrelated child process.
- Graceful shutdown stops ingress, stops claims, completes or releases active
  work according to deadline, joins services, and closes every client after the
  fork-safe lifecycle order.

### liblockdc

- `new_workflow()` performs transactional production without creating a thread.
- Repeated dispatcher acquisition on compatible workflows from one client
  returns one dispatcher; a configuration mismatch is rejected.
- Concurrent acquisition and stop never produce two live dispatchers for one
  canonical workflow configuration.
- Runtimes sharing one app/outbox identity reject a canonical workflow
  configuration mismatch before attempting leader election.
- Closing a workflow neither stops an acquired dispatcher nor loses a committed
  receipt, while a destroyed uncommitted workflow rolls back its transaction.
- Passive workflow handles reject consume, reconciliation, and dead-letter
  replay; their dispatcher equivalents perform those operations.
- Dispatcher stop, replacement after registry cleanup, and client close leave
  no live dispatcher with a dangling client or a stale Lua handler sink.
- `dispatcher_notify_outbox_key` dispatches a committed key without a namespace
  scan and never calls a user handler.
- Duplicate notifications are coalesced; bounded overflow repairs through
  indexed reconciliation.
- A failed handler retries with the stable job identity and configured policy;
  terminal-operation failures retain the active claim until retry or expiry.
- Lua `dispatcher:run()` never enters a Lua state from the private dispatcher
  thread and handles stop, handler exception, renewal, retry, and dead-letter
  paths deterministically.
- Multiple independent Lua worker processes safely compete for one namespace.

## Implementation order

1. Decouple liblockdc workflow construction from dispatcher acquisition; add
   and test direct-key notification plus the dependency-native Lua
   workflow/dispatcher facades.
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
