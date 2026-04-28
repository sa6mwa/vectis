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

- [x] Provision the `liblockdc` 0.3.0 SDK archives into `.cache/deps/...`.
- [x] Use the `liblockdc` SDK bundle as the source for lockdc, pslog, lonejson, curl, OpenSSL, libssh2, nghttp2, and zlib headers/libraries.
- [x] Verify downloaded `liblockdc` SDK archives with pinned SHA-256 checksums.
- [x] Provision pinned Lua 5.5.0 from lua.org into every target dependency root for the Vectis binary runtime.
- [x] Build Lua as a static `liblua.a` plus headers for host, Linux cross, musl, and optional Darwin target roots.
- [ ] Add dependency manifest validation in CMake for the pinned Lua version and checksum.
- [x] Support host debug and Linux release matrix roots.
- [ ] Add explicit dependency manifest validation in CMake, including expected dependency versions.
- [x] Add optional arm64 Apple Darwin dependency provisioning from the `liblockdc` 0.3.0 SDK archive.
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

- [x] Define opaque-handle-plus-method-table API surface.
- [x] Implement app config defaults.
- [x] Add defensive server/Kore guardrail defaults for connection count, request sizes, read/write timeouts, idle timeout, and keepalive behavior.
- [x] Rename server body/write timeouts to idle-timeout semantics and add minimum request-body rate guardrails.
- [x] Add per-route request-body policy presets for JSON/default bodies and streaming large uploads.
- [x] Implement thread-safe app lifetime and route registry state.
- [x] Support owned or borrowed `pslog` logger instances.
- [x] Store and validate lockd configuration without requiring a live lockd daemon during app construction.
- [x] Instantiate and own a real `lc_client` during non-Kore runtime startup, with structured lockdc errors on open failure.
- [x] Instantiate the app-owned `lc_client` before Kore starts and make it available to route handlers only after successful startup.
- [x] Expose stable accessors/helpers for the shared logger and raw lockd client escape hatch.
- [x] Implement dependency-backed C helpers for curl, SSH exec/SFTP, self-signed certificate bundles, request/response JSON parse/serialize, and JSON route auto-wiring.
- [x] Translate manual Kore TLS material from path, memory, and `lc_source` inputs into runtime server cert/key configuration.
- [ ] Complete the remaining dependency-backed behavior for CA-signed certificate generation, ACME, and full Kore TLS/runtime configuration.
- [x] Expose dependency headers and low-level handles/APIs as explicit escape hatches without making them the primary DX.
- [x] Define the first C pass of one Vectis-owned naming, source, error, timeout, ownership, and cleanup convention.
- [x] Add stable string helpers for status, error source, HTTP method, and body mode names.
- [ ] Mirror the C naming, source, error, timeout, ownership, and cleanup convention in Lua.
- [x] Define request/response abstractions that do not leak Kore internals unless explicitly requested.
- [x] Add common C source constructors for path, memory, and `lc_source` material inputs.
- [x] Add route constructors that infer literal, named-parameter, optional-parameter, and regex path kinds.
- [x] Mirror Kore's supported HTTP methods in the Vectis C SDK: GET, POST, PUT, DELETE, HEAD, OPTIONS, and PATCH.
- [x] Support one route handler registered for a method mask such as GET | HEAD.
- [x] Add upload route constructors that attach the streaming upload body policy in one call.
- [x] Add a handle-shaped C HTTP client API that can inherit app logging/defaults.
- [x] Add high-level C SDK examples before implementation hardens, so the intended DX drives API shape rather than wrappers around internals.
- [x] Group C examples by SDK domain under `examples/kore`, `examples/lockd`, `examples/curl`, `examples/ssh`, `examples/certs`, and `examples/raw`.
- [ ] Add C examples that build against installed Vectis headers and libraries.
- [ ] Add install-tree tests for static and shared downstream C consumers.
- [ ] Add runnable example smoke tests against local lockd and a real Vectis/Kore server once runtime startup is implemented.

## Area 5: HTTP / Kore Integration

