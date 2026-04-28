# vectis

`vectis`, or `veCtis`, is an unfinished SDK and runtime for building small,
workflow-driven services in C and Lua. The target is an API service stack that
can expose HTTP/TLS endpoints, serialize and deserialize JSON, coordinate state
through lockd, enqueue and consume queue work, call downstream APIs, move files
over SFTP, run remote SSH commands, and emit structured operational logs from
one coherent developer experience.

The project has two deliverables:

- A C SDK installed as headers plus a static or shared `libvectis`, including
  the dependency headers needed by downstream C applications.
- A `vectis` executable that runs Lua scripts with the Vectis framework and all
  provided native bindings available to the script.

The Lua executable is intended to support scripts with a shebang such as:

```lua
#!/usr/local/bin/vectis
local vectis = require("vectis")
```

The end state is that a developer can write a REST API, use lockd for document
storage, retrieval, queries, leases, queue workflows, and coordination, call
external HTTP APIs, store or retrieve SFTP files, run SSH commands, and handle
JSON and structured logging without assembling a separate C/Lua runtime stack by
hand.

## Runtime Model

`vectis` integrates these components:

- Kore provides the HTTP/TLS server runtime, routing boundary, process model,
  ACME support, and web-server features.
- `liblockdc` provides the lockd client and lockd Lua binding for durable state,
  queues, leases, and coordination.
- `lonejson` provides JSON parsing and serialization for C and Lua.
- `libpslog` provides structured logging for C and Lua.
- libcurl provides HTTP(S), SFTP, and other URL-oriented client operations.
- OpenSSL provides TLS and certificate handling.
- libssh2 provides SSH command execution and lower-level SSH/SFTP capability.

The C SDK should present these as one stable surface: one application object,
one configuration layer, one logger, one lockd configuration, one TLS/certificate
configuration story, one HTTP server boundary, and coherent helper APIs for JSON,
queue, downstream HTTP, SFTP, and SSH workflows.

Lockd is configured when the service needs it, but it is not mandatory for
Kore-only services. If no lockd endpoint or Unix socket is configured, Vectis
starts as an HTTP/TLS service without an app-owned lockd client. If TCP lockd
transport is configured, Vectis requires client bundle material from path,
`lc_source`, or memory.

The HTTP server boundary is defensive by default. `vectis_app_config_init()`
sets explicit guardrails for maximum concurrent connections, request-header
size, default request-body size, header-read deadline, body-read idle timeout,
minimum body transfer rate, response-write idle timeout, idle timeout,
keepalive timeout, and maximum requests per keepalive connection. These
defaults are intentionally part of the Vectis SDK surface so Kore integration
cannot accidentally inherit unsafe runtime defaults later.

Large request bodies are route policy, not global server policy. Normal API
routes default to a bounded body shape suitable for JSON. File upload routes can
opt into streaming/spool-to-disk behavior with `vectis_body_upload()` or set an
explicit cap with `vectis_body_upload_max(bytes)`, so accepting a multi-GB
upload does not require raising the default limit for every route.
For the common case, `vectis_upload_route()` creates a route with that streaming
upload policy already attached.

The dependency headers and libraries are shipped intentionally. C users should
be able to include and use Kore, `liblockdc`, `lonejson`, `libpslog`, libcurl,
OpenSSL, and libssh2 directly when Vectis does not cover a case or when they
need exact low-level control. That raw access is an escape hatch, not the main
experience. The primary C SDK should be Vectis-owned and service-oriented.

The Lua runtime should mirror that same model. Lua scripts should have raw
module access where useful, but the primary DX should expose Vectis-owned
helpers with the same naming, defaults, error behavior, and workflow shape as
the C SDK:

- configure certificates and lockd once,
- generate keys, CSRs, certificates, and client/server bundles through a Vectis
  certificate workflow backed by OpenSSL,
- start a Kore-backed API server,
- register route or method handlers,
- parse and emit JSON through `lonejson`,
- acquire/release coordination locks and load/save/query documents through
  lockd,
- enqueue follow-up work and run lockd consumer services,
- send JSON API requests and download/upload files through curl,
- run remote SSH commands through libssh2,
- run a separate consumer process when queue processing needs a blocking
  consumer loop.

Kore startup/configuration, lockd consumer service control, curl handles,
OpenSSL primitives, and libssh2 sessions may be exposed directly, but Vectis
should provide the friendly path on top for normal API-service work.

Vectis route paths should support both common named path parameters such as
`/orders/:id/items/:item_id`, optional named parameters such as
`/orders/:id?`, and explicit Kore/POSIX regex routes. Named
parameters are a Vectis-owned convenience layer and must be validated as whole
path segments with safe parameter names. Regex routes are an escape hatch for
Kore-compatible matching and are kept separate from named parameter extraction.
Vectis mirrors Kore's HTTP method set: `GET`, `POST`, `PUT`, `DELETE`, `HEAD`,
`OPTIONS`, and `PATCH`. A route can be registered for one method or for a method
mask, so common cases such as `GET | HEAD` can share one handler without
duplicating route declarations.

