# Vectis Service Runtime Lifecycle Spec

Status: draft implementation authority.

This document defines the production runtime model for Vectis applications that
combine Kore HTTP/WebDAV/API serving with background services such as lockdc
consumers, metrics snapshots, CAI/MCP helpers, OPC UA loops, curl-backed workers,
audio capture/playback, and SUS transcription workers.

The core constraint is simple: Kore must never fork workers from a process that
already has Vectis-owned service threads or unknown dependency-owned service
threads. Vectis must model service setup as declaration first, materialization
later, with a runtime topology that makes the fork boundary explicit and
testable.

## Goals

- Keep Kore in control of its own parent/worker lifecycle.
- Allow developers to declare background services before starting the web
  server.
- Ensure those declarations do not create pthreads, open daemon handles, start
  dependency event loops, or make callback entry possible before the runtime
  topology is selected.
- Run route-backed apps with background services through a supervisor topology:
  a Vectis supervisor process owns background services, and a thread-clean Kore
  child owns HTTP/WebDAV/API ingress.
- Preserve direct foreground Kore serving for apps that have no app-owned
  background services and pass the quiescence guard.
- Expose explicit topology policy so operators can accept automatic selection,
  require direct foreground Kore, or force supervised route runtime.
- Keep Lua state ownership explicit. No background service thread may enter an
  arbitrary Lua callback. Lua callbacks run only in the Lua state and process
  that owns them.
- Provide the same lifecycle semantics through C/libvectis and the vectis Lua
  binary.
- Provide graceful shutdown across Kore, lockdc consumers, metrics persistence,
  CAI/MCP, OPC UA, curl, audio, SUS, and future daemon-style services.
- Make the contract falsifiable with unit, integration, and e2e tests.

## Non-Goals

- Do not make every dependency fork-safe by trying to maintain an at-fork list.
  That approach does not scale with curl, OpenSSL, Lua, allocators, OPC UA, CAI,
  audio backends, SUS, and future dependencies.
- Do not call a background-thread Lua callback directly.
- Do not hide full-message buffering behind a streaming-looking API.
- Do not require users to hand-order `fork()`, `pthread_create()`, or Kore
  internals.
- Do not introduce versioned pack command variants, "legacy" runtime labels, or
  parallel old/new command models. Vectis is not shipped yet.

## Current Risk Model

Kore may fork worker processes during startup and may fork replacement workers
later if workers die. If the process that forks workers has already started
threads, a child can inherit locks held by threads that no longer exist. That
can deadlock inside libc, malloc, OpenSSL, curl, Lua, logging, or any dependency.

The production rule is therefore stricter than "do not start metrics early":

- no Vectis-owned service pthread before Kore's fork boundary;
- no dependency-owned daemon thread before Kore's fork boundary;
- no live app-owned service runtime handle that can dispatch callbacks across
  the fork boundary;
- no Kore parent worker refork from a process that also runs Vectis service
  pthreads.

## Runtime Domains

### Declaration Domain

The declaration domain is the process/thread that constructs `vectis_app`, Lua
`server`, routes, mounts, auth providers, service declarations, and runtime
policy.

Allowed:

- allocate and copy configuration;
- register route callbacks;
- register auth callback contracts;
- register service declarations;
- create pure value objects;
- create bounded in-memory mailboxes that do not start worker threads.

Disallowed for route-backed apps before runtime start:

- starting pthread-backed services;
- opening managed lockdc consumer services;
- opening long-lived dependency clients for app-owned services;
- starting OPC UA servers/clients or subscription loops as app-owned daemons;
- starting audio capture/playback devices;
- opening SUS workers intended to run as app services;
- creating curl worker pools or async transfer daemons;
- entering background Lua callbacks.

Direct dependency calls that perform one synchronous operation during
declaration are not automatically Vectis services, but they are outside the
app-owned lifecycle. If they start threads or leave long-lived handles open,
the route-backed app start must fail the quiescence guard.

### Kore Child Domain

The Kore child domain owns HTTP/WebDAV/API ingress. It is created from a
thread-clean declaration process and then lets Kore initialize and fork its own
workers.

Allowed:

- Kore parent/worker runtime;
- HTTP route callbacks;
- WebDAV route handlers;
- static file and packed asset handlers;
- MCP HTTP routes mounted through Kore;
- request-local synchronous dependency use when the handler explicitly does it;
- Lua route callbacks in the child Lua state copied at fork time.

Disallowed:

