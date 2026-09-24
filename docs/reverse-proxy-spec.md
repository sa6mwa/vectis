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
| `auth` or `preflight(in)` | Optional admission decision at headers time. It may proxy or send a local response before any upstream transfer. It sees headers and route metadata only; it cannot consume the body. |
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
Route matching uses Vectis's validated decoded path; forwarding and rewriting
start from a separately retained, validated raw path and raw query. The proxy
must not reconstruct the upstream target from decoded route parameters or
parsed query pairs, which can change escaping or repeated query fields.

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
configured upstream over HTTP or HTTPS, using HTTP/1.1 and negotiating that
version through ALPN for TLS. Preserve the client's WebSocket key, offered
subprotocols, and offered extensions; validate the upstream's `101` status,
upgrade tokens, accept value, and negotiated selections before committing the
downstream `101`. Non-`101` responses follow the ordinary HTTP response
contract and may have a streamed body.

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

## Kore transport integration

Implement one explicit proxy mode in Kore's HTTP connection lifecycle. The
mode owns the accepted request from header admission through normal completion
or raw upgrade. Ordinary Vectis handlers retain their current complete-body
contract. Do not add a second HTTP body parser, route matcher, or outbound
HTTP/TLS client to Vectis.

### Header-time admission and framing

After complete request headers, run the same ordered Vectis route selector
used by ordinary dispatch. It must distinguish a genuine WebSocket upgrade by
validated `Connection` and `Upgrade` tokens, not by path alone, and preserve
the established precedence of ordinary, static, and WebSocket routes. Reject
ambiguous framing and invalid headers before an upstream connection or local
handler starts. Run proxy `auth`/`preflight`/`rewrite` at this boundary; a
locally rejected request follows a defined drain-or-close policy so unread
body bytes cannot become the next request.

Kore currently passes body bytes from the initial socket read to
`http_body_update()` *before* `on_headers`, and dispatches most handlers only
after the body is complete. Move initial body delivery after admission and
retain any bytes beyond that request for the next HTTP/1.x request. Audit the
fixed-length counter before subtracting bytes: one read containing the end of
a body and the start of a pipelined request must neither underflow the length
nor feed the next request to the current sink. This is a required parser fix,
verified with coalesced-read and pipelining tests.

One authoritative incremental HTTP/1.x framing parser feeds a selected body
sink: the existing ordinary buffered/spooled path or the proxy's bounded
stream sink. Both consume fixed length and chunked bodies, framing trailers,
and bodies on any supported method according to framing headers. The sink
interface reports `ACCEPT`, `PAUSE`, or `ERROR`, including how many bytes were
accepted; a partial chunk remains owned by the parser until resumed. A proxy
pause stops application ingress without suppressing write progress, control
events, or TLS progress that requires opposite-direction readiness. Linux's
current whole-fd epoll disable is inadequate; implement independent logical
read/write interest with the corresponding epoll and kqueue behavior. Ordinary
requests must preserve their existing body limits and handler semantics.

`Expect: 100-continue` is resolved at header admission. Do not wait for body
bytes before starting the upstream handshake, and do not let libcurl's default
expect timer decide when the client may send. Relay an upstream `100`, or send
a local `100` under an explicit policy once the upstream is ready; an early
final response suppresses the upload and applies the drain-or-close rule.

### Proxy-owned outbound HTTP transport

Create a proxy-specific libcurl multi transport per Kore worker after fork,
with its own connection pool, socket/timer integration, and easy-handle
lifecycle. This isolates proxy protocol and timeout rules from Kore's current
buffer-oriented curl wrapper, whose completion path removes and frees easy
handles. Use direct header, upload, download, and trailer callbacks. An upload
callback pauses when its bounded queue is empty; a download callback pauses
when the downstream writer reaches its high-water mark. Account for bytes
libcurl may retain while a callback is paused. Resume on actual consumption,
never on a polling timer.

Require a libcurl build with asynchronous DNS capability, or prove equivalent
nonblocking resolution for every configured resolver path before enabling the
proxy. Pin negotiation to HTTP/1.1 and verify the actual connection protocol:
setting `CURLOPT_HTTP_VERSION` alone may permit reuse of a connection opened
with another version. Keep proxy and ordinary curl pools separate. Disable
automatic redirects, auth, decompression, cookies, implicit environment
proxies, and retries. An established SSE transfer is governed by idle and
optional route total timeouts, not Kore's default short curl transfer timeout.

