# CAI Agent And Vectis Integration

This note captures the intended ownership split between CAI agent mode and
Vectis. It is written as a handoff for CAI-side agent/tool/state design.

## Position

Agent orchestration should live in CAI. Vectis should not grow a parallel
agent framework or a duplicate "Agent Smith" tool runtime.

CAI already owns the agent-mode concept, model interaction, MCP/tool semantics,
and provider-specific AI behavior. Vectis is the process, service, storage,
auth, routing, packaging, and Lua/C host environment. The clean product shape is
for Vectis to host CAI agents and register Vectis-backed capabilities into CAI,
not for Vectis to implement half of the agent stack while CAI implements the
other half.

## Ownership Boundary

CAI should own:

- agent loop and planning/execution semantics;
- tool registry, tool schemas, argument/result contracts, and tool-call
  dispatch semantics;
- model/provider request handling, streaming, retries, and response parsing;
- MCP client/server protocol mechanics where they are AI/tool-protocol concerns;
- agent session semantics, including checkpoint and resume behavior;
- callback/event contracts needed by agent mode;
- default disk-backed agent state only as a standalone CAI fallback.

Vectis should own:

- embedding CAI inside the Vectis service runtime;
- exposing CAI through the Vectis C API and Lua facade without replacing CAI;
- Kore route mounting for CAI/MCP HTTP handlers;
- lifecycle, graceful shutdown, worker descriptors, and service supervision;
- auth integration for routes and protected tool surfaces;
- local and remote lockdc/pouch-backed storage adapters;
- typed adapters for Vectis capabilities such as lockdc, OPC UA, curl, SSH,
  WebDAV, cert management, metrics, audio, and SUS;
- packaging packed Lua/site assets into the Vectis binary.

The expected integration model is:

1. CAI defines stable agent extension contracts.
2. Vectis implements host adapters for those contracts.
3. Applications configure CAI agents through Vectis, then use CAI-native agent
   behavior with Vectis-backed tools and state.

## Required CAI Contracts

The following CAI contracts would let Vectis integrate cleanly without owning
agent behavior.

### Tool Provider Contract

CAI should expose a way for a host to register tool providers. A provider should
be able to declare:

- stable tool name;
- description;
- JSON schema for arguments;
- result shape and error shape;
- sync, async, or callback-driven execution mode;
- timeout/deadline policy;
- cancellation behavior;
- whether the tool may stream output, materialize bounded output, or return a
  file-backed or document-backed reference.

Vectis can then register tools backed by its own service surfaces:

- lockdc queue/document operations;
- pouch-backed local or remote document state;
- OPC UA reads/writes/subscriptions;
- curl-backed protocol clients;
- SSH/SCP operations;
- WebDAV file operations;
- cert and OpenSSL operations;
- audio/SUS transcription and voice workflows;
- metrics and operational inspection.

CAI should own the tool registry and call protocol. Vectis tools should be
ordinary providers plugged into that registry.

### State Backend Contract

CAI should expose a state backend interface for agent sessions and checkpoints.
The backend should support at least:

- create/load/update/delete session records;
- optimistic concurrency or revision checks;
- namespaced keys;
- binary-safe payloads or explicitly encoded JSON payloads;
- bounded reads/writes with actionable error reporting;
- optional lease/lock semantics where needed for concurrent agents;
- snapshot/checkpoint metadata;
- clear distinction between ephemeral runtime state and durable agent state.

Vectis should implement this backend using liblockdc:

- default local backend: `pouch://` under the Vectis XDG state directory;
- configurable remote backend: any lockdc endpoint configured by the
  application;
- namespace: a Vectis-owned namespace for agent/session state, with application
  override support.

CAI may keep JSON-on-disk state as a simple standalone fallback, but Vectis
should prefer lockdc/pouch so agent state participates in the same document
store semantics as the rest of a Vectis service.

### Callback And Event Contract

CAI should define callback hooks for agent lifecycle events, tool execution,
streaming deltas, session updates, approvals, and errors. Those hooks should be
host-neutral and should not assume that callbacks may run on arbitrary service
threads.

Vectis needs the following guarantee:

- CAI may notify the host through a C callback or event sink.
- Vectis may copy that event into a mailbox, lockdc queue, or route response.
- Lua callbacks are invoked only when the owning `lua_State` explicitly pumps or
  handles the event in its own runtime domain.

This preserves the current Vectis concurrency rule: background services,
worker threads, Kore workers, OPC UA callbacks, and lockdc consumer callbacks do
not directly enter Lua.

### Scheduler And Cancellation Contract

CAI should expose enough scheduler hooks for a host to run agent work inside an
owned service lifecycle:

- start/stop hooks;
- cooperative cancellation;
- deadline propagation;
- progress events;
- cleanup/finalization;
- no hidden long-running detached threads that Vectis cannot stop;
- explicit ownership of model clients, registries, and session handles.

Vectis can then run CAI agents as managed services or route-local operations
without weakening graceful shutdown.

## Vectis Integration Shape

Once CAI exposes the contracts above, Vectis should provide:

- a C adapter that registers Vectis capability providers into a CAI registry;
- a lockdc/pouch CAI state backend;
- a mailbox-backed event adapter for CAI callbacks;
- Lua helpers to configure the adapter without hiding the direct CAI module;
- examples showing a CAI agent using lockdc state and Vectis tools;
- tests proving that route, worker, and service shutdown paths cancel or drain
  CAI work correctly.

The Lua shape should stay consistent with the existing facade model:

```lua
local vectis = require("vectis")
local cai = require("cai")

local state = vectis.cai.lockd_state({
  endpoint = "pouch://${XDG_STATE_HOME}/vectis/storage",
  namespace = "vectis.agent",
})

local tools = vectis.cai.tools({
  lockd = true,
  curl = true,
  ssh = true,
  opcua = true,
})

local agent = cai.agent({
  state = state,
  tools = tools,
})
```

The exact names are illustrative. The important point is that `cai.agent`
remains CAI-owned while `vectis.cai.*` supplies host adapters.

## Non-Goals For Vectis

Vectis should not implement:

- a separate agent loop;
- a second CAI-compatible tool registry;
- a bespoke Agent Smith session format;
- model/provider request logic that belongs in CAI;
- direct background-thread Lua callback execution;
- duplicate terminal/tool helpers that will later need to migrate into CAI.

Agent Smith can remain a Vectis product/workflow name later, but its mechanics
should be CAI agent mode plus Vectis host adapters.

## Open CAI Design Questions

- What is the minimum stable tool-provider ABI/API that CAI can expose without
  committing to all future tool features?
- Should CAI tool execution be strictly request/reply, or should streaming tool
  output be first-class from the start?
- How should CAI represent file-backed and document-backed tool results?
- What session state fields must be indexed/queryable by a host backend?
- Does CAI need lease semantics in its state backend, or can that remain a
  backend-specific optimization?
- How does CAI want to model approvals, user interaction, and policy callbacks?
- What cancellation guarantees can CAI provide across provider requests,
  streaming responses, and tool execution?

## Vectis Implementation Trigger

Vectis should implement Agent Smith-related integration only after the CAI
agent contracts exist. Until then, the Vectis TODO should remain an integration
placeholder, not a commitment to build agent primitives inside Vectis.
