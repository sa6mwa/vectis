# Vectis Lua upgrade seam audit

## Current libmdf 0.14.0 upgrade

The upgrade starts from `ae48963` and changes only libmdf's seven SDK pins, its
Lua source pin and the expected version. Other dependency versions and archives
remain unchanged. This is development integration evidence, not release approval
or a resumption of the separately paused SQL facade goal.

[The published release](https://github.com/sa6mwa/libmdf/releases/tag/v0.14.0)
identifies `c018a03` and retains shared ABI 5. Comparison with 0.13.0 found no
changes to the public C API beyond generated version macros, Lua module/binding
sources, imported target names or dependency closure. The release contains internal ANSI link-label/wrap and
manual-finish fixes, table code-span pipe handling, emission-capacity failure
classification and HTML/deck variable-font deduplication. Existing receiver,
callback, allocator, ownership and process/thread contracts remain applicable.

| Artifact | 0.14.0 SHA-256 |
| --- | --- |
| x86_64 GNU SDK | `b250a82e2562897102aee69bca2c946e53eadffddd1d85131975eaea00651b65` |
| x86_64 musl SDK | `144827811d9e1477a6e0835b8a94492c2a1ba7eb7fa6d091ee0e0c73f2e7b2ee` |
| aarch64 GNU SDK | `2909a4888a8e422ba0fe59efd6676a105be692fce5f67d0b2d0d3ea3e978afb6` |
| aarch64 musl SDK | `0f6ebaf7dd8754fa08bef3b14b7a15dc8f5074bf64ecd353d39e007aa963a574` |
| armhf GNU SDK | `2534b31a2c6222fac57b30e6ed3f63424d6b981c8bf1d5ad54fdebb00827b136` |
| armhf musl SDK | `1b02a34681d4e730849b8bd90afe52c44f0d825436e52674aa45c70f5e2bdc5f` |
| arm64 Darwin SDK | `d4cf9445fe175810a2cd223a7928374094aab2944894a99b50b3b11cac8972fe` |
| Lua source | `fcff4bf1b4eb9165dadd76fa4dc9a2c4b8d8f409bb7200469f4b6e6177cf30e8` |

The embedded upstream binding links the same static SDK as the CLI; its direct
module and `vectis.libs.libmdf` identity are retained. `vectis.terminal` already
forwards the affected options and translates callback failures; no rename-only
wrapper is needed. Agent Smith checks native renderer failures on its existing
bounded renderer thread, which never calls Lua or Softline directly. The docs
pager uses the same public renderer and checked status. No new app lifecycle or
logging configuration is introduced. Source-rock optional-native loading and
static/shared CMake/pkg-config closure remain required consumer seams.

New deterministic tests cover the fixes through native C and direct/convenience
Lua, including normal and Linux packed execution. The escaped pipe Lua probe and
native bounded-emission status probe both failed against the old 0.13.0 SDK.
These are behavior checks, not assertions that a method merely exists.

The responsible agent reviewed the pin changes, callback cleanup, direct and
convenience calls, source-rock loading, packed execution and SDK consumers.
The existing source-rock smoke had retained the old MQTT upload assertion after
the POST fix; it now checks POST, upload=false and the exact buffered body. No
MQTT implementation was changed. Installed CMake consumers now render the
escaped-pipe fixture through public C receiver methods, and pkg-config consumers
create/destroy an MDF receiver, proving that the declared dependency is usable.

Completed verification:

- `make finalize-slice CTEST='ctest --stop-on-failure'`: lifecycle, tool discovery,
  resolver and privacy contracts; all 213 debug tests (37 fast and 176 broader).
- Focused rendering/Smith/example/C failure checks: all five passed. The new C
  failure test took under 0.01s and combined normal/packed Lua regression about
  0.2s before the full gate; no sleeps or delay-based faults were added.
- Native Memcheck of the C bounded-buffer failure probe and Lua regressions:
  zero errors, zero bytes allocated at exit, including callback failures.
- `make test-install-tree`: direct and extracted static/shared SDK consumers,
  CMake/pkg-config linkage, exact exports and pinned-runtime checks passed.
  Follow-up extracted static/shared checks passed with actual MDF rendering and
  receiver construction in the strengthened consumers.
- `scripts/test_lua_rock.sh`: passed after correcting the stale assertion.
- x86_64 musl release CLI: statically linked; the same rendering regression
  passed in normal and packed execution.
- All seven SDK pins and the Lua source pin match the published checksum
  manifest. GNU SONAME remains `libmdf.so.5`; its glibc symbol requirements match
  0.13.0. Public declarations and Lua sources agree with the released facade,
  allowing generated SDK version macros.

ARM and native Darwin execution were not run for this focused upgrade; their
pins are updated and validated, not runtime-certified. No hosted workflow,
release, push or deployment was performed. Other dependency versions and the
SQL facade prerequisite remain unchanged.

## Historical supervisor integration record

Status: recorded integration evidence for code committed as `e20ab55`. Earlier
iteration results below are historical; the completed verification section is
the final result for that code, not approval of a later release candidate.

This is an agent semantic integration review of the working candidate on `init`,
based on `b1fe538013c6f9e6bfceee03cf9ee006b0f6f48a`. That commit already
changed pins; the previous pin baseline is its parent `2fa69d8`. The reviewed
candidate also contains the supervisor/workflow implementation and the fixes
recorded below. It is not a release approval. Required upstream gaps remain,
and native Darwin/live-provider execution is not established by Linux tests.
A release must repeat the mandatory AGENTS.md reasoning review against its final
committed candidate; scripts, symbol presence and this matrix cannot approve it.

## Artifact identity and comparison

The configured Bootlin stable 2026.08 GNU compiler and current host-debug SDK
were used. Prior digest-addressed release headers, Lua sources, docs and examples
were extracted under ignored `build/lua-seam-audit/baseline`; no adjacent SDK or
repository was modified. Header/doc diff records there substantiate comparison.

| Artifact | Previous pin | Candidate | Candidate GNU SHA-256 |
| --- | --- | --- | --- |
| c.pkt.systems | 0.10.0 | 0.11.0 | `fbd3e3f357c22dc26b94cc7e5e200651b59ecb39357ce7bb8c4cb3d939a2b557` |
| LoneJSON | 0.44.0 | 0.45.0 | `aef11c4511bb6aa8ed4e0646204e14b7934affd6e5cec5b8e8bff36e80217826` |
| LoneJSON Lua | 0.44.0 | 0.45.0 | `1066e37671f0f1e4076aa7f71206a0e29220cafb1508526d510abca0ab9dfb7d` |
| Softline | 0.6.0 | 0.9.0 | `09612ec2cc3fc1ecdcefaf4afff4e5e80555d9618c13a7e6db3405d0ead835f5` |
| Softline Lua | 0.6.0 | 0.9.0 | `98421d66740042aca38a18eb32245e736b49f6495595ffacd282c51c99b139fe` |
| libpid0 header | 0.4.2 | 0.5.0 | `9647a9027e3f5086278cc628b4cfdf853e239a04c202b4f3577691202431c7e1` |
| liblockdc | 0.18.0 unchanged | 0.18.0 | `1e949887d613824716645dd5e9a85a1b89ad2d8d66431838c0a28c999b0e23da` |
| liblockdc Lua | 0.18.0 unchanged | 0.18.0 | `94852adfc713299d748d808cd4ff5cbe3e4d66f0bc6ce5be6e80d97cd981cc89` |
| pslog | 0.12.0 unchanged | 0.12.0 | `21239262bc06eecb504e45e0902f374ccd86751dcb32087dd84af4d2553ea0e2` |
| CAI | 0.6.0 unchanged | 0.6.0 | `f7f78d9f847f1265267bbc33a6d6cb2058602d82967357ab561f36ebb1d0daf8` |
| LQL | 0.3.0 unchanged | 0.3.0 | `fafb04f9af5d9610f77380ac1f848d1edd3d60dcf7ce6a098f6a04e30dd2b47a` |
| MDF | 0.13.0 unchanged | 0.13.0 | `6be317bb232702cab5fb2f7f6041fa6b6cc0cb6d3676cb1230f80a3778c1c4b4` |

The nominal transitive upgrade baseline includes SUS/whisper 1.9.1→1.9.4,
open62541 1.5.4→1.5.8 and libxml2 2.15.3→2.15.4. The extracted
prior **0.10.0 archive manifest already reports** whisper 1.9.4, open62541
1.5.8 and libxml2 2.15.4. These actual digest-pinned artifact contents, rather
than version labels inferred from earlier planning, govern the semantic diff.
SUS/audio public headers are unchanged between those resolved archives.
The resolved open62541 patchset changes from `mqtt-prefix-packet-headers-2`
to `mqtt-prefix-packet-headers-crypto-logging-24`, adding crypto/logging/lifecycle
ownership fixes at the same resolved upstream version.
LoneJSON Lua init files are identical upstream; the Vectis copied partial root
was the omission. LoneJSON public header changes document zero-initializing
curl parser adapters and safe reinitialization; Vectis already zeroes that
state before initialization and cleans it on success/failure. Deployment docs
switch upstream development fixtures to rootless Podman; Vectis does not import
the upstream fixture orchestrator.

The prior GNU digests from `2fa69d8:scripts/deps.sh` are:

| Changed artifact | Prior SHA-256 |
| --- | --- |
| c.pkt.systems | `fb64caa3cad66e01669705412cd88cb4267025ca0a083fa936fe25786011391d` |
| LoneJSON | `5255a32cef6dd89cd4f068c2a4f1bd6a8f8e79d5cffe6d6922cb2506d320c520` |
| LoneJSON Lua | `7a967f0231867357be22cf23d4fa7b5f7437eaccd54765d1c02a857c51b47160` |
| Softline | `b1e4f2fb8ad4d8072acf1d72be31ec23066d8ccbf66a6fe10e15b7243ac8ac2f` |
| Softline Lua | `f8cc7ea7c3dbf3b336c55be10972435a03b5783d8773b8506e928806ebefaa36` |
| libpid0 header | `907fb7f084d192da1c6d92d26b1b6cd93bb4dafe36737856696f47de906dd5e8` |

Unchanged Lua-source digests also checked in the configured manifest:
pslog `35e46f594b2ce12b42bba95357109fd662ce1c2ab7a175702e9b9d141cb0ec2b`,
CAI `977e9b615dd78127fba6574eb2ac8527f52c4e6c578faeece15f39c8ed7eaeab`,
LQL `c4610615f4315081c519afe47d519e72c1242cce449620cf90f8f8828eccdfe1`,
MDF `a4c4ac66c2bf67aeb6d19f76e13ea245e34dce291221803dea079db8d7df1eb8`.

## All dependency and bundled-library decisions

| Dependency | Public/native and Lua boundary | Vectis integration and exclusion |
| --- | --- | --- |
| Lua 5.5.1 | cpkt runtime plus new strict-C89 full Lua facade and warning callback | Existing owner runtime, preload/resources, app/routes and callback ownership retained. Native stack/warning hooks are embedder C facilities; standard Lua warning semantics remain. No arbitrary foreign-state entry. |
| LoneJSON 0.45 | Full upstream core/facade, schemas, records, visitors, Base64/JWT/JWK/JWKS/OIDC/OAuth2 and streaming sinks | Upstream resource import plus minimal schema/rewrite overlay; provider compile targets enabled. Existing auth/REST/OpenAPI/DSV/curl streaming workflows kept. Materialized tables and file-backed spools named accurately. |
| Softline 0.9 | Full automatic native import; queue modes, native output streams/quotes/status/image path; removed boxed layout | `terminal.editor` passes official options. Smith queue/output streaming already uses public methods. Queue/path failures verified; X11 and interactive resize remain separate device gates. Private bundled c-ares 1.34.8 (MIT, plus BSD address sorting) closes its resolver linkage. |
| libpid0 0.5 | Public header integration | Diff removes a 50ms post-child delay; API and ownership unchanged. Existing child/signal/runtime supervision tests verify behavior; no Lua PID wrapper invented. |
| liblockdc 0.18 | Full upstream state/queue/outbox/source/sink import plus newly identified public endpoint helpers | App-owned canonical options/logging bridge, consumer and workflow post-fork clients, real receipt FD delivery, recovery and cleanup. Direct root remains upstream. Pull/auto-renew/kind selection/candidate FD limits detailed below; shared leadership is future Vectis work. |
| pslog 0.12 | Full upstream Lua facade and official native `pslog_lua.h` capability/ref interface | OPC UA native sink interop, exact lengths/FATAL nontermination, native app/logd/service seeds. Function outputs rejected where worker emission is possible. Borrowed native clients close children before seed cleanup. |
| CAI 0.6 | Full upstream facade | Existing local constructors/response/agent/tools/MCP, worker envelopes, genuine CAI client request/output streams and logger inheritance retained. Lua app MCP is deliberately buffered and stateless: public CAI has an HTTP response sink, but this established Lua route collects tool results/responses. Persistent/streamed Lua server behavior needs an explicit future product contract; no MCP streaming claim is made. Provider/MCP remote execution requires opt-in gates; constructor presence is not a live-provider claim. |
| LQL 0.3 | Full upstream facade | Local data pipelines and packed dependency surface retained. No redundant renamed wrapper; repeated Vectis-owned query lifecycle would trigger convenience integration. |
| MDF 0.13 | Full upstream facade | Terminal Markdown and Smith incremental rendering retained. Renderer document ownership remains explicit; whole-response buffering is not introduced. |
| OpenSSL 3.6.4 | Existing Lua EVP digest/HMAC/sign/verify/codecs/CSPRNG; new SDK C89 facade | Auth/OIDC/cert/TLS/ACME/public signing workflows retained. Native OpenSSL pointer graphs remain C-only; require a concrete ownership-bearing workflow before extending Lua. |
| curl 8.22.0 | Existing public options/upload/download/read/write facade | HTTP/REST/WebDAV/SMTP/MQTT, worker config/logging, HTTP and CAI/DSV streams retained. Public option passthrough provides existing auth mechanisms. No new GSS/SASL context objects are needed to retain curl's current options. Live broker/mail/provider gates remain opt-in. |
| nghttp2 1.70.0 | HTTP/2 dependency plus new C89 facade | Native curl/Kore protocol closure retained; protocol frames and callback pointer machinery remain C-only until a Vectis-owned HTTP/2 operation requires them. |
| zlib 1.3.2 | Full existing Lua compression helpers; configured header guard change | Embedded resources/content compression and static/shared closure retained. |
| libxml2 2.15.4 | Existing XML↔LoneJSON mapping and native writer | Lua parsing materializes mapped values/records, potentially with spooled fields. New Lua `xml.write` genuinely pushes native chunks from private table input; explicit buffered serialization shares its emitter. No DOM stream is claimed. DOM pointer trees intentionally C-only; no new DOM product requirement. |
| miniaudio 0.11.25 | Existing cpkt audio public receiver facade/Lua | Decoder/encoder/VOX/PTT and managed copied mailbox work retained. Lua decoder callbacks run under public owner reads; device capture remains opt-in. |
| whisper.cpp 1.9.4 / SUS ABI 0 | Existing catalog/cache/transcriber/segmented methods and global backend logging | Foreign logs copy into bounded owner queue; other Lua callbacks reject foreign entry. Explicit owner log source supports app loops. Per-app backend logger inheritance is excluded until public per-model/context routing exists. Native global continuation severity remains an upstream race before our synchronized sink; independent constructor emitters have no shared serialization. Model inference remains opt-in. |
| open62541 1.5.8 / OPC UA ABI 0 | Full cpkt client/server facade plus structured logger constructors/setters | Public pslog capability retention, managed ownership lease and app sink before startup, joined teardown/mute, duplicate rejection, callback reentry guards and deferred GC in both whole-VM creation orders. Existing subscriptions/events/async/security/discovery/PubSub graph retained; native pointers/plugins excluded. External event-loop delete contract requires embedder quiescence. |
| MQTT-C 1.1.2, commit `0f4c34c8cc00b16cfee094745d68b8cdbaecd8e0` | OPC UA PubSub dependency plus new C89 raw facade | Existing OPC UA graph/config and curl MQTT facade kept. No separate raw packet/session Lua API without an established workflow. Live MQTT is not claimed by disabled graph probes. |
| libssh2 1.11.1 | Public C library plus new C89 facade | Existing Vectis SSH/SFTP/SCP receivers preserved. Raw dependency-native channels deliberately C-only. SSH investigation/live execution parked by task authority. |
| MIT Kerberos 1.22.2 / cpkt GSSAPI | **New public C89 facade**, native security contexts | Deliberate C-only context/credential ownership boundary. Existing curl/auth workflows remain through their official options. Reconsider with a specified app Kerberos context/delegation workflow. |
| Cyrus SASL 2.1.28 / cpkt SASL and SASL-plugin | **New public C89 facades**, mechanisms/plugins/callbacks | Native plugin/credential/concurrency boundary; no current application plugin DSL or LDAP ownership workflow. Reconsider concrete SASL-context integration; do not misclassify as a mere archive. |
| OpenLDAP 2.6.15 | New transitive/public native library | Auth/curl/Postgres deployment closure inspected; no existing Vectis LDAP client product. No new LDAP Lua facade inferred. |
| PostgreSQL 18.6 / cpkt Postgres | New public C89 facade and native library | Lua/database facade explicitly separate task; current task does not claim one. GSS/SASL-related closure remains native until that task defines ownership. |
| SQLite 3.53.4 / cpkt SQLite | New public C89 facade/constants | Explicitly separate task; no persistence migration invented for current Pouch products. |
| libHaru 2.4.6 / cpkt PDF | New full public C89 PDF facade | Explicitly separate PDF task. |
| libpng 1.6.58 | New upstream full native header/library surface | Explicitly separate PNG task; does not imply a Lua facade here. |
| iODBC 3.52.16 | New native deployment/link dependency | No existing Vectis ODBC application interface. Keep SDK consumer closure; a defined ODBC workflow would trigger separate design. |
| vendored Kore `1ff3c0f6f7973dc7fc8bf38369790ef2d8079b3b` | Existing patched server/event/fork control runtime | App worker/control/auth/transports preserved. Supervisor uses a separate bounded public channel; VRC1 stays private. Real worker routes prove owner PID/main-state effects via FD fast path. Native Darwin kqueue workflow retained. |
| Embedded Swagger UI / QR support | Existing resource/vendor inputs | Static assets and TOTP/QR behavior unchanged; no new export inferred. |

## Traced seams and fixes

LoneJSON constructors now reach the authoritative facade rather than a copied
subset. Resources/preload and source self-description expose both overlay and
upstream. The source rock excludes the embedded LoneJSON overlay, preserving legitimate
optional absent native cores without hiding loader exceptions. The native object
also receives JWT/OIDC/OpenSSL compile usage requirements: a successful link alone
had previously left features compiled out.

OPC UA direct client/server/JSON/file constructors install logging before native
initialization; setters retain replacements through C changes and old destruction.
The shared native log adapter maps levels/categories/status and calls exact-length
`log_view`, including NUL and nonterminating FATAL. App service workers inherit
or mute the seed before startup; quiesced cleanup removes the borrowed app sink
before context/logger release. App-owned Lua servers reject concurrent direct
operations and defer GC teardown until the borrow ends.

SUS uses a static mutex/context and bounded copied records. Foreign producers
never touch Lua, oversize/full/error counts are observable, replacement/VM-close
discard queued generations, and Lua executes without the mutex. The first
explicit setup installs a stable native hook before workers; later replacement
and unregister do not rewrite upstream's unsynchronized global callback fields.
Concurrent VM registration claims the subscriber atomically and rejects a foreign
owner. Segment/progress/abort/segmented/cache callbacks guard the creation owner. Explicit `sus_logs`
provides owner-loop pumping without overwriting process-wide sink ownership.

Canonical workflow **and consumer** options resolve actual native app defaults
post-fork, propagate flat crypto/bundles and pass original query syntax to public
native open. Decoded helper checks replace handwritten query-name parsers in
native app and ACME default encryption. App-owned Pouch roots default to shared
writer mode for default and flat configurations as well as explicit shared queries;
exclusive query mode remains native-authoritative and cannot support additional
app-owned clients. Typed exclusive mode is rejected. Percent-encoded explicit key names cannot
be overwritten by a default. App-owned private client children close before the
borrowed app logger. Native app Lockd options reject unknown fields, NUL strings
and malformed arrays rather than silently diverging from child clients; explicit
app mute survives logger derivation. Standalone interop is deliberately excluded because child
handles can retain clients beyond Lua close without an official logger capability.

Supervisor inbox allocation is checked and bounded to 16 MiB. Producer send and
owner receive/start/stop do not wait for a producer-held process-shared mutex;
contention leaves level readiness intact. Signals alternate fairly with ordered
data and rotate among finite kinds. Ingress closes atomically before app-owned
workers join; mapping/fds remain until safe destruction/reprepare. External C
producer threads must be quiesced by their owner before restart/destruction.
Linux robust owner death poisons ingress rather than accepting corrupted frames;
Darwin has no Linux robust-attribute branch and must pass native platform gates.
Apple's current [libpthread mutex implementation](https://github.com/apple-oss-distributions/libpthread/blob/main/src/pthread_mutex.c)
accepts `PTHREAD_PROCESS_SHARED`; an old rwlock manual is not evidence that it
rejects shared mutexes. The tracked native Darwin workflow is narrowly allowed
by the local lifecycle contract, without authorizing a push or hosted run.

Scope failure stores a copied code/message and emits owned service diagnostics;
other stop-policy scopes continue. Handler-returned native status works with a
null error pointer. Cleanup runs once after a failed start/stop; successful
restart clears diagnostics. Timer disable removes native deadlines and enable
resets monotonic scheduling. Meaningful unsupported lane/restart/quota/scope
options fail explicitly. GC callbacks restore live app scope ownership and close
sources before logger teardown. Protected client opens pin immutable endpoint strings across callback mutation
and collection. Protected transactions/opens retain the app and reject lifecycle
changes while private native children borrow its logger, preserving table errors
and all return values. Canonical client validation rejects sparse/dictionary
endpoint lists, native string NULs and truthy substitutes for booleans.
Transaction false or nil/error rolls back;
caught unknown-kind failures remain sticky. Short stateful handlers support guarded
checkpoint update/completion; renewal there fails explicitly.

## Prior iteration behavior evidence and limits

The focused fixtures are executable specifications: `upgrade_seams` covers
schemas/runtime/visitor failures/cleanup, binary Base64, malformed JWT/PKCE,
Softline queues/config/image-path failures and OPC UA native logger lifetimes.
`supervisor_client_options` covers app defaults, encoded query crypto, flat config,
encrypted reopen, client logging, source/workflow GC, copied scope diagnostics
and explicit SUS owner-loop pumping, endpoint mutation/GC and callback lifecycle
rejection without poisoning later successful calls. Both execute normal and Linux packed modes.
Native units exercise pthread and SIGSTOP contention, finite bounds, fairness,
cleanup counts, disabled deadlines and optional error pointers; actual SUS source
fixtures prove foreign callback rejection, bounded logs, replacement/two-VM/
coroutine shutdown. OPC UA source fixtures prove worker-thread exact-length FATAL
records and cleanup. Live network/model/device gates are separate.

Real Kore route tests use two worker processes and owner-main-state effects.
The direct case sets candidate probes to 1,000,000 ms and disables discovery;
the one-shot SUS timer is drained and disabled before HTTP commits begin;
all eight sequential commits produce owner-main-state effects through the FD path.
Eight-commit
saturation/recovery is a separate scenario. Same-process producer auto-notify
statistics alone are not accepted as that proof. See the
[supervisor workflow audit](supervisor-workflow-audit.md) for detailed commands.

Fresh remediation verification was serialized. Logs are under ignored
`build/lua-seam-audit/`; these are working-candidate results, not release approval.

| Command/check | Result and evidence |
| --- | --- |
| `make test-failures-fast` | 32/32 passed. Builds CLI plus the three required native lifetime units before running the existing failure-fast label. New normal/packed upgrade, workflow, worker-route and client fixtures are included. `failure-fast-final.log`. |
| Focused OPC UA e2e/reentry, ACME state, runnable workflow example and upstream stateful contract probe | Passed. Actual local OPC UA service behavior remains separate from constructor probes; the stateful probe demonstrates the upstream defect, not successful renewal. `verification-final.log` (6/6). |
| Workflow and canonical-client normal/packed `ctest --repeat until-fail:5` | Each passed five consecutive repetitions. Initial repeat exposed a test assumption that automatic retry could not immediately repeat; upstream full jitter includes zero. The corrected test verifies bounded attempts and durable retry state, plus an explicit 30-second delayed retry without waiting. `repeat-workflow-final.log`. |
| `vectis_cli_self_describe` repeated three times | Passed. Raw exported `lonejson.upstream` equals the installed authoritative source; stdout's annotated transcript is intentionally a different interface. `repeat-final.log`. |
| Valgrind with full definite/indirect/possible leak checks, origins, exit-on-error 99 | Supervisor/SUS/OPC UA native units and Lua upgrade fixture: zero errors, all heap freed. Workflow and canonical-client/GC fixtures: zero errors and zero lost bytes; respectively 4,955,634 bytes/9 blocks and 4,956,212 bytes/17 blocks reachable in upstream process caches. No suppressions. `memcheck-*-final.log`. |
| Bootlin ASAN+UBSAN supervisor build/run (`detect_leaks=1:abort_on_error=1`, `halt_on_error=1`) | Passed, including same-process producer stop/join/mapping lifetime, SIGSTOP contention and cleanup callback counts. `asan-build-final.log`, `asan-supervisor-final.log`. |
| Static/shared build, install under `build/`, `verify_installed_sdk.sh ... static/shared .cache/deps/host-debug` | Both passed: installed C89/C++98 and pkg-config consumers, exported-symbol policy, 36 example runtime closures per linkage mode and missing-runtime detection. `shared-build-final.log`, `sdk-*-final.log`. |
| `VECTIS_VERSION=0.0.0 VECTIS_DIST_DIR=$PWD/build/lua-seam-audit/dist bash scripts/stage_lua_rock_sources.sh` | Source tarball, rendered rockspec, source rock and archive readback passed. Registered Vectis curl facade retained; embedded LoneJSON overlay excluded. `source-rock-final.log`. No release artifacts were published. |
| Ordinary Lua 5.5 `tests/lua/source_rock_path.lua` with artifact-first path and canonical upstream LoneJSON path | The earlier run used a minimal core identity fixture. Fresh ordinary Lua 5.5.1 artifact loading now first sets empty `package.preload` and `package.cpath`, proving whole-Vectis legitimate source-only loading with available pure helpers and curl/core absence. A separately labeled fixture checks canonical upstream identity, string/table exception preservation, unknown nested absence and custom searchers. Native installation is not claimed. `followup-source-path.log` (genuine no-stub loading and separate identity probes). |
| Format, lifecycle, Lua matrix/behavior, service lifecycle, OPC UA surface, shell syntax, `git diff --check` | Passed. Narrow declared Darwin workflow exception resolves the old blanket ban. The adopted workflow now includes native supervisor, OPC UA lifetime and normal Lua workflow/route gates with `-j1`; it was not dispatched. `format-final.log`, `lifecycle-final.log`, `matrix-final.log`, `behavior-final.log`, `service-final.log`, `opcua-surface-final.log`, `syntax-final.log`, `diff-final.log`. |

Public additions use a separate supervisor receiver shell and free functions;
the existing app vtable/layout is preserved. No released ABI is broken by these
additions, so the shared ABI remains 0. Installed C89/C++98 consumers verify
the public headers and both linkage modes.

Installed consumers and source Lua loading verify different product closures.
Native dependencies are not shipped inside a Vectis source rock. Live provider,
model/device, remote Lockd, claim-expiry endurance and native Darwin checks were
not run here. Those prior iteration checks do not establish the final full-repository gate or final independent review.

## Prior parent review remediation

The interrupted focused session left no result file. Its completed evidence is
`followup-behavior-final.log` (13/13 affected checks),
`followup-memcheck-{queue,opcua,upgrade}.log`, `followup-source-rock.log`, and
`followup-source-path.log`. Source-only artifact loading genuinely begins with
empty preload and C-module paths; later identity/failure fixtures are separate.
No packaging input changed in this parent remediation, so that successful
artifact evidence is retained rather than presented as a fresh package build.

The native signal fix attempts a nonblocking wake for every accepted producer,
even when its separate bounded coalesced slot is already pending. EAGAIN means
an FD token is already armed. Redundant datagrams may cause harmless empty
receives; retained signals remain coalesced, and each receive drain stays bounded.
The actual-source pthread/condition test suspends A immediately before `send`,
completes B, copies the delivered signal while A is paused, then releases and
joins A and verifies its late token produces no duplicate signal. A closed
precheck precedes frame trylock; the existing contention handshake verifies
closed status immediately after completed stop while the producer still owns
the mutex. No sleep or delay-based fault injection is used.

HTTP response sources now carry their owning route's app token into a scoped
strong app reference, shared protected calls and balanced callback depth.
Generic routes, DSV completion and upload completion use that path. Source
read/reset/close and app destruction reentry are guarded; cleanup errors are
caught, and caught teardown failures clear the closing guard. The protected
teardown preserves ordinary close idempotence. Upload open/chunk/complete/close
were already protected, and their rejection is now exercised explicitly.
Standalone DSV and HTTP client producers retain their own dependency ownership;
they are not indiscriminately mapped to an app. When invoked inside an owned
route/source callback, the outer app guard remains active throughout those calls.

| Current check | Exact evidence / limits |
| --- | --- |
| Debug build `vectis_failures_fast`, targeted unit/real Kore tests | Passed. `parent-build-final.log`, `parent-targeted-final.log`, `parent-source-all-contexts.log`. The new Kore fixture runs normal and Linux packed responses, reset/read/close rejection, uncaught read/reset failures (HTTP 500), upload/DSV source ownership and callback guards, later healthy responses, graceful route stop, stop through wait, and repeated ordinary close. |
| Full final `ctest --preset debug -j1 --output-on-failure -L failure-fast` | 35/35 passed; `parent-fast-final.log`. This is the full fast label, not the whole-suite standard gate. |
| Strict Valgrind native source-lifetime and supervisor units | Zero errors and zero lost bytes, no suppressions. Source unit and supervisor parent free all allocations; forked supervisor fixtures retain inherited reachable process state at exit. `parent-unit-memory.log`, `parent-supervisor-memory.log`. Actual CLI source is included; the instrumented destructor delegates to the real native app close exactly once. Tests cover caught teardown failure and clean retry, cleanup reentry, balanced depth and oversized-chunk stack cleanup. |
| Targeted ASan+UBSan (`detect_leaks=1:abort_on_error=1`, `halt_on_error=1`) | 3/3 passed; `parent-asan-build.log`, `parent-asan-tests.log`. Expanded upload/DSV real Kore normal/packed fixture also passed, `parent-asan-all-contexts.log`. Collection occurs inside read/reset/close and other owned callbacks. No suppressions. |
| Real Kore strict Memcheck diagnostic | **Not clean**, `parent-source-memory.log` and its per-process logs. Parent has zero errors. Fork children report Lua interior-pointer possible losses at exit and uninitialized message padding from `kore_msg_send`/worker logging; an error-exit worker respawn also reports 24 bytes of pool-name loss. These reports are not source-lifetime proof, are not suppressed, and this check is not recorded as passed. Full service-memory qualification remains with the parent final gate. |
| Darwin YAML and exact CMake target/CTest readback | Passed, `parent-darwin-readback.log`. OPC UA names were already fixed. SUS and app source lifetime targets/tests are retained alongside the selected Lua fixtures. Every selected target/test matches CMake registrations, including APPLE-only registrations. Native Darwin remains unrun; no hosted dispatch. |
| Formatting, lifecycle and facade contracts; syntax/readback | Passed, `parent-format.log`, `parent-lifecycle.log`, `parent-matrix.log`, `parent-behavior-contract.log`; Python AST, documentation readback/whitespace and `git diff --check` passed. The assertion-enabled unit rebuild and rerun passed, `parent-unit-assertions-build.log`, `parent-unit-assertions-test.log`. |

All pre-existing coherent work is retained. Four SQLite/Postgres/libHaru/libpng
facades remain separate and SSH stays parked. Dependency pins, public ABI and
streaming/materialization contracts are unchanged. That prior iteration left the final independent review, whole-suite gate and
installed closures to the parent. The subsequent remediation below runs the
verification gates; final independent review and verification are recorded below.
This evidence does not approve a release.

## Final independent-review remediation and fresh verification

The independent review recorded at 04:09 UTC found four actionable issues.
All four were implemented without reverting the coherent earlier task edits:

1. Returned HTTP response/source configuration is resolved and pinned inside a
   scoped app-owned protected conversion. Generic route and before/after, DSV,
   upload and native callback-auth conversion share the owning guard; partial
   source state/references are released on error. Upload cleanup remains reachable.
   Numeric strings are rooted, fallback getters preserve selection order, and
   header key normalization leaves Lua iteration keys intact. Configuration
   snapshots never consume or materialize callback payloads.
2. OPC UA setters validate liveness before retention, root their receiver through
   protected getter evaluation, reject close/borrow during that evaluation,
   revalidate ownership afterward and release partial logger retention on every
   failure. Native sinks remain foreign-thread-safe; no arbitrary Lua log output
   is accepted at the native logging boundary.
3. The supervisor route test's fast label is assigned only when Kore registers it.
4. The fast target now builds `vectis_audio_lua_validation` as part of its closure.

The whole standard gate additionally exposed a stale example mock of `dequeue`.
It was migrated to the existing batch-one/client-terminal contract, asserting
page size, correct raw-message ack and exactly-once close. This was fixture repair;
no new queue facade or alternate runtime path was introduced.

Fresh final gates on the uncommitted `init` candidate based on `b1fe538`:

| Gate | Final result and current evidence under `build/lua-seam-audit/` |
| --- | --- |
| Required serial `CMAKE_BUILD_PARALLEL_LEVEL=1 CTEST_PARALLEL_LEVEL=1 TMPDIR=$PWD/build/lua-seam-audit/tmp make -j1 finalize-slice CTEST='ctest -j1'` | Exit 0; formatting and all lifecycle/toolchain/target/facade/privacy contracts passed. **Fast 35/35, 17.04s; standard 176/176, 245.35s; 0 failures, 0 CTest skips.** `remediation-finalize-slice-final.log`. |
| Clean fast target closure | New `clean-fast` directory configured with the debug preset and identical pinned dependencies; actual audio executable compile/link appears in `remediation-clean-fast-build.log`. Initial clean label 35/35 in 15.60s; the final scalar/test refinement passed its native check and the full final fast gate above. No earlier audio binary supplied the clean closure. |
| Supported Kore-disabled configure/build/tests | Fresh `no-kore-final` with runtime OFF and tests ON; build passed, four relevant tests 4/4 in 0.61s. `remediation-no-kore-{configure,build,tests}.log`. |
| Observable HTTP conversion and ownership | Normal and Linux packed fixture runs in the fast gate; 5 contexts × 6 getter phases × 2 modes plus 2 auth cases = **62 conversion cases per execution**, every case followed by healthy service. Callback collection and all **12 upload cleanups** asserted per execution. Native checks also cover partial construction and pinned/normalized values. |
| Final installed SDK core | `cmake --build build/supervisor-shared -j1` rebuilt changed `vectis.c` and `vectis_supervisor.c`; install prefix is under that build. Static and shared each passed 3 C89/C++98/pkg-config consumers plus **36 example runtime closures**, including missing-runtime detection. Canonical export policy returned **0**. `remediation-shared-*-final.log`, `remediation-sdk-{static,shared}-final.log`, `remediation-export-policy-final.log`, `remediation-installed-core-digests.log`. Installed shared bytes differ because installation removes the build runtime path, as the install log records. |
| Source rock and ordinary Lua | Staged version **0.0.0** entirely under build; archive/rockspec/source-rock validation passed. Actual Lua **5.5.1** ran `source_rock_path.lua` against the source rock's extracted nested archive with empty initial preloads/C path. Genuine source-only behavior precedes explicitly separate stub identity/error probes; no native-installation claim. `remediation-source-rock-final.log`, `remediation-source-rock-path-final.log`. |
| Targeted ASan/UBSan final paths | **6/6, 1.90s**: five native lifetime units and normal/packed real service fixture; leak detection, abort/halt on error, no suppressions. `remediation-asan-{build,tests}-final.log`. |
| Strict native Memcheck | **5/5 exit 0**, zero errors and definite/indirect/possible lost bytes, no suppressions. Supervisor fork children retain reachable inherited state. `remediation-native-memory/`. |
| Strict real-service Memcheck | **Not clean: driver exit 1** after HTTP assertions completed. Parent 0 errors/all heap freed; completed worker 0 definite/indirect loss, **181,317 possible bytes / 282 blocks**, **123 errors / 122 contexts**, two other fork logs without terminal summaries. Origins identify Kore message padding. Possible-loss graphs include Lua and Lua-owned LoneJSON schema/runtime resources; they are not all cpkt allocator records. `remediation-service-memory-final.log`, `remediation-service-memory/normal/memory.*.log`. |
| Baseline memory mechanisms | Byte-identical pinned/current Kore send/pool-init bodies; fresh standalone minimized padding exit 99 (1 error), respawn names exit 99 (24 definite bytes / 2 blocks in child; parent clean). `remediation-kore-minimal-*.log`; self-contained `stash/kore-strict-memcheck-baseline.md`. No global shutdown change or suppression. |

The first broad standard run was 175/176 in 244.59s with only the stale example
mock failing. It is preserved in `remediation-finalize-slice.log`; the repaired
example passed before the final whole-gate rerun above. Earlier failed scalar-test
assertions were corrected to the actual strict file-path / numeric-string API;
final native, fast, sanitizer and memory checks cover those assertions.

Three internal standard-suite omissions remain explicit despite successful CTest
exit: optional Paramiko SSH output integration unavailable, live SUS/audio model
validation disabled, and loaded-model example skipped. Provider/model/device and
remote endurance gates are not implied. Native Darwin remains unrun; optimized
musl verification was optional and not performed. Four SQLite/Postgres/PDF/PNG
Lua facades remain separate and SSH investigation stays parked.

A fresh narrow read-only **self-review**, not an independent review, traces final
conversion/partial cleanup, logger ownership/collection, test registration and
clean build closure against the observed results. No additional actionable local
finding remained from that self-review. The subsequent independent review found
the allocation-error boundary fixed below; its re-review and final whole gate
are recorded in the completed verification section. No commit, push, publish,
goal, notification, deployment or out-of-repository change was performed by that
remediation worker. Required upstream contracts and future
local phases below remain distinct; a successful ordinary gate is not a release
approval and does not make the strict service-memory gate clean.

## Final independent P2: protected source processing under Lua OOM

The remaining finding in `build/lua-seam-audit/clean-review-result.md` is fixed
in the working candidate on `init` at base `b1fe538`. Only the CLI Lua source
adapter changes native behavior; core SDK code, public ABI and dependency pins
are unchanged. Earlier remediation and the parent's isolated fixture cwd,
resolved absolute arguments and removed root PID file are preserved.

Read/reset now run callback invocation, EOF/type normalization, numeric string
conversion, pending-reference creation/release and reset state changes inside
one protected operation with the validated owning app rooted and guarded.
Errors return through `lc_error`; outside protection error inspection reads only
actual strings or a fixed fallback, never coercing numeric/table errors. Native
source ownership remains explicit in partial construction until close or response
transfer. Close callbacks remain protected; reference release touches existing
registry/free-list slots and does not allocate or invoke Lua. Stack and lifecycle
guards balance on failure. Generic routes and DSV/upload completion use this same
adapter; producer chunks remain incremental without new payload materialization.

The actual-CLI source-lifetime unit uses a custom Lua allocator, armed only in the
read callback immediately before returning its numeric or oversized string chunk.
Both Lua allocation attempts (including the retry after emergency GC) fail in
each case. Each proves one close while allocation is still limited, unchanged
FD count, removal of the observed spool path, collected weak app/callback roots,
zero stack/guard depth, and healthy conversion after a controlled disarm. The
unit also preserves normal numeric chunks, nil/false EOF, oversized pending
chunks, pending reset, malformed results, getter failures and close reentry.
Normal/packed service cases separately prove numeric/table read/reset errors
through routes, DSV and upload completion, twelve exact-once source closes, four
upload cleanups and healthy requests after each failure. Service fixtures do not
inject Lua allocator OOM; allocator evidence comes from the native unit.

Current focused verification, all serialized:

- `cmake --build --preset debug --target vectis_failures_fast -j1`: passed;
  existing C89 core policy retained (CLI/actual-CLI unit remain project C99).
  `build/lua-seam-audit/source-oom-build.log`.
- Seven debug CTests: actual-source unit, normal/packed lifetime fixture, DSV,
  HTTP, app restart and failure masks 0/15: **7/7 passed**.
  `build/lua-seam-audit/source-oom-final-debug.log`.
- Strict native Valgrind actual-source unit: **zero errors; zero bytes/blocks at
  exit**, no suppressions, definite/indirect/possible leaks are errors, origin
  tracking and error exit 99 enabled.
  `build/lua-seam-audit/source-oom-memcheck.log` and
  `build/lua-seam-audit/source-oom-memcheck-output.log`.
- ASan/UBSan fast target rebuilt with `-j1`; source unit, normal/packed fixture,
  OPC UA cleanup, audio validation, SUS lifetime and supervisor cohort:
  **6/6 passed**, `detect_leaks=1:abort_on_error=1`, `halt_on_error=1`.
  `build/lua-seam-audit/source-oom-asan-build.log` and
  `build/lua-seam-audit/source-oom-asan-tests.log`.
- Focused formatting, Python syntax, documentation readback and diff whitespace:
  `build/lua-seam-audit/source-oom-readback.log` and
  `build/lua-seam-audit/source-oom-diffcheck.log`.

These focused checks preceded the fresh whole gate below. The unchanged
full-service Kore Memcheck baseline was not repeated. Native Darwin (including
the unit's Darwin FD-path branch), optimized musl and live-provider/device/endurance
gates remain unrun. The final independent review traces protected native returns,
explicit ownership and cleanup and reports no actionable findings. Existing
upstream limitations remain as recorded below.

## Completed independent review and final verification

The first independent whole-candidate review found four issues, all fixed in
the remediation above. A second independent review identified the remaining
source allocation-error boundary; the final read-only re-review of that fix is
clean, with **no actionable findings**. Its scope and limitations are preserved
in `build/lua-seam-audit/source-oom-final-review-result.md`. The reviewer traced
the source and recorded evidence; it did not run additional tests.

After that last executable change, the parent ran the standard gate serially:

```sh
CMAKE_BUILD_PARALLEL_LEVEL=1 CTEST_PARALLEL_LEVEL=1 \
TMPDIR="$PWD/build/lua-seam-audit/tmp" \
make -j1 finalize-slice CTEST='ctest -j1'
```

**Exit 0: 35/35 failure-fast checks in 16.05s and 176/176 broader checks in
248.39s**, zero failures and zero CTest skips. Formatting, lifecycle, toolchain,
target, facade and privacy contracts passed. Exact output is retained in
`build/lua-seam-audit/parent-finalize-slice.log`. Internal opt-in omissions listed
above still apply; successful CTest exits do not establish live coverage.

The final correction changes CLI source handling, not core SDK code, public
headers, Lua modules or package inputs. Therefore the earlier installed
static/shared consumers (three consumers plus 36 example closures per mode),
export policy and genuine ordinary-Lua source-rock checks remain applicable to
those unchanged artifacts. The changed CLI has fresh whole-gate, focused
ASan/UBSan and strict native Valgrind evidence above. No claim of a repeated
full-service Memcheck or optimized musl build is made.

The real source fixture now resolves executable/script arguments and runs Kore
in its build fixture directory. Normal/packed and ASan/UBSan fixture checks
passed after this containment change; no generated root `kore.pid` is retained.
The final worktree inventory contains **84 coherent task-owned source, test,
documentation and policy files** (54 modified, 30 new before staging).
Ignored `build/` evidence and self-contained `stash/` reports are excluded from
the commit. Documentation readback and local links are checked before commitment.

This closes the local implementation review and verification loop. It does
**not** approve a release: required upstream contracts remain unresolved,
full-service Memcheck remains nonclean, and native Darwin execution is unrun.
The four new database/PDF/PNG facades remain a separate task as instructed.

## Required upstream contracts and reports

`stash/` contains single self-contained reports with versions, embedded source,
expected/current behavior and no attachments:

- `liblockdc-lua-empty-dequeue.md`: native `LC_OK,NULL` vs Lua truthy closed handle,
  reproduced for both single variants; public empty-batch workaround.
- `liblockdc-lua-batch-message-terminal-leak.md`: measured 400-byte native message
  loss plus retained client after batch ack; public client terminal+close workaround.
- `liblockdc-pouch-stateful-renew-completion.md`: renewal and checkpoint write
  succeed, then deferred completion fails; nonrenewed controls succeed.
- `liblockdc-supervisor-gaps.md`: required upstream contracts for true pull reader,
  native automatic renewal and kind-filtered intake; optional reusable enhancements
  for candidate-ready FD, standalone logger/endpoint integration and future
  Vectis canonical shared leadership. Public leases/transactions can implement
  the latter locally.
- `kore-strict-memcheck-baseline.md`: confirmed unchanged native message padding
  and worker-respawn pool-name ownership mechanisms; embedded minimized C
  reproduction, actual service traces and qualified Lua/fork-exit diagnostics.
- `cpkt-sus-per-model-logging.md`: global setter cannot preserve distinct per-app
  backend ownership; explicit global owner queue delivered without implicit routing.
  Source tracing confirms an additional upstream continuation-severity race;
  static local hook installation does not fix that pre-callback field.

Push streaming is delivered; a pull `payload_source` is not. Manual stateless
renewal is delivered; blocking-handler automatic renewal is not. Stopping one
kind pauses its workflow; different workflow identities remain independent.
Shared runtimes are rejected as a future Vectis-owned mode. Public client
leases and transactions can implement canonical identity and leadership; no
upstream impossibility is claimed. Native recovery discovery defaults to 30
seconds; zero is explicit opt-out and a readiness probe is not discovery. Long
stateful handlers remain blocked by the reproduced renewal/completion defect.

Delivered current serial behavior, deliberate future local phases (isolated
lanes, quotas, restart policies, shared leadership and managed request/reply),
and unmet current public contracts (pull source, blocked-handler automatic
renewal, kind selection and renewed stateful completion) are distinct. The
full authoritative specification remains intact; future work is not silently
accepted as implemented configuration. The unmet current contracts prevent
claiming the full product promise or passing its release gate. Required upstream
fixes or an explicit authority decision must resolve those contracts. Future
local phases do not acquire an invented upstream prerequisite.
