# vectis TODO

This file is the implementation contract. It tracks the product shape Vectis is
moving toward, not just the next patch. Keep items observable and testable.

## Area 1: Project Skeleton

- [x] Create CMake project with presets and Makefile entrypoints.
- [x] Add static/shared library build options.
- [x] Add unit test wiring.
- [x] Add sanitizer and fuzz presets.
- [x] Add `make clean` that removes `build/`, `dist/`, and `.cache/`.
- [x] Enforce a C89 (`-std=c89` / C90) contract for the `vectis` public API and internal implementation.

## Area 2: Dependency Provisioning

- [x] Provision split `liblockdc` 0.9.0, `c.pkt.systems` 0.1.0, `lonejson` 0.19.0, and `libpslog` 0.4.1 SDK archives into `.cache/deps/...`.
- [x] Use `liblockdc` only as the source for lockdc, `c.pkt.systems` as the source for curl/OpenSSL/libssh2/nghttp2/zlib, and standalone LoneJSON/libpslog bundles for their C headers/libraries.
- [x] Verify downloaded SDK archives with pinned SHA-256 checksums.
- [x] Provision pinned Lua 5.5.0 from lua.org into every target dependency root for the Vectis binary runtime.
- [x] Build Lua as a static `liblua.a` plus headers for host, Linux cross, musl, and optional Darwin target roots.
- [x] Add dependency manifest validation in CMake for the pinned Lua version and checksum.
- [x] Provision pinned libxml2 2.15.3 from GNOME into every target dependency root.
- [x] Build libxml2 as both static and shared libraries plus headers without relying on distro XML packages.
- [x] Support host debug and Linux release matrix roots.
- [x] Add explicit dependency manifest validation in CMake, including expected dependency versions.
- [x] Add optional arm64 Apple Darwin dependency provisioning from the split SDK archive set.
- [ ] Verify musl and cross targets against the full release matrix in CI.
- [ ] Verify Darwin arm64 packages on real Apple Silicon hardware.
- [x] Add release packaging checks that prove downstream C consumers can include Vectis headers and all dependency headers from the installed SDK.

## Area 3: Kore Upstream Management

- [x] Vendor Kore as an upstream checkout plus patch stack workflow.
- [x] Add upgrade/apply/status make targets.
- [x] Record and pin a vetted upstream revision for the first shipping integration.
- [x] Add actual Kore patch files for vectis-specific behavior.
- [x] Add automated assertions that the patch stack applies cleanly after upgrade.
- [ ] Decide whether Kore remains patched in-tree or is built as a Vectis-owned downstream runtime library for release packaging.

## Area 4: C SDK Surface

- [x] Define public handle structs with direct function-pointer method semantics
  (`app->start(app, ...)`, `client->get(client, ...)`) and private `impl`
  pointers, while keeping free functions as lower-level compatibility levers.
- [x] Implement app config defaults.
- [x] Add defensive server/Kore guardrail defaults for connection count, request sizes, read/write timeouts, idle timeout, and keepalive behavior.
- [x] Rename server body/write timeouts to idle-timeout semantics and add minimum request-body rate guardrails.
- [x] Add per-route request-body policy presets for JSON/default bodies and streaming large uploads.
- [x] Implement thread-safe app lifetime and route registry state.
- [x] Support owned or borrowed `pslog` logger instances.
- [x] Store and validate lockd configuration without requiring a live lockd daemon during app construction.
- [x] Instantiate and own a real `lc_client` during non-Kore runtime startup, with structured lockdc errors on open failure.
- [x] Open app-owned lockd clients process-locally: during non-Kore startup and lazily inside Kore workers so route handlers do not inherit pre-fork sockets.
- [x] Expose stable accessors/helpers for the shared logger and direct lockd client escape hatch.
- [x] Implement dependency-backed C helpers for curl, SSH exec/SFTP, self-signed certificate bundles, request/response JSON parse/serialize, and JSON route auto-wiring.
- [x] Translate manual Kore TLS material from path, memory, and `lc_source` inputs into runtime server cert/key configuration.
- [x] Complete dependency-backed behavior for CA-signed certificate generation and manual/ACME Kore TLS startup.
- [ ] Complete the remaining full Kore runtime configuration surface, including low-level escape hatches where useful.
- [x] Expose dependency headers and low-level handles/APIs as explicit escape hatches without making them the primary DX.
- [x] Define the first C pass of one Vectis-owned naming, source, error, timeout, ownership, and cleanup convention.
- [x] Add stable string helpers for status, error source, HTTP method, and body mode names.
- [x] Mirror the C naming, source, error, timeout, ownership, and cleanup convention in Lua.
- [x] Define request/response abstractions that do not leak Kore internals unless explicitly requested.
- [x] Add common C source constructors for path, memory, and `lc_source` material inputs.
- [x] Add route constructors that infer literal, named-parameter, optional-parameter, and regex path kinds.
- [x] Mirror Kore's supported HTTP methods in the Vectis C SDK: GET, POST, PUT, DELETE, HEAD, OPTIONS, and PATCH.
- [x] Support one route handler registered for a method mask such as GET | HEAD.
- [x] Add upload route constructors that attach the streaming upload body policy in one call.
- [x] Add a handle-shaped C HTTP client API that can inherit app logging/defaults.
- [x] Add high-level C SDK examples before implementation hardens, so the intended DX drives API shape rather than wrappers around internals.
- [x] Group C examples by SDK domain under `examples/kore`, `examples/lockd`, `examples/curl`, `examples/ssh`, `examples/certs`, and `examples/dependency`.
- [x] Add C examples that build against installed Vectis headers and libraries.
- [x] Add install-tree tests for static and shared downstream C consumers.
- [x] Add runnable example smoke tests against local lockd and a real Vectis/Kore server once runtime startup is implemented.

