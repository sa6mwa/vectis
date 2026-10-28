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
- `headers`: table of protocol headers. String keys must be valid HTTP tokens
  and produce `name: value`; array values are passed as raw header lines.
  Values and raw lines cannot contain CR, LF, or NUL bytes.
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

- `app:curl_worker_service(opts)` registers the C-owned worker. `opts`
  requires `request_mailbox` or `requests`, accepts optional `reply_broker` or
  `broker`, `name`, `poll_timeout_ms`, `start`, and an `http` table with the
  `vectis_http_client_config` fields supported by the C worker: `base_url`,
  `client_bundle_path`, `ca_bundle_path`/`ca_file`, `timeout_ms`,
  `connect_timeout_ms`, `follow_redirects`, `proxy_url`/`proxy`,
  low-speed controls, and retry controls.
- `app:curl_worker_service_states()` returns copied managed-service
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

JSON uploads use lonejson's native seek callback for body-preserving redirects
and its rewind API for configured retries. Replay does not measure or buffer the
JSON document. One-shot sources cannot be replayed and return a curl rewind
error; they are not silently materialized to make retries possible.

## Reusable sessions and concurrent ordinary transfers

`curl.session()` owns a native easy handle and exposes `perform(opts)`, `reset()`
and `close()`. Every perform clears prior transfer settings and results while
preserving libcurl's connection cache. Reset clears settings/results and retains
connections; close releases the cache. Handles belong to their creating process
and thread; construct them after the applicable fork boundary.

These receivers accept ordinary transfer options listed above: URL/protocols,
method, headers, body, upload, upload_path/body_path, download_path, transport,
TLS, proxy, authentication and SSH key options. MIME, SMTP, schema mapping and
automatic retry remain on existing helpers; unsupported receiver options return
`nil, error` before transfer initiation. No automatic replay is added. Options
and headers are copied per request. Files are streamed through bounded buffers.

Additional options:

- `read(capacity)` returns 1..capacity bytes, nil for EOF, or `nil, error` for
  failure. Capacity is at most 65536. Empty or oversized strings fail. A read
  callback selects upload mode; use `method='POST'` for HTTP POST. Body strings,
  upload files and read callbacks are mutually exclusive.
- `write(bytes)` receives chunks of at most 65536 bytes and must return true or
  `nil, error`. It replaces response-body collection, so `result.body` is absent
  and `result.streamed` is true. There is no cumulative callback body ceiling.
  It is mutually exclusive with download_path. Headers retain the 64 KiB limit.
- `progress(record)` returns true to continue or false to cancel. The record
  contains download_total, downloaded, upload_total and uploaded byte counts.

Callbacks run synchronously on the invoking Lua state and cannot yield.
Exceptions and explicit callback failures abort the transfer. The original error
object is available as result.callback_error and result.error; native/callback
status also appears in result.failure. Reentry or close during a callback fails
the transfer even when the callback catches that error. Close/GC never invokes
user callbacks. File read/write/close errors are observable. One-shot callback
uploads cannot be replayed for a body-preserving redirect and may report a native
rewind error; they are never silently collected to enable replay.

`curl.multi()` owns concurrent easy transfers:

- `add(opts)` captures a request and returns its transfer ID.
- `step()` advances available work without waiting and returns running count.
- `poll(timeout_ms)` waits for readiness for 0..60000 ms (default zero), returning
  a readiness count. Queued completions cause an immediate zero return; drain
  them before waiting. Poll does not invoke Lua callbacks.
- `completions(limit)` returns up to limit result tables with IDs (default 64;
  valid range 1..64). Use a small limit to drain buffered results within the
  Lua memory budget, and continue until the queue is empty. Transport
  and callback failures are per transfer. Successful copying acknowledges the
  selected batch; an allocation failure leaves all completions queued for retry.
- `close()` cancels/releases outstanding requests without invoking Lua callbacks.

At most 64 requests may be active or awaiting completion consumption together.
The caller owns advancement, deadlines and draining. Vectis creates no worker
threads and does not register on Kore's event loop; native resolver work is
governed by the bundled libcurl build.

```lua
local curl = require('curl')
local multi = assert(curl.multi())
local ids = {}
for _, url in ipairs({'https://example.com/', 'https://example.org/'}) do
  ids[assert(multi:add({url=url, timeout_ms=5000}))] = url
end
repeat
  local running = assert(multi:step())
  for _, result in ipairs(assert(multi:completions())) do
    print(ids[result.id], result.status, result.ok)
  end
  if running == 0 then break end
  assert(multi:poll(100))
until false
assert(multi:close())
```

The C API in `vectis/curl.h` exposes separate session/multi receiver shells.
A synchronous configuration hook uses supported public libcurl options; bounded
C read/write/progress hooks must return status rather than longjmp. Configurers
must only set options and must not replace the engine's callbacks/private/error
storage. Hook context and option storage remain caller-owned until reset/close
or completion consumption. Results borrow native-owned data during that period.
C multi completion(offset) peeks a zero-based queue entry; consume(count)
acknowledges a successfully copied prefix. This separation preserves results
when a caller's allocation/copy fails.