- Vectis supervisor service pthreads;
- supervisor-owned lockdc consumer loops;
- supervisor metrics snapshot worker threads;
- arbitrary entry into the supervisor's Lua state;
- relying on in-process `vectis_mailbox` to communicate with the supervisor.

### Supervisor Domain

The supervisor domain owns app background services for route-backed apps. It
forks the Kore child while still thread-clean, then materializes and starts
declared services in the supervisor process.

Allowed:

- lockdc consumer services;
- metrics sampling and persistence;
- CAI/MCP helper clients not mounted as Kore routes;
- OPC UA client/server loops declared as app services;
- curl-backed worker services;
- audio capture/playback services;
- SUS transcription worker services;
- service-local C callbacks;
- supervisor Lua pumps that run on the supervisor's Lua state.

Disallowed:

- Kore worker forking;
- direct use of Kore request pointers;
- direct calls into the Kore child's Lua state;
- hidden sharing of ordinary in-process pointers with Kore child workers.

## Topologies

### T1: Direct Kore Runtime

Used when the app has routes and no app-owned background services.

Flow:

1. Validate app config.
2. Validate process quiescence.
3. Enter `vectis_internal_kore_run()` on the caller thread.
4. Kore owns signals and shutdown.

This topology is valid for simple HTTP/WebDAV/API apps. It is not valid once
the app declares services that require supervisor materialization.

### T2: Supervised Kore Runtime

Used when the app has routes and at least one app-owned background service, or
when runtime policy explicitly requests supervision.

Flow:

1. Validate app config.
2. Validate process quiescence.
3. Create supervisor-child control channels.
4. Fork the Kore child.
5. Child closes supervisor-only channel ends and enters Kore.
6. Child reports readiness only after the listener, domains, and routes are
   configured successfully.
7. Parent waits for readiness or child exit before reporting successful start.
8. Parent materializes declared supervisor services.
9. Parent starts services according to declared start policy.
10. Parent monitors signals, child exit, service failures, and shutdown
   deadlines.

This is the normal production topology for "webserver plus daemon work".

### T3: Service-Only Runtime

Used when the app has no routes.

Flow:

1. Materialize declared services in the current process.
2. Start requested services.
3. Wait for process signals, service failures, or explicit stop.
4. Stop services and close app-owned resources.

No Kore fork boundary exists in this topology.

## App Lifecycle Semantics

`app->run(app, error)` and `server:run()` are production entry points:

- route-backed with no services: T1 direct Kore runtime;
- route-backed with services or metrics persistence: T2 supervised Kore runtime;
- service-only: T3 service-only runtime.

`app->start(app, error)` and `server:start()` are managed starts:

- route-backed with no services: T2 may still be used when the caller must
  continue while Kore serves;
- route-backed with services: T2;
- service-only: T3 without blocking.

`wait()` blocks until signal, child exit, service failure, or explicit stop
condition, then runs the same shutdown sequence.

In supervised route runtimes, `wait()` must actively monitor the Kore child.
If the child exits before a shutdown signal, Vectis marks the child as reaped,
tears down supervisor-owned services and resources, and returns
`VECTIS_ERR_STATE` with the child exit status or terminating signal in the
diagnostic message. It must not keep waiting indefinitely for a process signal
after the HTTP ingress process has died.

For managed route-backed `start()`, Vectis must not report success until the
Kore child has reported readiness over the supervisor control channel. If the
child exits first, or readiness times out, `start()` returns an error and leaves
the app in the not-started state. Supervisor-owned services must not materialize
until this readiness barrier has passed. Metrics persistence is an app-owned
supervisor service: when enabled, the metrics worker starts after the readiness
barrier and performs persistent snapshot writes outside the request path.

For asynchronously started supervisor services, Vectis owns service terminal
state observation. Dependencies such as liblockdc that expose only blocking
`wait()` calls are monitored by a supervisor-owned monitor thread that starts
only after the Kore child fork boundary. If a monitored service exits before
Vectis requested shutdown, `wait()` fails closed, stops the remaining app-owned
services, and returns the dependency diagnostic. Explicit `service->wait()` and
`service->close()` join the same monitor instead of calling the dependency
`wait()` a second time.

`stop()` is idempotent only after the app has entered a started/running state.
Calling `stop()` on a never-started app remains an error unless the API is
explicitly changed everywhere.

## Service Declaration Contract

An app-owned service handle has two layers:

- descriptor: copied declaration, no dependency runtime handle, no service
  thread;