- [x] Build and link Kore against the dependency bundle shipped by `liblockdc`.
- [x] Start/stop a real Kore runtime from `vectis`.
- [x] Translate the currently supported `vectis_server_config` guardrails into concrete Kore runtime/config settings.
- [ ] Implement the remaining guardrails that Kore does not expose directly yet: response-write idle timeout, minimum body transfer rate, and keepalive request count.
- [x] Wire `pslog` into the Kore runtime path so Vectis server logs and Kore runtime diagnostics use the configured app logger.
- [ ] Expose raw Kore configuration/runtime escape hatches where practical.
- [x] Register Vectis C routes as Kore handlers.
- [x] Support method-specific handlers and router-style dispatch.
- [x] Draft route path classification for literal paths, named path parameters, and explicit POSIX regex routes.
- [x] Add public route-builder helpers for literal, named/optional parameter, and regex route registration.
- [x] Add internal Vectis router dispatch for literal, regex, named parameters, optional parameters, method masks, and safe path-param population.
- [x] Attach the internal Vectis router dispatch to real Kore request handling.
- [x] Expose request body, headers, query parameters, path parameters, and response helpers.
- [x] Register JSON routes as ordinary Vectis routes with lonejson parse/serialize adapter dispatch.
- [ ] Translate per-route body policies into concrete Kore request-body buffering, streaming, and spool-to-disk behavior.
- [ ] Support Kore features through Vectis where practical, including TLS, ACME, websocket, static asset, file upload, and runtime configuration features.
- [ ] Define the supported one-process multi-instance behavior based on what Kore can safely provide.
- [x] Add real HTTP integration tests.
- [ ] Add real HTTPS integration tests.

## Area 6: TLS / Certificate Configuration

- [x] Define Vectis-owned TLS config surface with default TLS-first semantics.
- [x] Validate bundle versus cert/key inputs before runtime startup.
- [x] Add flexible path/source/memory TLS material configuration for Kore server cert/key bundles, split cert/key material, CA bundles, and client-CA bundles.
- [x] Translate manual Vectis TLS cert/key bundle, split cert/key, and client-CA material into Kore runtime behavior.
- [ ] Translate Vectis ACME TLS config into Kore runtime behavior.
- [ ] Support lockd client certificate bundle configuration from both C and Lua.
- [ ] Support server certificate bundle configuration from both C and Lua.
- [ ] Add Vectis-owned OpenSSL-backed helpers for generating private keys, CSRs, self-signed certificates, CA material, and client/server PEM bundles.
- [ ] Expose lower-level OpenSSL access for advanced users while keeping certificate workflows as the primary C and Lua DX.
- [ ] Support embedding a lockd client certificate bundle into a packaged Vectis binary.
- [ ] Support ACME configuration and certificate lifecycle.
- [ ] Support reload/update of key material where Kore allows it.

## Area 7: JSON Surface

- [x] Integrate `lonejson` into the Vectis build and expose validation helpers.
- [x] Keep Kore JSON-RPC unsupported in Vectis phase 1.
- [x] Patch Kore ACME JSON handling and request-body helpers to use `lonejson`.
- [ ] Replace the remaining Kore JSON parsing surface with `lonejson`.
- [ ] Expose C request-body parse helpers that support buffer, alloc, and spool-to-disk strategies.
- [x] Draft request-body policy configuration for buffer versus streaming/spool-to-disk strategies.
- [ ] Expose C response serialization helpers for large payload streaming.
- [ ] Expose Lua JSON encode/decode helpers through the bundled `lonejson` Lua rock or a Vectis facade.
- [ ] Make `lonejson` the backing implementation for Vectis request parsing, response serialization, lockd document helpers, and JSON HTTP client helpers.
- [ ] Add higher-level JSON REST helpers for request validation, response serialization, and downstream API calls.
- [x] Add first-pass typed JSON route auto-wiring backed by lonejson mapped structs.

## Area 8: lockd / Workflow Runtime

