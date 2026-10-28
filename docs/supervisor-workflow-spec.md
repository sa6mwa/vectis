# Supervisor and Workflow Dispatch Specification

Status: serial supervisor, public IPC and outbox integration implemented;
remaining contracts are identified below.

This specification extends the established Vectis T2 supervisor runtime into a
reusable background-execution model. It uses liblockdc 0.18.0's threadless
outbox and explicit dispatcher APIs. The serial default host, named logical
scopes, bounded public channels, timers, receivers, queue consumers, and
transaction-to-dispatcher receipt forwarding are implemented. This remains the
authority for the complete intended contract, including requirements blocked
by the current upstream public API.

## Delivery status

The implemented default runs callbacks serially in the supervisor's main Lua
caller state. Declarations precede fork; clients and dispatchers materialize
after fork. One persistent dispatcher per workflow identity receives committed
keys through a bounded FD-waking channel. Overflow coalesces a separate repair
signal; startup reconciliation and candidate probes recover durable work.
The private VRC1 service-control bus remains private.

[`lua-supervisor.md`](../manual/lua-supervisor.md) describes the available API;
[`supervisor-workflow-audit.md`](supervisor-workflow-audit.md) records behavior
evidence and limitations. In particular, the `payload_source()` pull interface
and automatic lease renewal throughout a blocking handler remain blocked:
liblockdc 0.23.0 exposes push payload streaming and explicit renewal, but neither
a public pull source nor a safe automatic-renewal facility for Lua-owned jobs.
The implementation exposes genuine `write_payload(sink)` streaming and explicit
`renew(ttl)` instead. These do not satisfy those two stronger requirements.

Isolated/module Lua lanes, bounded automatic restart policies, and cross-runtime
leader/configuration guards remain future work. Unsupported lanes and shared
runtime mode are rejected. Stopping a kind-owning scope pauses the workflow's
shared dispatcher while that scope is stopped: upstream has no kind-filtered
claim API. Different workflows retain independent lifecycle control. Release
approval must account for these limits; this status does not weaken the intended
contracts below.

Stateful effect claims support checkpoint read/update and explicit renewal
before or after writes, including repeated renewal. liblockdc 0.23.0 fixes the
earlier completion version defect. Normal/packed tests prove complete, retry
and dead-letter decisions plus the native checkpoint cleanup/retention policy.
This does not supply automatic renewal during blocked handlers.

Current logical health is state plus copied last-error diagnostics. Independent
quotas, lane permits, automatic restart/backoff and stronger health policies below
remain intended contracts; meaningful unsupported options are rejected.

The public supervisor transport is separate from the private VRC1 lifecycle
control channel; it does not expose or repurpose control frames. Existing
lifecycle safety rules remain in force: Kore is forked from a thread-clean process; supervisor services
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

Logical supervisors have independent names, source registrations, bounded inboxes,
state/error diagnostics, failure policy, start/stop state and message endpoints.
Independent quotas and stronger health policies remain requirements. They share the
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

The receiver shell provides `start`, `stop`, `wait`, `state`, `channel`, `timer`
and `close`. C source adapters use the configured event handler; Lua provides
the typed source-registration helpers. The shell's public method
table reserves eight `void *` extension slots. Every operation is also available
as a free function so callers never need to depend on vtable layout.

Each logical supervisor has the lifecycle:

```text
declared → materializing → running → stopping → stopped
                    └──────────────────────→ failed
```

Implemented failure policy is explicit: fail the app or stop only this logical
supervisor. Automatic restart with bounded backoff is a remaining requirement.
A failure never leaves a source silently claiming durable work.

### Implemented source adapters and outcomes

The serial supervisor schedules one owner-state callback at a time. It does
not redefine the source's durable delivery contract.

| Adapter | Input | Current handler outcome |
| --- | --- | --- |
| workflow outbox | claimed `lc_outbox_job` | returned complete, retry or dead-letter token |
| Lockd queue | batch-one queue message | returned ack or nack token; queue policy controls dead letters |
| timer | scheduled tick | continue; false/"disable" disables; exception follows scope failure policy |
| IPC receiver | copied bounded frame or signal | accept; false or nil,error follows scope failure policy |
| SUS log safe point | bounded copied backend logs | explicit owner pump through a timer |

Generic native watch adapters, permits/independent quotas, timer retry policies
and managed IPC replies remain separate extensions. The current implementation
does not accept their configuration fields or claim their semantics.

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

