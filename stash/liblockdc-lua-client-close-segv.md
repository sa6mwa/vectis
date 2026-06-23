# liblockdc Lua client crashes on real open/close

## Summary

Vectis embeds the Lua binding sources from `lockdc-0.12.1-1.src.rock` and
links them against the `liblockdc` 0.12.1 SDK archive. The binding loads and
pure helpers work, but a real `lockdc.open()` against the compose lockd service
crashes the `vectis` process when the Lua client is closed. Real client methods
also crash before returning useful Lua errors.

This is why the current Vectis e2e suite did not catch the issue: the suite has
compose-backed lockd integration coverage for C examples and Kore flows, and it
has Lua runner/module smoke coverage, but it does not currently run lockdc Lua
client operations against the compose lockd services.

## Versions

- Vectis dependency manifest expects `liblockdc` 0.12.1.
- Lua binding source archive: `lockdc-0.12.1-1.src.rock`
- Vectis embeds `share/lockdc-source/src/lua/lockdc_lua.c`.
- The failing binary is the Vectis Lua runner built by `make build`.

## Minimal Reproducer

Start the normal Vectis compose e2e environment:

```sh
make test-e2e
```

During or after the e2e services are up, run this script with the debug
`vectis` binary:

```lua
local lockdc = require("lockdc")

local client, err = lockdc.open({
  endpoints = { os.getenv("LOCKD_ENDPOINT") },
  client_bundle_path = os.getenv("LOCKD_CLIENT_BUNDLE"),
  default_namespace = "examples",
})

assert(client, err and err.message or "open failed")
client:close()
```

Command:

```sh
LOCKD_ENDPOINT=https://127.0.0.1:29441 \
LOCKD_CLIENT_BUNDLE=devenv/volumes/lockd-config/client.pem \
build/debug/vectis /tmp/lockdc-open-close.lua
```

Observed result:

```text
Segmentation fault (core dumped)
```

The same happens if the script calls `client:info()` before `client:close()`.
The same also happens with the ASAN binary as a fatal signal rather than a Lua
error.

## Backtrace

Representative gdb backtrace from `build/debug/vectis`:

```text
Program received signal SIGSEGV, Segmentation fault.
0x0000000006400000 in ?? ()
#0  0x0000000006400000 in ?? ()
#1  0x00005555557b11c2 in lc_engine_client_close ()
#2  0x00005555557c1b17 in lc_client_close_method ()
#3  0x00005555555baabb in lcdc_client_gc (L=0x555555a09ae8)
    at deps/share/lockdc-source/src/lua/lockdc_lua.c:674
#4  0x00005555555bb180 in lcdc_client_close (L=0x555555a09ae8)
    at deps/share/lockdc-source/src/lua/lockdc_lua.c:803
#5  0x00005555558e4857 in luaD_precall ()
#6  0x00005555558f6062 in luaV_execute ()
#7  0x00005555558e4c48 in luaD_callnoyield ()
#8  0x00005555558e37b0 in luaD_rawrunprotected ()
#9  0x00005555558e5072 in luaD_pcall ()
#10 0x00005555558e0d1c in lua_pcallk ()
#11 0x0000555555707731 in cpkt_lua_runtime_call_with_status ()
#12 0x000055555570843f in cpkt_lua_runtime_run_file ()
#13 0x000055555569f158 in vectis_lua_run_script (...)
#14 0x000055555569f6ee in vectis_cli_main (...)
#15 0x000055555569f767 in pid0_run (...)
#16 0x00005555556a04a2 in main (...)
```

ASAN reproducer:

```text
AddressSanitizer:DEADLYSIGNAL
==3866535==ERROR: AddressSanitizer: SEGV on unknown address 0x000006400000
==3866535==The signal is caused by a READ memory access.
AddressSanitizer: nested bug in the same thread, aborting.
```

## Additional Observations

- The existing C lockd examples using the same compose service and bundle pass:
  `vectis_example_lockd_open`, `vectis_example_lockd_lease`, and queue flows.
- The crash is not caused by the new Lua example scripts. A minimal
  `lockdc.open(...); client:close()` is enough.
- A cache-only experiment that kept the Lua binding's endpoint array alive after
  `lc_client_open()` did not fix the crash.
- A cache-only experiment that zeroed `lc_client_config` before
  `lc_client_config_init()` did not fix the crash.
- `lockdc` Lua module smoke coverage currently only verifies load/pure helpers:
  `lockdc.open` is a function, `version_string()` returns a string, and JSON
  encode/decode helpers work.

## Test Gap In Vectis

`scripts/test-e2e.sh` currently has:

- Lua runner/shebang/pack smoke tests.
- A libmdf Lua example smoke.
- C lockd examples against disk and S3 lockd compose services.
- Kore lockd API/workflow tests.

It does not have a lockdc Lua client e2e test. Adding such a test immediately
exposes this crash, but keeping that test enabled would make the current Vectis
e2e gate fail until the lockdc Lua/runtime issue is fixed.

## Expected Behavior

The Lua binding should either:

- open and close a real client successfully, or
- return a structured Lua error from `lockdc.open()` / client methods.

It must not crash the embedding process.
