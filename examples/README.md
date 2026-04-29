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
- `kore/kore_regex_routes.c`: raw Kore/POSIX regex route shape for cases where
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

- `lockd/lockd_open_client.c`: raw liblockdc client setup with flexible bundle
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
  the raw liblockdc managed consumer service config, with separate app and
  lockd client loggers.

## `curl/`

- `curl/curl_json_api.c`: handle-shaped curl-backed client setup, GET, HEAD,
  OPTIONS, DELETE, and POST/PUT/PATCH JSON helpers.
- `curl/curl_downstream_e2e.c`: controlled local downstream server/client flow
  covering JSON methods, HEAD/OPTIONS, streaming responses, downloads, and
  uploads.
- `curl/curl_transfer.c`: handle-shaped generic curl-backed file download and
  upload.
- `curl/curl_sftp.c`: SFTP upload/download through curl.
- `curl/mqtt_publish.c`: MQTT raw and JSON publish through curl.

## `ssh/`

- `ssh/ssh_command.c`: libssh2 command execution.
- `ssh/ssh2_sftp.c`: SFTP upload/download through libssh2.

## `certs/`

- `certs/cert_bundle.c`: OpenSSL-backed server and lockd client certificate
  bundle generation.

## `raw/`

- `raw/raw_escape_hatches.c`: direct inclusion and use of bundled dependency
  headers.

The examples are compiled by the normal build so the public SDK shape remains
mechanically valid. New examples should prefer
`vectis_source_from_path()`, `vectis_source_from_memory()`, `vectis_route()`,
`vectis_route_regex()`, and `vectis_json_route()` unless they are intentionally
showing a lower-level compatibility field.