### Implemented Lua surface

`app:supervisor(opts)` declares an app-owned logical scope without creating a
process. `scope:channel()` and `app:ipc(name)` return its bounded producer
endpoint. `send(kind, bytes[, opts])` and `signal(kind)` return `true` or
`nil,status`, including "full" and "closed". Signal kinds are declared before
fork. Frame metadata accepts explicit uint32 correlation IDs and flags; no
managed reply routing or `send_json()` helper is supplied.

`app:workflow(opts)` declares one canonical workflow identity.
`workflow:outbox(opts)` registers one handler per kind on the lazy default
scope or `opts.supervisor`; `scope:outbox(opts)` selects that scope explicitly.
`workflow:transaction(callback)` owns commit/rollback and automatically
forwards successful commit receipts. Its callback exposes `tx:append_outbox`
and upstream participant operations such as `tx:acquire`, with guarded lifetimes.
An unregistered kind poisons the transaction even when caught by the callback.

Jobs expose genuine push `write_payload(sink)`, explicit `renew(ttl)`,
scoped state access and typed complete/retry/dead-letter tokens. The selected
token must be returned; terminalization happens afterward. Handler exceptions
or absent/incorrect tokens follow upstream retry policy. No callback deadline
or preemption is supplied. Stateful manual renewal preserves checkpoint
versions through deferred completion, retry and dead-letter decisions.

Use [Lua supervisors and workflows](../manual/lua-supervisor.md) for runnable
examples, options and return contracts. Superseded pseudo-APIs such as
`tx:update_json(order)`, `orders:prune_expired()` and `job:payload_source()`
are not current Vectis methods. The pull-source and isolated-lane requirements
remain explicit below rather than being mixed into runnable examples.

### Workflow dispatch and one local dispatcher

For one app runtime and one outbox namespace, Vectis runs one persistent
outbox dispatcher in the supervisor host, not one dispatcher per Kore route
worker. Route workers use their own post-fork outbox clients for transaction
production and commit transactions concurrently. The supervisor owns a
separate post-fork client and one outbox dispatcher.

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

The serial dispatcher claims one job per pump. Automatic renewal during a
blocking foreign operation remains required and unavailable; current stateless
handlers can explicitly renew between bounded operations. A successful effect completes the job;
a transient failure returns it to `retry_wait`; a permanent, diagnosed failure
dead-letters it. If the process stops before a terminal action, the claim
expires and liblockdc recovery makes the immutable job eligible again. Effects
are at-least-once and must send the stable `effect_key` to the foreign system as
an idempotency key.

**Remaining shared-runtime contract:** for multiple Vectis runtimes that
deliberately share one app/outbox identity, the active dispatcher must be guarded
by a Lockd leader lease. Current `shared_runtime=true` is rejected. A replacement gains
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

liblockdc 0.18.0 is authoritative for durable outbox transactions, claims,
retries, dead letters, and recovery. Vectis must not recreate these mechanisms
or retain the retired `lc_workflow` API as a compatibility layer.

### Producer and dispatcher separation

`lc_client_new_outbox()` constructs a threadless `lc_outbox`. A producer
uses `begin()`, `append()`, or `accept_command()`, stages participants,
and commits once. A committed result contains the durable outbox receipt keys;
failed or rolled-back transactions expose no wakeable key. The producer does
not run handlers or claim jobs. Closing it rolls back an uncommitted
transaction but does not stop an attached dispatcher.

`outbox->get_or_start_dispatcher()` acquires the compatible process-local
`lc_outbox_dispatcher`. It is explicitly owned by a service/supervisor, not
a route. The dispatcher supports `next()`, `notify_outbox_key()`,
`get_stats()`, `reconcile()`, dead-letter operations, and stop/wait/close.
Its private thread maintains bounded candidate and recovery notifications;
a caller-side `next()` claims work. Different client instances are not
assumed to share a dispatcher.

After a route commits, Vectis sends its returned receipt key over local IPC
to the owning supervisor. The supervisor calls `notify_outbox_key()`.
Overflow or lost IPC must be repaired by liblockdc reconciliation. No route
waits for effect execution or invokes a remote wake request. Notification is
a latency optimization, never the durability mechanism. The supervisor
rejects a mismatched dispatcher configuration rather than attaching to an
incompatible outbox.

### Dependency-native Lua placement

The liblockdc Lua facade follows the same ownership boundary:

