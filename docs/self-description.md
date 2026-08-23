# Self-contained documentation and Lua source

The `vectis` binary embeds the documentation and public Lua facade source
that shipped with that exact build. This is intended for offline inspection and
for coding agents that begin with only a Vectis binary.

`vectis -a docs` writes the complete documentation bundle to standard output.
Each document begins with an HTML comment that records its embedded path and
SHA-256 digest.

`vectis -a source` lists available public Lua source modules. The list contains
the module name, origin, embedded path, and SHA-256 digest. Select source with
either `--all` or one or more exact `--module NAME` options:

```sh
vectis -a source --module vectis --module vectis.webdav
vectis -a source --all --output-dir lua-source
vectis -a source --module lockdc --output lockdc.lua
```

Selected source writes to standard output by default. Concatenated output is an
inspection transcript: every file is preceded by Lua comments that identify
the embedded path, module, origin, and digest. It is not an executable combined
Lua program. `--output` writes one selected module as its original raw bytes;
`--output-dir` writes selected raw files under their embedded module paths.
Neither output mode overwrites an existing file.

The source bundle contains Lua implementation and facade files only. Native
C-backed modules remain documented but their implementation source is not
included. `vectis.kore` and `vectis.version` are included as the standalone
Lua fallback sources; an embedded Vectis runtime supplies those modules from
native code.

# Restoring a packed executable

On platforms where packed executables are supported, running `-a unpack` on a
packed Vectis executable reconstructs a runnable, generic release layout:

```sh
./packed-vectis -a unpack --output-dir restored
./restored/vectis ./restored/app.lua
```

The default output directory is the current directory. The operation writes
`vectis`, `app.lua`, an optional `lockd-bundle.pem`, an optional `assets.json`,
and an `assets/` tree that preserves the packed virtual asset paths. The app
script and assets are the packed bytes, and the generic binary is the exact
runner prefix from the packed executable.

Unpack validates every pack hash before writing and never overwrites output
files or the `assets` directory. It does not restore the original host paths
used as pack inputs; those are intentionally absent from the pack format.