## Concurrency Boundary

Lua is single-threaded in the current dependency stack. Until Vectis owns a real
Lua thread/runtime manager, a single Lua process must not try to run both the
Kore server loop and a blocking `liblockdc` consumer service loop.

The supported model is:

- API process: run Kore through `vectis`, perform normal lockd operations, and
  use dequeue explicitly inside handlers where appropriate.
- Worker process: run a separate Vectis Lua script as a lockd consumer service.
- Async workflow: API handlers enqueue lockd queue messages; worker processes
  consume and process them.

This boundary should be reflected in both the C API and Lua DX. If a future
runtime manager changes the constraint, it should be a deliberate new design,
not an accidental side effect.

## Single Binary Services

Vectis should support a packaging mode that combines the `vectis` runner, one
Lua script, and optionally a liblockdc certificate bundle into a single output
binary.

The preferred implementation is a self-describing trailer appended to the Vectis
ELF binary:

- a fixed magic value and format version,
- offsets and lengths for embedded Lua and optional certificate bundle payloads,
- hashes for integrity checks,
- metadata needed by the runner to decide whether to execute an embedded script
  or a script path from argv.

Embedded lockd client bundles should be handed to liblockdc through its
`lc_source`-based in-memory or callback bundle loading APIs, rather than being
materialized to temporary files. Runtime file materialization should be reserved
for dependencies that still require path-only configuration.

The same rule applies to Vectis/Kore TLS material. Server cert/key bundles,
split certificate/private-key material, CA bundles, and client-CA bundles for
mTLS verification should be configurable from paths, borrowed sources, or
in-memory PEM bytes so packed services do not need to unpack certificates before
startup.

ACME mode defaults to the Let's Encrypt production directory
`VECTIS_ACME_DIRECTORY_LETSENCRYPT_PRODUCTION` unless the application overrides
`tls.acme_directory_url`.

Appending data to an ELF executable is technically viable on Linux because the
loader uses the ELF program headers rather than requiring end-of-file to match
the last loaded segment. The implementation still needs to be conservative:
validate all trailer fields, never scan unbounded data, fail closed on malformed
payloads, preserve normal `vectis script.lua` execution, and document that some
signing, packaging, or hardening tools may strip or reject appended data. If that
becomes a deployment issue, the fallback is an ELF section or generated object
linked into a new runner binary.

## Dependency Baseline

The current baseline is `liblockdc` 0.3.0. Its SDK archives include the C
headers and libraries for lockdc, pslog, lonejson, curl, OpenSSL, libssh2,
nghttp2, and zlib. The 0.3.0 release also ships the `lockdc` Lua rock. The Lua
rocks for `libpslog` 0.3.1 and `lonejson` 0.4.1 are available separately, and
their C headers/libraries are also included in the `liblockdc` 0.3.0 SDK
archives. The 0.3.0 lockdc client API supports flexible client bundle sourcing,
which is the required basis for packed Vectis services with embedded lockd
certificate/key material.

Vectis owns the Lua dependency for the `vectis` executable. The dependency
provisioning step downloads pinned Lua 5.5.0 from lua.org, verifies its
SHA-256, builds a target-specific static `liblua.a`, and installs the Lua
headers into the same `.cache/deps/<target>` root as the liblockdc bundle. The
C SDK does not make downstream C consumers depend on Lua by default; Lua is a
runtime dependency of the Vectis binary and the Lua module build.

The Vectis binary runtime must not depend on host Lua, LuaRocks, runtime
`LUA_PATH`/`LUA_CPATH`, or dynamically discovered Lua `.so` modules. Bundled Lua
modules should be compiled against the provisioned Lua 5.5 ABI and registered
statically through `package.preload`. LuaRocks is still useful for distribution:
Vectis should publish a thin `vectis` rock for users who want to run the Vectis
facade inside their own Lua 5.5 environment, but that rock is not part of the
self-contained `vectis` binary runtime contract.

The bindings still missing from Vectis are:

- Kore Lua bindings and the high-level Kore/Vectis Lua web framework.
- libcurl Lua bindings with JSON-friendly request/response helpers.
- OpenSSL Lua bindings for certificate and TLS configuration workflows.
- libssh2 Lua bindings for remote command execution and lower-level SSH/SFTP
  operations where curl is not enough.

## Current State

The current implementation provides:

- The initial C `vectis` runtime/config API and method-table surface.
- Draft C SDK helper surface for curl-backed HTTP/SFTP/MQTT, libssh2 command
  execution, OpenSSL certificate bundle generation, JSON request/response
  handling, and JSON-aware route registration. The current C helpers are
  dependency-backed and covered by unit/runtime smoke tests where local
  verification is practical.
