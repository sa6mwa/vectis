# Production Webserver Hardening

Vectis now has the landed-style production webserver surface needed for both
libvectis embedders and the Lua-controlled `vectis` binary workflow, without
carrying landed/taktiv.se-specific C bindings, layouts, stats pages, or site
assets.

## Contract

Production webserver mode is opt-in. It is enabled by:

- C: `vectis_app_config_init_production_webserver()`
- Lua: `vectis.server.new({profile = "production_webserver"})`

The profile applies production defaults to the C-owned runtime surface:

- strict quiescence before routes dispatch;
- fail-closed app-owned service startup and failure behavior;
- one shared graceful-shutdown deadline across Kore, metrics, lockdc consumer
  services, and managed workers;
- explicit request-body guardrails;
- conservative autoblock defaults for repeated auth/status, TCP-stall, and
  TLS-failure events.

The profile deliberately does not auto-enable routes, auth, metrics, WebDAV,
static mounts, TLS material, ACME, or persistence. Those remain explicit
application registrations so a Vectis service cannot silently expose a surface
because a profile was selected.

## Implemented Surfaces

The production webserver shape includes:

- C and Lua production profile initialization and validation;
- Lua server profile selection with invalid-profile rejection;
- shared native auth providers usable by ordinary routes, metrics, WebDAV, and
  callback adapters;
- generic metrics/stats JSON and HTML routes, disabled by default, with optional
  shared-route auth and optional lockdc/pouch persistence;
- static directory serving for extracted packed assets;
- mutable WebDAV fileserver mounts and read-only WebDAV mounts over embedded
  packed assets;
- packed Lua webserver execution with generated assets, auth, email-token login,
  WebDAV key issuance, WebDAV reads/writes/list/copy/move/delete, traversal
  denial, and embedded-vs-mutable storage isolation;
- packed service execution where Kore HTTP/WebDAV serving and a lockdc
  `startconsumer` service run in the same self-contained Vectis binary/process;
- service lifecycle and signal-shutdown coverage for graceful termination.

## Evidence

The current executable coverage includes:

- `tests/unit/test_vectis_config.c` for C production profile defaults and app
  construction;
- `tests/lua/smoke.lua` for Lua production profile selection and invalid
  profile errors;
- `vectis_lua_http` for authenticated metrics JSON/HTML routes and pouch
  snapshot persistence;
- `vectis_example_lua_metrics_persistent`,
  `vectis_example_lua_metrics_authenticated`, and
  `vectis_example_lua_metrics_ephemeral` for user-facing metrics examples;
- `vectis_lua_pack` for packed webserver/auth/WebDAV/static/service scenarios;
- `vectis_example_lua_webdav_fileserver` and
  `vectis_example_lua_webdav_fileserver_pack` for mutable WebDAV server DX;
- `vectis_server_signal_shutdown` and `test-service-runtime-lifecycle` for
  termination/lifecycle contracts.

The normal slice gate is `make finalize-slice`, which includes the lifecycle
runtime audit and the relevant deterministic Lua/example coverage.

## Remaining Non-Blocking Work

The production webserver hardening item is considered implemented. Remaining
work tracked elsewhere is not part of this closure:

- broader Kore feature expansion such as WebSocket and deeper runtime config;
- certificate/key reload hooks where Kore supports them;
- broader Lua facade coverage audits;
- Darwin/Mach-O pack and codesign support;
- full cross-target release matrix verification.
