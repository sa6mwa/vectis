# Vectis C SDK Examples

These examples are DX probes. They intentionally use the public Vectis C SDK
surface directly, without local helper layers that would hide awkward API shape.

## `kore/`

- `kore/rest_api_lockd_lonejson.c`: JSON body route setup, lonejson mapped
  request and response structs, safe lockd key formatting, and Vectis lockd
  state save helpers.
- `kore/kore_basic_server.c`: minimal Kore-backed Vectis server shape with
  pslog and a shared GET/HEAD health route.
- `kore/kore_json_routes.c`: JSON route auto-wiring shape with lonejson maps.
- `kore/kore_json_multi_output.c`: typed JSON route shape where the handler
  chooses the HTTP status and response map at runtime.
- `kore/kore_openapi_docs.c`: optional OpenAPI metadata attachment and JSON/YAML
  spec generation from lonejson maps.
- `kore/kore_regex_routes.c`: direct Kore/POSIX regex route shape for cases where
  named Vectis path parameters are not the right fit.
- `kore/kore_tls_acme.c`: manual TLS and ACME server configuration shape.
- `kore/kore_tls_memory_bundles.c`: server cert/key, CA, and client-CA bundles
  supplied from in-memory PEM for packed-service deployments.
- `kore/kore_lockd_api.c`: API handler shape that combines Vectis path
  parameters, pslog, lockd state save/load helpers, safe lockd key formatting,
  and lonejson responses. It uses a scoped Kore logger and a separate lockd
  client logger with its own palette.
- `kore/kore_file_upload.c`: large upload route using the Vectis upload route
  constructor and streaming body-policy preset.
- `kore/kore_generated_response.c`: generated JSON response body written through
  lonejson to a temporary file for Kore to serve without application buffering.
- `kore/kore_static_assets.c`: static file and directory route helpers with
  GET/HEAD defaults and traversal-safe request paths.

## `lockd/`

- `lockd/lockd_open_client.c`: direct liblockdc client setup with flexible bundle
  sourcing.
- `lockd/lockd_acquire_save_load_release.c`: Vectis-owned lockd state
  save/load helpers using lonejson mapped structs.
- `lockd/lockd_query.c`: query result streaming into a file sink.
- `lockd/lockd_attachments.c`: attachment upload/download against a lease.
- `lockd/lockd_enqueue.c`: queue producer.
- `lockd/lockd_dequeue_ack_nack.c`: manual dequeue, payload copy, extend, ack,
  and nack error path.
- `lockd/lockd_consumer_service.c`: managed consumer service with SDK logging
  and process-level example logging.
- `lockd/lockd_consumer_service_with_state.c`: managed consumer service using
  dequeue-with-state and lonejson mapped state mutation.
- `lockd/lockd_vectis_consumer_service.c`: Vectis app-owned lockd setup plus
  the direct liblockdc managed consumer service config, with separate app and
  lockd client loggers.

## `curl/`

- `curl/curl_json_api.c`: handle-shaped curl-backed client setup, GET, HEAD,
  OPTIONS, `client->del()`, and POST/PUT/PATCH JSON helpers.
- `curl/curl_downstream_e2e.c`: controlled local downstream server/client flow
  covering JSON methods, HEAD/OPTIONS, streaming responses, downloads, and
  uploads.
- `curl/curl_transfer.c`: handle-shaped generic curl-backed file download and
  upload.
- `curl/curl_sftp.c`: SFTP upload/download through curl.
- `curl/mqtt_publish.c`: MQTT byte payload and JSON publish through curl.

## `dsv/`

- `dsv/dsv_lonejson.c`: row-only CSV input from memory or a file path, streamed
  into a lonejson mapped struct row callback by map field order, with opt-in
  whole-line comments, plus explicit materializing JSON-array conversion through
  the same lonejson map and scalar mapped-struct serialization back to CSV.

## `xml/`

- `xml/xml_lonejson.c`: XML input from a Vectis source into a typed lonejson
  struct, including element text, attributes, scalar arrays, and nested object
  arrays. Repeated array elements are intentionally required to be contiguous so
  the parser never buffers and reorders XML. Use LoneJSON spooled stream fields
  with the default `trim_text = 0` for large XML text/blob payloads.

## `mdf/`

- `mdf/mdf_render.c`: libmdf Markdown rendering from both a C string and a
  callback-backed source into a callback-backed sink.

## `lua/`

Lua examples follow the same rule as the C examples: they are self-contained
scripts and keep facade calls visible at the call site. Inline callbacks are
used where the demonstrated API requires a callback, but local helper layers are
kept out of the examples. A Lua example may require Vectis or bundled product
modules, but it must not require another file from `examples/lua/`.

- `lua/mdf_render.lua`: libmdf Lua Markdown rendering through both
  `render()` and `render_stream()`.
