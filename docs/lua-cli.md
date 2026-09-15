# Lua command-line applications

`require("vectis.cli")` provides a small declarative command-line parser for
Lua applications run by Vectis. The application owns its interface: Vectis
passes every argument after a source script path unchanged, and packed
applications receive all ordinary arguments unchanged.

```lua
local cli = require("vectis.cli").new({
  summary = "Deploy reports",
  version = "1.0.0",
})

cli:option({
  long = "config", short = "c", value = "FILE", required = true,
  description = "Configuration file",
})

local deploy = cli:command("deploy", {
  summary = "Deploy a report",
  run = function(args)
    print(args.options.config)
  end,
})
deploy:flag({long = "dry-run", description = "Validate only"})

cli:main(arg)
```

The parser supports commands, a configured `default_command`, positional
arguments, flags, typed options (`string`, `integer`, and `number`), defaults,
required values, repeatable values, choices, and `--` to end option parsing.
Short options accept `-v`, flag groups such as `-vv`, and attached values such
as `-cFILE`. Long options accept both `--config FILE` and `--config=FILE`.
Long-option abbreviations are intentionally unsupported.

`cli:parse(argv)` returns either a structured result or `nil, message`; it is
appropriate when the application owns error presentation. `cli:main(arg)` is
the normal entry point: it generates root or command help for `-h`/`--help`,
prints the configured version for `--version`, dispatches the command `run`
callback, and exits with status 64 for usage errors.

Use `result.command`, `result.options`, and `result.arguments` in callbacks.
Option keys default to the long name with hyphens converted to underscores.

## Vectis runtime commands

For a source script, Vectis options precede the script path:

```sh
vectis -v app.lua --config production deploy --dry-run
```

For a packed executable, every normal option—including `--help`, `--version`,
`-v`, and `-a`—belongs to the application:

```sh
./report-tool --config production deploy --dry-run
./report-tool --help
```

Use the explicit `--vectis` namespace to invoke a runner action. It is
available from both the generic runner and a packed application, and is
evaluated before Lua starts:

```sh
vectis --vectis docs
vectis --vectis --action pack --help
./report-tool --vectis unpack --output-dir restored
./report-tool --vectis docs
./report-tool --vectis -v smith
```

The invocation forms are deliberately distinct:

| Invocation | Owner |
| --- | --- |
| `vectis -a ACTION ...` or `vectis --action ACTION ...` | Generic Vectis runner |
| `vectis --vectis [VERBOSITY] ACTION ...` | Generic Vectis runner |
| `vectis [VECTIS OPTIONS] SCRIPT [APPLICATION ARGUMENT ...]` | Vectis owns only options before `SCRIPT`; the script owns the rest |
| `PACKED_APP [APPLICATION ARGUMENT ...]` | Packed Lua application |
| `PACKED_APP --vectis [VERBOSITY] ACTION ...` | Packed executable's Vectis runner |
| `PACKED_APP --vectis -a ACTION ...` or `PACKED_APP --vectis --action ACTION ...` | Packed executable's Vectis runner |

For a packed app, `--vectis` is reserved only when it is the first argument.
Use `--` with `vectis.cli` when the application needs a literal `--vectis` as
its first option or positional argument; the parser consumes the delimiter.

```sh
./report-tool -- --vectis
```

Without `vectis.cli`, the Lua script receives the conventional `--` delimiter
as well as the following literal argument.