```lua
-- In a producer domain, including a web route:
local outbox = assert(client:new_outbox({ ns = "myapp.orders" }))
local tx = assert(outbox:append(entry, payload))
assert(tx:commit())
outbox:close()

-- In a dedicated worker or Vectis supervisor domain:
local worker_outbox = assert(client:new_outbox({ ns = "myapp.orders" }))
local dispatcher = assert(worker_outbox:dispatcher())
assert(dispatcher:run({ handlers = handlers }))
```

`new_outbox()` is threadless and safe for transactional production. The
dispatcher is acquired explicitly; `run()` is a blocking owner-Lua-state
loop, so it never belongs inside a Kore route. `pump()` also invokes Lua
handlers on its calling state and is not a route-dispatch architecture.
The native dispatcher thread must never enter a foreign Lua state.
Vectis owns the process-specific placement, local IPC, and one persistent
supervisor handler lane. An application using only liblockdc may instead
run a dedicated Lua worker process without Vectis machinery.

Multiple workers may compete safely through LockDC claims. The single local
dispatcher policy is per compatible client/outbox configuration, not a
promise of one dispatcher globally across hosts. Cross-host notifications
are a possible future liblockdc latency optimization; correctness must
continue to depend on durable reconciliation after loss or partition.

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

- `new_outbox()` performs transactional production without creating a thread.
- Repeated dispatcher acquisition on compatible workflows from one client
  returns one dispatcher; a configuration mismatch is rejected.
- Concurrent acquisition and stop never produce two live dispatchers for one
  canonical workflow configuration.
- Runtimes sharing one app/outbox identity reject a canonical workflow
  configuration mismatch before attempting leader election.
- Closing an outbox neither stops an acquired dispatcher nor loses a committed
  receipt, while a destroyed uncommitted transaction rolls back.
- Passive outbox handles reject consume, reconciliation, and dead-letter
  replay; their dispatcher equivalents perform those operations.
- Dispatcher stop, replacement after registry cleanup, and client close leave
  no live dispatcher with a dangling client or a stale Lua handler sink.
- `notify_outbox_key()` dispatches a committed key without a namespace
  scan and never calls a user handler.
- Duplicate notifications are coalesced; bounded overflow repairs through
  indexed reconciliation.
- A failed handler retries with the stable job identity and configured policy;
  terminal-operation failures retain the active claim until retry or expiry.
- Lua `dispatcher:run()` never enters a Lua state from the private dispatcher
  thread and handles stop, handler exception, renewal, retry, and dead-letter
  paths deterministically.
- Multiple independent Lua worker processes safely compete for one namespace.

## Remaining implementation requirements

The liblockdc integration, public bounded channel, C supervisor lifecycle,
serial Lua scopes/timers/receivers/queue consumers, outbox handlers and automatic
receipt forwarding are implemented. They are no longer pending implementation
steps. The audits and manual record their executable evidence.

The following requirements remain unchanged and are not supported configuration:

- A handler-scoped pull `job:payload_source()` that reads storage incrementally,
  with explicit lifetime and errors. Push `write_payload(sink)` is delivered;
  hidden materialization or spooling cannot substitute for a public pull source.
- Safe automatic native claim renewal while the owner Lua handler is blocked.
  Explicit manual renewal, including stateful renewal, does not satisfy this
  stronger requirement.
- Kind-filtered intake so stopping one kind's scope does not pause other kinds
  of the same workflow. Different workflow identities already remain independent.
- Cross-runtime canonical-configuration checks and Lockd leader leases. Public
  leases/transactions can support the local implementation; it is future Vectis
  work, not an upstream impossibility.
- Isolated Lua lanes using module/entry declarations, independent quotas,
  bounded permits, stronger health policies and automatic restart/backoff.
  Inline closures stay on the single owner state; arbitrary closures never move
  between Lua states and one state is never entered concurrently.
- Managed channel request/reply with bounded pending slots, runtime correlation
  allocation, deadlines and cleanup on success, timeout, send failure or close.
  Late/unknown replies must be dropped. Current metadata alone is not this API.
- Migration of existing C-owned worker descriptors onto logical scopes while
  preserving their native service contracts; additional typed/watch adapters
  require explicit source outcomes and capacity policy.

The required verification above applies to each contract when implemented;
current passing tests do not imply completion of these remaining requirements.
Login/onboarding email remains synchronous and is not an outbox handler.
