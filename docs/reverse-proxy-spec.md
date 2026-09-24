# Reverse Proxy Design and Verification Spec

Status: proposed; this document specifies a future Vectis feature. No proxy
route or transport described here is implemented yet.

## Objective

Provide an in-process reverse proxy route for Kore-backed Vectis apps. One
route accepts ordinary HTTP requests, long-lived Server-Sent Events (SSE)
responses, and WebSocket upgrades. Kore terminates inbound TLS; Vectis forwards
to a configured HTTP or HTTPS upstream, with request rewrites and optional
application decisions before forwarding. `https` covers `wss` upstreams for
WebSocket upgrades.

Streaming means bytes move between producer and consumer through bounded chunk
buffers. Neither direction may accumulate, spool, parse, or serialize an entire
body or WebSocket message. An idle SSE connection and a slow peer must not
occupy a worker thread or grow memory with the duration or total size of the
transfer.

The design has one public proxy route and two internal data paths: an
asynchronous HTTP transfer for HTTP/SSE, and a raw duplex connection after an
upstream-approved WebSocket upgrade. The route's policy and lifecycle are
shared; application WebSocket message callbacks are a separate Vectis feature.

## Public contract

The proposed C registration is `app->proxy_route(config, error)` with a
corresponding Lua `app:proxy(opts)`. The exact C identifiers are illustrative
until implementation, but the following fields and callback phases are the
intended contract:

| Field or hook | Contract |
| --- | --- |
| `path`, `methods`, `path_kind` | Use Vectis route matching and ordering. WebSocket upgrade requires GET. |
| `target` | Required configured `http://` or `https://` base URL. Its scheme and authority are fixed for the route. |
| `auth` or `preflight(in)` | Optional admission decision at headers time. It may proxy or send a local response before any upstream transfer. It cannot consume the body. |
| `rewrite(in, out)` | Optional synchronous, borrowed callback. `in` is immutable inbound metadata; `out` is sanitized mutable outbound metadata. It may select an explicitly configured target, change method/path/query/Host and edit end-to-end headers. |
| `modify_response(response)` | Optional status/header decision after final upstream headers and before downstream headers are committed. It does not receive a materialized body. |
| `on_error(error)` | Optional local error response while headers are uncommitted. Later errors abort the stream and are logged. |
| `connect_timeout`, `idle_timeout`, `total_timeout`, `buffer_limit` | Explicit per-route resource policy. Total timeout defaults to disabled for an established SSE or WebSocket stream; connect and idle limits remain active. |

The normal flow is: match route; run authentication and `preflight`; copy and
sanitize inbound metadata; apply default target/path rewrite; run `rewrite`;
validate the final destination and headers; start the upstream transfer. A
custom handler/director uses `preflight` and `rewrite` around this operation,
not an ordinary buffered `app:route()` handler. Callbacks execute in the owning
Kore worker, must return promptly, and may not retain borrowed request or
response views. Lua callbacks have the same rule and do not yield into the
transport.

The default URL rewrite joins the configured target base path with the inbound
path and retains the query string. Rewriters can replace the path and query
explicitly. The default outbound Host is the target authority; retaining the
inbound Host is an explicit choice. The proxy does not rewrite `Location` in
redirect responses by default and never follows upstream redirects itself.
The proxy rejects a rewrite to an unconfigured scheme or authority. A route
that needs several backends declares those targets up front; a director may
select among them without becoming an unrestricted outbound request facility.

