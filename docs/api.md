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

Major groups:

- Runtime/server: `vectis_app`, `vectis_server_config`,
  `vectis_register_route()`, foreground `vectis_run()`, process-backed
  `vectis_start()`, and `vectis_stop()`.
- Requests/responses: `vectis_request_*`, `vectis_response_*`,
  materialized/spilled body helpers, JSON replies, file responses, and
  source-backed responses.
- Static and embedded assets: `vectis_static_file_config`,
  `vectis_static_directory_config`, and `vectis_static_embedded_config`.
- WebDAV/auth routes: `vectis_webdav_mount_config`,
  `vectis_webdav_embedded_site_config`, and `vectis_auth_routes_config`.
- Auth credentials: native users, issued credentials, OAuth2/OIDC WebDAV keys,
  and `vectis_auth_basic_authorization()`.
- OpenAPI: `vectis_openapi_document`, route docs, request/response schemas, and
  generation helpers.
- Lockd workflows: `vectis_lockd_config`, state helpers, queue/consumer
  service registration, and consumer receivers.
- DSV/XML/JSON: DSV parse/write/spill helpers, JSON array rewrite/iteration,
  XML-to-LoneJSON parsing, and request JSON helpers.
- HTTP/curl workflows: `vectis_http_client_config`, `vectis_http_request`,
  `vectis_http_response`, client execute methods, JSON helpers, file upload,
  and file download.
- SSH/SFTP/SCP: `vectis_ssh_config`, `vectis_sftp_config`,
  `vectis_ssh_exec()`, SFTP file upload/download, SFTP filesystem operations
  (`stat`, `mkdir`, `remove`, `rmdir`, `rename`, `chmod`), and SCP file
  upload/download.
- MQTT: `vectis_mqtt_config`, publish, and JSON publish helpers.
- Certificates: key, CSR, bundle, CA, validation, and inspection workflows.

## Lua Surface

The Lua surface is tracked by [Lua surface](lua.md) and
[Lua coverage matrix](lua-coverage-matrix.md). Dependency-native facades should
stay thin over their upstream C implementations. Vectis-owned modules should
cover service workflows where Vectis owns the cross-library DX.

Lua docs:

- [Lua auth](lua-auth.md)
- [Lua certificates](lua-certs.md)
- [Lua CAI](lua-cai.md)
- [Lua curl](lua-curl.md)
- [Lua DSV](lua-dsv.md)
- [Lua HTTP](lua-http.md)
- [Lua libmdf](lua-libmdf.md)
- [Lua MQTT](lua-mqtt.md)
- [Lua OpenSSL](lua-openssl.md)
- [Lua server](lua-server.md)
- [Lua softline](lua-softline.md)
- [Lua SMTP](lua-smtp.md)
- [Lua SSH](lua-ssh.md)
- [Lua SUS](lua-sus.md)
- [Lua audio](lua-audio.md)
- [Lua WebDAV](lua-webdav.md)
- [Lua XML](lua-xml.md)
- [Lua zlib](lua-zlib.md)

## Release Contract

Before release, new public C SDK groups must be represented here and declared in
`include/vectis/vectis.h`. New Lua modules or workflow helpers must be linked
from `docs/lua.md` and represented in `docs/lua-coverage-matrix.md`.