## C-first boundary

The first C SDK pass is implemented and covered by unit, install-tree, ASAN, and
compose-backed e2e tests. Remaining C items below are either deeper runtime
coverage, future feature expansion, or post-C-SDK polish; they should not block
starting the Lua runtime/facade phase unless they directly affect Lua API shape.
Lua runtime work remains documented below and should mirror the C naming, source,
error, timeout, ownership, cleanup, logger, and streaming conventions.

## Area 5: HTTP / Kore Integration

- [x] Build and link Kore against the dependency bundle shipped by `liblockdc`.
- [x] Start/stop a real Kore runtime from `vectis`.
- [x] Translate the currently supported `vectis_server_config` guardrails into concrete Kore runtime/config settings.
- [x] Implement the remaining guardrails that Kore does not expose directly yet: response-write idle timeout, minimum body transfer rate, and keepalive request count.
- [x] Wire `pslog` into the Kore runtime path so Vectis server logs and Kore runtime diagnostics use the configured app logger.
- [x] Expose low-level Kore request/runtime escape hatches where practical for C handlers.
- [ ] Expose additional direct Kore configuration hooks where the startup lifecycle can report errors cleanly.
- [x] Register Vectis C routes as Kore handlers.
- [x] Support method-specific handlers and router-style dispatch.
- [x] Draft route path classification for literal paths, named path parameters, and explicit POSIX regex routes.
- [x] Add public route-builder helpers for literal, named/optional parameter, and regex route registration.
- [x] Add internal Vectis router dispatch for literal, regex, named parameters, optional parameters, method masks, and safe path-param population.
- [x] Attach the internal Vectis router dispatch to real Kore request handling.
- [x] Expose request body, headers, query parameters, path parameters, and response helpers.
- [x] Register JSON routes as ordinary Vectis routes with lonejson parse/serialize adapter dispatch.
- [x] Enforce per-route no-body and max-body policies in the Kore request path before handler dispatch.
- [x] Expose request bodies as reader-first `lc_source` streams; no Kore handler path may require upfront framework buffering.
- [x] Add transparent body materialization for fixed buffers, memory caps, and automatic spill-to-disk when the body does not fit.
- [x] Add static file and directory route helpers with traversal-safe path handling.
- [x] Document and enforce Kore's server-global request-body ceiling: Vectis route body policies are semantic/materialization limits and must fit under `vectis_server_config.max_request_body_bytes`.
- [ ] Translate per-route body policies into any remaining concrete Kore request-body streaming and disk-offload tuning behavior that does not broaden the global ingress ceiling.
- [ ] Support Kore features through Vectis where practical, including websocket, static asset, file upload, and deeper runtime configuration features.
- [x] Define the supported one-process multi-instance behavior based on what Kore can safely provide.
- [x] Add real HTTP integration tests.
- [x] Add real HTTPS integration tests.

## Area 6: TLS / Certificate Configuration

- [x] Define Vectis-owned TLS config surface with default TLS-first semantics.
- [x] Validate bundle versus cert/key inputs before runtime startup.
- [x] Add flexible path/source/memory TLS material configuration for Kore server cert/key bundles, split cert/key material, CA bundles, and client-CA bundles.
- [x] Translate manual Vectis TLS cert/key bundle, split cert/key, and client-CA material into Kore runtime behavior.
- [x] Translate Vectis ACME TLS config into Kore runtime behavior.
- [x] Support lockd client certificate bundle configuration from C.
- [x] Support server certificate bundle configuration from C.
- [x] Support lockd client certificate bundle configuration from Lua.
- [x] Support server certificate bundle configuration from Lua.
- [x] Add Vectis-owned OpenSSL-backed helpers for generating self-signed certificates, CA material, and CA-signed client/server PEM bundles.
- [x] Add Vectis-owned OpenSSL-backed helpers for generating standalone private keys and CSRs.
- [x] Expose initial lower-level OpenSSL access for advanced users while keeping certificate workflows as the primary C and Lua DX.
- [ ] Extend lower-level OpenSSL Lua access only where concrete signing, verification, digest, encoding, or key-inspection workflows require it.
- [x] Support embedding a lockd client certificate bundle into a packaged Vectis binary.
- [x] Support ACME runtime configuration through Kore for C services.
- [ ] Add ACME lifecycle examples/tests beyond startup validation, using a controlled ACME test server if practical.
- [ ] Support reload/update of key material where Kore allows it.

## Area 7: JSON Surface