This follows Go's newer `Rewrite(in, out)` model rather than copying the
behavior of its older `Director`: sanitize first, then let application code
modify the outbound request. See the [Go ReverseProxy contract](https://pkg.go.dev/net/http/httputil#ReverseProxy).

## Wire behavior

### Requests and responses

- Forward the method, path/query, status, and end-to-end headers without
  interpreting payloads. Preserve repeated fields such as `Set-Cookie`.
  Request-body presence follows HTTP framing rather than a hard-coded method
  list; a framed GET or OPTIONS body must stream through as well.
- Remove `Connection` and every header named by its tokens, plus standard
  hop-by-hop headers, on both legs. Recreate only transport-required framing
  and the validated WebSocket `Connection: Upgrade` and `Upgrade: websocket`
  fields. Reject malformed or conflicting framing before opening an upstream
  connection.
- Remove client-supplied `Forwarded` and `X-Forwarded-*` by default. Set
  forwarding headers from Kore's accepted peer, TLS state, requested Host,
  and Vectis's configured trusted-proxy policy. A policy may explicitly
  preserve a trusted chain; an untrusted client cannot supply its own chain.
- Preserve `Content-Type`, content encoding, and entity bytes. Disable curl
  automatic decompression, cookie storage, automatic authentication, redirects,
  environment proxy selection, and implicit protocol fallbacks. Restrict the
  upstream protocol to HTTP/1.1 initially, including connection reuse and TLS
  ALPN negotiation, so pausing a transfer cannot hide a large HTTP/2 or HTTP/3
  multiplexing buffer.
- Forward a known `Content-Length` when the body length remains known and
  unchanged. Otherwise use HTTP/1.1 chunked framing. Never send a body for
  HEAD responses or status codes that forbid one. If a length mismatch occurs
  after headers, abort the downstream connection.
- Handle `Expect: 100-continue` without buffering the upload. Forward or
  generate `100` only when the upstream path is ready to receive the body;
  propagate other informational responses such as `103` if present. If the
  upstream sends a final response before the upload finishes, stop forwarding
  the upload and close the downstream connection after that response unless
  unread request bytes have been safely drained within a fixed bound.
- Support HTTP/1.1 chunked client uploads and request/response trailers.
  Decode inbound chunk framing before forwarding body bytes; send declared
  request trailers with libcurl's trailer callback; relay permitted response
  trailers after the body. Enforce header/trailer count and byte limits and
  reject forbidden trailer fields. Do not silently discard trailers.
- Preserve downstream connection reuse only when framing and body completion
  are unambiguous. An aborted stream or partially consumed request closes its
  downstream connection.

Inbound HTTP is limited initially to the HTTP/1.x versions Kore serves. Route
registration fails if the Kore runtime is disabled. Unsupported methods and
upgrade protocols receive explicit errors; `CONNECT` is outside this route's
scope.

### SSE

SSE is a normal HTTP response with `Content-Type: text/event-stream`. Forward
its bytes unchanged; do not parse events or wait for an entire event. Commit
and flush headers once the upstream final headers arrive, then flush each
available body chunk promptly. An upstream stream with unknown length also
uses immediate flushing. Backpressure may delay delivery when the client is
slow, but it never triggers full-response buffering. Keepalive comments are
ordinary bytes. A client disconnect cancels the upstream transfer.

### WebSocket upgrade

For an upgrade request, forward the sanitized opening handshake to the
configured upstream over HTTP/1.1 or HTTPS. Preserve the client's WebSocket
key, offered subprotocols, and offered extensions; validate the upstream's
`101` status, upgrade tokens, accept value, and negotiated selections before
committing the downstream `101`. Non-`101` responses follow the ordinary HTTP
response path and may have a streamed body.

After `101`, switch Kore's accepted connection from HTTP parsing to a raw
bidirectional relay. Send the upstream's selected handshake headers downstream
and copy subsequent bytes through a fixed-size buffer in each direction.
Preserve masked frames, fragmentation, control frames, close codes,
subprotocols, and negotiated extensions as wire bytes. The proxy does not
reframe messages, answer pings, or inspect message payloads. A close or error
on either side tears down both transport handles after pending bounded writes
are handled according to the close policy. Both directions have independent
read pause and resume; stalled writes cannot accumulate frames.

The existing Kore WebSocket server API cannot provide this transparency: it
completes the client handshake, handles ping and close itself, and rejects
continuation frames. libcurl's WebSocket API does not negotiate extensions.
The tunnel therefore uses curl only to establish a raw TCP/TLS upstream
connection, then sends and reads the opening HTTP handshake and tunneled bytes
through curl's connect-only send/receive interface. No blocking
`curl_easy_perform()` call runs in a Kore worker.

## Required internal changes

1. **Header-time dispatch.** Identify proxy routes in Kore's `on_headers`
   phase, before the body is complete. The normal Vectis route handler remains
   synchronous. Kore currently delivers any body bytes already present in the
   header read *before* calling `on_headers`; move that first delivery after
   proxy dispatch so no bytes are buffered or lost before the upstream starts.
   Keep proxy state bound to the Kore request and accepted connection, with
   cleanup on completion, cancellation, worker shutdown, and downstream
   disconnect.
2. **Request ingress backpressure.** Extend the vendored Kore HTTP body path
   so a proxy body callback can stop and later resume reads without returning
   a 500 or disabling all socket events. Add incremental incoming chunked
   parsing and trailer exposure with the same body and metadata limits as
   fixed-length requests. For proxy routes, detect bodies on every supported
   method from framing headers; Kore currently marks some methods complete at
   header parse time. Preserve already-read bytes when pausing.
3. **Asynchronous HTTP egress.** Use Kore's curl multi event loop with direct
   libcurl header, read, and write callbacks. Do not call Vectis's synchronous
   `vectis_http_client` or Kore's buffer-oriented `kore_curl_http_setup()` /
   `kore_curl_tobuf()` path. Keep one bounded body chunk in each direction.
   Reuse compatible HTTP/1.1 upstream connections; a WebSocket tunnel owns its
   upstream connection until the upgrade closes.
   Pause curl uploads when no request chunk is available; pause curl downloads
   when Kore's send queue is full. Resume only after the relevant chunk is
   consumed or sent. Treat libcurl's paused callback data as part of the
   per-connection memory budget.
4. **Response egress.** Reuse the existing Vectis live response bridge's
   send-completion and chunk-framing mechanics, but drive its next chunk from
   asynchronous upstream availability. Its current `lc_source->read()` is
   synchronous and `0` means EOF, so it cannot represent “wait for curl.”
   Add a separate pending state rather than blocking or returning EOF.
5. **Upgrade handoff.** Add a narrow Kore raw-connection handoff after a
   validated upstream `101`. It must preserve prefetched client bytes, keep
   TLS read/write functions active, avoid restarting HTTP receive, and control
   read interest independently from write interest. A whole-fd epoll disable
   is insufficient. Retain curl connect-only handles after setup; Kore's
   current curl completion path removes and frees them. Drive raw upstream
   readiness through Kore's worker event loop and `curl_easy_send/recv`,
   including `CURLE_AGAIN` and TLS-internal buffered bytes.
6. **Lifecycle and limits.** Every proxy request has one owner, one cancellation
   path, and at most one queued chunk per direction plus documented library
   buffers. Bound connection count, header bytes, trailer bytes, upload size,
   idle time, connect time, and both send queues. A failed upstream connection
   returns a gateway error before downstream headers; an error after headers
   aborts the stream. Disable automatic retries for streamed or upgraded
   requests. Copy route configuration during app declaration, but create curl
   handles only inside the Kore worker after its fork boundary.

These are focused additions to the vendored Kore transport, maintained through
the repository's Kore patch workflow. They are not new buffering surfaces for
ordinary Vectis handlers. The relevant current code is
[`vectis_kore_bridge.c`](../src/vectis_kore_bridge.c),
[`http.c`](../vendor/kore/upstream/src/http.c),
[`curl.c`](../vendor/kore/upstream/src/curl.c), and
[`websocket.c`](../vendor/kore/upstream/src/websocket.c).

## Security and operational policy

- Verify upstream TLS certificates and hostnames by default. Use the URL host
  for SNI; allow an explicit CA bundle and client certificate. Never convert
  `https` to `http` after a connection failure.
- Resolve only configured upstream authorities. Rewrites cannot use arbitrary
  URLs, local files, Unix sockets, or curl proxy environment variables. Apply
  connection limits and DNS/connect deadlines to every attempt.
- Reject invalid header names/values, CR/LF injection, inconsistent
  `Content-Length`/`Transfer-Encoding`, unsupported transfer coding, and
  invalid WebSocket negotiation. Enforce finite header and handshake limits
  before starting a body stream or tunnel.
- Distinguish connect timeout from stream idle timeout. A healthy SSE or
  WebSocket connection can outlive ordinary request timeouts, but an inactive
  connection is reclaimed. Shutdown cancels upstream operations and closes
  both tunnel legs within the app's existing graceful-shutdown deadline.
- Record request ID, route, configured upstream name, status, bytes in each
  direction, duration, disconnect side, and error category. Do not log
  credentials, cookies, query secrets, WebSocket payloads, or SSE contents.

## Verification strategy

Tests assert observable network behavior through a controllable upstream and
real Kore listener. Unit tests cover only pure policy functions; the primary
evidence is integration and end-to-end behavior.

| Layer | Required cases and assertions |
| --- | --- |
| Policy unit tests | Target/path/query joining and escaping; Host and forwarding policy; hop-by-hop token removal; duplicate headers; allowed-target selection; malformed framing and handshake rejection. |
| HTTP integration | GET, HEAD, POST, PUT, PATCH and error statuses; fixed-length and chunked uploads; chunked and fixed-length responses; trailers; interim `100`/`103`; early upstream final response; redirects and repeated `Set-Cookie`; TLS termination to both HTTP and verified HTTPS upstreams. Assert upstream receives the expected bytes and metadata. |
| Streaming integration | Upstream emits the first chunk, then waits before finishing; client must receive that chunk before upstream completion. Repeat with slow upstream, slow downstream, and simultaneous upload/download. Assert bounded queue depths and no spool files or full-body allocations. Use multi-gigabyte logical generators in the opt-in stress run. |
| SSE integration | Headers and first event arrive before upstream completion; periodic comments and events arrive at their production cadence; idle timeout behavior is explicit; downstream disconnect cancels upstream promptly. |
| WebSocket integration | Successful `ws`/`wss`, selected subprotocol, offered extension pass-through, fragmented messages larger than Kore's normal frame limit, interleaved ping/pong, close code/reason, non-`101` rejection, and client/server disconnect. Verify exact byte relay after the handshake, including masking. |
| Failure and lifecycle | DNS/connect/TLS failure, upstream reset before and after headers, malformed upstream headers, slowloris, callback rejection, worker shutdown, app stop, and connection limits. Assert no orphan transfer, retained curl handle, leaked fd, or hanging test process. |
| Sanitizers and fuzzing | ASan/UBSan integration runs; bounded fuzz targets for URL/header rewrite, chunk parser, trailer parser, and upgrade response validation. |

Use local fixtures rather than a public network service. Every test fixture
starts under the test runner, has a hard deadline, and is joined or killed on
all exits. Resource assertions use per-test worker/child ownership rather than
machine-wide process scans. The normal project `make test` and relevant
`make test-e2e` gates must pass; the new proxy integration tests run in the
deterministic suite without requiring Podman when their fixtures are local.

## Performance strategy and acceptance

Measure a direct-client-to-upstream baseline, then the same client and upstream
through Vectis. Use an isolated machine or cgroup, fixed worker count, pinned
payload generators, warmup, multiple repetitions, and identical HTTP/TLS
settings. Report the distribution across runs; do not infer a throughput
claim from one run. Record request rate or MiB/s, p50/p95/p99 first-byte and
completion latency, SSE event delay, WebSocket echo latency, CPU time, file
descriptors, and worker/cgroup peak memory. Record both absolute results and
incremental proxy cost relative to the direct path.

Run at least these profiles: small HTTP requests; large streaming download;
large upload with a slow upstream; full-duplex upload/download; 1, 32, and 256
concurrent SSE streams; 1, 32, and 256 concurrent WebSocket tunnels; slow
readers/writers; and HTTPS/WSS upstreams. Repeat a long-lived soak with
connection churn and app shutdown. Include both a fast path and intentionally
backpressured path, because peak throughput alone cannot reveal hidden
buffering.

The hard acceptance criteria are behavioral:

1. The first response chunk or SSE event is observable before the upstream
   finishes; request chunks reach the upstream before client upload EOF.
2. Per-transfer application queue occupancy never exceeds the configured
   chunk budget in either direction. Measure worker memory at fixed
   concurrency while increasing transferred bytes and duration by orders of
   magnitude; the memory envelope must remain flat apart from bounded
   connection, TLS, and header state. Record the measured envelope and its
   constituent budgets in the implementation documentation.
3. A stalled peer applies backpressure instead of increasing queue depth,
   CPU spin, or temporary-file usage. A disconnect releases both legs and
   returns the worker to its starting resource baseline.
4. SSE and WebSocket latency stays bounded under the measured concurrency
   profile; compare p95/p99 added latency to the direct baseline. Establish
   numerical regression thresholds from repeatable baseline data before
   making performance claims or enabling a CI performance gate.

Run a short, deterministic memory/backpressure smoke in ordinary CI. Keep
throughput and soak benchmarks opt-in or on dedicated runners so unrelated
host load does not turn the correctness gate into a noisy performance test.

## External API notes

The transport choices above depend on documented libcurl behavior:
[`CURLOPT_WRITEFUNCTION`](https://curl.se/libcurl/c/CURLOPT_WRITEFUNCTION.html)
and [`CURLOPT_READFUNCTION`](https://curl.se/libcurl/c/CURLOPT_READFUNCTION.html)
can pause an asynchronous transfer;
[`curl_easy_pause`](https://curl.se/libcurl/c/curl_easy_pause.html) documents
buffering while paused, especially on multiplexed HTTP;
[`CURLOPT_HEADERFUNCTION`](https://curl.se/libcurl/c/CURLOPT_HEADERFUNCTION.html)
reports response header blocks, including interim responses, and trailers;
[`CURLOPT_TRAILERFUNCTION`](https://curl.se/libcurl/c/CURLOPT_TRAILERFUNCTION.html)
sends request trailers; and
[`CURLOPT_CONNECT_ONLY`](https://curl.se/libcurl/c/CURLOPT_CONNECT_ONLY.html)
requires a multi-owned handle to remain attached while raw send/receive runs.
The [libcurl WebSocket interface](https://curl.se/libcurl/c/libcurl-ws.html)
does not support extensions, which is why transparent upgrades use a raw
tunnel instead of its frame API.
