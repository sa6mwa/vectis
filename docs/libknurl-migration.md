# Migrate Vectis from vendored Kore to libknurl

## Decision for the current checkout

Use libknurl's public C API as the server boundary. The released 0.1.0 SDK
supports the main route, upload, proxy, WebDAV and host lifecycle operations
needed for the extraction. The cutover is a moderate refactor across the C
receivers, Lua bindings and SDK packaging. The proposed cutover requires a
CAI release built for LoneJSON ABI 27 and a libknurl preflight operation
that preserves Vectis's startup error boundary.
Resolve the raw Kore SDK header contract before removing the vendored headers.

## Baseline and release evidence

This assessment compares Vectis `45aa88579b0dd456d83c42336d700a0a89a4ed61` with
libknurl 0.1.0, tag commit `af24dac260efc123a03a4e9d5ea4067125e1bb8a`.
The reference adapter in libknurl targets Vectis
`6b1e263e0436687f68a7768ca55f4361a6e59e19`. The current Vectis delta relevant
to this cutover is the owner-thread Lua upload and DSV streaming path added
with cpkt 0.7.0. The remaining delta is dependency, compression and docs
work. [Release](https://github.com/c89systems/libknurl/releases/tag/v0.1.0) ·
[Reference adapter contract](https://github.com/c89systems/libknurl/blob/v0.1.0/docs/vectis-integration-contract.md)

Vectis pins pristine Kore `1ff3c0f6f7973dc7fc8bf38369790ef2d8079b3b`.
libknurl packages `c5fe59f521bdc1f2c486b5d8cc90227d72fcc628`, three upstream
commits later. One changes HTTP request queue insertion. The other two are
task and PostgreSQL changes outside Vectis's built Kore source list. All
42 Vectis patch files match the first 42 libknurl patches byte for byte
and in order. The additional 24 patches cover the library boundary, typed
hooks, native I/O lifetime, process ownership and further backend fixes.
The released SDK's `share/knurl/BACKEND` and a runtime call to
`knurl_kore_revision()` identify the new Kore pin. During implementation,
verify behavior under the released build's guards and recheck request
ordering on the wire. [Vectis Kore pin](../vendor/kore/REVISION) ·
[libknurl patch series](https://github.com/c89systems/libknurl/blob/v0.1.0/patches/kore/series) ·
[Backend defect record](https://github.com/c89systems/libknurl/blob/v0.1.0/KORE.md)

Inspected assets were checked against the release digests.

| Asset | SHA-256 |
| --- | --- |
| x86_64 GNU SDK | `6b8ad0b1ec40cedb70cc19d61749ec9309d70cb29cd0814d086a49bbe97bfd77` |
| x86_64 musl SDK | `1ca9dcb3f48776915ad8122782ae340636f644adfe63a05b5657c56f6185dceb` |
| Lua source archive | `15395db50c33c4751f4746f2172590c05c1b52d2c6460a800c9b0669a2fabcbb` |

The installed C package provides `knurl::static`, `knurl::shared` and
`knurl.pc`. It ships separate from cpkt, LoneJSON and pslog. Both inspected
ELF SDKs use `libknurl.so.0` and depend on `liblonejson.so.27`. The tag
source was used for comparison and to compile three public reference
fixtures. The SDK libraries came from the published assets.
[SDK layout](https://github.com/c89systems/libknurl/blob/v0.1.0/docs/sdk.md)

## Dependency alignment

| Dependency | Required action |
| --- | --- |
| cpkt and pslog | Vectis already pins the exact versions required by libknurl, cpkt 0.7.0 and pslog 0.13.0. Match the target and digest in each SDK's `DEPENDENCIES` record when adding the pins. |
| LoneJSON | Move the native SDK and authoritative Lua source to 0.47.0 together. The receiver layouts changed and the shared ABI moved from 26 to 27. Recompile the Vectis CLI bindings, app schema and record consumers, DSV, REST, XML, OpenAPI, outbound clients, and any downstream examples using those layouts. |
| Lockdc | Vectis's 0.28.0 shared library names `liblonejson.so.26` in its dynamic dependencies. Lockdc 0.29.0 is released for LoneJSON 0.47.0. Upgrade the core, matching PostgreSQL provider, and Lua package together. The 0.29.0 release retains Lockdc core ABI 9, PostgreSQL provider ABI 0 and the 0.28.x Pouch storage contract. |
| CAI | Vectis's CAI 0.6.0 shared library also needs `liblonejson.so.26`. Its installed CMake ABI check explicitly rejects 27. The latest CAI release inspected was 0.6.0. Obtain ABI 27 SDKs and the matching Lua source from CAI upstream before changing the Vectis dependency closure. |

The released libknurl CMake config rejected Vectis's current 0.46.0 SDK.
It configured with 0.47.0, and a subsequent `find_package(cai 0.6.0 EXACT)`
rejected the ABI 27 SONAME. Use one LoneJSON ABI throughout the executable,
SDK, and Lua VM. Header overrides, SONAME aliases and loading both versions
would leave prebuilt code compiled against different receiver layouts.
[LoneJSON 0.47.0](https://github.com/c89systems/lonejson/releases/tag/v0.47.0) ·
[Lockdc 0.29.0](https://github.com/c89systems/liblockdc/releases/tag/v0.29.0) ·
[CAI 0.6.0](https://github.com/c89systems/cai/releases/tag/v0.6.0)

## Upstream handoff and compatibility decisions

### libknurl startup preflight

Vectis's `vectis_app_start()` calls `vectis_internal_kore_validate()` before
creating the HTTP child control socket and forking the runtime. That check
probes the listener and redirect listener, checks the configured access-log
file, and prepares the body spool and TLS/ACME settings. A bad listener or
log path returns a structured error to the original caller at that point.
The current child run path passes a null error output and the parent mainly
observes readiness or child exit.
[Host startup](../src/vectis.c:12765) ·
[Current preflight](../src/vectis_kore_bridge.c:3839)

libknurl's `knurl_server_validate()` intentionally validates declarations and
TLS/ACME material without binding listeners. It leaves file access-log
opening to runtime setup. In both GNU static and shared probes an occupied
loopback port and a directory as the access-log destination returned
`KNURL_OK` from validation. `knurl_server_run()` then returned
`KNURL_BACKEND` with a bind or access-log diagnostic. This is a compatibility
gap in the cutover and is consistent with the released API contract.
[Validation and run implementation](https://github.com/c89systems/libknurl/blob/v0.1.0/src/kore_runtime.c) ·
[Access-log setup](https://github.com/c89systems/libknurl/blob/v0.1.0/src/kore_logging.c)

Request a public, opt-in startup resource preflight from libknurl and a new
SDK release. It should reuse the real backend's listener and file validation
and return operation, native cause and resource context in `knurl_error`
before Vectis forks. Cover all listener kinds, configured access logs and
applicable body spool policies. Preserve declaration-only validation and the
authoritative checks during `run()`. The probe should release any temporary
descriptors and leave the server ready for a subsequent run. Port availability
can change between the probe and the real bind. Acceptance cases are a busy
HTTP or redirect listener, an unusable log destination, an unusable configured
spool when offload is enabled, and a corrected configuration that starts.
Duplicating bind and open policy in the Vectis adapter would split ownership of
the same checks. A change to report these failures after forking would require
an explicit Vectis startup contract decision and structured failure forwarding
on the existing control channel.

### Raw Kore access and file logging

`vectis_request_kore()` can forward the borrowed opaque pointer through
`knurl_kore_request()`. The libknurl SDK installs `<knurl/kore.h>` with a
forward declaration and keeps the native Kore definitions private. The current
Vectis SDK installs `<kore/*.h>` and its installed C test includes
`<kore/kore.h>`. That fixture only uses a null native pointer, so it provides
no evidence of an in-tree raw transport consumer. The released shared knurl
export set contains the accessor and hides native transport operations.
Keep the opaque accessor and move the installed fixture to the supported
header contract. Before dropping the raw headers, confirm whether any
supported downstream caller dereferences native structures or drives a native
response. A required inspection needs a versioned public accessor upstream.
A required transport handoff needs a supported libknurl operation with explicit
ownership, using its public channel API where that covers the actual use.
Copying private layouts from either Kore revision would bind consumers to the
wrong public contract. [Installed Vectis fixture](../tests/install/consumer.c) ·
[libknurl accessor](https://github.com/c89systems/libknurl/blob/v0.1.0/include/knurl/kore.h)

Explicitly set `access_log_rotation_disabled` for Vectis's existing
`access_log_path` option to preserve its append-only behavior. libknurl's
default is 10 MiB and three total files. Its writer also requires an owned
regular path, rejects final-component symlinks, and continues serving on
startup ENOSPC. Vectis's old preflight opens the path for append and returns
the open error. Include these cases in the preflight handoff and agree on
which path and failure policy the Vectis option promises before the switch.
[libknurl file logging](https://github.com/c89systems/libknurl/blob/v0.1.0/docs/logging.md) ·
[Vectis option](../manual/lua-app.md:143)

## Adapter boundaries

Keep the existing Vectis C/Lua declaration shapes and receivers. Translate
configuration, errors, borrowed views, callback state and ownership into
libknurl operations. Preserve application policy and object lifetimes in Vectis.
Use the public reference adapters as examples for the mapping. In production,
register Vectis's actual declarations and callbacks with the libknurl router.

| Surface | Migration |
| --- | --- |
| App runtime and signals | Translate the current `vectis_server_config` defaults and limits into initialized `knurl_server_config`, including listener/redirect declarations, deadlines, access levels and effective worker settings. Replace backend globals and stop signaling with `knurl_server_run`, `knurl_server_request_stop` and `knurl_server_notify_signal`. Retain the outer supervisor, quiescence checks and process-group ownership. |
| Readiness, control and cleanup | Use `on_parent_ready`, the serving gate, `knurl_server_supervision_take`, loop watches and timers. Carry the existing READY/STOP and failure/metrics protocol on the owned FD, including EOF and malformed control handling. Keep the parent maintenance and auth cleanup callbacks. Use the terminal worker hook after native teardown and the existing forked-parent cleanup after run returns. Preserve error status through both paths, including a handler that frees the app. |
| Worker notifications | Keep the bounded Vectis supervisor producer/consumer channel and its worker-to-supervisor wake descriptor. Create process-local resources after fork. `knurl_waker_signal` addresses cross-thread notifications on one owning loop. The existing XA/outbox commit receipt path still crosses processes and requires the Vectis channel and its wake. |
| Routes and bodies | Forward path kinds, method masks, route groups, auth and before/after hooks into the router. Preserve mixed registration order and handler selection. Map ordered headers/query fields, request identity and principal, body policies, borrowed readers and independent copies. Keep Vectis request userdata alive through the request-close callback. |
| Uploads and DSV | Register `knurl_router_upload_stream` for owner-loop Lua upload and DSV open/write/finish/close. The C callback can report bytes consumed and pause with AGAIN. Lua and the DSV parser stay on the request owner thread. Native blocking reader routes use `knurl_router_upload_reader` with copied metadata and acknowledged cleanup. Keep DSV parsing and app-specific authorization in Vectis. |
| Replies, JSON and REST/OpenAPI | Map staged replies to `knurl_reply_*` and live replies to the source/response operations. A small `lc_source` adapter forwards read/reset/close and preserves borrow/take and one terminal close. Map the existing explicitly file-backed source response to the spooled operation and keep live streaming incremental. Forward mapped JSON, array readers, XML and REST/OpenAPI to libknurl using the same LoneJSON records. Check ordinary JSON completes before after-hooks, live source reset follows hooks, and HEAD/bodyless framing and repeated Set-Cookie retain current semantics. |
| XML and reusable codecs | Forward mapped parse/write, DOM/XPath/SAX and source/sink operations through the public XML receivers. Keep Vectis-owned public wrappers and app callbacks. Remove the duplicate server-side execution paths once the matching public cases pass. Outbound HTTP and independent application codecs keep their existing entry points. |
| Static and WebDAV | Supply packed asset open/stat/read/list/close and the durable-store provider from Vectis. Move routing, static ranges/conditions, WebDAV method handling and standalone filesystem operations to libknurl. Map path, quota, admission, deadlines, auth/principal, status and commit results. Preserve direct-root and managed-cache semantics and marker handling. Use the shared filesystem implementation for standalone consumers, including a worker without a running server. |
| Reverse proxy and WebSocket | Translate targets, per-route defaults, edits, admission/error/response hooks and terminal exchange observations to `knurl_router_proxy`. Vectis retains its application log/metric formatter. Replace the server-side HTTP/WS relay, curl loop, queues and framing with libknurl execution. Use its application WebSocket/SSE operations for ordinary app routes. |
| TLS/ACME, policy and metrics | Translate TLS identities, source material, redirect/host selection and ACME callbacks. Keep durable credential lookup and ACME state ownership in Vectis. Borrow the same public pslog loggers, preserve category and status levels, and forward trusted-proxy/admission/autoblock config. Map terminal request/TLS/connection and proxy exchange observations to Vectis metrics. Check that each outcome is counted once, including early rejects. |

The previous extraction gaps around standalone filesystem calls, mount admission,
deferred deadlines, fixed WebDAV capacity and proxy terminal accounting have
public operations in 0.1.0. Preserve the Vectis-specific disabled deadlines
where the reference translates them. Current route composition, Lua lifetimes,
and actual provider callbacks still need consumer tests. An operation requiring
a new transport state machine, buffering for a streaming route, or private
backend access belongs in a specific upstream request with a parity case.
[Operation inventory](https://github.com/c89systems/libknurl/blob/v0.1.0/docs/vectis-integration-contract.md) ·
[Extraction contract](https://github.com/c89systems/libknurl/blob/v0.1.0/docs/server-api-spec.md)

## Implementation and packaging sequence

1. Obtain the upstream fixes and aligned CAI release. Complete the LoneJSON
   0.47.0, Lockdc 0.29.0 and CAI dependency alignment as an independently
   testable Vectis change. Rebuild every native module and verify the one-ABI
   link closure, schema/record ownership, app workflows and embedded Lua bytes.
2. Add the selected libknurl SDK and hashes for all seven targets to
   `scripts/dependency-pins.tsv` and the normal acquisition/manifest flow.
   Use an independent prefix, imported-target headers and conditional
   `find_package(knurl CONFIG REQUIRED COMPONENTS ...)` for the server build.
   Link the static SDK and its provider closure for the Vectis executable
   and static library consumers. Use the shared target for shared Vectis,
   with proper relative loading in relocated artifacts. Keep the existing
   server-disabled build supported.
3. Implement the full forwarding adapter and remove the replaced implementations
   in the same cutover. Retire the Kore object list, generated assets/platform
   headers, local feature defines and hook externs, and old vendor header
   injection. Remove the vendoring scripts, patch prerequisites and source
   archive staging of the patched tree. Update Make targets, CI, presets,
   instrumentation and clean-build/source-reconstruction checks to prepare
   installed SDKs. Port backend-private fixtures to public or wire-level
   assertions, or run them as libknurl upstream tests where they cover private
   implementation.
4. Update the installed `vectisConfig.cmake`, `vectis.pc`, SDK bundling,
   manifests/licenses, example dependencies and Darwin install-name handling
   for `deps/libknurl` and its runtime closure. Verify actual compile and link
   commands in clean and incremental builds. Remove stale vendored inputs
   from owned build outputs. Update manual, coverage and release evidence with
   the actual application behavior.
5. Keep the existing Vectis Lua facade as the application layer over the C
   adapter, including Linux packed builds. The separate authoritative Knurl
   Lua package currently builds a shared `knurl::lua` target against shared
   Knurl, Lua and provider modules. Its preload function accepts an existing
   VM. Direct `require("knurl")` inside the static executable would be a
   separate exposure decision. If required, ask upstream for a supported
   static embedding target with the same released facade, Lua entry point and
   provider references, so it can share the executable's one runtime.

Relevant wiring is in [dependency discovery](../cmake/vectis_dependencies.cmake),
[Kore build sources](../CMakeLists.txt:415), [Makefile](../Makefile),
[source staging](../scripts/stage_release_sources.sh),
[SDK packaging](../cmake/package_archive.cmake), and the
[released Knurl Lua build](https://github.com/c89systems/libknurl/blob/v0.1.0/lua/CMakeLists.txt).

## Verification and acceptance

- **Investigation results.** GNU C89 probes linked and executed against both
  released library modes. The configuration, host lifecycle and service
  forwarding fixtures passed against the released shared SDK. The host
  fixture exercises readiness, control stop,
  malformed control, EOF and fork-exit cleanup. The services fixture exercises
  upload, static and WebDAV adapters. The negative ABI configurations and
  startup preflight observations are recorded above. The musl SDK received
  artifact and dependency inspection. Scratch probes and logs are under
  `build/libknurl-0.1.0-migration/`.
- **First consumer gate.** Build the complete adapter before the broad suite.
  Compare the Vectis C and Lua paths on direct and supervised startup,
  application readiness, stop/restart, startup failures, worker death policy,
  control EOF/parent death and terminal cleanup. Prove bounded XA/outbox
  notification reaches the supervisor without a reconciliation cycle.
- **Wire and ownership gate.** Exercise route order, raw query and repeated
  headers, after-hook failure timing, true incremental upload/DSV/XML/SSE,
  streaming cancellation and source close, HEAD/204/205/304, zero-length
  files, HTTP pipelining, proxy duplex backpressure and WS/WSS, interrupted
  WebDAV replacement and storage recovery, TLS and local-CA ACME. Compare
  existing app diagnostics and metrics, including distinct attempted and
  completed proxy exchange outcomes.
- **Runtime and stack gate.** Use the existing normal and Linux packed
  application suites and representative live service fixtures. Verify native
  musl default-worker stacks and constrained glibc Debug workers through
  nested callbacks, TLS, codecs, WebDAV and proxy flows. Run cleanup, allocation
  failure and memory checks with the appropriate pinned runtime. Sanitizer
  coverage inside libknurl itself requires its instrumented upstream build.
- **Artifact gate.** Build all seven targets. Exercise native GNU/musl and
  applicable ARM runtime cases and record Darwin compile/link separately from
  native kqueue evidence. Verify server-disabled builds, standalone static
  CLI and packed execution, relocated static/shared C89/C++98 and pkg-config
  consumers, examples, complete runtime closure, source archives and Lua rocks.
  Check embedded manual and Lua source bytes. Require a fresh Vectis semantic
  integration review before release.

The reference results qualify the released provider and its sample mappings.
Actual Vectis normal/packed parity is a gate for the consumer cutover.