- `lua/terminal_tools.lua`: Vectis terminal helper DX over libmdf Markdown
  rendering and softline bounded editor construction.
- `lua/logging.lua`: Vectis logging helper DX over pslog with default fields
  and structured Vectis error metadata.
- `lua/data_formats.lua`: typed DSV/CSV/TSV parsing, row callbacks,
  serialization, and XML-to-LoneJSON table parsing through `vectis.dsv` and
  `vectis.xml`.
- `lua/crypto_certs.lua`: dependency-native OpenSSL digest, HMAC, encoding,
  random, signing, and verification helpers alongside Vectis certificate bundle
  generation, validation, and inspection.
- `lua/protocol_clients.lua`: curl-backed MQTT and SMTP workflow helpers plus
  SCP validation/error handling without requiring live external services.
- `lua/curl_protocols.lua`: protocol-neutral `curl.perform()` file-backed
  transfer, upload, download, and protocol allowlist behavior for workflows
  where adding a Vectis-specific wrapper would be redundant.
- `lua/sftp_handles.lua`: stateful `vectis.ssh.sftp_open()` session, file,
  directory, stat, rename, and cleanup receiver workflow against the opt-in
  e2e SSH/SFTP service.
- `lua/opcua_client.lua`: dependency-native cpkt OPC UA Lua client connect,
  read, write, disconnect, and manual client lifecycle against the local e2e
  server.
- `lua/cai_local.lua`: dependency-native CAI Lua module constants, model
  metadata, dotenv parsing, tool schema, response params, registry, and MCP
  handler lifecycle without live provider calls.
- `lua/local_data_pipeline.lua`: dependency-native liblql filtering/projection,
  pslog JSON logging, softline editor state operations, and zlib string/file
  compression through their direct Lua modules.
- `lua/webdav_fileserver.lua`: mutable WebDAV fileserver mount and
  `vectis.webdav` client operations against a deterministic local server.
- `lua/api_server.lua`: packable Lua API server script using direct
  `server:json()`/`server:auth_json()` receivers, OpenAPI generation,
  `vectis.rest` client helpers for self-test traffic, and native issued
  credentials. By default it self-tests and exits; set
  `VECTIS_LUA_API_EXAMPLE_SERVE=1` to keep it listening. CTest also packs and
  executes this script as `vectis_example_lua_api_server_pack`.
- `lua/downstream_api.lua`: packable Lua downstream API client example using
  direct `server:json()` receivers for the deterministic local API and
  `vectis.rest` base-URL client helpers for the downstream calls. CTest runs
  both normal and packed forms.
- `lua/sftp_transfer.lua`: curl-backed Lua SFTP upload/download through
  `vectis.http.sftp_upload()` and `sftp_download()`. The local e2e harness runs
  it against the compose SSH/SFTP service.
- `lua/ssh_command.lua`: libssh2-backed Lua SSH command execution through
  `vectis.ssh.exec()`. The local e2e harness runs it against the compose
  SSH/SFTP service with known_hosts pinning.
- `lua/lockd_state.lua`: Vectis lockd helper state save/load workflow against
  `LOCKD_ENDPOINT`.
- `lua/lockd_queue.lua`: Vectis lockd helper queue enqueue/dequeue workflow
  with explicit ack against `LOCKD_ENDPOINT`.
- `lua/consumer_service.lua`: long-running Lua service shape using
  `server:consumer_service()` with the C-owned `webdav_marker` handler. In
  self-test mode it enqueues a lockd message and observes marker files through a
  static route; set `VECTIS_LUA_CONSUMER_EXAMPLE_SERVE=1` to keep it running.

## `ssh/`

- `ssh/ssh_command.c`: libssh2 command execution.
- `ssh/ssh2_sftp.c`: SFTP upload/download through libssh2.

## `certs/`

- `certs/cert_bundle.c`: OpenSSL-backed server and lockd client certificate
  bundle generation.

## `dependency/`

- `dependency/dependency_escape_hatches.c`: direct inclusion and use of bundled dependency
  headers.

The examples are compiled by the normal build so the public SDK shape remains
mechanically valid. New stateful examples should prefer handle methods such as
`app->route(app, ...)`, `app->start(app, ...)`, `client->get(client, ...)`,
`client->del(client, ...)`, `ssh->exec(ssh, ...)`, and
`service->run(service, ...)`. App-owned accessors such as
`app->logger(app)`, `app->lockd_client(app)`, `app->openapi(app, ...)`,
`app->webdav(app, ...)`, and `app->auth_routes(app, ...)` should use the same
facade style. Constructors and stateless helpers such as
`vectis_source_from_path()`, `vectis_route()`, `vectis_route_regex()`, and
`vectis_json_route()` remain normal free functions.