- A common `vectis_source` input model for path, `lc_source`, and in-memory
  bytes across lockd client bundles, Kore TLS material, downstream HTTP/MQTT
  client bundles, and SSH/SFTP private-key inputs.
- A first-pass `vectis_server_config` with explicit DDoS/slow-client guardrails
  for connection counts, request sizes, keepalive behavior, idle read/write
  timeouts, and minimum request-body transfer rate.
- A real embedded Kore runtime path for C routes, including HTTP smoke coverage,
  request metadata mapping, body-size guardrails, `pslog` runtime logging, and
  manual HTTPS startup from path, memory, or `lc_source` cert/key material.
- Per-route request-body policy presets for no-body routes, JSON/buffered
  bodies, and streaming large uploads.
- Large upload routes can receive Kore-spooled request bodies through
  `vectis_request_body_path()` instead of forcing multi-GB uploads into memory.
- OpenSSL-backed self-signed, CA, and CA-signed client/server PEM bundle
  generation plus validation helpers for bundles and split cert/key material.
- Optional lockd configuration, so Kore-only examples and services do not need
  placeholder lockd sockets.
- Route constructors for literal paths, named and optional path parameters, and
  explicit regex routes, with automatic path-kind inference for the common
  `vectis_route()` and `vectis_json_route()` cases.
- A first pass at a dependency-aware error model that records Vectis status,
  error source, dependency code, HTTP status, summary message, and detail text.
- Stable string helpers for statuses, error sources, HTTP methods, and body
  modes so examples and applications do not grow ad hoc name tables.
- A handle-shaped HTTP client API that can be created directly or from an app
  so app logging/defaults can flow into downstream calls.
- A raw curl configuration callback on HTTP client configs and individual HTTP
  requests for protocol/options escape hatches without leaving Vectis helpers.
- A `vectis` executable that embeds the provisioned Lua 5.5 runtime, supports
  normal script and shebang execution, and is wrapped on Linux with the
  single-header `libpid0` 0.3.0 helper so it can run correctly as PID 1 in
  `FROM scratch` containers.
- Thread-safe app object lifecycle and route registry management.
- `lonejson`-backed JSON validation helpers.
- `pslog`-backed owned or borrowed logger handling.
- OpenSSL-backed self-signed and CA-signed certificate/key PEM bundle
  generation helpers.
- Dependency provisioning from the `liblockdc` 0.3.0 SDK bundle.
- Compile-checked examples grouped under `examples/kore`, `examples/lockd`,
  `examples/curl`, `examples/ssh`, `examples/certs`, and `examples/raw` that
  exercise the intended C SDK DX without local helper layers.
- A Kore upstream checkout plus tracked patch-series workflow.
- A patched Kore build path that links against the bundled OpenSSL, libcurl,
  libssh2, pslog, and lonejson toolchain. Vectis builds embedded Kore with
  direct TLS material loading so the in-process runtime does not depend on
  Kore's external key-manager flow.
- A C89 portability contract for the `vectis` public API and implementation.

The full Lua framework facade, missing Lua bindings, consumer runtime, ACME
integration, higher-level REST helpers, packaging, and single-binary service
builder remain tracked in [TODO.md](TODO.md).

## Build

Typical local development flow:

```sh
make build
make test
```

The default debug flow provisions dependencies into `.cache/deps/host-debug`.

On Linux, the `vectis` executable is compiled with the single-header
`libpid0` helper. When `vectis` runs as PID 1, `libpid0` supervises the actual
runner child, forwards signals to its process group, and reaps adopted
children. When it is not PID 1, it calls the runner directly.

Release packaging:

```sh
make package
```

Darwin arm64 packaging is release-only and optional. It is built when an
osxcross toolchain is available under `OSXCROSS_ROOT` or
`$HOME/.local/cross/osxcross`; otherwise the Darwin archive and smoke zip are
skipped without failing the Linux-first release flow.

## Kore Workflow

```sh
make vendor-kore
make vendor-kore-apply
make build-kore
make verify-kore-patches
make vendor-kore-upgrade
```

`vendor/kore/upstream/` is a local checkout of `https://git.kore.io/kore.git`.
Patch files are stored in `vendor/kore/patches/` and ordered by
`vendor/kore/patches/series`.

Current Kore policy in `vectis`:

- `vectis` does not expose Kore JSON-RPC and does not enable Kore's `JSONRPC`
  build feature.
- Kore ACME JSON parsing is patched to use `lonejson`.
- Kore logging is being moved behind a `pslog` backend so HTTP/runtime logs can
  converge on one structured logger.
- Embedded Vectis/Kore TLS currently supports manual cert/key material from
  paths, memory, and `lc_source`; ACME remains a tracked implementation area.
