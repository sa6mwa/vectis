# Backend logging must be routed through pslog

## Required contract

All diagnostic output from backends exposed by Vectis must go through a
pslog logger at the corresponding severity. This includes construction,
configuration, operation, failure cleanup, and destruction. Application data,
CLI help, and returned structured errors are separate from backend diagnostic
output. Disabling logging must silence the backend rather than restore its
stdout/stderr/syslog fallback.

The Vectis wrappers should accept a pslog logger, retain it for Lua handle
lifetimes, and inherit the configured Vectis logger for managed services.
Backend threads must call native sinks only. Logger destruction must follow
backend shutdown and completion of outstanding log callbacks.

This report records gaps in the current SDK interfaces. It is not a claim
that Vectis already implements this contract everywhere.
The dependency pins inspected here are c.pkt.systems 0.10.0 and libpslog 0.10.0.

## c.pkt.systems OPC UA: constructor-time hook is missing

The public `cpkt/opcua.h` provides native configuration callbacks only after
`cpkt_opcua_client_new()` or a server constructor returns. No logging option
is accepted by the constructors.

Source inspection of `src/opcua.c` shows:

- `cpkt_opcua_client_new()` calls `UA_Client_new()` followed by
  `UA_ClientConfig_setDefault()` before returning the handle.
- `cpkt_opcua_server_new()` calls `UA_Server_new()` followed by
  `UA_ServerConfig_setMinimal()` before returning the handle.
- JSON constructors likewise construct/configure the native server before
  exposing it to the caller.

open62541 installs `UA_Log_Stdout_new(UA_LOGLEVEL_INFO)` when the config has
no logger. Configuration and teardown paths log through this default plugin.
Installing a Vectis adapter afterward cannot cover those earlier records or
constructor-failure cleanup. Vectis's cleanup test also visibly emits native
OPC UA records outside pslog.

### Requested upstream surface

Expose a C89-compatible log callback and context in constructor configuration,
installed before any open62541 initialization that can log. Cover client,
plain server, JSON server, security reconfiguration, and failure cleanup.
Allow explicit silence without creating a stdout logger. Preserve the same
callback through event-loop, certificate, security-policy, and PubSub logging;
these plugins can retain pointers to the initial logger.

The callback should receive already formatted message bytes and an explicit
length, severity, and category, borrowed for the duration of the call. Handle
open62541's extended `UA_String_format` specifiers upstream; ordinary
`vsnprintf()` is not a substitute for its formatter. Define callback threading,
ownership, and quiescence before releasing the callback context.

Vectis can then map trace/debug/info/warning/error directly to pslog and
retain the category as a structured field. Emit backend fatal diagnostics at
pslog error level with a `backend_level=fatal` field: pslog fatal/panic emission
can independently terminate the host, and the adapter must not add that effect.

## c.pkt.systems audio: device logging cannot be configured publicly

`cpkt/audio.h` exposes no log sink or logger in its configuration. In
`src/audio.c`, native capture/playback use `ma_device_init(NULL, ...)` or
`ma_device_init_ex(..., NULL, ...)`. The internally created miniaudio context
therefore cannot receive a Vectis log object before initialization.

miniaudio supports a context `pLog`, but its implementation here is private
and built with `MA_API static`. Its debug-output callback uses `printf()`
when `MA_DEBUG_OUTPUT` is enabled. This is a potential bypass, not evidence
that this SDK enables that macro. Process audio currently redirects child
stderr to `/dev/null`, so those diagnostics are discarded rather than routed.

### Requested upstream surface

Add an optional C89 log sink/context to audio construction configuration.
Install a miniaudio log callback before device/context initialization and keep
it through teardown; disable the native debug-output sink. Specify which
decoder, encoder, converter, and device paths can emit diagnostics.

For process devices, route child diagnostic lines through the same callback
using bounded chunks, including long lines and EOF fragments. Drain diagnostics
concurrently with PCM I/O so a filled stderr pipe cannot block audio. Specify
severity for process diagnostics that carry no native severity. Silence must
be explicit, and process shutdown must wait for the diagnostic reader before
releasing its callback context.

