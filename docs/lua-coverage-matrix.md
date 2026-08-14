# Lua Coverage Matrix

The `vectis` executable is a primary product surface. This matrix tracks the
Lua exposure contract for bundled dependencies and Vectis C SDK workflows. The
developer-facing entry point is [`docs/lua.md`](lua.md).

Coverage states:

- `yes`: implemented and exposed to Lua.
- `partial`: useful Lua coverage exists, but the row still has explicit gaps.
- `planned`: no complete Lua surface yet.
- `n/a`: intentionally not a direct Lua API; explain the reason in notes.

Columns:

- `Native`: thin dependency/module access, preferably preserving upstream naming.
- `DX`: Vectis-owned helper layer for common service workflows.
- `Local`: deterministic local test coverage.
- `Packed`: packed-binary scenario coverage.
- `Live`: opt-in live-provider or hardware/service coverage.

## Dependency Facades

| ID | Surface | Native | DX | Local | Packed | Live | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| dep:lua-runtime | cpkt Lua 5.5 runtime facade | yes | yes | yes | yes | n/a | Runtime is embedded; not a user module. |
| dep:lockdc | liblockdc / lockdc Lua module | yes | partial | yes | yes | n/a | `require("lockdc")` and `vectis.lockd`; JSON passthroughs, embedded bundle config DX, one-shot state save/load, and queue enqueue helpers are documented in `docs/lua-lockd.md`; additional helpers remain only where direct lockdc usage is too noisy. |
| dep:lonejson | LoneJSON Lua module | yes | partial | yes | yes | n/a | Native encode/decode/schema available; Vectis JSON route/downstream DX remains incomplete. |
| dep:pslog | libpslog Lua module | yes | partial | yes | yes | n/a | The dependency-native logging module is preloaded, documented in `docs/lua-pslog.md`, and packed-tested; `vectis.log` adds JSON logger defaults, default fields, and structured Vectis error logging while keeping direct pslog access available. |
| dep:lql | liblql Lua module | yes | n/a | yes | yes | n/a | Dependency-native module is preloaded, documented in `docs/lua-lql.md`, smoke-tested, and packed-tested through `examples/lua/local_data_pipeline.lua`; no Vectis-owned helper is planned until a repeated service workflow crosses Vectis concepts. |
| dep:cai | CAI Lua module | yes | planned | yes | yes | planned | Dependency-native CAI 0.3.0 module is preloaded, documented in `docs/lua-cai.md`, and packed-tested for local constructors, model metadata, tool schemas, response params, registries, and MCP handler construction; Vectis service integration remains future glue around CAI rather than a second CAI wrapper. |
| dep:libmdf | libmdf Lua module | yes | partial | yes | yes | n/a | Dependency-native module is preloaded, documented in `docs/lua-libmdf.md`, and packed-tested; `vectis.terminal` adds Markdown render DX while keeping direct libmdf access available. |
| dep:softline | softline Lua module | yes | partial | yes | yes | n/a | Dependency-native module is preloaded, documented in `docs/lua-softline.md`, and packed-tested; `vectis.terminal` adds bounded editor construction while keeping direct softline access available. |
| dep:curl | libcurl Lua facade | yes | partial | yes | yes | planned | `curl`, `vectis.http`, `vectis.webdav`, `vectis.mqtt`, and `vectis.smtp`; generic protocols are available through option tables. |
| dep:openssl | OpenSSL | yes | partial | yes | yes | n/a | `openssl` exposes version, SHA-256, general EVP digest/HMAC, hex/Base64 codecs, PEM-backed signing/verification, and CSPRNG helpers; `vectis.cert` owns certificate workflows. |
| dep:libssh2 | libssh2 | planned | partial | yes | partial | yes | `vectis.ssh.open` exposes the public C SSH receiver to Lua; `vectis.ssh.exec`, libssh2-backed SFTP/SCP file upload/download, one-shot SFTP filesystem operations, and stateful SFTP session/file/directory receivers exist; named `SFTP_OPEN_*` flags are exposed; packed SSH command and SCP validation coverage exists; opt-in SSH/SFTP e2e covers live handles; lower-level dependency-native channels and advanced host-key workflows remain missing. |
| dep:libxml2 | libxml2/XML | partial | partial | yes | yes | n/a | `vectis.xml` exposes XML-to-LoneJSON parsing with packed example coverage; dependency-native DOM/libxml2 and XML serialization remain missing. |
| dep:dsv | Vectis DSV/CSV/TSV helpers | yes | yes | yes | yes | n/a | `vectis.dsv` exposes materialized parse, typed source-to-callback iteration for dynamic and fixed-capacity string fields, spill, row serialization, packed execution coverage, and streaming `server:dsv` upload row routes. |
| dep:opcua | cpkt-opcua | partial | planned | yes | yes | planned | `require("opcua")` covers client/foundation plus server lifecycle, value, node-model, reference, deletion, and common attribute APIs with normal and packed local e2e against deterministic C and Lua-owned test servers; methods, browse/path traversal callbacks, array attributes, events, security config, PubSub, async, subscriptions, and callback-heavy workflows remain. |
| dep:sus | cpkt SUS / whisper | partial | planned | yes | yes | planned | Dependency-native metadata, catalog lookup, path/cache open error handling, model handles, offline cache status callbacks, process-wide log sink configuration, model-created transcriber handles, PCM transcription methods, segmented decoder/VOX methods, and transcriber callback registration are exposed, documented in `docs/lua-sus.md`, and packed-tested; loaded-model behavioral coverage remains opt-in/future. |
| dep:audio | cpkt audio / miniaudio | partial | planned | yes | yes | planned | Dependency-native constants, result strings, capability checks, decoder file/URL/callback open, encoder file/callback open, callback reader/writer, VOX, PTT, and capture/playback receiver shells are exposed, documented in `docs/lua-audio.md`, and packed-tested; live devices remain opt-in. |
| dep:nghttp2 | nghttp2 | n/a | n/a | yes | yes | n/a | Transport dependency behind curl/Kore; no direct app-facing Lua API expected unless HTTP/2 controls become product surface. |
| dep:zlib | zlib | yes | n/a | yes | yes | n/a | `require("zlib")` exposes buffered string deflate/inflate, gzip/gunzip, auto-decompress, file-backed bounded transforms, version, explicit output limits, smoke coverage, and packed local data pipeline example coverage; no Vectis-owned compression helper is planned until a repeated workflow crosses Vectis HTTP, pack, or asset concepts. |
| dep:miniaudio | miniaudio | partial | planned | yes | yes | planned | Covered with `dep:audio`; capture/playback device helpers are exposed with opt-in live-device coverage. |

