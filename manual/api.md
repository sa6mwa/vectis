# Vectis API Surface

Vectis has two product-facing API surfaces:

- the C SDK in `include/vectis/vectis.h`, shipped as `libvectis`;
- the embedded Lua surface in the `vectis` executable, documented in
  [Lua surface](lua.md).

The `vectis` executable statically embeds its Lua runtime and preloaded modules.
The C SDK does not ship Lua runtime state or package-manager artifacts.

## C SDK

The public C SDK is declared in `include/vectis/vectis.h`. It is organized
around explicit config structs, status-returning functions, and caller-owned
cleanup functions.

Core conventions:

- Functions that can fail return `vectis_status`.
- Diagnostic detail is reported through `vectis_error`.
- Public status values are named by `vectis_status_string()`.
- Config structs have `*_init()` helpers.
- Owned output structs have cleanup/destroy/close helpers where required.
- Stateful receiver handles reserve eight private method slots, so compatible
  future receiver methods can consume a slot without extending the layout.

Major groups:

- Runtime/server: `vectis_app`, `vectis_server_config`,
  `vectis_register_route()`, foreground `vectis_run()`, process-backed
  `vectis_start()`, `vectis_stop()`, and `vectis_restart()`.
- Requests/responses: `vectis_request_*`, `vectis_response_*`,
  materialized/spilled body helpers, JSON replies, file responses, and
  source-backed responses.
- Static and embedded assets: `vectis_static_file_config`,
  `vectis_static_directory_config`, and `vectis_static_embedded_config`.
- WebSocket routes: `vectis_websocket_route_config`,
  `vectis_register_websocket()`, and callback-borrowed
  `vectis_websocket_send_*()` helpers.
- Reverse proxy routes: `vectis_proxy_route_config` in
  `include/vectis/proxy.h` registers bounded streaming HTTP, SSE, and
  WebSocket forwarding. `tls_ca_pem` optionally supplies a copied CA bundle
  for verified HTTPS and WSS origins; without it, libcurl uses its default
  trust store. Its optional `rewrite` callback receives borrowed inbound
  metadata and changes the configured target index, method, raw path/query,
  Host, and end-to-end headers through `vectis_proxy_outbound_*` helpers.
  Invalid rewrites fail before an upstream connection. The optional
  `modify_response` callback edits final downstream status and end-to-end
  headers through a borrowed `vectis_proxy_response` view before headers are
  committed. It receives no body, and a successful WebSocket `101` bypasses
  it. `libvectis` sources and public headers compile as strict C89; vendored
  Kore and the embedded CLI dependency modules have separate C99 targets.
- TLS/server hardening: `vectis_tls_config` supports manual/ACME material,
  client CA verification, protocol version selection, and OpenSSL cipher lists;
  `vectis_server_config` covers listener backlog, processing budget, and HSTS
  controls, process-wide WebSocket frame and idle-timeout limits, the HTTP
  Server header value, and structured access logging through
  `vectis_access_log_config`. Access records derive `sub=http`, use trace for
  1xx--3xx, warn for 4xx, and error for 5xx by default; exact status rules
  override those categories. The legacy file path is separate and opt-in.
  Kore quiet mode, worker death policy, and opt-in pretty framework error pages
  remain available.
- WebDAV/auth routes: `vectis_webdav_mount_config`,
  `vectis_webdav_embedded_site_config`, and `vectis_auth_routes_config`.
- Metrics: `vectis_metrics_config`, `app->metrics()`, and
  `vectis_metrics_snapshot_json()` provide the opt-in dashboard, JSON
  snapshots, and lockdc/Pouch checkpoint persistence documented in
  [Metrics](metrics.md).
- Auth credentials: native users, issued credentials, OAuth2/OIDC WebDAV keys,
  Lockd-backed opaque browser sessions configured with
  `vectis_auth_browser_session_config`, and
  `vectis_auth_basic_authorization()`.
- OpenAPI: `vectis_openapi_document`, route docs, request/response schemas, and
  generation helpers.
- Lockd workflows: `vectis_lockd_config` (including encrypted local Pouch key
  provisioning), state helpers, queue/consumer
  service registration, consumer receivers, and bounded consumer JSON payload
  decoding with `vectis_lockd_consumer_json_into()`.
- DSV/XML/JSON: DSV parse/write/spill helpers, JSON array rewrite/iteration,
  XML-to-LoneJSON parsing, LoneJSON-to-XML writing, and request JSON helpers.
- HTTP/curl workflows: `vectis_http_client_config`, `vectis_http_request`,
  `vectis_http_response`, client execute methods, JSON helpers, file upload,
  and file download.
- SSH/SFTP/SCP: `vectis_ssh_config`, `vectis_sftp_config`,
  `vectis_ssh_exec()`, SFTP file upload/download, SFTP filesystem operations
  (`stat`, `mkdir`, `remove`, `rmdir`, `rename`, `chmod`), SCP file
  upload/download, known_hosts verification, and optional SHA-256 host-key
  fingerprint pinning through `host_key_sha256`.
- MQTT: `vectis_mqtt_config`, publish, and JSON publish helpers.
- Certificates: key, CSR, bundle, CA, validation, and inspection workflows.
- Agent Smith: `vectis_smith_store` adapts durable lockdc state to CAI session
  callbacks; `vectis_smith` provides owner-thread Smith open, submit,
  steering, queued-turn, pump, state, and wakeup-fd operations. See
  [Agent Smith](agent-smith.md).

## Lua Surface

The Lua surface is documented in [Lua surface](lua.md). Dependency-native
facades stay thin over their upstream C implementations. Vectis-owned modules
cover service workflows where Vectis owns the cross-library DX.

Lua docs:

- [Lua auth](lua-auth.md)
- [Lua certificates](lua-certs.md)
- [Lua CAI](lua-cai.md)
- [Agent Smith](agent-smith.md)
- [Lua command-line applications](lua-cli.md)
- [Lua curl](lua-curl.md)
- [Lua DSV](lua-dsv.md)
- [Lua embedded assets](lua-embedded.md)
- [Lua HTTP](lua-http.md)
- [Lua Kore](lua-kore.md)
- [Lua libmdf](lua-libmdf.md)
- [Lua lockd](lua-lockd.md)
- [Lua logging](lua-log.md)
- [Lua LQL](lua-lql.md)
- [Lua mailbox](lua-mailbox.md)
- [Lua MQTT](lua-mqtt.md)
- [Lua OpenSSL](lua-openssl.md)
- [Lua OPC UA](lua-opcua.md)
- [Lua app](lua-app.md)
- [Lua pslog](lua-pslog.md)
- [Lua REST](lua-rest.md)
- [Serving a Lua site](lua-site.md)
- [Lua softline](lua-softline.md)
- [Lua SMTP](lua-smtp.md)
- [Lua SSH](lua-ssh.md)
- [Lua status](lua-status.md)
- [Lua SUS](lua-sus.md)
- [Lua audio](lua-audio.md)
- [Lua terminal](lua-terminal.md)
- [Lua WebDAV](lua-webdav.md)
- [Lua XML](lua-xml.md)
- [Lua zlib](lua-zlib.md)

## Release Contract

Before release, new public C SDK groups must be represented here and declared in
`include/vectis/vectis.h`. New Lua modules or workflow helpers must be linked
from `manual/lua.md`.

The embedded Kore runtime is a private implementation detail. The Vectis SDK
does not ship a standalone public `libkore` ABI.
