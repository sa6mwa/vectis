# CAI Agent And Vectis Integration

CAI 0.5 owns Agent Smith's coding-agent behavior. Vectis hosts it without
creating a parallel agent implementation.

## Ownership

CAI owns:

- the Smith preset, agent loop, provider protocol, and response streaming;
- tool schemas, tool-call dispatch, review subagents, and cancellation;
- session checkpoint and journal callback contracts;
- ChatGPT OAuth token handling and refresh.

Vectis owns:

- the `vectis -a smith` terminal product surface;
- bounded CAI-to-libmdf-to-Softline presentation;
- the workspace-rooted terminal configuration;
- LockDC/Pouch session persistence;
- the public C adapter and the non-TUI `vectis.smith` Lua facade.

This split is deliberate. Vectis may add host tools and durable storage, but it
does not duplicate CAI model orchestration, tool protocol, or session format.

## Runtime and presentation

Smith has an exec mode and an interactive TUI mode. Exec mode is a normal
owner-thread CAI pump loop that emits streamed output and exits. Interactive
mode has a stronger architectural contract: CAI must progress independently of
the editable Softline UI, and external output/state changes must wake the UI
immediately.

The required interactive topology is:

```text
CAI owner thread              libmdf renderer thread       Softline UI thread
----------------              ---------------------        ------------------
pump CAI                      read bounded Markdown        own all sl_t calls
publish state snapshot        write bounded ANSI chunks    wait on stdin + wake FD
copy events to bounded -----> signal wake FD ----------->  bounded drain
queues                         never call Softline          print_above/status/queue
```

CAI's public owner-thread contract remains intact: the worker opens, pumps,
reads state, and closes the runtime. UI-to-agent controls cross an explicit
bounded, synchronized bridge. The CAI worker executes each control according
to its current state, preventing races between a UI state snapshot and an
active-to-idle transition. It can use CAI's thread-safe steering/queued
admission where appropriate, but it never lets a non-owner UI thread call
owner-only `pump` or `state` APIs.

CAI text events are copied into a bounded Markdown queue. libmdf consumes
that source and produces bounded ANSI chunks without materializing a response.
The renderer and agent worker signal an application wake FD after publishing
output or a status snapshot. Softline waits on that FD alongside terminal
input and invokes a UI-owner callback. The callback drains a bounded amount of
output through `sl_print_above()` and applies the latest status snapshot before
returning to input handling. No worker calls Softline. This requires the
required Softline external-event API.

### Status projection

The CAI worker and renderer publish a monotonic, immutable UI snapshot before
signalling the wake FD. The UI owns the displayed projection and never calls
CAI to refresh it. At minimum it carries current run state, model/request or
tool activity when available, active tool/subagent identity, the CAI-side turn
state, renderer/stream health, the most recent bounded failure, and worker
liveness. Softline adds its local queue count and editor-local status on the UI
thread. A callback applies only the newest available snapshot, so a burst of
events cannot show an older state after a newer one. Any relevant state change
signals the wake FD; status is not dependent on a later keystroke.

Softline owns ordinary interactive follow-up prompts in its local FIFO, using
manual queue delivery. During an active CAI turn, Enter appends locally. When
the worker publishes a terminal state, the UI takes exactly the oldest item and
submits it as one normal CAI turn; it does not bulk-submit or duplicate entries.
A typed steering input, or promotion of the newest local queue item from an
empty steering editor, is sent through the control bridge immediately. The
worker sends it as steering if still active, otherwise as the next normal turn.
Queue mutation and preview rendering remain inside Softline.

The current idle-callback implementation is transitional and does not satisfy
this contract: it pumps CAI on the Softline owner thread and has no external
wakeup integration. Smith must not be described as a complete interactive TUI
until this topology is implemented and verified under concurrent streaming,
editing, queue manipulation, status changes, and tool execution.

## Interactive verification gate

Interactive Smith is complete only when PTY/integration tests prove all of the
following against a controllable streaming CAI fixture:

- The user can continuously type and edit a partial UTF-8/pasted draft while
  model deltas, tool-state updates, and status changes arrive; each delta is
  rendered before turn completion and the final draft is unchanged.
- A stream/output flood is bounded and fair: input, Ctrl-C, queue editing,
  steering, and quit retain the documented latency bound.
- Active-turn Enter uses the Softline-owned local FIFO. After terminal state,
  exactly one oldest entry is submitted, in order; no entry is lost, duplicated,
  or prematurely handed to CAI. The UI stays editable across every transition.
- Steering and empty-editor promotion reach an active CAI turn promptly; if a
  terminal transition wins the race, the same text becomes exactly one normal
  turn rather than failing or disappearing.
- Worker, renderer, CAI failure, EOF, and shutdown paths wake the UI, show a
  bounded actionable status, restore terminal state, join all workers, and
  preserve the durable session semantics.
- Thread instrumentation or equivalent deterministic ownership tests prove
  that every Softline call is on the UI thread and every owner-only CAI call is
  on the CAI owner thread.

## State adapter

`vectis_smith_store` adapts LockDC to CAI's
`cai_agent_session_store` callbacks. It stores the latest checkpoint and a
strictly ordered append-only event journal under hashed opaque scope/session
keys. A checkpoint watermark makes resume deterministic: CAI reloads the
checkpoint and Vectis replays only subsequent events. A LockDC lease serializes
each update, and the adapter is mutex-protected so a shared store is safe for
CAI callbacks from multiple runtimes.

The default CLI store is a user-owned encrypted Pouch directory. Deployments
can choose a different LockDC endpoint and namespace. ChatGPT OAuth state is
not agent session state and remains CAI-owned, as described in
[Agent Smith](agent-smith.md).

## Host integration

The public C API intentionally exposes the borrowed underlying
`cai_agent_runtime` for advanced composition while retaining simple Vectis
wrappers for lifecycle and owner-thread control. A host that supplies a
`vectis_smith_store` must retain its LockDC client and store until all
borrowing runtimes have closed. A host that needs another CAI session backend
can supply `runtime.session_store` directly instead.

Lua keeps the same ownership boundary. `vectis.smith` delegates to CAI's native
runtime and exposes no terminal UI. Lua applications may supply CAI's typed
session-store handle; the Vectis CLI uses the C LockDC adapter for its
durability.