- [ ] Wrap the public `liblockdc` 0.3.0 C API behind Vectis service-friendly helpers where doing so improves DX.
- [ ] Integrate the `lockdc` Lua binding into the Vectis Lua runtime.
- [ ] Provide C and Lua helpers for document store, retrieval, query, leases, enqueue, dequeue, ack/nack, and retry-oriented workflow patterns.
- [ ] Expose raw `liblockdc`/`lockdc` access for complete API coverage while making Vectis helpers the preferred workflow API.
- [x] Make lockd optional for Kore-only C services while preserving validation for configured lockd transports.
- [ ] Define consumer registration and lifecycle APIs for C.
- [ ] Expose lockd consumer service startup/configuration directly, then layer Vectis-owned worker DX on top.
- [ ] Define Lua consumer-service runner mode as a separate process from Kore server mode.
- [ ] Make it clear in API errors when a Lua script attempts to run incompatible server and consumer loops in one process.
- [ ] Add integration tests covering enqueue/dequeue/ack workflows.
- [x] Add raw liblockdc C examples for open client, lease save/load, query, attachments, enqueue, manual dequeue, and managed consumer services.

## Area 9: Downstream HTTP / curl

- [x] Add dependency-backed C helpers around libcurl for service-friendly downstream HTTP calls.
- [x] Draft the C helper API for libcurl-backed HTTP, JSON request methods, generic upload/download, SFTP, and MQTT publish workflows.
- [x] Add an opaque C HTTP client handle so repeated downstream calls do not have to pass raw config everywhere.
- [x] Add handle-level C helpers for GET, DELETE, POST JSON, PUT JSON, and PATCH JSON.
- [x] Add dependency-backed C MQTT publish helpers through libcurl.
- [ ] Add Lua curl bindings that support all libcurl URL schemes made available by the bundled libcurl build.
- [ ] Provide JSON-aware Lua helpers for common API calls using `lonejson`.
- [ ] Provide C and Lua Vectis helpers for JSON API requests, downloads, uploads, streaming responses, SFTP upload/download, retries, and structured errors.
- [ ] Expose raw curl handles/options for complete protocol coverage.
- [x] Support SFTP file retrieval and storage through curl where it is sufficient.
- [ ] Expose timeout, retry, header, TLS, client certificate, proxy, redirect, and streaming configuration in C and Lua.
- [ ] Add integration tests for HTTP(S) downstream calls and SFTP operations.

## Area 10: SSH / libssh2

- [ ] Add Lua bindings for libssh2 session setup, authentication, command execution, stdout/stderr capture, exit status, and timeout handling.
- [ ] Add C and Lua Vectis helpers for connecting to SSH servers and running commands with captured stdout, stderr, exit status, timeout, and structured error handling.
- [x] Draft the C helper API for libssh2 command execution with captured stdout/stderr, exit status, and SFTP upload/download.
- [x] Implement libssh2-backed C SSH command execution with captured stdout/stderr and exit status.
- [x] Implement libssh2-backed C SFTP upload/download helpers for cases where curl-backed SFTP is not enough.
- [ ] Expose raw libssh2 sessions/channels for advanced control.
- [ ] Decide which lower-level SFTP operations need libssh2 bindings beyond curl-backed file transfer.
- [ ] Add C helpers only where they support the Vectis service model rather than exposing libssh2 wholesale.
- [ ] Add integration tests for remote command execution against a controlled test SSH server.

## Area 11: OpenSSL / Certificate Binding

- [ ] Add Lua bindings or facades for certificate loading, parsing, validation, and bundle assembly.
- [ ] Keep raw OpenSSL exposure narrow; prefer Vectis certificate workflows over dumping OpenSSL APIs into Lua.
- [ ] Support client and server certificate management in the shared Vectis config model.
- [x] Draft the C helper API for OpenSSL-backed certificate bundle generation.
- [x] Implement OpenSSL-backed self-signed certificate/key PEM bundle generation for the C SDK.
- [ ] Add tests for valid, expired, malformed, and missing certificate material.

## Area 12: Lua Runtime and Framework