## c.pkt.systems SUS: process-global lifetime and severity contract

`cpkt_sus_log_set()` already accepts a C89 callback, defaults to silence, and
bridges whisper/ggml logs. Vectis currently exposes `sus.set_log_sink(fn)`,
which calls Lua directly from this backend callback. That interface should
be replaced with a pslog logger interface under the new requirement.

Before enabling it on native workers, upstream must document or implement
safe replacement/unregistration. The inspected implementation writes global
callback/context variables without synchronization while logging reads them;
there is no drain/quiescence API. A Lua registry reference alone cannot keep
a freed callback context safe against an in-flight native callback.

Use one process logger for this global backend, rather than attributing a
global callback to whichever service happened to install it last. Define
when configuration locks, when shutdown drains, and how multiple runtimes
share registration. Verify severity conversion explicitly instead of casting
ggml's enum to the facade enum; normalize continuations without unbounded
line accumulation and account for concurrent producers.

## libpslog Lua interop: native-sink capability is not exposed

The SDK's `pslog_lua_check_logger()` / `pslog_lua_ref_logger()` validate and
retain a Lua logger, but do not reveal whether its sink invokes Lua. The
borrowed-view `flags` field is currently reserved. Native logging threads
cannot safely invoke a Lua function sink, even if the pslog logger serializes
its own writes.

Expose a documented sink capability through the public interop API, propagated
through derived loggers. A native-thread adapter must reject Lua-bound sinks
without inspecting private userdata layouts. Retaining the logger must also
keep the sink's resources valid until native logging has stopped.

## Local Vectis fixes completed

- WebSocket callback errors now use the app logger at error level. The live
  test covers all three callbacks, configured file output, suppression,
  collection of temporary logger objects, and absence of stderr duplicates.
- The seed's owned sink stays open while derived app/service loggers use it.
  Previously, closing the root after derivation shut down the shared file sink.
- The live logging test passes in Debug and ASan and fails against the old
  binary. The full Debug suite passes all 176 tests; formatting also passes.

## Remaining Vectis work

- Add logger options to standalone OPC UA/audio wrappers once constructor
  logging is available; service lifecycle logging alone does not cover the
  backend itself.
- Replace SUS's arbitrary log callback with a safe native pslog bridge once
  sink capability and global callback lifetime are enforceable.
- Add a logger to XML parsing configuration and use libxml2 context/reader
  error handlers. The current reader uses `XML_PARSE_NONET | XML_PARSE_COMPACT`
  and installs no error handler, so default parser diagnostics are a gap.
- Make Kore's optional file access log use pslog too. Structured access records
  and normal Kore operational logging already have pslog adapters; the
  independent legacy file sink is still outside the requested contract.
- Audit dependency-native CAI/lockdc Lua constructors as well as their managed
  service configuration. Managed clients already receive pslog loggers.
- libcurl verbose/debug output is not enabled by the inspected Vectis paths.
  If added, route only diagnostics through pslog, avoid logging payloads or
  credentials, and keep libcurl's returned errors in the existing error API.

## Acceptance evidence for an updated SDK and Vectis wrappers

Capture stdout/stderr separately from a configured pslog sink. Exercise
successful creation, forced initialization failure, normal traffic, warnings,
TLS/security failures, and teardown. Assert diagnostics appear only in pslog,
at the expected level and with subsystem/category fields. Repeat with logging
disabled and with concurrent native producers. Include multiple Lua states,
coroutine collection, logger collection/close, callback replacement, and
shutdown racing a log callback. Verify bounded handling of long diagnostic
chunks and absence of audio I/O stalls.

Run focused Debug and ASan/UBSan tests plus native Valgrind lifetime checks;
test packaged static and shared SDKs, not only upstream build trees. Native
macOS CI can use the existing GitHub runner pattern documented in
[the Darwin SDK report](cpkt-darwin-libcurl-async-dns.md). Add logging fixtures
to the native Linux/macOS suites; model downloads and physical audio hardware
must not be required for the fast callback/lifetime tests.

No dependency source or extracted SDK has been modified as part of this audit.
