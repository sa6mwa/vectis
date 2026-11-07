# Self-contained documentation and Lua source

The `vectis` binary embeds the curated public manual and public Lua facade
source that shipped with that exact build. This is intended for offline
inspection and for coding agents that begin with only a Vectis binary.

`vectis -a docs` writes the embedded manual to standard output.
Each document begins with a visible Markdown blockquote that records its
embedded path and SHA-256 digest. This keeps the output Markdown-only while
preserving the provenance needed to inspect the exact shipped documentation.

`vectis -a docs --pager` views that manual on a terminal. It borrows a seekable
view of the embedded bytes and headings; it does not concatenate or spool the
manual. MDF reads on demand with its default 1 MiB rendered-content cache and
separately bounded search/index workspace. Backward cache misses and resize may
replay source bytes. Quit restores terminal state; a returning SIGINT handler
produces cancellation and exit status 130.

`vectis -a source` lists available public Lua source modules. The list contains
the module name, origin, embedded path, and SHA-256 digest. Select source with
either `--all`, one or more exact `--module NAME` options, or one or more exact
embedded module names or resource paths:

```sh
vectis -a source --module vectis --module vectis.webdav
vectis -a source vectis.auth vectis.cai vectis.dsv lockdc libmdf
vectis -a source vectis/auth.lua
vectis -a source --all --output-dir lua-source
vectis -a source --module lockdc --output lockdc.lua
```

Selected source writes to standard output by default. Concatenated output is an
inspection transcript: every file is preceded by Lua comments that identify
the embedded path, module, origin, and digest. It is not an executable combined
Lua program. `--output` writes one selected module as its original raw bytes;
`--output-dir` writes selected raw files under their embedded module paths.
Neither output mode overwrites an existing file unless `-f` or `--force` is
given. `docs` has no output path to force; it writes stdout or uses `--pager`.

The source bundle contains Lua implementation and facade files only. Native
C-backed modules remain documented but their implementation source is not
included. `vectis.kore` and `vectis.version` are included as the standalone
Lua fallback sources; an embedded Vectis runtime supplies those modules from
native code.

# Tracing Lua execution

Use `vectis -x script.lua [args]` to print executed source locations to stderr,
similar to shell tracing. For a packed Linux app, use
`./packed-app --vectis -x [args]`. Script stdout, arguments and failure status
are preserved. A plain packed `-x` is passed to the application.

Records look like `+ script.lua:12`. They identify locations, not variable
values or expanded statements. Coroutine execution, Lua close handlers and
finalizers are included; native C execution and stripped chunks have no line
events. Long source identifiers are limited to 160 bytes with an ellipsis
(file paths retain their suffix), and control bytes become `?`.

The cpkt runtime's public C89 observer owns event delivery and retains its
instruction-limit policy. Tracing does not expose a Lua hook into that policy.

# Restoring a packed executable

On platforms where packed executables are supported, running `--vectis unpack` on a
packed Vectis executable reconstructs a runnable, generic release layout:

```sh
./packed-vectis --vectis unpack --output-dir restored
./restored/vectis ./restored/app.lua
```

The default output directory is the current directory. The operation writes
`vectis`, `app.lua`, an optional `lockd-bundle.pem`, an optional `assets.json`,
and an `assets/` tree that preserves the packed virtual asset paths. The app
script and assets are the packed bytes, and the generic binary is the exact
runner prefix from the packed executable.

Unpack validates every pack hash before writing and refuses to overwrite an
existing output by default. Use `-f` or `--force` only when intentionally
replacing the regular output files and refreshing packed assets:

```sh
./packed-app --vectis unpack --force
```

Force still rejects symlinks and non-regular file outputs. It does not restore
the original host paths used as pack inputs; those are intentionally absent
from the pack format.