- instance: materialized dependency handle in one runtime domain.

`app->consumer_service(...)` must produce a descriptor-backed
`vectis_consumer_service`. It must not open a lockdc client or call
`lc_client_new_consumer_service()` merely to register intent for an app-owned
service.

`service->start(service, error)` before app runtime start means:

- in a service-only app: materialize and start immediately;
- in a route-backed not-yet-started app: mark start intent on the descriptor;
- in a running supervised app: materialize/start in the supervisor domain;
- in a direct Kore child/domain: fail with an actionable lifecycle error.

`service->native(service)` returns the materialized native handle only after
the service is materialized in the current domain. Before materialization it
returns `NULL`.

`service->run()` and `service->run_until()` are service-only operations unless
the app is already in a safe supervisor domain. They must fail when they would
start a service thread before a route-backed Kore fork.

## Lockdc Consumer Services

The descriptor owns:

- copied `lc_consumer_service_config`;
- copied consumer array;
- copied string fields used by dequeue requests where Vectis can own them;
- callback function pointers and callback contexts as borrowed process-local
  values;
- Vectis receiver runtimes for built-in C receiver kinds such as `mailbox` and
  `webdav_marker`.

Materialization:

1. Open or reuse a supervisor-domain lockdc client.
2. Call `lc_client_new_consumer_service()` in that domain.
3. Start the native service only after the Kore child has been forked for T2.
4. Start a supervisor monitor thread that blocks in `lc_consumer_service_wait()`
   and records terminal state for app-level failure propagation.

Lockdc transport validation must distinguish local and network endpoints.
`pouch://` endpoints are local storage backends and do not require TLS client
bundle material. TCP/TLS endpoints still require explicit client material unless
the app uses a Unix-domain socket.

Callbacks:

- C callbacks run on lockdc service threads in the supervisor domain.
- They may call C facades safe for that domain.
- They may publish to a supervisor mailbox or process bus.
- They may not call arbitrary Lua callbacks.

Lua `server:consumer_service(opts)`:

- registers a descriptor;
- defaults `start = true`, meaning "start with the app runtime";
- keeps direct Lua `on_message` callbacks rejected until a Lua pump contract is
  used;
- supports built-in C receiver kinds without requiring reusable Lua helper
  functions.

## Metrics And Snapshots

Metrics remain disabled by default.

When enabled:

- in T1, request counters and route metrics are collected in the Kore domain;
- in T2, Kore-domain metrics are reported to the supervisor through the runtime
  control channel or a bounded metrics channel;
- persistent snapshots are written by the supervisor, not by a request worker;
- route-backed metrics persistence selects T2 supervision even when no other
  background service is declared;
- snapshot frequency is at most once every five minutes;
- sampling frequency for loadavg and other system metrics is at most once per
  minute;
- persistence uses a lockdc pouch endpoint under XDG state by default.

The metrics HTTP endpoint can render JSON and the landed-style dashboard, but it
must be mounted behind the same Vectis auth mechanism used by ordinary routes
when auth is configured.

## CAI And MCP

Two CAI/MCP shapes exist:

- Kore-mounted MCP server route: belongs to the Kore child domain because the
  HTTP request enters Kore.
- Supervisor CAI worker/client: belongs to the supervisor domain when declared
  as a background service.

MCP HTTP route callbacks:

- run in the Kore child domain;
- may call Lua only in the child Lua state that owns the route callback;
- must not call supervisor Lua callbacks or share supervisor-only handles.

CAI clients opened through `app->cai_client()` must be domain-local. A client
opened in one process must not be reused after fork in another process.

## OPC UA

OPC UA has three valid ownership modes:

- direct caller-owned facade use, outside app lifecycle;
- Kore child request-local use;
- declared supervisor service use.

Managed OPC UA services must be descriptor-backed and materialized in the
supervisor domain for T2. Monitor callbacks may publish copied payloads into a
mailbox or runtime bus. They may not enter Lua directly.

The existing OPC UA mailbox adapter remains valid as a C-side adapter. A future
OPC UA service declaration should compose that adapter rather than duplicating
its payload projection rules.

## Curl-Backed Workers

Synchronous `curl_easy_perform()` calls are allowed inside the domain that owns
the current callback, subject to existing timeout and `no_signal` defaults.

Curl-backed worker services are app-owned daemons and must follow the descriptor
and supervisor materialization model. They must not be started during
declaration before Kore forks.