- [x] Integrate `lonejson` into the Vectis build and expose validation helpers.
- [x] Keep Kore JSON-RPC unsupported in Vectis phase 1.
- [x] Patch Kore ACME JSON handling and request-body helpers to use `lonejson`.
- [x] Replace the remaining Kore JSON parsing surface with `lonejson`.
- [x] Make C request JSON parsing stream from the request reader into lonejson instead of requiring buffered bodies.
- [x] Expose additional C request-body helpers that support explicit reader and transparent memory-or-file materialization.
- [x] Expose C request-body streaming parse helpers where this is practical above Kore's body callback model.
- [x] Draft request-body policy configuration for buffer versus streaming/spool-to-disk strategies.
- [x] Expose C response helpers for large file payloads without application-memory buffering.
- [x] Expose C response serialization helpers for generated large payload streaming.
- [x] Add delimiter-separated value support for CSV, TSV, and configurable any-SV input.
- [x] Stream CSV/TSV/any-SV rows from `lc_source` and `vectis_source` readers into caller-provided `lonejson`-mapped structs without requiring whole-file buffering.
- [x] Provide row callback APIs for DSV parsing so handlers can validate, transform, store to lockd, enqueue, or write downstream output one row at a time.
- [x] Provide convenience DSV-to-JSON-array conversion, including a typed `lonejson` map-backed variant.
- [x] Add DSV-to-JSON-array memory-limit and spill-to-disk behavior consistent with other Vectis body/materialization helpers.
- [x] Provide `lonejson`-mapped struct serialization back to CSV, TSV, and configurable any-SV streams.
- [x] Handle headers, headerless map-order fields, explicit field mappings, delimiter/quote/escape configuration, CRLF/LF normalization, empty values, and strict versus permissive row-width validation.
- [x] Add explicit row-only CSV/TSV presets for the common headerless typed-lonejson path.
- [x] Add opt-in whole-record DSV comment skipping with configurable prefixes.
- [x] Add C examples and tests for CSV, TSV, custom delimiter, streamed row parsing, and DSV-to-JSON-array conversion.
- [x] Add C examples and tests for struct-to-DSV serialization once that API lands.
- [x] Define the first XML-to-lonejson mapping contract: child elements by field key, repeated elements to arrays, attributes by name or configured prefix, and object text to `config.text_key`.
- [x] Add libxml2-backed XML reader helpers that parse `vectis_source` or direct `lc_source` inputs into lonejson mapped structs.
- [x] Add true push-through XML-to-lonejson streaming for huge text fields so intermediate generated JSON strings do not need to remain memory-backed.
- [x] Expose Lua JSON encode/decode helpers through the bundled `lonejson` Lua module.
- [x] Make `lonejson` the backing implementation for Vectis request parsing, response serialization, lockd document helpers, and JSON HTTP client helpers.
- [x] Stream mapped JSON request bodies into curl through LoneJSON's generator, using `lonejson_generator_measure()` for replayable mapped structs when `Content-Length` is required and chunked transfer for non-rewindable values.
- [x] Evaluate LoneJSON object-framed stream, selected-array stream/rewrite, generic JSON value, writer, SSE, and multipart APIs for Vectis-owned helpers where they improve API, workflow, or file-ingestion DX without hiding buffering semantics.
- [x] Integrate LoneJSON selected-array streaming and selected-array rewrite helpers without materializing complete documents or selected arrays.
- [x] Add higher-level JSON REST helpers for request validation, response serialization, and downstream API calls.
- [x] Add first-pass typed JSON route auto-wiring backed by lonejson mapped structs.
- [x] Add multi-output typed JSON routes where handlers choose status-specific response maps at runtime.
- [x] Add optional OpenAPI generation from route and lonejson map metadata, without requiring handlers to predeclare every possible output shape just for documentation.

## Area 8: lockd / Workflow Runtime

- [ ] Extend Vectis service-friendly lockd helpers only where they reduce real C workflow friction without obscuring the public `liblockdc` 0.13.1 API.
- [x] Integrate the `lockdc` Lua binding into the Vectis Lua runtime.
- [x] Provide first-pass C helpers for lockd-backed typed state load/save/update workflows.
- [x] Add first Vectis-owned Lua `vectis.lockd` helper for config normalization, embedded bundle source wiring, and client cleanup.
- [ ] Provide additional C helpers for retry-oriented queue workflow patterns only where direct `liblockdc` remains too noisy in real examples.
- [ ] Provide Lua helpers for document store, retrieval, query, leases, enqueue, dequeue, ack/nack, and retry-oriented workflow patterns.
- [x] Add Lua `vectis.lockd.with_dequeued_json()` to remove dequeue/payload/cleanup boilerplate while leaving ack/nack explicit in the handler.
- [x] Expose direct `liblockdc`/`lockdc` access for complete API coverage while making Vectis helpers the preferred workflow API.
- [x] Make lockd optional for Kore-only C services while preserving validation for configured lockd transports.
- [x] Define consumer registration and lifecycle APIs for C.
- [x] Expose lockd consumer service startup/configuration directly, then layer Vectis-owned worker DX on top.
- [x] Complete support for one Vectis process to run a Kore-backed API/WebDAV server and an app-owned liblockdc `startconsumer` service simultaneously, with receiver-shell C APIs and Lua registration hooks; Kore serving and consumer service startup are concurrent capabilities, not mutually exclusive runtime modes.
- [x] Add a same-process scenario test that serves an API/WebDAV mount while receiving lockd messages through `startconsumer`, proving HTTP/WebDAV responsiveness during active consumer work and covering the production shape of a web/API fileserver plus lockd consumer in one Vectis process.
- [x] Define Lua consumer-service runner behavior for the combined server-plus-consumer process model.
- [x] Make API errors explicit when a Lua script attempts to run genuinely incompatible runtime loops in one process.
- [x] Add integration tests covering enqueue/dequeue/ack workflows.
- [x] Add direct liblockdc C examples for open client, lease save/load, query, attachments, enqueue, manual dequeue, and managed consumer services.

