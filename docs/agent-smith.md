# Agent Smith

Vectis ships CAI 0.5's `smith` coding-agent preset as a Vectis-hosted product
surface. CAI owns the agent loop, model protocol, tool semantics, review
subagents, and session journal contract. Vectis owns the command-line
experience, LockDC/Pouch persistence, workspace boundary, and C/Lua facades.

## Command Line

Start an interactive session in the current directory:

```sh
vectis -a smith
```

Use `-w DIR` to select the workspace and `-s ID` to resume or create a named
session. The terminal starts in that workspace and its requested working
directory cannot escape it. It has CAI's fixed system `PATH`
(`/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`), so ordinary
commands such as `/bin/sh`, `make`, and `git` remain available. It does not
inherit the Vectis process environment, credentials, or per-user `PATH` entries
such as `~/.local/bin`.

CAI 0.5's managed Smith terminal is not Bubblewrap-contained: the workspace is
its working-directory constraint, not a filesystem sandbox. CAI's separately
registered `exec_command` tool does use Bubblewrap on Linux and fails closed if
it is unavailable. Do not use the Smith terminal against an untrusted host or
where commands must be prevented from accessing files outside the workspace.

The default interactive presentation is Softline's normal chat theme. Output
is streamed from CAI in bounded chunks into libmdf's streaming ANSI renderer
and printed above the live prompt; it is never assembled into a complete
response first. While a turn is active, ordinary input steers the current run
and input submitted with Softline's queue command (Tab) is retained as the
next turn. `:quit` and `:exit` leave the editor.

For one finished turn without a TUI:

```sh
vectis -a smith -s release-notes -e 'Summarize the current changes.'
```

This still streams output, but exits after CAI finishes that turn.

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
- `vectis_smith_open()` creates an owner-thread CAI Smith runtime and exposes
  `submit`, `submit_steering`, `submit_queued`, `pump`, `state`, and
  `wakeup_fd` wrappers.

Applications pass either a borrowed `cai_client` or `cai_client_config`, and
provide a workspace in `vectis_smith_config.runtime.workspace_directory`.
Passing `store` installs its LockDC session store; it is intentionally
exclusive with a caller-supplied CAI session store. The caller drives `pump`
from the runtime owner thread and closes the runtime before destroying its
store or LockDC client.

## Lua

`require("vectis.smith")` supplies the matching non-TUI CAI facade:
`open`, `with_runtime`, `submit`, `steer`, `queue`, `pump`, `state`, and
`session_id`. It accepts a native typed CAI `session_store` where Lua needs
durability. The terminal interface belongs only to `vectis -a smith`; Vectis
does not create a second TUI abstraction for Lua applications.