Global curl/OpenSSL initialization must not be used as a substitute for the
quiescence guard. The guard is about active threads and long-lived handles, not
only library initialization order.

## Audio And SUS

Audio capture/playback and SUS transcription handles can create long-lived
backend state and dependency callbacks. When used as app services, they must be
declared and materialized in the supervisor domain.

Direct Lua facade use before starting a route-backed app is caller-owned. If it
starts threads or leaves daemon handles active, Vectis must detect a non-
quiescent process at app start and fail with an actionable error.

When audio/SUS work is triggered by HTTP, the preferred production pattern is:

1. Kore route validates and accepts work.
2. Route sends a copied request to a supervisor worker through lockdc or the
   runtime bus.
3. Supervisor worker owns the audio/SUS handle and returns a result through the
   selected reply path.

## Lua State Ownership

Lua state ownership is process-local and thread-local by policy:

- Kore route Lua callbacks run in the Kore child process.
- Supervisor Lua pumps run in the supervisor process.
- Lockdc, OPC UA, audio, SUS, curl, CAI, and metrics worker threads do not enter
  arbitrary Lua callbacks.
- Lua callbacks registered as policy hooks are invoked only by an explicit pump
  on the owning Lua state.

This means a Lua route callback and a Lua service callback are not the same
callback surface in T2. They communicate through copied messages, lockdc, the
runtime bus, or explicit persistent storage.

## Communication Between Domains

Ordinary in-process pointers and `vectis_mailbox` handles are not cross-process
contracts.

Valid cross-domain channels:

- lockdc queues and pouch storage for durable communication;
- a Vectis runtime bus over Unix sockets/socketpairs for bounded copied control
  and request/reply messages;
- explicit files or storage configured by the app;
- shared memory only for narrow process-shared data structures that are
  designed and tested as process-shared.

The runtime bus should carry:

- child readiness and shutdown events;
- service failure events;
- metrics snapshots or metrics deltas;
- optional route-to-supervisor request/reply messages when configured;
- structured errors with Vectis status/source metadata.

## Quiescence Guard

Before starting T1 or forking the T2 Kore child, Vectis must verify that the
process is safe to cross the Kore fork boundary.

Required checks:

- no app-owned service instance is already running;
- no app-owned service descriptor has been materialized in the declaration
  domain for a route-backed app;
- process thread count is one on platforms where exact inspection is available;
- on platforms where exact thread count is unavailable, strict mode must fail
  closed and non-strict mode must warn through the logger;
- known Vectis-managed dependency services are not active outside the selected
  runtime domain.

On Linux, exact thread count can be checked through `/proc/self/task`. Darwin
needs a platform-specific implementation or strict-mode failure until it exists.

Error messages must identify the unsafe condition and tell the developer to
register the work as an app service or start the app before creating
caller-owned threads.

## Shutdown Sequence

Shutdown is one coordinated runtime state transition:

1. Mark runtime stopping.
2. Stop new supervisor service ingress where possible:
   - lockdc consumers stop dequeuing;
   - OPC UA subscriptions/server loops stop accepting new work;
   - audio capture stops;
   - curl worker queues stop accepting new work;
   - CAI/MCP supervisor workers stop accepting new work.
3. Stop Kore ingress:
   - direct T1: signal Kore stop path;
   - supervised T2: send stop over control channel and/or signal child.
4. Wait bounded grace for in-flight work.
5. Persist final metrics snapshot when metrics persistence is enabled.
6. Wait for service threads.
7. Close service instances.
8. Close domain-local lockdc, CAI, curl, OPC UA, audio, and SUS handles.
9. Release Lua references in the owning process after callbacks cannot run.

If a child or service exits unexpectedly, the default policy is fail closed:
mark the app stopping, stop the rest of the runtime, and return an error from
`run()`/`wait()`.

Supervised child termination must be bounded. The supervisor first requests a
graceful Kore shutdown with `SIGTERM`, reaps with nonblocking observation until
the configured shutdown deadline, then escalates to `SIGKILL` and reaps the
child so `stop()` cannot hang indefinitely. `vectis_app_config.shutdown_grace_ms`
and Lua `vectis.server.new({shutdown_grace_ms = ...})` configure this grace
period; zero or omission uses `VECTIS_APP_DEFAULT_SHUTDOWN_GRACE_MS`.
`vectis_app_config.supervision_policy` and Lua
`vectis.server.new({supervision_policy = ...})` configure route-backed topology:
`auto` chooses direct foreground Kore unless app-owned services require
supervision, `direct` fails closed when such services are declared, and
`supervised` forces the managed supervisor topology.