## Area 9: Downstream HTTP / curl

- [x] Add dependency-backed C helpers around libcurl for service-friendly downstream HTTP calls.
- [x] Draft the C helper API for libcurl-backed HTTP, JSON request methods, generic upload/download, SFTP, and MQTT publish workflows.
- [x] Add an opaque C HTTP client handle so repeated downstream calls do not have to pass direct config everywhere.
- [x] Add handle-level C helpers for GET, DELETE, POST JSON, PUT JSON, and PATCH JSON.
- [x] Add dependency-backed C MQTT publish helpers through libcurl.
- [x] Add Lua curl bindings that support all libcurl URL schemes made available by the bundled libcurl build.
- [x] Provide JSON-aware Lua helpers for common API calls using `lonejson`.
- [x] Provide C Vectis helpers for JSON API requests, downloads, uploads, SFTP upload/download, and structured errors.
- [x] Add C proxy, low-speed, and streaming-response helpers where the current curl configuration callback is not enough.
- [x] Add C retry helpers where the current curl configuration callback is not enough.
- [x] Add a C libcurl SMTP helper for native email-token auth delivery with deterministic mock-SMTP unit coverage.
- [x] Add packed webserver smoke coverage for SMTP-delivered email-token login before WebDAV key issuance.
- [x] Provide Lua Vectis helpers for JSON API requests, downloads, uploads, streaming responses, SFTP upload/download, retries, and structured errors.
- [x] Expose direct curl option configuration callbacks for complete protocol coverage escape hatches.
- [x] Support SFTP file retrieval and storage through curl where it is sufficient.
- [x] Expose timeout, header, TLS, client certificate, proxy, redirect, and streaming-response configuration in C.
- [x] Expose retry configuration in C.
- [x] Expose timeout, retry, header, TLS, client certificate, proxy, redirect, and streaming configuration in Lua.
- [x] Add integration tests for SFTP operations.
- [x] Add integration tests for HTTP(S) downstream calls against a controlled local downstream server.

## Area 10: SSH / libssh2

- [x] Add a Lua Vectis facade for libssh2-backed command execution with authentication, stdout/stderr capture, exit status, timeout handling, and structured error handling.
- [ ] Add lower-level Lua bindings for dependency-native libssh2 session/channel control if real service workflows require them.
- [x] Add C Vectis helpers for connecting to SSH servers and running commands with captured stdout, stderr, exit status, timeout, and structured error handling.
- [x] Add Lua Vectis helpers for connecting to SSH servers and running commands with captured stdout, stderr, exit status, timeout, and structured error handling.
- [x] Draft the C helper API for libssh2 command execution with captured stdout/stderr, exit status, and SFTP upload/download.
- [x] Implement libssh2-backed C SSH command execution with captured stdout/stderr and exit status.
- [x] Implement libssh2-backed C SFTP upload/download helpers for cases where curl-backed SFTP is not enough.
- [x] Implement stateful libssh2-backed C SFTP session, file read/write/stat, and directory iteration receiver shells.
- [ ] Expose dependency-native libssh2 sessions/channels for advanced control.
- [x] Decide and expose lower-level SFTP operations needed beyond curl-backed file transfer: one-shot filesystem operations plus stateful session/file/directory handles.
- [ ] Add C helpers only where they support the Vectis service model rather than exposing libssh2 wholesale.
- [x] Add integration tests for remote command execution against a controlled test SSH server.
- [x] Add integration tests proving Lua SSH command execution honors known_hosts pinning.

## Area 11: OpenSSL / Certificate Binding

- [x] Add Lua bindings or facades for certificate loading, parsing, validation, and bundle assembly.
- [x] Add Lua certificate bundle and cert/key pair validation helpers backed by the C certificate workflow API.
- [ ] Keep dependency-native OpenSSL exposure narrow; prefer Vectis certificate workflows over dumping OpenSSL APIs into Lua.
- [x] Support client and server certificate management in the C shared Vectis config model.
- [x] Support client and server certificate management in the Lua shared Vectis config model.
- [x] Add runtime Lua HTTPS coverage for manual split cert/key server configuration through the shared Vectis TLS model.
- [x] Draft the C helper API for OpenSSL-backed certificate bundle generation.
- [x] Implement OpenSSL-backed self-signed certificate/key PEM bundle generation for the C SDK.
- [x] Implement OpenSSL-backed CA certificate/key PEM bundle generation for the C SDK.
- [x] Implement OpenSSL-backed CA-signed certificate/key PEM bundle generation from existing CA cert/key material.
- [x] Add tests for valid, expired, malformed, and missing certificate material.

## Area 11.5: Future CAI Integration

- [ ] Track CAI as a future dependency once its C SDK surface stabilizes; current Vectis-side feedback is parked in `../cai/stash/feedbackfromvectis.md`.
- [ ] Add a Vectis CAI config section that can borrow an external `cai_client` or create an app-owned client with Vectis logger inheritance.
- [ ] Mirror the lockd logger model for CAI: dedicated CAI logger, fallback to app/Kore logger, and `logger_disabled` opt-out.
- [ ] Add thin Vectis adapters for request-body reader to `cai_source`, CAI output to Vectis response/lockd payload, and CAI errors to JSON API responses.
- [ ] Keep CAI itself as the primary SDK for OpenAI mechanics; Vectis should provide integration glue and service-oriented DX, not a second CAI wrapper.

