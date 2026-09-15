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
  long = "config", short = "c", value = "FILE", env = "REPORT_CONFIG",
  required = true,
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

An option or flag can declare an explicit environment binding with `env`. Its
value is resolved in this order: an explicit command-line option, the declared
environment variable, then the declaration's default. Environment values use
the option's normal type and choice validation. The parser never guesses an
environment-variable name, so applications can use any explicit name such as
`REPORT_CONFIG` or `LOG_LEVEL`.

```lua
cli:option({
  long = "log-level", short = "l", value = "LEVEL", env = "LOG_LEVEL",
  default = "info", description = "Application log level",
})
cli:flag({long = "debug", env = "REPORT_DEBUG"})
```

For flags, environment values must be `true`, `false`, `1`, `0`, `yes`, `no`,
`on`, or `off` (case-insensitive). An invalid or empty configured value is a
usage error. A repeatable option receives one environment-derived value; any
explicit command-line occurrence replaces that fallback before later command
line values are appended. Generated help identifies each environment binding.
`vectis.cli` only resolves application options: it never configures libpslog or
any other subsystem implicitly.

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

Use the explicit `--vectis` namespace to invoke a runner action from a packed
application. It is evaluated before Lua starts:

```sh
./report-tool --vectis unpack --output-dir restored
./report-tool --vectis docs
./report-tool --vectis -v smith
```

The invocation forms are deliberately distinct:

| Invocation | Owner |
| --- | --- |
| `vectis -a ACTION ...` or `vectis --action ACTION ...` | Generic Vectis runner |
| `vectis [VECTIS OPTIONS] SCRIPT [APPLICATION ARGUMENT ...]` | Vectis owns only options before `SCRIPT`; the script owns the rest |
| `PACKED_APP [APPLICATION ARGUMENT ...]` | Packed Lua application |
| `PACKED_APP --vectis [VERBOSITY] ACTION ...` | Packed executable's Vectis runner |

For a packed app, `--vectis` is reserved only when it is the first argument.
The packed action is always direct: `PACKED_APP --vectis docs`, not
`PACKED_APP --vectis -a docs`. In contrast, `-a` and `--action` remain
application arguments in every ordinary packed invocation.

The generic runner rejects `--vectis` with an actionable error; use `-a` or
`--action` for its management commands.

Use `--` with `vectis.cli` when the application needs a literal `--vectis` as
its first option or positional argument; the parser consumes the delimiter.

```sh
./report-tool -- --vectis
```

Without `vectis.cli`, the Lua script receives the conventional `--` delimiter
as well as the following literal argument.