## C API Surface Changes

The existing receiver-shell style remains.

Required additions or semantic changes:

- `vectis_app_config.shutdown_grace_ms` for shutdown grace.
- `vectis_app_config.supervision_policy` for explicit `auto`, `direct`, or
  `supervised` route-backed topology selection.
- future runtime config fields still need quiescence strictness and service
  failure policy.
- descriptor-backed `vectis_consumer_service`.
- service state query helpers for tests and diagnostics.
- app-owned service registration APIs for future OPC UA/curl/audio/SUS worker
  declarations.
- runtime bus primitives or a documented narrow internal control channel.

Public comments must state whether a function declares, materializes, starts,
or runs a service.

## Lua Surface Changes

Required semantics:

- `server:run()` selects T1, T2, or T3 automatically from app declarations.
- `server:start()` starts the selected managed runtime and returns.
- `server:wait()` waits for the selected runtime and shuts it down.
- `server.new({supervision_policy = "auto" | "direct" | "supervised"})`
  mirrors the C topology policy.
- `server:consumer_service({ start = true })` means start with the app runtime,
  not start a pthread during declaration for route-backed apps.
- Direct Lua callbacks for background services remain rejected unless they are
  attached to an explicit owner-state pump.
- Examples must not use `os.execute`, shell `sleep`, or external commands for
  waiting.

## Implementation Slices

1. Baseline cleanup and tests:
   - remove unsafe Kore-in-pthread lifecycle;
   - keep route-backed `start()` process-backed;
   - keep signal shutdown tests passing.
2. Quiescence guard:
   - implement thread-count inspection;
   - fail route-backed starts when unsafe app-owned services are already
     running;
   - add negative tests.
3. Descriptor-backed lockdc consumer services:
   - registration copies declarations and does not open lockdc;
   - start intent is recorded before app start;
   - service-only apps materialize immediately when started.
4. Supervised route runtime:
   - fork Kore child before materializing services;
   - wait for child readiness before reporting successful start;
   - supervisor starts lockdc consumer services and metrics after readiness;
   - metrics persistence is written by the supervisor worker, not request
     handlers;
   - parent monitors signals and child/service exit.
5. Runtime bus/control channel:
   - child readiness;
   - child stop;
   - service failure propagation;
   - metrics snapshot transfer.
6. Lua lifecycle updates:
   - align `server:consumer_service`, `server:run`, `server:start`,
     `server:wait`;
   - remove example shell sleeps;
   - document callback ownership.
7. Extend service declarations:
   - OPC UA managed service descriptors;
   - curl worker descriptors where needed;
   - audio/SUS worker descriptors;
   - CAI/MCP supervisor worker descriptors.
8. Hardening:
   - repeated start/stop stress;
   - child crash and service crash tests;
   - shutdown deadline tests;
   - review command from lifecycle skill until actionable issues are clean.

## Verification Requirements

Unit tests:

- descriptor registration does not open lockdc;
- `service->native()` is `NULL` before materialization;
- `service->start()` before route-backed app start records intent;
- quiescence guard rejects a known extra thread;
- app-owned running service blocks direct Kore start;
- shutdown state transitions are idempotent where promised and errors where not.

Integration/e2e tests:

- Lua `server:consumer_service()` plus HTTP route starts without pre-Kore
  service thread creation;
- supervised app handles SIGINT/SIGTERM/SIGQUIT cleanly;
- lockdc consumer and Kore route run concurrently through a safe channel;
- metrics endpoint works with persistence in supervised mode;
- child death stops supervisor services and returns failure;
- service failure stops Kore child by default;
- no examples use `os.execute` or shell sleep for waits;
- direct Lua background callbacks are not invoked from service threads.

Standard gates:

- focused runtime tests during implementation;
- full debug CTest before completion;
- project finalize/prerelease gates as appropriate for the touched surfaces;
- lifecycle review command with actionable issues resolved before completion.

## Open Decisions Before Coding Beyond Lockdc

The lockdc service descriptor and supervised runtime can be implemented without
external product decisions. The following choices should be committed before
implementing later service families:

- exact Lua pump API for supervisor-owned Lua callbacks;
- runtime bus public exposure level: internal only, C API, Lua API, or both;
- whether route-to-supervisor request/reply should prefer runtime bus or lockdc
  by default;
- Darwin quiescence policy before exact thread-count inspection exists.
