# Agent Smith

Vectis ships CAI 0.5's `smith` coding-agent preset as a Vectis-hosted product
surface. CAI owns the agent loop, model protocol, tool semantics, review
subagents, and session journal contract. Vectis owns the command-line
experience, LockDC/Pouch persistence, workspace boundary, and C/Lua facades.

## Modes

Smith has two distinct products. They must not be described as interchangeable.

### Exec mode

Exec mode is non-interactive command-line execution. It submits one prompt,
streams its response to the invoking terminal, and exits after that turn:

```sh
vectis -a smith -s release-notes -e 'Summarize the current changes.'
```

It has no live editor or local prompt queue. It remains useful for scripts and
one-shot terminal work.

### Interactive TUI mode

Interactive mode is the full coding-agent terminal UI:

```sh
vectis -a smith
```

It has a non-negotiable liveness contract:

- The user can always type, edit, paste, queue, edit queued messages, issue
  supported commands, interrupt, and submit steering input. An active model
  request, tool call, renderer, checkpoint, or any other agent activity never
  takes ownership of the editor or delays input processing.
- CAI runs on a dedicated owner thread. Softline runs only on the UI thread.
  No worker, including CAI and libmdf, may call a Softline receiver directly.
- CAI text events flow immediately through libmdf's real streaming renderer to
  the live Softline prompt. Output is bounded-chunk producer-to-consumer flow;
  it is never held until a turn completes or materialized as a full response.
- While CAI has an active turn, ordinary Enter submissions stay in Softline's
  local FIFO queue. When CAI reaches a terminal state, Smith submits exactly
  the oldest local queued turn and repeats until the local queue is empty.
- A steering submission is delivered to CAI's active turn immediately. A
  steering action on an empty editor promotes the newest local queued message
  and delivers it as steering. If CAI has become idle before it is accepted,
  the CAI owner submits it as the next normal turn instead; it is never lost.
- The Softline status bar is a UI-thread projection of the latest available
  agent/runtime state: run state, model/tool activity, queue state, stream and
  renderer failures, and other relevant bounded status. It is refreshed when
  the relevant subsystem signals a state change, not only after the next key.

Use `-w DIR` to select the workspace and `-s ID` to resume or create a named
session. The terminal starts in that workspace and its requested working
directory cannot escape it. It has CAI's fixed system `PATH`
(`/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`), so ordinary
commands such as `/bin/sh`, `make`, and `git` remain available. It does not
inherit the Vectis process environment, credentials, or per-user `PATH` entries
such as `~/.local/bin`.

CAI's managed Smith terminal is not Bubblewrap-contained: the workspace is
its working-directory constraint, not a filesystem sandbox. Vectis uses this
CAI terminal behavior unchanged. CAI's separately registered `exec_command`
tool does use Bubblewrap on Linux and fails closed if it is unavailable.

`:quit` and `:exit` leave the editor after a defined shutdown policy has
stopped/drained the agent and renderer.

### Current implementation status

The current `vectis -a smith` loop is **not yet compliant interactive TUI
mode**. It pumps CAI only from Softline's idle callback on the UI thread, so
typing can delay CAI work and output. It must not be called a complete coding
agent UI. The required cutover is documented in
[CAI Agent And Vectis Integration](cai-agent-vectis-integration.md); it is
blocked on Softline's required external-event and queue-control APIs.

Smith first opens CAI's persisted ChatGPT subscription authentication state.
CAI uses `CAI_CHATGPT_AUTH_JSON` when set, otherwise its XDG state file. If no
ChatGPT state is available, Smith falls back to `OPENAI_API_KEY`. Vectis does
not copy OAuth refresh tokens into its LockDC store.

## Durable sessions

The CLI persists Smith checkpoints and ordered steering/queued-turn events in
LockDC. Its default endpoint is:

```text
pouch://$XDG_STATE_HOME/vectis/smith?single_writer=false
```

When `XDG_STATE_HOME` is unset, it uses
`$HOME/.local/state/vectis/smith`. Pouch encryption follows LockDC's normal
per-user key-file handling. Override the store deliberately when a deployment
needs shared or remote state:

```sh
vectis -a smith --state-endpoint 'https://lockd.example/v1' \
  --state-namespace engineering -s triage
```

The session scope is CAI's opaque workspace identity. A checkpoint includes a
watermark; Vectis replays only the strictly later durable events, in sequence,
when a session resumes. The LockDC adapter serializes updates with a short
lease. It is valid for use by multiple CAI runtimes as long as the caller keeps
the `lc_client` and `vectis_smith_store` alive for every borrowing runtime.

## C API

`include/vectis/vectis.h` exposes two layers:

- `vectis_smith_store_new()` creates the LockDC-backed
  `cai_agent_session_store` adapter.
- `vectis_smith_open()` creates the owner-thread CAI Smith runtime and exposes
  `submit`, `submit_steering`, `submit_queued`, `pump`, `state`, and
  `wakeup_fd` wrappers for non-TUI hosts and the future dedicated Smith worker.

Applications pass either a borrowed `cai_client` or `cai_client_config`, and
provide a workspace in `vectis_smith_config.runtime.workspace_directory`.
Passing `store` installs its LockDC session store; it is intentionally
exclusive with a caller-supplied CAI session store. The caller drives `pump`
from the runtime owner thread and closes the runtime before destroying its
store or LockDC client. The interactive UI is a host of this API, not a reason
to weaken its owner-thread contract.

## Lua

`require("vectis.smith")` supplies the matching non-TUI CAI facade:
`open`, `with_runtime`, `submit`, `steer`, `queue`, `pump`, `state`, and
`session_id`. It accepts a native typed CAI `session_store` where Lua needs
durability. The terminal interface belongs only to `vectis -a smith`; Vectis
does not create a second TUI abstraction for Lua applications.
