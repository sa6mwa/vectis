# vectis

Vectis is a C SDK and Lua-capable runtime for building small operational
services without assembling a separate web, state, protocol, certificate, and
automation stack by hand.

The project is intentionally service-oriented. It wraps a set of lower-level
libraries into one SDK shape for HTTP/TLS APIs, typed data routes, durable
state and queue workflows, downstream protocol clients, AI/tool integrations,
certificate handling, and local/release lifecycle automation.

## What It Ships

Vectis has two primary deliverables:

- `libvectis`: a C SDK installed as public headers plus static and/or shared
  libraries.
- `vectis`: an executable that embeds the provisioned Lua runtime and statically
  registered native modules for script and packed-service execution.

The C SDK is the primary surface today. The Lua runtime is present and can run
scripts, but the higher-level Lua framework is still being filled in.

## Scope

Vectis is for services that need several of these concerns in one process or
one deployable package:

- Kore-backed HTTP/TLS servers with route registration, static assets, upload
  handling, OpenAPI metadata, and explicit runtime guardrails.
- Typed JSON, XML, and delimiter-separated input mapped into `lonejson` structs.
- Lockd-backed state, leases, attachments, queue producers, and consumer
  services through `liblockdc`.
- Downstream HTTP, SFTP, MQTT, and transfer clients through libcurl.
- OPC UA clients through the `c.pkt.systems` C89 OPC UA facade.
- Audio capture/playback and whisper-backed speech surfaces through the
  `c.pkt.systems` audio and SUS facades.
- SSH command execution and SFTP transfers through libssh2.
- OpenSSL-backed key, CSR, certificate, CA, and PEM bundle workflows.
- Structured logging through `libpslog`.
- CAI-backed OpenAI API, agent, tool, and MCP primitives.
- libmdf-backed Markdown rendering for terminal and HTML output.
- A raw escape hatch to the bundled dependency headers when the Vectis facade is
  intentionally not enough.

MTConnect is planned as a protocol surface. It is not documented here as an
implemented runtime feature yet; the intended direction is to make it a typed
protocol integration alongside XML/DSV/JSON and the existing curl/SSH/SFTP/MQTT
client surfaces.

## Dependency Baseline

Debug and release builds consume pinned SDK bundles extracted under `.cache/`.
Release archives are fetched through the shared verified
`${CPKT_DEPENDENCY_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/c.pkt.systems/deps}`
archive cache. The current expected dependency set is:

- `c.pkt.systems` 0.9.0 for curl, OpenSSL, libssh2, nghttp2, zlib, Lua 5.5.0,
  the C89 Lua runtime facade, libxml2 2.15.3, OPC UA, audio/miniaudio,
  SUS/whisper, and supporting package metadata.
- `liblockdc` 0.13.0 for lockd C and Lua surfaces, including pouch local
  storage support.
- `lonejson` 0.42.0 for typed JSON parsing, serialization, streaming arrays,
  spooled fields, and C/Lua bindings.
- `cai` 0.3.0 for OpenAI API, agent, tool, MCP, and Lua binding sources.
- `liblql` 0.2.0 for the LQL dependency exposed by `liblockdc` 0.13.0.
- `libmdf` 0.6.0 for Markdown-to-ANSI/HTML rendering and Lua binding sources.
- `softline` 0.2.0 for line editing, terminal prompt UX, and Lua binding
  sources.
- `libpslog` 0.9.0 for structured logging and Lua binding sources.
- `libpid0` 0.4.2 for Linux PID 1 behavior in the `vectis` executable.

Vectis validates the dependency manifest during CMake configure. A stale or
mixed dependency root should fail early instead of producing a subtly mismatched
SDK.

## C SDK Model

Public configuration structs must be initialized with their matching
`vectis_*_init()` function before use. The init helpers apply non-zero defaults
for timeouts, limits, retry behavior, TLS mode, and route body policies.

Stateful SDK objects use handle structs with explicit `self` method calls:

```c
vectis_app_config config;
vectis_app *app;
vectis_route_config route;
vectis_error error;

vectis_app_config_init(&config);
config.app_name = "orders-api";
config.tls.bind = "127.0.0.1";
config.tls.port = 8080;
config.tls.mode = VECTIS_TLS_MODE_DISABLED;

app = vectis_app_new(&config, &error);
if (app == NULL) {
  return 1;
}

route = vectis_route_methods(VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD,
                             "/health", health, NULL);
if (app->route(app, &route, &error) != VECTIS_OK) {
  app->close(app);
  return 1;
}

if (app->start(app, &error) != VECTIS_OK) {
  app->close(app);
  return 1;
}

app->close(app);
```

The same pattern applies to HTTP clients, SFTP sessions, SSH sessions, MQTT
publishers, and lockd consumer services: initialize config, construct a handle,
call methods with explicit `self`, and close the handle. Free functions remain
available for lower-level use and compatibility.