## Area 12: Lua Runtime and Framework

The `vectis` binary is a primary product surface, not a demo runner for
`libvectis`. Lua coverage must therefore be first-class: every major Vectis C
SDK workflow and every bundled dependency that is useful from application code
must be reachable from Lua either as a thin dependency-native facade, a
Vectis-owned workflow/DX helper, or both. Any intentionally C-only surface must
be documented with a concrete reason such as unsafe callback lifetime,
allocator/`FILE *` ownership, or an embedding-only concern.

- [x] Reserve Lua-facing runtime concerns in the TODO contract and API design.
- [x] Add a `vectis` executable that embeds Lua.
- [x] Make the `vectis` executable build depend on the Vectis-provisioned Lua 5.5 static library rather than a host Lua installation.
- [x] Add the Linux `vectis` executable entrypoint wrapped in single-header `libpid0` for PID 1 behavior in `FROM scratch` containers.
- [x] Replace the placeholder executable submain with the real Lua runner.
- [x] Configure Lua package paths so bundled/native Vectis modules load without user setup.
- [x] Provide the first statically preloaded `require("vectis")` facade for the embedded Lua runner.
- [x] Expand `require("vectis")` into the full high-level framework facade for currently documented workflow modules, with direct module identity preserved for every top-level alias.
- [x] Register C-owned workflow tables (`vectis.auth`, `vectis.cert`, `vectis.embedded`, `vectis.server`, and `vectis.ssh`) as direct preloaded modules and top-level `require("vectis")` aliases.
- [x] Add user-facing docs and smoke coverage for the direct `vectis.embedded` packed asset workflow module.
- [x] Add a Lua coverage matrix under `docs/` that tracks every Vectis C SDK workflow and bundled dependency facade as `native`, `vectis DX`, `tested`, `packed-tested`, `live-tested`, or `intentionally C-only`.
- [x] Add a lifecycle contract that fails when a bundled dependency or Vectis-owned C workflow is added without an explicit Lua coverage-matrix entry.
- [x] Ensure the `vectis` Lua facade exposes structured status/error objects consistently across all Vectis-owned Lua helpers, mirroring C status, source, timeout, ownership, and cleanup conventions.
- [x] Normalize outbound `vectis.rest.client` JSON encode failures into structured result envelopes instead of raising Lua assertion errors.
- [x] Normalize `vectis.rest.error_response` JSON encode failures into minimal structured JSON fallback responses instead of raising Lua assertion errors.
- [x] Add a public `cpkt` error source and structured Vectis status/source envelopes to dependency-native cpkt-backed Lua facades (`audio`, `sus`, and `opcua`) while preserving dependency-native diagnostics.
- [x] Convert packed embedded bundle/assets Lua helper failures to structured Vectis status/source errors and cover them in the packed scenario.
- [x] Add a preloaded pure-Lua `vectis.status` helper and use it to enrich curl-backed HTTP/WebDAV/MQTT/SMTP normalized errors with Vectis status/source metadata, timeout mapping, dependency codes, and HTTP status diagnostics.
- [x] Use the shared `vectis.status` helper for Vectis-owned DSV and lockd workflow helper errors, including LoneJSON decode-source attribution in `vectis.dsv`, lockdc-source attribution for lockd operations, and Vectis-source attribution for lockd callback failures.
- [x] Integrate existing Lua rocks/source archives for lockdc, pslog, liblql, and dependency Lua modules from pinned dependency sources.
- [x] Build lockdc, pslog, and Vectis-owned Lua modules against the provisioned Lua 5.5 ABI.
- [x] Register bundled Lua modules statically through `package.preload` in the `vectis` binary.
- [x] Statically preload every bundled dependency-native Lua facade that exists or is Vectis-owned: lockdc, lonejson, pslog, lql, cai, libmdf, softline, curl, opcua, XML/libxml2, DSV/CSV/TSV, OpenSSL, libssh2/SFTP/SCP, WebDAV client helpers, sus/whisper, audio/miniaudio, and future protocol facades.
- [x] Add a top-level `vectis.libs` namespace that collects bundled dependency Lua facades (`lockdc`, `lonejson`, `pslog`, `lql`, `cai`, `libmdf`, `softline`, `curl`, `opcua`, `openssl`, `zlib`, `audio`, `sus`) without replacing direct `require(...)` access.
- [x] Keep `vectis.libs` coverage current whenever any bundled Lua facade is added or renamed, including direct module aliasing, Lua smoke assertions, and coverage-matrix documentation in the same change.
- [x] Add a dependency-native zlib Lua facade for buffered string deflate/inflate, gzip/gunzip, auto-decompress, file-backed bounded transforms, version, and explicit output limits.
- [ ] For each dependency-native Lua facade, add a Vectis-owned helper only where it reduces real service workflow friction; do not replace or hide the direct dependency module when complete API coverage matters.
- [x] Add an OPC UA Lua client/foundation facade over the `cpkt-opcua` C89 facade and statically preload it in the `vectis` binary as `require("opcua")`.
- [ ] Extend the OPC UA Lua facade to cover server-side APIs once Lua callback ownership and method/access-control contracts are defined.
- [ ] Extend the OPC UA Lua facade to cover subscriptions, async client calls, browse callbacks, and PubSub with explicit Lua callback lifetime/error semantics.
- [x] Add a Lua facade for XML parsing backed by the existing libxml2/lonejson C helpers, including memory/path XML-to-lonejson mapped-record workflows and deterministic tests.
- [ ] Add XML serialization coverage once the C SDK has an XML writer contract; keep the behavior explicit as serialization, not hidden JSON/string conversion.
- [x] Add Lua facades for DSV/CSV/TSV materialized parsing and serialization backed by the existing C helpers, including typed row callbacks through Lua-owned LoneJSON records, custom delimiter, strict/permissive width, comments, and spill-to-disk behavior.
- [x] Extend DSV typed Lua parsing to support dynamic string fields without violating LoneJSON Lua record ownership/cleanup invariants.
- [x] Add packed scenario coverage for DSV Lua workflows.
- [x] Add Lua route-row handler integration for DSV routes.
- [x] Add Lua WebDAV client helpers for PROPFIND, MKCOL, GET, PUT, COPY, MOVE, DELETE, auth headers, depth handling, destination handling, structured errors, and file-backed transfer; keep server-side WebDAV helpers on `vectis.server`.
- [x] Add a C-owned Lua `server:webdav()` helper for ordinary Vectis-managed mutable WebDAV mounts, including unauthenticated mounts plus native and callback auth providers.
- [x] Add a C-owned Lua `server:webdav_embedded()` helper for read-only WebDAV mounts over packed embedded assets, including native/callback auth providers.
- [x] Expand Lua server-side WebDAV helpers beyond Vectis-managed storage so ordinary Lua apps can mount direct mutable disk docroots as WebDAV mounts without C glue.
- [x] Add an initial narrow dependency-native OpenSSL Lua facade for stable primitives not covered by `vectis.cert`, while keeping Vectis certificate workflows as the default DX.
- [x] Expand the dependency-native OpenSSL Lua facade with general EVP digest and HMAC helpers while keeping fixed SHA-256 helpers for common workflows.
- [x] Expand the dependency-native OpenSSL Lua facade for advanced signing, verification, encoding, and key/certificate inspection operations that are not covered by `vectis.cert`.
- [x] Expand `vectis.cert` Lua helpers to cover private key generation, CSR generation, self-signed bundles, CA bundles, CA-signed cert/key pairs, cert/key pair validation, CA validation, and inspection.
- [ ] Add certificate reload/update hooks where Kore can support them.
- [x] Add Vectis-owned Lua libssh2-backed SFTP file upload/download helpers with host-key, known-hosts, timeout, and structured error contracts where curl-backed SFTP is insufficient.
- [x] Add one-shot Lua libssh2-backed SFTP filesystem helpers for stat, mkdir, remove, rmdir, rename, and chmod where curl-backed SFTP is insufficient.
- [x] Add broader Lua libssh2-backed SFTP session, file open/read/write/stat, and directory iteration handles where curl-backed SFTP is insufficient.
- [ ] Add broader Lua libssh2 facades for dependency-native SSH sessions/channels and advanced host-key verification workflows where Vectis service workflows require them.
- [x] Add Vectis-owned Lua SSH/SCP workflow helpers on top of the dependency-native libssh2 facade for common service operations, while preserving `vectis.http.sftp_*` for curl-backed transfers.
- [x] Add Lua MQTT publish helpers/tests where the generic `curl.perform()` facade is sufficient but workflow defaults improve DX.
- [x] Add other libcurl protocol examples/tests where the generic `curl.perform()` facade is sufficient but workflow defaults improve DX.
- [x] Add Lua helpers for HTTP form bodies and non-JSON simple `get`, `post`, `put`, `patch`, `delete`, and `head` helpers in addition to JSON helpers.
- [x] Add Lua multipart upload helpers backed by libcurl MIME parts, including text shorthand and file-backed parts.
- [x] Add reusable Lua HTTP client defaults for retry policies, proxy/TLS/client-cert settings, protocol allowlists, timeouts, credentials, and merged headers.
- [x] Add richer Lua HTTP file-backed response presets if repeated app workflows need more than `download_path`/`upload_path` plus client defaults.
- [x] Add C-owned Lua fixed route helpers for text responses and redirects alongside JSON/auth JSON routes.
- [ ] Add Lua route APIs for request handlers, middleware-like before/after hooks, path/query/header/body access, JSON/body streaming policies, file responses, SSE/streaming responses, and auth-guarded route groups.
- [x] Add C-owned Lua `server:route()` buffered request callback routes with method/path, named path/query/header lookup, bounded buffered body access, response headers, string bodies, status-only responses, and file responses.
- [x] Add native/callback auth provider guards to Lua `server:route()` and expose the allowed principal to the handler request table.
- [x] Add Lua `server:group()` buffered route groups with shared prefix/default auth/body/method settings and buffered before/after hooks, including short-circuit response support.
- [x] Add explicit file-backed Lua `spooled_source` route responses for callback-produced bodies that should avoid one large Lua string without claiming true response streaming.
- [x] Add Lua OpenAPI route metadata and JSON/YAML generation helpers on the C-owned `vectis.server` receiver, including schema lifetime retention and packed API example coverage.
- [x] Document and expose the core Lua auth facade for user DB configuration, credential storage location, password/TOTP/email-token factor policy, OAuth2/OIDC flows, WebDAV key issuance/revocation, and callback/native provider registration.
- [ ] Add higher-level Lua auth DX helpers for opinionated admin/browser flows on top of the existing C-owned native auth routes and provider contract.
- [ ] Add Lua CAI integration helpers once the CAI C SDK surface stabilizes: borrow/create clients, inherit logging, stream request bodies into `cai_source`, stream CAI output to HTTP/lockd/file sinks, expose tool callbacks, and preserve CAI as the primary OpenAI SDK.
- [x] Add explicit Lua facade contracts for cpkt `sus`/whisper and cpkt audio/miniaudio, including streaming audio source/sink ownership, transcription/voice workflow shape, and deterministic smoke tests without requiring live external services.
- [x] Add cpkt `sus`/whisper Lua transcriber receiver shells from `docs/lua-sus-audio-contract.md`, including model-created transcribers, PCM table transcription methods, revised text, and segment/progress/abort callback registration.
- [x] Add cpkt `sus`/whisper Lua process-wide backend log sink configuration and log-level constants.
- [x] Add the audio/SUS Lua interop boundary and expose `sus` segmented decoder/VOX transcription methods that borrow `audio.decoder` and `audio.segment` handles without duplicating private userdata layouts.
- [ ] Implement remaining loaded-model cpkt `sus`/whisper Lua coverage from `docs/lua-sus-audio-contract.md`, including callback propagation coverage against a loaded model and live/local model coverage.
- [x] Implement the remaining cpkt audio/miniaudio Lua facade receiver shells from `docs/lua-sus-audio-contract.md`, including capture/playback device helpers behind opt-in tests.
- [x] Add initial dependency-native cpkt `sus` and audio Lua modules with deterministic metadata, catalog/cache, callback decoder/encoder, VOX, PTT, preload, example, and packed execution coverage.
- [x] Add user-facing Lua docs for the dependency-native cpkt `sus` and audio facades.
- [x] Add Lua helpers for libmdf/softline where Vectis-owned terminal workflows need higher-level DX; keep their direct modules available without wrapping.
- [ ] Add Agent Smith-specific terminal helpers once the agent workflow contract exists.
- [x] Add deterministic Lua examples for dependency-native pslog, liblql, and softline module workflows without adding redundant Vectis-owned wrappers.
- [x] Add packed execution coverage for dependency-native pslog, liblql, and softline Lua module workflows.
- [x] Add a narrow Vectis-owned Lua logging helper over pslog for JSON logger defaults, default fields, and structured Vectis error metadata without replacing direct pslog access.
- [ ] Add deeper Lua logger inheritance hooks for `vectis.server` and app-owned service components once the server/logger ownership contract is explicit.
- [x] Add packed execution coverage for deterministic local Lua facade examples covering XML, dependency-native OpenSSL/certs, generic curl protocols, CAI local APIs, MQTT/SMTP helper validation, and SCP helper validation.
- [x] Add packed execution coverage for dependency-native libmdf render/stream examples and the mutable WebDAV fileserver example.
- [x] Add deterministic Lua example and packed coverage for opt-in audio capture/playback device workflows.
- [x] Add user-facing Lua docs and direct JSON passthrough helpers for `vectis.lockd`, keeping direct `lockdc` access available as `vectis.lockd.raw`.
- [x] Add narrow `vectis.lockd` workflow helpers for one-shot JSON enqueue and acquired-lease handler cleanup without hiding direct `lockdc` document/queue APIs.
- [x] Add narrow `vectis.lockd` JSON state save/load helpers that mirror C acquire/update-or-load/release workflow semantics without hiding direct `lockdc` leases.
- [x] Migrate public Lua lockd state/queue examples onto `vectis.lockd` helpers and cover them with deterministic local stub tests.
- [ ] Publish a separate `vectis` Lua rock for users who want to run the Vectis facade inside their own Lua 5.5 environment.
- [x] Keep LuaRocks out of the `vectis` binary runtime and release artifacts.
- [ ] Add Vectis-owned Lua modules for Kore and broader libssh2 coverage.
- [x] Add a Vectis-owned Lua SSH command execution helper backed by libssh2.
- [ ] Keep the Lua framework model aligned with the C SDK model: dependency-native access plus a Vectis-owned DX layer with matching concepts and behavior.
- [x] Support shebang execution through `#!/usr/local/bin/vectis`.
- [x] Provide a friendly REST API DX for defining buffered JSON routes, handlers, middleware-like hooks, JSON responses, and downstream JSON calls.
- [x] Add `head` to the `vectis.rest.client` downstream JSON client for method parity with `vectis.http.head`.
- [x] Add `options` to `vectis.http` and `vectis.rest.client` for method parity with C curl-backed OPTIONS workflows.
- [x] Add `options_json` to `vectis.http` and its reusable client for JSON-normalized OPTIONS workflows.
- [ ] Extend the REST DX with lockd operation presets only where examples show repeated service boilerplate.
- [x] Add a C-owned Lua `server:static_directory()` helper for serving disk docroots such as extracted packed asset trees.
- [x] Add a C-owned Lua `server:json()` helper for fixed unguarded JSON endpoints in packed API service scenarios, including explicit method and status configuration.
- [x] Add matching method and status configuration to the C-owned Lua `server:auth_json()` helper for fixed guarded API endpoints.
- [x] Add Lua examples for API server, including packed execution coverage, lockd document workflow, and queue producer/dequeue workflows.
- [x] Add Lua examples for downstream API calls with normal and packed execution coverage.
- [x] Add a Lua example for SFTP transfer with deterministic local e2e coverage.
- [x] Add a Lua example for long-running consumer service workflows with deterministic local e2e coverage.
- [x] Add a Lua example for SSH command execution with deterministic local e2e coverage.
- [x] Add Lua examples for XML, DSV/CSV/TSV, WebDAV client, mutable WebDAV server, dependency-native OpenSSL, certificate management, SCP, MQTT, SMTP, generic curl, OPC UA client, and dependency-native CAI local workflows as those facades land.
- [x] Add a Lua example for stateful SFTP/libssh2 lower-level handle operations with opt-in live e2e coverage.
- [ ] Add Lua examples for CAI live/service-adapter workflows and loaded-model sus/whisper transcription as those facades land.
- [ ] Add Lua unit and end-to-end tests for every Lua facade, including preload smoke, method-call DX, ownership/finalizer cleanup, structured error objects, packed execution, large-value streaming/spooling, and local deterministic protocol scenarios.
- [x] Add release/package verification that the `vectis` binary includes all statically preloaded Lua modules while C binary SDK artifacts do not accidentally ship Lua source/runtime/package-manager state.
- [x] Add a checked Lua facade documentation index that links dependency-native modules, Vectis-owned workflow modules, the coverage matrix, and module-level docs.
- [x] Expose the full public Vectis status enum in Lua and cover structured DSV callback-stop errors with `status`, `status_string`, and `message`.
- [x] Extend Vectis-owned Lua `nil, err` objects with C error source metadata and optional dependency/http/detail diagnostics.
- [x] Add checked Lua facade conventions and a focused contract test for structured errors, programmer misuse, ownership, and explicit payload naming.

