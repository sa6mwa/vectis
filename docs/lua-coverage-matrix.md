# Lua Coverage Matrix

The `vectis` executable is a primary product surface. This matrix tracks the
Lua exposure contract for bundled dependencies and Vectis C SDK workflows.

Coverage states:

- `yes`: implemented and exposed to Lua.
- `partial`: useful Lua coverage exists, but the row still has explicit gaps.
- `planned`: no complete Lua surface yet.
- `n/a`: intentionally not a direct Lua API; explain the reason in notes.

Columns:

- `Raw`: thin dependency/module access, preferably preserving upstream naming.
- `DX`: Vectis-owned helper layer for common service workflows.
- `Local`: deterministic local test coverage.
- `Packed`: packed-binary scenario coverage.
- `Live`: opt-in live-provider or hardware/service coverage.

## Dependency Facades

| ID | Surface | Raw | DX | Local | Packed | Live | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| dep:lua-runtime | cpkt Lua 5.5 runtime facade | yes | yes | yes | yes | n/a | Runtime is embedded; not a user module. |
| dep:lockdc | liblockdc / lockdc Lua module | yes | partial | yes | yes | n/a | `require("lockdc")` and `vectis.lockd`; document/queue helpers still need fuller DX. |
| dep:lonejson | LoneJSON Lua module | yes | partial | yes | yes | n/a | Raw encode/decode/schema available; Vectis JSON route/downstream DX remains incomplete. |
| dep:pslog | libpslog Lua module | yes | planned | yes | no | n/a | Raw logging module is preloaded; Vectis logger inheritance in Lua needs a clear helper surface. |
| dep:lql | liblql Lua module | yes | planned | yes | no | n/a | Raw module is preloaded. |
| dep:cai | CAI Lua module | yes | planned | yes | no | planned | Raw module is preloaded; Vectis service integration waits on CAI SDK stabilization. |
| dep:libmdf | libmdf Lua module | yes | planned | yes | no | n/a | Raw module is preloaded; Agent Smith / terminal DX helpers are future work. |
| dep:softline | softline Lua module | yes | planned | yes | no | n/a | Raw module is preloaded; higher-level prompt/terminal helpers are future work. |
| dep:curl | libcurl Lua facade | yes | partial | yes | yes | planned | `curl` plus `vectis.http`; generic protocols are available through option tables. |
| dep:openssl | OpenSSL | planned | partial | yes | yes | n/a | `vectis.cert` exists; raw-but-narrow OpenSSL Lua facade is missing. |
| dep:libssh2 | libssh2 | planned | partial | yes | no | n/a | `vectis.ssh.exec` exists; raw sessions, channels, SCP, and deeper SFTP are missing. |
| dep:libxml2 | libxml2/XML | planned | planned | no | no | n/a | C SDK XML helpers exist; Lua XML facade is missing. |
| dep:dsv | Vectis DSV/CSV/TSV helpers | planned | planned | no | no | n/a | C SDK DSV helpers exist; Lua DSV facade is missing. |
| dep:opcua | cpkt-opcua | partial | planned | yes | no | planned | `require("opcua")` covers client/foundation; server, async, subscriptions, PubSub remain. |
| dep:sus | cpkt SUS / whisper | planned | planned | no | no | planned | No Lua facade yet; needs streaming source/sink contract. |
| dep:audio | cpkt audio / miniaudio | planned | planned | no | no | planned | No Lua facade yet; needs audio buffer/stream ownership contract. |
| dep:nghttp2 | nghttp2 | n/a | n/a | yes | yes | n/a | Transport dependency behind curl/Kore; no direct app-facing Lua API expected unless HTTP/2 controls become product surface. |
| dep:zlib | zlib | n/a | planned | yes | yes | n/a | Compression is transitive today; add Lua helpers only when archive/body compression workflows need them. |
| dep:miniaudio | miniaudio | planned | planned | no | no | planned | Covered with `dep:audio`; separate row kept because it is independently provisioned. |

## Vectis Workflow Facades