Borrowed inputs stay borrowed. Strings, `vectis_source` backing storage,
`lc_source` objects, loggers, callbacks, and raw dependency handles supplied
through config or route registration must outlive the Vectis object or route
that references them.

## Typed Routes And Streaming

Vectis has both typed route helpers and raw streaming escape hatches.

Typed routes parse into caller-provided static `lonejson_map` structs:

- `vectis_json_typed_route()` parses JSON request bodies into a mapped input
  struct and lets the handler respond with status-specific mapped output through
  `vectis_json_reply()`.
- `vectis_xml_route()` parses XML with libxml2's reader API directly into the
  mapped input struct. It does not build an XML DOM and does not translate XML
  through generated JSON.
- `vectis_dsv_route()` gives handlers a pull iterator over typed DSV rows
  parsed into mapped row structs.

Raw streaming upload routes are separate. Use `vectis_upload_reader_route()`
when the handler needs the upload byte stream itself instead of a typed parser.
Use file upload routes when the product contract is file-oriented.

Request-body policy is explicit:

- Small API bodies can be materialized through `vectis_request_body_materialize()`.
- Streaming routes use bounded internal buffers and live reader flow.
- Upload/file routes can set max bytes and memory thresholds deliberately.
- Vectis never silently raises the server-wide Kore body ceiling from route
  registration.

## Data Formats

JSON is handled by LoneJSON. Vectis uses mapped structs for request parsing,
response serialization, HTTP client bodies, selected-array streaming, and
source-to-sink array rewriting.

XML is parsed with libxml2 tokens and LoneJSON maps. Elements map by key,
attributes map by name or configured prefix, nested objects recurse through
submaps, repeated contiguous elements fill LoneJSON arrays, and large text
fields can use LoneJSON spooled stream fields. Non-contiguous repeated array
elements are rejected rather than buffered and reordered.

DSV covers CSV, TSV, and configurable delimiter-separated values. Row-only input
can infer column order from a map; headered input can infer names from the first
record or use explicit columns. DSV row parsing is streaming. Conversion helpers
that produce a complete JSON array are named and treated as materializing APIs.

## Runtime And Protocol Surfaces

Kore is the embedded HTTP/TLS runtime. Vectis currently allows one active
Kore-backed routed app per process. That process must still be able to host
ordinary API routes, WebDAV routes, and app-owned lockd consumer services
together; separate processes are for independent service instances, not for the
normal web server plus consumer app shape.

Lockd is optional for Kore-only services. When configured, app-owned lockd
clients are process-local and opened lazily in Kore workers where appropriate.
Non-Kore consumer services fail fast during startup when lockd configuration is
invalid.

CAI is included in the dependency set and Lua runtime as the AI and tool
protocol layer. The intended Vectis role is to make CAI usable beside ordinary
service work: lockd-backed state, HTTP routes, queue consumers, tool callbacks,
MCP integration, and OpenAI API calls should share the same logging,
configuration, and packaging story.

libmdf and softline are included so AI/agent terminal workflows have the
rendering and prompt-editing dependencies needed by future Agent Smith work.
Vectis does not yet wire CAI streaming responses through libmdf or softline
automatically, but the C SDK dependencies and `require("libmdf")` /
`require("softline")` Lua modules are available for that integration.

MTConnect is the next protocol family planned for this stack. The expected
shape is a typed, streaming-aware integration rather than a raw string/DOM API.
Until that surface lands, MTConnect should be treated as forward-looking
roadmap, not an advertised SDK feature.

## Lua Runtime

The `vectis` executable embeds the provisioned Lua 5.5 runtime facade and does
not depend on host Lua, LuaRocks, `LUA_PATH`, `LUA_CPATH`, or dynamically
discovered Lua modules for normal execution.

Typical script entry:

```lua
#!/usr/bin/env vectis
local vectis = require("vectis")
```

Bundled modules are registered statically through `package.preload`. The
runtime currently registers Vectis-owned Lua support plus dependency bindings
such as lockdc, lonejson, cai, curl, libmdf, and softline where available from
the provisioned sources. The embedded curl Lua facade exposes `curl.perform()`,
`curl.json()`, and `curl.stream_json()` for libcurl-supported URL schemes,
including SMTP usage through the same option-table model.

`vectis -a pack` can append a Lua script, optional site/template assets, and
optionally a lockd client bundle to the Linux executable with hashes and trailer
metadata. Packed Lua can use `vectis.embedded.has_assets()`, `list()`, `read()`,
`chunks()`, and `extract()` to serve read-only assets or initialize an extracted
WebDAV docroot.

The high-level Lua web/service framework is still an active implementation
area. The C SDK is the stable design source until Lua parity catches up.

## Local Development

The normal local loop is:

```sh
make build
make test
```

Useful gates:

```sh
make finalize-slice
make asan
make test-all
make prerelease
```