## Area 13: Single Binary Packaging

- [x] Add a `vectis -a pack` action that creates a new runnable binary from the Vectis runner plus a Lua script on Linux.
- [x] On Linux/ELF, use a self-describing appended trailer with magic, version, lengths, hashes, and metadata.
- [ ] On Darwin/Mach-O, embed Lua and certificate payloads through a generated object/section layout rather than relying on arbitrary appended EOF data.
- [x] Define one shared payload manifest format across ELF and Mach-O so runtime validation and Lua startup behavior stay platform-independent.
- [x] Support optional embedding of the liblockdc client certificate bundle payload in the Linux pack format.
- [x] Validate embedded lockd client bundle hashes before executing a packed Lua script.
- [x] Wire embedded lockd client bundles into the statically registered lockdc Lua module through flexible bundle sources (`lc_source` memory/callback sources) without writing private runtime files.
- [x] Validate payload bounds and hashes before executing embedded Lua.
- [x] Preserve normal `vectis script.lua` execution when no embedded payload exists.
- [ ] Add Darwin packing flags for automatic codesigning after the final Mach-O artifact is produced, including `--codesign <identity>`, `--ad-hoc-codesign`, `--hardened-runtime`, `--timestamp`, and `--entitlements <path>`.
- [ ] Ensure Darwin packing never mutates the executable after codesigning.
- [ ] Add verification commands/tests for packed Darwin binaries using `codesign --verify --strict --verbose=4` and, when available, `spctl --assess --type execute`.
- [x] Document operational limits around signing, notarization, stripping, hardening tools, and platform-specific executable formats.
- [x] Add first smoke test that packages and executes a Lua script artifact.
- [x] Add deterministic e2e coverage for a packed Lua webserver with generated embedded assets, native auth routes, SMTP-delivered email-token login, auth-guarded JSON API route, WebDAV key issuance, authenticated embedded WebDAV reads, mutable WebDAV writes, WebDAV list/copy/move/delete, traversal denial, and embedded-versus-WebDAV mutation isolation.
- [x] Add packed smoke coverage that extracts embedded assets and serves the extracted docroot through the Lua `server:static_directory()` C-owned route.
- [x] Add tests that package a Lua API service and a Lua consumer service, then execute both artifacts.
- [x] Add a packed-service scenario that runs the embedded site/WebDAV server and lockd `startconsumer` service in the same self-contained Vectis binary and process, with HTTP/WebDAV requests still succeeding while the consumer receives and handles lockd messages.

