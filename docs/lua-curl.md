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
- `timeout_ms`, `connect_timeout_ms`: total and connect timeout controls.
- `low_speed_limit`, `low_speed_time`: low-speed abort controls.
- `follow_redirects`: enables HTTP redirect following.
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
  `use_ssl`, and optional `probe`.

The option families above cover the main protocol classes expected from the
bundled libcurl build: HTTP/HTTPS, WebDAV, SMTP/SMTPS, SFTP/SCP, FTP/file
transfer, and MQTT-style publish payloads. Unsupported URL schemes remain
libcurl runtime errors and are reported in the result table.

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