`make help` is the authoritative command index. Important targets include:

- `make deps-debug` provisions host debug dependencies into `.cache/`.
- `make lua-test` runs the embedded Lua runtime and bundled module smoke tests.
- `make lua-env` prints exports for running Lua examples with the built CLI.
- `make test-e2e` runs the compose-backed integration smoke suite.
- `make test-install-tree` verifies downstream CMake/pkg-config consumers from
  an installed SDK tree.
- `make package` builds release SDK archives.
- `make package-verify` verifies checksums, archive layout, privacy, and
  relocatability.
- `make release-darwin-smoke-bundle` builds the Darwin SDK and smoke bundle
  when osxcross is available.
- `make release-matrix` builds, checksums, and verifies supported release
  targets.

Generated state lives under `build/`, `dist/`, `.cache/`, `devenv/volumes/`,
and the generated Kore upstream checkout. `make clean` removes generated state.

## Local Integration Environment

`docker-compose.yaml` defines the local service environment used by e2e tests:

- MinIO for S3-backed lockd testing.
- lockd disk transport with generated mTLS material.
- lockd S3 transport with generated mTLS material.
- SSH/SFTP test server.
- MQTT broker.

Entry points:

```sh
make dev-up
make dev-ps
make dev-logs
make dev-down
make dev-reset
make test-e2e
```

`make test-e2e` resets generated state, starts the local services, runs lockd,
MQTT, SSH, curl-backed SFTP, and libssh2-backed SFTP smoke tests, and then stops
the services unless `VECTIS_E2E_KEEP_DEVSERVICES=1` is set.

## Examples

Examples live under `examples/` and are built by the normal debug build. They
are DX probes for the public SDK:

- `examples/kore`: HTTP/TLS servers, JSON routes, OpenAPI, static assets, file
  upload, generated responses, and lockd-backed APIs.
- `examples/lockd`: state, leases, attachments, queues, and consumer services.
- `examples/curl`: HTTP JSON clients, file transfer, SFTP, and MQTT.
- `examples/dsv`: typed DSV parsing and serialization.
- `examples/xml`: typed XML parsing into LoneJSON structs.
- `examples/ssh`: SSH commands and libssh2 SFTP.
- `examples/certs`: certificate and PEM bundle generation.
- `examples/raw`: direct dependency escape hatches.

See [examples/README.md](examples/README.md) for the file-by-file map.

## Packaging

Binary SDK archives are produced under `dist/`:

```text
dist/vectis-<version>-<target-id>.tar.gz
dist/vectis-<version>-CHECKSUMS
```

Supported Linux target IDs include:

- `x86_64-linux-gnu`
- `x86_64-linux-musl`
- `aarch64-linux-gnu`
- `aarch64-linux-musl`
- `armhf-linux-gnu`
- `armhf-linux-musl`

`arm64-apple-darwin` is built when the configured osxcross toolchain is
available.

SDK archives are expected to contain relocatable headers, libraries, CMake
package metadata, pkg-config metadata, docs, examples when shipped, and package
metadata. Package verification extracts checksum-listed artifacts and scans for
local paths, non-relocatable runtime paths, stale artifacts, missing metadata,
and broken downstream consumers.

## Vendored Kore

Vectis carries a patched Kore workflow:

```sh
make vendor-kore
make vendor-kore-apply
make build-kore
make verify-kore-patches
make vendor-kore-upgrade
```

Patch files are tracked under `vendor/kore/patches/` and applied to
`vendor/kore/upstream/`. Vectis builds Kore as an embedded runtime dependency
against the same provisioned SDK root as the rest of the project.

## Current Status

Implemented and covered by local tests:

- C SDK object/config/error surfaces.
- Kore-backed route registration, startup, request mapping, static assets,
  response helpers, and upload handling.
- Typed JSON, XML, and DSV parser/route helpers.
- OpenAPI JSON/YAML generation from route metadata and LoneJSON maps.
- curl-backed HTTP, transfer, SFTP, and MQTT helpers.
- libssh2-backed SSH and SFTP helpers.
- OpenSSL-backed certificate/key/bundle helpers.
- Optional lockd app integration and managed consumer-service helpers.
- Lua runner, shebang/script execution, and Linux packed-script support.
- Bundled CAI, libmdf, and softline Lua modules for AI/tool workflows,
  Markdown rendering, and terminal prompt UX.
- Release packaging, install-tree checks, lifecycle tests, privacy checks, and
  generated SDK verification.

Still active or planned:

- Higher-level Lua service framework and broader Lua protocol helpers.
- Deeper CAI/Vectis/libmdf/softline facade integration beyond bundled
  dependency/runtime registration.
- MTConnect protocol support.
- Darwin-specific packed-service signing/runtime polish.
- Additional protocol examples and long-running hardening gates.

Track detailed engineering work in [TODO.md](TODO.md).