## Area 14: Verification and Release

- [x] Add unit coverage for current runtime/config behavior.
- [x] Add a first fuzz target around `lonejson` validation via Vectis.
- [x] Add a non-conflicting local integration compose environment for mTLS lockd disk, mTLS lockd S3 over MinIO, SSH/SFTP, and MQTT.
- [x] Add Makefile entrypoints for local integration services: `dev-up`, `dev-down`, `dev-reset`, `dev-ps`, and `dev-logs`.
- [x] Add `make test-e2e` and `make test-all` entrypoints.
- [x] Add the first compose-backed lockd e2e smoke against disk and S3-backed lockd services.
- [x] Add integration/e2e tests against local lockd and Kore.
- [x] Add integration/e2e tests against the local SSH/SFTP and MQTT compose services.
- [x] Add integration/e2e tests for Lua runner behavior.
- [x] Add package/archive generation for the C SDK and Vectis binary.
- [x] Add package/archive generation for the current C SDK.
- [x] Add optional arm64 Apple Darwin release archive and smoke-test zip generation when osxcross is available.
- [ ] Add a GitHub Actions Darwin arm64 verification workflow using hosted `macos-15`/`macos-latest` runners to execute the smoke-test zip and codesign checks.
- [ ] Add release verification for GNU and musl deliverables.
- [ ] Add on-device Darwin smoke verification once a Mac is available.
- [x] Add generated or checked API docs for C and Lua.
