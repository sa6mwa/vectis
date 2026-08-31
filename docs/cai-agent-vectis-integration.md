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

`vectis -a smith` opens CAI's `smith` runtime on its owner thread. CAI events
copy response text into a bounded producer/consumer queue. A libmdf streaming
renderer consumes that queue on a renderer thread and produces bounded ANSI
chunks. Softline, on its owner thread, prints those chunks above the active
chat prompt. The renderer is streaming end to end; it does not concatenate a
whole model response, use a spool file, or render after completion.

Softline's idle callback pumps CAI while the prompt remains usable. Input sent
while CAI is active is steering; a Softline-queued input becomes CAI's next
FIFO turn. The corresponding direct C operations are
`vectis_smith_submit_steering()` and `vectis_smith_submit_queued()`.

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