- [x] Reserve Lua-facing runtime concerns in the TODO contract and API design.
- [ ] Add a `vectis` executable that embeds Lua.
- [x] Make the `vectis` executable build depend on the Vectis-provisioned Lua 5.5 static library rather than a host Lua installation.
- [x] Add the Linux `vectis` executable entrypoint wrapped in single-header `libpid0` for PID 1 behavior in `FROM scratch` containers.
- [ ] Replace the placeholder executable submain with the real Lua runner.
- [ ] Configure Lua package paths so bundled/native Vectis modules load without user setup.
- [ ] Provide a `require("vectis")` facade that exposes the high-level framework.
- [ ] Integrate existing Lua rocks for lockdc, pslog, and lonejson.
- [ ] Build lockdc, pslog, lonejson, and Vectis-owned Lua modules against the provisioned Lua 5.5 ABI.
- [ ] Register bundled Lua modules statically through `package.preload` in the `vectis` binary; do not require runtime `.so` loading.
- [ ] Publish a separate `vectis` Lua rock for users who want to run the Vectis facade inside their own Lua 5.5 environment.
- [ ] Keep LuaRocks out of the `vectis` binary runtime and release artifacts.
- [ ] Add Vectis-owned Lua modules for Kore, curl, OpenSSL certificate workflows, and libssh2.
- [ ] Keep the Lua framework model aligned with the C SDK model: raw dependency access plus a Vectis-owned DX layer with matching concepts and behavior.
- [ ] Support shebang execution through `#!/usr/local/bin/vectis`.
- [ ] Provide a friendly REST API DX for defining routes, handlers, middleware-like hooks, JSON responses, lockd operations, and downstream calls.
- [ ] Add Lua examples for API server, lockd document workflow, queue producer, queue consumer, downstream API call, SFTP transfer, and SSH command execution.
- [ ] Add Lua unit and end-to-end tests.

## Area 13: Single Binary Packaging

- [ ] Add a `vectis pack` or equivalent command that creates a new runnable binary from the Vectis runner plus a Lua script.
- [ ] On Linux/ELF, use a self-describing appended trailer with magic, version, offsets, lengths, hashes, and metadata.
- [ ] On Darwin/Mach-O, embed Lua and certificate payloads through a generated object/section layout rather than relying on arbitrary appended EOF data.
- [ ] Define one shared payload manifest format across ELF and Mach-O so runtime validation and Lua startup behavior stay platform-independent.
- [ ] Support optional embedding of the liblockdc client certificate bundle.
- [ ] Load embedded lockd client bundles through liblockdc 0.3.0 flexible bundle sources (`lc_source` memory/callback sources) without writing private runtime files.
- [ ] Validate payload bounds and hashes before executing embedded Lua or loading embedded certificates.
- [ ] Preserve normal `vectis script.lua` execution when no embedded payload exists.
- [ ] Add Darwin packing flags for automatic codesigning after the final Mach-O artifact is produced, including `--codesign <identity>`, `--ad-hoc-codesign`, `--hardened-runtime`, `--timestamp`, and `--entitlements <path>`.
- [ ] Ensure Darwin packing never mutates the executable after codesigning.
- [ ] Add verification commands/tests for packed Darwin binaries using `codesign --verify --strict --verbose=4` and, when available, `spctl --assess --type execute`.
- [ ] Document operational limits around signing, notarization, stripping, hardening tools, and platform-specific executable formats.
- [ ] Add tests that package a Lua API service and a Lua consumer service, then execute both artifacts.

## Area 14: Verification and Release

- [x] Add unit coverage for current runtime/config behavior.
- [x] Add a first fuzz target around `lonejson` validation via Vectis.
- [ ] Add integration/e2e tests against local lockd and Kore.
- [ ] Add integration/e2e tests for Lua runner behavior.
- [ ] Add package/archive generation for the C SDK and Vectis binary.
- [x] Add package/archive generation for the current C SDK.
- [x] Add optional arm64 Apple Darwin release archive and smoke-test zip generation when osxcross is available.
- [ ] Add release verification for GNU and musl deliverables.
- [ ] Add on-device Darwin smoke verification once a Mac is available.
- [ ] Add generated or checked API docs for C and Lua.