### Downstream response writer

Use a proxy-specific asynchronous response writer attached to the Kore
connection. It owns final/interim header emission, framing, a bounded send
queue, and completion callbacks. It can flush headers and body chunks without
waiting for the upstream transfer to finish. Reuse the existing live stream
bridge's proven send-completion and chunk-framing mechanics where applicable,
but not its synchronous `lc_source->read()` interface: there `0` means EOF,
so an empty queue cannot represent an asynchronous wait. Kore's ordinary
response helper injects connection and content-length behavior and cannot
transparently emit a `101`; the proxy writer must handle `100`, `103`, final
responses, trailers, and `101` with explicit framing rules. A response may
begin before the request body ends. Once final headers are committed, failures
abort the downstream stream rather than attempting a second response.

### Raw WebSocket handoff

For WebSocket, use a proxy-owned connect-only curl easy handle on the proxy
multi to establish the upstream TCP/TLS connection. Retain that handle for
the tunnel lifetime; do not pass it through Kore's normal curl completion
cleanup. Force and verify HTTP/1.1 ALPN before writing the handshake over a
TLS connection. Write and parse the bounded HTTP/1.1 upgrade exchange
explicitly, validate the upstream selection, then hand the accepted Kore
connection to a raw relay after the downstream `101`. A non-`101` reply uses
bounded upstream HTTP/1.1 response framing, including chunked bodies, to
feed the same downstream response writer; it is not treated as tunnel bytes.
Preserve bytes prefetched beyond either handshake. The relay copies bounded
byte chunks in both directions, with independent ingress pause/resume and
TLS-aware readiness; `CURLE_AGAIN` is a wait state, not a fatal error. Kore's
WebSocket message API remains for application WebSockets and is not used by
transparent proxy tunnels.

### Ownership, shutdown, and resource limits

One proxy exchange owns the request, accepted connection, easy handle, queues,
timers, and optional raw tunnel. Cancellation is idempotent; no callback can
refer to the exchange after final cleanup. Specify the state transitions for
headers pending, streaming, half-close, upgrade, cancellation, and worker
shutdown. A downstream disconnect cancels the upstream; an upstream failure
before headers returns a gateway error; a failure after headers closes the
downstream connection. Do not reuse a downstream keepalive connection until
its request body is fully consumed and response framing is complete.

Apply per-worker and per-route limits to accepted and upstream sockets,
headers/trailers, upload bytes, each application queue, curl/TLS retained
buffers, and outstanding exchanges. Connect, no-progress, idle, and optional
total timers have distinct meanings. Intentional backpressure pauses the
corresponding no-progress clock; it does not disable peer liveness or worker
shutdown. Kore currently skips its connection idle timer while an HTTP
request is attached, and ordinary body/minimum-rate timers may conflict with
deliberate pauses, so the proxy owns its stream timeouts explicitly. Stop or
worker exit tears down both legs within the existing graceful-shutdown
deadline.

Kore changes belong in the repository's vendor patch workflow; the upstream
checkout is disposable. The relevant current code is
[`vectis_kore_bridge.c`](../src/vectis_kore_bridge.c),
[`http.c`](../vendor/kore/upstream/src/http.c),
[`connection.c`](../vendor/kore/upstream/src/connection.c),
[`linux.c`](../vendor/kore/upstream/src/linux.c),
[`curl.c`](../vendor/kore/upstream/src/curl.c), and
[`websocket.c`](../vendor/kore/upstream/src/websocket.c).

## Delivery gates and architecture risks

Deliver the transport foundation first: header-time admission, shared
framing/sink semantics, independent readiness, proxy-owned curl multi, and
asynchronous response writer. Prove fixed-length and chunked full-duplex
HTTP streaming under slow peers before adding SSE and raw upgrade. Then add
informational responses and trailers, followed by WebSocket tunneling. These
are implementation milestones, not exemptions from the wire contract above;
the proxy route is complete only when all required cases pass.

