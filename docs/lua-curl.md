# Lua Curl Facade

Vectis preloads `curl` in the embedded Lua runtime. The facade is a thin
libcurl surface intended to work across URL schemes supported by the bundled
libcurl build rather than one command per protocol.

## Entry Points

- `curl.version()` returns the libcurl version string.
- `curl.perform(opts)` executes one transfer and returns a result table.
- `curl.json(opts)` is a buffered JSON convenience wrapper around
  `curl.perform(opts)`.
- `curl.stream_json(opts)` connects LoneJSON schema records to libcurl streaming
  callbacks.

`curl.perform(opts)` returns:

- `ok`: boolean success flag from libcurl.
- `code`: numeric `CURLcode`.
- `code_name`: libcurl error string for `code`.
- `status`: protocol response status when libcurl reports one.
- `body`: buffered response body, empty for streaming JSON responses.
- `headers`: buffered response headers when the protocol provides headers.
- `effective_url`: final URL when libcurl reports one.
- `error`: transfer error text when `ok` is false.

Buffered response bodies are capped at 8 MiB and response headers at 64 KiB.
Exceeding either limit aborts the transfer and reports an error. The
`curl.stream_json(opts)` response body ceiling also applies to bytes fed from
libcurl into `lonejson_curl_write_callback()`.

## Options

- `url`: required string. The URL scheme selects the libcurl protocol.
- `protocols`: libcurl protocol allowlist string, such as
  `http,https,smtp,smtps,sftp,scp,ftp,file,mqtt`.
- `method`: `GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD`, `OPTIONS`,
  `PROPFIND`, `MKCOL`, `COPY`, or `MOVE` for HTTP-oriented transfers,
  including WebDAV.
- `headers`: table of protocol headers. String keys produce `name: value`;
  array values are passed as raw header lines.
- `body`: request, upload, SMTP, or publish payload string.
- `upload`: boolean enabling libcurl upload mode with `body`, useful for
  FTP/file and SFTP/SCP style transfers.
- `upload_path`, `body_path`: file-backed upload source streamed through
  libcurl's read callback.
- `download_path`: file-backed response sink streamed through libcurl's write
  callback. The result `body` is empty when this is used.
- `multipart`: table of MIME parts for multipart transfers. Numeric entries
  are part tables. String keys are accepted as shorthand text fields or as
  fallback names for part tables. Part tables use `name`, `value` or `body`, or
  `path`/`file_path` for file-backed parts, plus optional `filename` and
  `content_type`.
- `timeout_ms`, `connect_timeout_ms`: total and connect timeout controls.
- `low_speed_limit`, `low_speed_time`: low-speed abort controls.
- `follow_redirects`: enables HTTP redirect following.
- `retry`: false disables retry. A table accepts `max_attempts`,
  `initial_delay_ms`, `max_delay_ms`, and `conditions`.
- `retry_max_attempts`, `retry_initial_delay_ms`, `retry_max_delay_ms`,
  `retry_conditions`: top-level aliases for the same retry controls.
- `http2`: asks libcurl for HTTP/2 over TLS where possible.
- `username`, `password`: protocol credentials.
- `proxy`, `proxy_type`, `proxy_username`, `proxy_password`: proxy controls.
- `interface`: outbound interface selector.
- `user_agent`: libcurl user agent.
- `accept_encoding`: accepted content encodings; an empty string asks libcurl
  to advertise all built-in encodings.
- `ca_file`, `ca_path`: TLS trust configuration.
- `verify_peer`, `verify_host`: TLS verification toggles, default true.
- `client_cert`, `client_key`, `client_cert_type`, `key_password`: client TLS
  certificate controls.
- `ssh_private_key`, `ssh_public_key`, `ssh_known_hosts`: SSH/SFTP/SCP key and
  host verification controls.
- `tcp_keepalive`: enables TCP keepalive.
- `no_signal`: keeps libcurl signal-free, default true.
- `smtp`: table enabling SMTP upload with `mail_from`, `rcpt`, optional
  `use_ssl`, and optional `probe`. SMTP payloads may use `body`, `body_path`,
  or `upload_path`.

The option families above cover the main protocol classes expected from the
bundled libcurl build: HTTP/HTTPS, multipart uploads, WebDAV, SMTP/SMTPS,
SFTP/SCP, FTP/file transfer, and MQTT-style publish payloads. Unsupported URL
schemes remain libcurl runtime errors and are reported in the result table.
Protocol-specific helpers such as `vectis.http`, `vectis.webdav`,
`vectis.mqtt`, and `vectis.smtp` use this lower-level facade.

## Managed Worker

`vectis.curl_worker` exposes the Vectis managed-service curl worker helpers.
This is separate from direct `curl.perform()`: Lua builds copied mailbox events,
the C-owned worker service performs transfers in the managed runtime domain, and
Lua decodes copied replies.

- `server:curl_worker_service(opts)` registers the C-owned worker. `opts`
  requires `request_mailbox` or `requests`, accepts optional `reply_broker` or
  `broker`, `name`, `poll_timeout_ms`, `start`, and an `http` table with the
  `vectis_http_client_config` fields supported by the C worker: `base_url`,
  `client_bundle_path`, `ca_bundle_path`/`ca_file`, `timeout_ms`,
  `connect_timeout_ms`, `follow_redirects`, `proxy_url`/`proxy`,
  low-speed controls, and retry controls.
- `server:curl_worker_service_states()` returns copied managed-service
  lifecycle diagnostics for registered curl workers.
- `vectis.curl_worker.http_request(opts)` returns a normal
  `vectis.mailbox` event table with `kind`, `payload`, and `expects_reply`.
  `opts` supports `method`, `url`, `headers`, `body`, `content_type`,
  `timeout_ms`, and `max_response_body_bytes`.
- `vectis.curl_worker.decode_http_response(event)` decodes a worker reply event
  into `ok`, `transfer_status`, `transfer_status_string`, `dependency_code`,
  `status`, `status_code`, `content_type`, `body`, `message`, and `detail`.

The first worker envelope is HTTP-specific. Other libcurl protocol workers
should add explicit copied request/reply envelopes rather than overloading the
HTTP payload format.

Retry conditions may be `"transport"`, `"429"`, `"status_429"`, `"5xx"`,
`"status_5xx"`, `"default"`, or `"none"`, or a table combining condition
names. Streaming JSON responses cannot be retried because the response parser is
consumed by the first transfer attempt.

## JSON

`curl.json(opts)` accepts the same transport, TLS, protocol, proxy, and SMTP
independent options as `curl.perform(opts)`. If `opts.body_json` or `opts.json`
is present, it is encoded with `lonejson.encode_value()` and sent as `body`.
Non-empty response bodies are decoded with `lonejson.decode_value()` when
possible and exposed as both `result.json` and `result.response_json`.

`curl.json(opts)` is intentionally buffered.

## Streaming JSON

`curl.stream_json(opts)` accepts:

- `request = {schema = schema, value = table}` for streaming JSON upload.
- `response = {schema = schema}` for streaming JSON response parsing.

Request values are copied into a Lua-owned LoneJSON record and streamed through
`lonejson_curl_read_callback()` as libcurl asks for upload chunks. Response
bytes are fed directly from libcurl's write callback into
`lonejson_curl_write_callback()` and converted back to a Lua table at the end
of the transfer.