| ID | Surface | Raw | DX | Local | Packed | Live | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| workflow:server-runtime | Kore-backed Lua server lifecycle | n/a | partial | yes | yes | n/a | `vectis.server.new/start/stop/close`; handler routes and middleware-like DX remain. |
| workflow:routes-json | JSON/API route helpers | n/a | partial | yes | yes | n/a | Fixed `server:json` and `server:auth_json`; dynamic handler DX is missing. |
| workflow:routes-streaming | Request/response streaming route helpers | n/a | planned | no | no | n/a | Must preserve real streaming, spooling, and file-backed naming. |
| workflow:static-assets | Static files and embedded assets | n/a | yes | yes | yes | n/a | Disk, embedded read-only, extract, and packed scenarios exist. |
| workflow:webdav-server | WebDAV server mounts | n/a | partial | yes | yes | n/a | Packed embedded-site flow exists; ordinary mutable/read-only Lua app mounts need fuller DX. |
| workflow:webdav-client | WebDAV client helpers | planned | planned | no | no | n/a | Use curl manually today; explicit PROPFIND/MKCOL/COPY/MOVE helpers are missing. |
| workflow:auth | Native auth, OAuth2/OIDC, WebDAV keys | n/a | partial | yes | yes | planned | Core functions/routes exist; high-level Lua admin/login flow needs polish. |
| workflow:certs | Certificate/key/CSR/CA workflows | n/a | partial | yes | yes | n/a | Bundle validation/generation exists; CSR/private key/full CA workflow coverage should be completed. |
| workflow:http-client | Downstream HTTP client | yes | partial | yes | yes | planned | JSON helpers exist; simple non-JSON verbs, form/multipart, richer file helpers remain. |
| workflow:sftp-curl | curl-backed SFTP | yes | yes | yes | yes | n/a | Covered through `vectis.http.sftp_download/upload`. |
| workflow:ssh-exec | SSH command execution | planned | yes | yes | no | n/a | `vectis.ssh.exec` exists; raw libssh2 remains missing. |
| workflow:scp | SCP upload/download | planned | planned | no | no | n/a | No Lua facade yet. |
| workflow:sftp-libssh2 | libssh2-backed SFTP | planned | planned | no | no | n/a | C SDK has deeper SFTP; Lua facade is missing. |
| workflow:xml | XML parse/serialize workflows | planned | planned | no | no | n/a | C SDK exists; Lua coverage missing. |
| workflow:dsv | CSV/TSV/DSV parse/serialize workflows | planned | planned | no | no | n/a | C SDK exists; Lua coverage missing. |
| workflow:lockd-state | lockd document/state workflows | yes | partial | yes | yes | n/a | Raw lockdc plus first `vectis.lockd`; richer document/query helper coverage remains. |
| workflow:lockd-queue | lockd queue and consumer workflows | yes | partial | yes | yes | n/a | Queue examples exist; retry-oriented helper DX remains. |
| workflow:server-consumer | Same-process server plus lockd consumer | n/a | yes | yes | yes | n/a | Scenario coverage exists. |
| workflow:opcua-client | OPC UA client workflows | yes | planned | yes | no | planned | Local server-backed e2e exists; Vectis workflow helper is not designed yet. |
| workflow:opcua-server | OPC UA server workflows | planned | planned | no | no | planned | Needs Lua callback lifetime and method/access-control contract. |
| workflow:opcua-async | OPC UA async/subscription/PubSub workflows | planned | planned | no | no | planned | Needs callback and event-loop ownership contract. |
| workflow:cai | AI/client/tool/MCP workflows | yes | planned | yes | no | planned | Raw CAI preloaded; Vectis service adapters are future work. |
| workflow:sus | Whisper/transcription workflows | planned | planned | no | no | planned | No Lua facade yet. |
| workflow:audio | Audio capture/playback/processing workflows | planned | planned | no | no | planned | No Lua facade yet. |
| workflow:terminal-agent | libmdf/softline terminal and Agent Smith workflows | yes | planned | yes | no | n/a | Raw modules exist; Vectis-owned agent DX is future work. |
| workflow:pack | Pack/self-contained binary workflows | n/a | yes | yes | yes | n/a | `vectis -a pack` and packed Lua e2e exist. |
| workflow:totp-qr | TOTP and QR helpers | n/a | yes | yes | yes | n/a | Exposed under `vectis.auth.totp` and `vectis.auth.qr`. |
| workflow:openapi | OpenAPI generation | n/a | planned | yes | no | n/a | C SDK has generation support; Lua route metadata integration is missing. |

## Completion Rules

- New bundled dependencies that are useful from app code must add a `dep:*`
  row in this file before release.
- New Vectis C SDK workflows must add a `workflow:*` row before release.
- A `planned` or `partial` row is acceptable only when the Notes cell states
  the concrete missing surface or ownership decision.
- Local deterministic tests are required when a row moves from `planned` to
  `partial` or `yes`, unless the Notes cell documents why the surface is
  live-provider-only.
- Packed tests are required for workflow helpers intended to work in
  self-contained `vectis -a pack` applications.
