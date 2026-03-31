# vectis TODO

This file is the contract for implementation status. Areas are intentionally
coarse enough that we can ask whether one area is complete without ambiguity.

## Area 1: Project Skeleton

- [x] Create CMake project with presets and Makefile entrypoints.
- [x] Add static/shared library build options.
- [x] Add unit test wiring.
- [x] Add sanitizer and fuzz presets.
- [x] Add `make clean` that removes `build/`, `dist/`, and `.cache/`.
- [x] Enforce a C89 (`-std=c89` / C90) contract for the `vectis` public API and internal implementation.

## Area 2: Dependency Provisioning

- [x] Auto-download `liblockdc` dev archives into `.cache/deps/...`.
- [x] Auto-download single-header `pslog` and `lonejson`.
- [x] Support host debug and Linux release matrix roots.
- [ ] Add runtime/shared-library artifact provisioning for dynamic release packaging validation.
- [ ] Verify musl and cross targets against the full release matrix in CI.

## Area 3: Kore Upstream Management

- [x] Vendor Kore as an upstream checkout plus patch stack workflow.
- [x] Add upgrade/apply/status make targets.
- [x] Record and pin a vetted upstream revision for the first shipping integration.
- [x] Add actual Kore patch files for vectis-specific behavior.
- [x] Add automated assertions that the patch stack applies cleanly after upgrade.

## Area 4: Core vectis Runtime

- [x] Define opaque-handle-plus-method-table API surface.
- [x] Implement app config defaults.
- [x] Implement thread-safe app lifetime and route registry state.
- [x] Support owned or borrowed `pslog` logger instances.
- [ ] Instantiate and own a real `lc_client`.
- [ ] Support shared lockd client/runtime access between handlers and consumers.

## Area 5: HTTP / Kore Integration

- [x] Build and link Kore against the dependency bundle shipped by `liblockdc`.
- [ ] Start/stop a real Kore runtime from `vectis`.
- [ ] Register vectis routes as Kore handlers.
- [ ] Support one-process multi-instance operation to the extent Kore allows.
- [ ] Add real HTTPS integration tests.

## Area 6: TLS / ACME DX Surface

- [x] Define vectis-owned TLS config surface with default TLS-first semantics.
- [x] Validate bundle versus cert/key inputs before runtime startup.
- [ ] Translate vectis TLS config into Kore config/runtime behavior.
- [ ] Support ACME configuration and certificate lifecycle.
- [ ] Support reload/update of key material where Kore allows it.

## Area 7: JSON Surface

- [x] Integrate `lonejson` into the vectis build and expose validation helpers.
- [x] Keep Kore JSON-RPC unsupported in vectis phase 1.
- [x] Patch Kore ACME JSON handling and request-body helpers to use `lonejson`.
- [ ] Replace the remaining Kore JSON parsing surface with `lonejson`.
- [ ] Expose request-body parse helpers that support buffer, alloc, and spool-to-disk strategies.
- [ ] Expose response serialization helpers for large payload streaming.
- [ ] Design and implement higher-level JSON REST / RPC helpers.

## Area 8: Queue Consumer Runtime

- [ ] Define consumer registration and lifecycle APIs.
- [ ] Start lockd queue consumers from vectis-managed worker threads or Kore-compatible tasks.
- [ ] Share logger and lockd runtime with HTTP handlers.
- [ ] Add integration tests covering enqueue/dequeue/ack workflows.

## Area 9: Lua Preparation

- [x] Reserve Lua-facing runtime concerns in the TODO contract and API design.
- [ ] Add clean runtime boundaries for future Lua bindings.
- [ ] Add Kore start/stop/config bindings suitable for Lua exposure.
- [ ] Integrate upcoming `liblockdc` Lua bindings once available.

## Area 10: Verification and Release

- [x] Add unit coverage for current runtime/config behavior.
- [x] Add a first fuzz target around `lonejson` validation via vectis.
- [ ] Add integration/e2e tests against local lockd and Kore.
- [ ] Add package/archive generation.
- [ ] Add release verification for GNU and musl deliverables.