Treat three feasibility checks as decision gates before committing to the
full implementation: (1) curl multi and the chosen TLS build support
nonblocking DNS and bounded paused transfers; (2) a retained connect-only
easy handle supports the intended TLS WebSocket relay under the worker event
loop; (3) Kore's Linux and BSD readiness paths can pause one direction while
allowing writes and TLS handshake progress. A failed gate requires revisiting
the transport design or dependency, not substituting a worker thread per
stream, full-body buffer, or hidden spool file.

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
| Policy unit tests | Target/raw-path/raw-query joining and escaping; repeated query fields; Host and forwarding policy; hop-by-hop token removal; duplicate headers; allowed-target selection; malformed framing and handshake rejection; proxy and ordinary route precedence. |
| Parser and readiness integration | Initial read containing headers plus body, exactly at and beyond declared length; pipelined next request in the same read; split and malformed chunk boundaries; conflicting lengths; trailers; pausing midway through a chunk; resume with pending writes; TLS `WANT_READ`/`WANT_WRITE` progress on Linux and BSD. Assert byte ownership and no desynchronization or spin. |
| HTTP integration | GET, HEAD, OPTIONS, POST, PUT, PATCH and error statuses, including framed bodies on normally bodyless methods; fixed-length and chunked uploads; chunked and fixed-length responses; trailers; interim `100`/`103`; `Expect` with delayed and early-final upstream replies; redirects and repeated `Set-Cookie`; TLS termination to both HTTP and verified HTTPS upstreams. Assert upstream receives the expected bytes and metadata. |
| Streaming integration | Upstream emits the first chunk, then waits before finishing; client must receive that chunk before upstream completion. Request chunks must arrive upstream before client EOF. Repeat with slow upstream, slow downstream, simultaneous upload/download, and a response that starts while upload is active. Assert bounded application and libcurl queue depths, active backpressure, and no spool files or full-body allocations. Use multi-gigabyte logical generators in the opt-in stress run. |
| SSE integration | Headers and first event arrive before upstream completion; periodic comments and events arrive at their production cadence; idle timeout behavior is explicit; downstream disconnect cancels upstream promptly. |
| WebSocket integration | Successful `ws`/`wss`, selected subprotocol, offered extension pass-through, fragmented messages larger than Kore's normal frame limit, interleaved ping/pong, close code/reason, non-`101` rejection, and client/server disconnect. Include handshake and first frame in one read on either leg; verify exact byte relay after the handshake, including masking. |
| Failure and lifecycle | DNS/connect/TLS failure, upstream reset before and after headers, malformed upstream headers, slowloris, callback rejection, partial request body, downstream disconnect, worker shutdown, app stop, and connection limits. Assert no orphan transfer, retained curl handle, leaked fd, hanging test process, or accidentally reusable connection with unread request bytes. |
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
buffering. Record DNS/connect time, upstream connection reuse, event-loop
latency, and per-worker file-descriptor headroom so a proxy cost cannot be
hidden in the connection pool or mistaken for scheduler delay.

The hard acceptance criteria are behavioral:

1. The first response chunk or SSE event is observable before the upstream
   finishes; request chunks reach the upstream before client upload EOF.
2. Per-transfer application queue occupancy never exceeds the configured
   chunk budget in either direction. Measure worker memory at fixed
   concurrency while increasing transferred bytes and duration by orders of
   magnitude; the memory envelope must remain flat apart from bounded
   connection, TLS, curl, and header state. Record the measured envelope,
   maximum open descriptors, and their constituent budgets in the
   implementation documentation.
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
[`CURLOPT_HTTP_VERSION`](https://curl.se/libcurl/c/CURLOPT_HTTP_VERSION.html)
does not by itself guarantee that a reused connection uses that version;
[`curl_version_info`](https://curl.se/libcurl/c/curl_version_info.html)
exposes asynchronous DNS capability; and
[`CURLOPT_EXPECT_100_TIMEOUT_MS`](https://curl.se/libcurl/c/CURLOPT_EXPECT_100_TIMEOUT_MS.html)
documents libcurl's fallback expect timer.
The [libcurl WebSocket interface](https://curl.se/libcurl/c/libcurl-ws.html)
does not support extensions, which is why transparent upgrades use a raw
tunnel instead of its frame API.