## Vectis Workflow Facades

| ID | Surface | Native | DX | Local | Packed | Live | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| workflow:server-runtime | Kore-backed Lua server lifecycle | n/a | partial | yes | yes | n/a | `vectis.server.new/start/stop/close`, fixed routes, buffered Lua callback routes, route groups, and buffered before/after hooks. |
| workflow:routes-json | JSON/API route helpers | n/a | partial | yes | yes | n/a | Fixed `server:json`, `server:auth_json`, `server:text`, `server:redirect`, and buffered `server:route`/`server:group` callbacks with request metadata/body, response tables, file responses, file-backed `spooled_source` responses, hooks, and native/callback auth guards. |
| workflow:rest | Buffered JSON REST route and client DX | n/a | yes | yes | yes | n/a | `vectis.rest` wraps buffered server routes/groups with JSON decode/validation/error responses and wraps `vectis.http` with a base-URL JSON client. |
| workflow:routes-streaming | Request/response streaming route helpers | n/a | yes | yes | yes | n/a | `server:upload` provides true request-body streaming through a bounded upload reader and Lua chunk callbacks; `server:route` supports live callback-backed `stream_source` responses and explicit file-backed `spooled_source` responses; `server:sse` provides live SSE framing; the packable Lua API example exercises stream, SSE, and upload routes. |
| workflow:status-errors | Status/error metadata helpers | n/a | yes | yes | yes | n/a | `vectis.status` mirrors public Vectis status and error-source constants for pure-Lua helpers; curl-backed HTTP/WebDAV/MQTT/SMTP errors, dependency-native cpkt-backed audio/SUS/OPC UA errors, and packed embedded helper errors carry status/source metadata plus dependency diagnostics where applicable. |
| workflow:libs | Bundled dependency namespace | yes | yes | yes | yes | n/a | `vectis.libs` aliases `lockdc`, `lonejson`, `pslog`, `lql`, `cai`, `libmdf`, `softline`, `curl`, `opcua`, `openssl`, `zlib`, `audio`, and `sus` to their direct `require(...)` module tables; it does not wrap, replace, or hide the individual dependency modules. |
| workflow:logging | Lua service logging | yes | partial | yes | yes | n/a | `vectis.log` wraps `pslog` logger creation and structured Vectis error metadata; deeper app/server logger inheritance remains future work. |
| workflow:static-assets | Static files and embedded assets | n/a | yes | yes | yes | n/a | Disk, embedded read-only, extract, and packed scenarios exist; `vectis.embedded` is a direct preloaded module and top-level `vectis.embedded` alias; packed embedded helper failures return structured Vectis errors. |
| workflow:webdav-server | WebDAV server mounts | n/a | partial | yes | yes | n/a | `server:webdav` covers Vectis-managed mutable storage and direct mutable disk `root_dir` mounts with native/callback auth; `server:webdav_embedded` covers read-only packed asset mounts; packed embedded-site mutable flow exists. |
| workflow:webdav-client | WebDAV client helpers | yes | yes | yes | yes | n/a | `vectis.webdav` wraps curl for PROPFIND, MKCOL, GET, PUT, COPY, MOVE, DELETE, auth/depth/destination headers, structured Vectis/curl errors, and file-backed transfer. |
| workflow:auth | Native auth, OAuth2/OIDC, WebDAV keys | n/a | yes | yes | yes | yes | Core auth functions, Basic Authorization formatting, native/callback providers, auth routes, email tokens, OAuth2/OIDC flows, WebDAV-key issuance, and `vectis.auth.browser_flow` route/provider/WebDAV-key DX are exposed, documented, and covered by Lua provider/email-token/OAuth/browser-flow contract tests. |
| workflow:certs | Certificate/key/CSR/CA workflows | n/a | partial | yes | yes | n/a | Private-key, CSR, bundle generation, bundle inspection, bundle validation, cert/key pair validation, CA bundles, CA-signed cert/key pairs, and Lua server TLS path/PEM/client-CA config exist; reload/update hooks and broader inspection remain. |
| workflow:http-client | Downstream HTTP client | yes | yes | yes | yes | planned | JSON helpers including OPTIONS, simple non-JSON verbs including HEAD and OPTIONS, form bodies, multipart, file upload/download presets, retry/proxy/TLS pass-through, reusable client defaults, and structured Vectis/curl errors exist. |
| workflow:sftp-curl | curl-backed SFTP | yes | yes | yes | yes | n/a | Covered through `vectis.http.sftp_download/upload`. |
| workflow:ssh-exec | SSH command execution | planned | yes | yes | yes | n/a | `vectis.ssh.exec` and reusable `vectis.ssh.open(...):exec(...)` receiver calls exist; packed e2e covers command execution and known_hosts rejection; lower-level dependency-native libssh2 channels remain missing. |
| workflow:scp | SCP upload/download | planned | yes | yes | yes | n/a | `vectis.ssh.scp_upload_file` and `scp_download_file` expose libssh2-backed file transfer, with packed validation coverage; dependency-native libssh2 channel/session control remains absent. |
| workflow:sftp-libssh2 | libssh2-backed SFTP | planned | yes | yes | yes | yes | `vectis.ssh.sftp_upload_file`, `sftp_download_file`, `sftp_stat`, `sftp_mkdir`, `sftp_remove`, `sftp_rmdir`, `sftp_rename`, `sftp_chmod`, `sftp_open`, named `SFTP_OPEN_*` flags, and `vectis.ssh.open(...):sftp_*` expose one-shot plus reusable SSH and stateful SFTP open/read/write/stat/directory iteration handles, including opt-in SSH/SFTP e2e coverage. |
| workflow:mqtt | MQTT publish workflows | yes | yes | yes | yes | planned | `vectis.mqtt.publish` wraps curl upload mode for MQTT publish with structured Vectis/curl errors and packed validation coverage; live broker coverage remains opt-in. |
| workflow:smtp | SMTP send workflows | yes | yes | yes | yes | planned | `vectis.smtp.send` wraps curl SMTP upload with memory and file-backed payloads plus structured Vectis/curl errors; packed validation coverage exists and live SMTP coverage remains opt-in. |
| workflow:xml | XML parse/serialize workflows | partial | partial | yes | yes | n/a | `vectis.xml.parse()` materializes a table and `parse_record()` returns a Lua-owned LoneJSON record; XML parsing has packed example coverage and XML serialization remains missing. |
| workflow:dsv | CSV/TSV/DSV parse/serialize workflows | yes | yes | yes | yes | n/a | Lua covers memory/path parse, typed row callbacks for dynamic and fixed-capacity string fields, custom delimiters, comments, spill, serialization, packed embedded asset parsing, and streaming upload route-row handling through `server:dsv`. |
| workflow:lockd-state | lockd document/state workflows | yes | partial | yes | yes | n/a | Direct lockdc access plus `vectis.lockd` config/JSON helpers, `load_json`, `save_json`, and `with_acquired_lease`; public examples use the helper layer; richer document/query helper coverage remains only where direct lockdc usage is too noisy. |
| workflow:lockd-queue | lockd queue and consumer workflows | yes | partial | yes | yes | n/a | Direct lockdc access plus `vectis.lockd.enqueue_json` and `with_dequeued_json`; public examples use the helper layer with explicit ack; retry-oriented helper DX remains only where direct lockdc usage is too noisy. |
| workflow:server-consumer | Same-process server plus lockd consumer | n/a | yes | yes | yes | n/a | Scenario coverage exists. |
| workflow:opcua-client | OPC UA client workflows | partial | planned | yes | yes | planned | Local server-backed normal and packed e2e exists for sync read/write and connection lifecycle; async, browse, method-call, subscription, and remote node-management workflows remain. |
| workflow:opcua-server | OPC UA server workflows | partial | planned | yes | yes | planned | Server lifecycle/value/node-model/reference/common-attribute APIs are covered by direct and packed local e2e; methods, browse/path traversal callbacks, array attributes, security config, events, and PubSub remain. |
| workflow:opcua-async | OPC UA async/subscription/PubSub workflows | planned | planned | no | no | planned | Callback and event-loop ownership contract is documented in `docs/lua-opcua.md`; implementation remains. |
| workflow:cai | AI/client/tool/MCP workflows | yes | planned | yes | yes | planned | Dependency-native CAI local workflows are packed-tested through `examples/lua/cai_local.lua`; CAI remains the primary SDK surface. Vectis service adapters for request sources, HTTP/lockd/file sinks, error mapping, and logger inheritance need an explicit ownership and streaming contract. |
| workflow:sus | Whisper/transcription workflows | partial | planned | yes | yes | planned | Catalog, cache-control paths, model-created transcribers, PCM transcription methods, audio decoder/VOX segmented methods, and callback registration are exposed; live/local loaded-model tests remain. |
| workflow:audio | Audio capture/playback/processing workflows | partial | planned | yes | yes | planned | File/callback decode/encode, VOX/PTT, and capture/playback receiver shells are exposed; live device behavior remains opt-in. |
| workflow:terminal-agent | libmdf/softline terminal and Agent Smith workflows | yes | partial | yes | yes | n/a | `vectis.terminal` covers Markdown render and editor construction DX; Agent Smith-specific workflows remain future work. |
| workflow:pack | Pack/self-contained binary workflows | n/a | yes | yes | yes | n/a | `vectis -a pack` and packed Lua e2e exist. |
| workflow:totp-qr | TOTP and QR helpers | n/a | yes | yes | yes | n/a | Exposed under `vectis.auth.totp` and `vectis.auth.qr`. |
| workflow:openapi | OpenAPI generation | n/a | yes | yes | yes | n/a | `server:openapi_doc()` attaches route metadata, route helpers accept `openapi` metadata tables, and `server:openapi()` generates JSON/YAML using LoneJSON schemas retained by the server; the packable Lua API example serves `/openapi.json`. |

## Completion Rules

- New bundled dependencies that are useful from app code must add a `dep:*`
  row in this file and an entry in `docs/lua.md` before release.
- New Vectis C SDK workflows must add a `workflow:*` row before release.
- New Lua module documentation must be linked from `docs/lua.md`.
- A `planned` or `partial` row is acceptable only when the Notes cell states
  the concrete missing surface or ownership decision.
- Local deterministic tests are required when a row moves from `planned` to
  `partial` or `yes`, unless the Notes cell documents why the surface is
  live-provider-only.
- Packed tests are required for workflow helpers intended to work in
  self-contained `vectis -a pack` applications.
