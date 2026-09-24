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
| `upstream_http_version` | `auto` by default: prefer HTTP/2 over HTTPS for ordinary HTTP/SSE, with HTTP/1.1 fallback; use HTTP/1.1 for cleartext HTTP and every WebSocket upgrade. `http1` forces HTTP/1.1 for the whole route. Neither mode uses h2c. |
| `auth` or `preflight(in)` | Optional admission decision at headers time. It may proxy or send a local response before any upstream transfer. It sees headers and route metadata only; it cannot consume the body. |
| `rewrite(in, out)` | Optional synchronous, borrowed callback. `in` is immutable inbound metadata; `out` is sanitized mutable outbound metadata. It may select an explicitly configured target, change method/path/query/Host and edit end-to-end headers. |
| `modify_response(response)` | Optional status/header decision after final upstream headers and before downstream headers are committed, for ordinary HTTP/SSE and non-`101` WebSocket rejections. A successful WebSocket `101` bypasses this hook so the validated handshake cannot be altered. The hook does not receive a materialized body. The transport owns framing fields and validates bodyless final statuses. |
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
parsed query pairs, which can change escaping or repeated query fields. Build
the outbound origin-form request target from those validated raw components
and pass it with `CURLOPT_REQUEST_TARGET`; the configured URL still selects
the connection authority and TLS peer. Reject absolute-form targets, fragments,
control characters, and ambiguous escaping before passing that target to
libcurl, which sends it verbatim.

This follows Go's newer `Rewrite(in, out)` model rather than copying the
behavior of its older `Director`: sanitize first, then let application code
modify the outbound request. See the [Go ReverseProxy contract](https://pkg.go.dev/net/http/httputil#ReverseProxy).

## Wire behavior

### HTTP version scope

The proxy accepts downstream HTTP/1.1. The bundled Kore server parser handles
only HTTP/1.0 and HTTP/1.1; the linked nghttp2 library belongs to the libcurl
client dependency and does not add HTTP/2 ingress to Kore. The proxy rejects
downstream HTTP/1.0 as specified below and does not negotiate HTTP/2 on the
inbound listener. For ordinary HTTP and SSE, libcurl prefers HTTP/2 to an
HTTPS upstream through TLS ALPN and falls back to HTTP/1.1 when the upstream
offers only that version. Cleartext upstreams use HTTP/1.1. A route can force
HTTP/1.1. An HTTP/2-only HTTPS upstream is supported for ordinary HTTP/SSE
when the HTTP/2 memory gate below passes; a forced-HTTP/1.1 route fails with
`502` before committing downstream headers.

The supported WebSocket handshake is HTTP/1.1 `Upgrade: websocket` followed
by a bounded raw byte relay. Reject `Upgrade: h2c` at header admission; it
does not create a proxy tunnel. WebSocket over HTTP/2 is a different handshake:
RFC 8441 uses extended `CONNECT` with `:protocol=websocket` on an HTTP/2
stream, not HTTP/1.1 `Upgrade` and `101`. Neither HTTP/2 ingress nor RFC 8441
WebSocket proxying is part of this design.

Disable libcurl HTTP/2 multiplexing for proxy transfers: no connection may
carry two simultaneous exchanges. This trades HTTP/2 connection sharing for
predictable backpressure and must be included in the connection and latency
benchmarks. When one HTTP/2 stream is paused, another
active stream on the same connection would force libcurl to drain and retain
up to a flow-control window of data for the paused stream. Libcurl documents
up to 10 MB with its default window and exposes no public option that sets a
small per-stream window directly. `buffer_limit` caps Vectis-owned queues, not
libcurl's internal HTTP/2 state. Bound the latter with one stream per
connection, measured per-connection memory allowance, and per-worker
connection and memory admission limits. A numerical allowance must be
established against the pinned libcurl build before HTTP/2 is enabled; do not
infer a small-memory guarantee from `CURLOPT_BUFFERSIZE` or a finite HTTP/2
window. This is a measured operational envelope, not a hard per-transfer
allocation cap exposed by libcurl. If a strict small cap is required, or the
slow-consumer test cannot establish an acceptable stable envelope, the route
must use HTTP/1.1 until a transport with controllable HTTP/2 flow control is
available.

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
  environment proxy selection, and HTTP/3/Alt-Svc upgrades. Permit only the
  explicit HTTP/2-to-HTTP/1.1 TLS ALPN fallback described above; do not
  enable libcurl multiplexing or reuse a connection across protocol pools.
- Select libcurl's actual request behavior from the presence of an upload and
  the response-body rule for the method. Set the forwarded method separately;
  `CURLOPT_CUSTOMREQUEST` changes only the wire method string. A framed GET or
  OPTIONS body must therefore have an upload callback, while HEAD must still
  suppress a response body.
- On the upstream leg, forward a known, unchanged request `Content-Length`;
  otherwise use chunked framing on HTTP/1.1 or DATA to end-of-stream on
  HTTP/2, without forwarding `Transfer-Encoding` on that leg. On the
  downstream HTTP/1.1 leg, use chunked framing for every body-bearing response
  in the `auto` pool, even when its connection fell back to HTTP/1.1. This
  keeps the response framing decision independent of a protocol query during
  header callbacks and permits later HTTP/2 trailers after an upstream
  `Content-Length`, even without a `Trailer` declaration. In the forced-HTTP/1.1
  pool, preserve a fixed response `Content-Length` only when upstream framing
  is fixed; otherwise use chunked framing. Never send a body for HEAD
  responses or status codes that forbid one. Preserve a valid upstream
  representation `Content-Length` on HEAD or `304` when HTTP permits it,
  without treating that value as bytes to send; omit forbidden framing on
  `1xx` and `204`.
  Validate any upstream declared length against body bytes even when
  downstream framing is chunked; a mismatch after headers aborts the
  downstream connection.
- Handle `Expect: 100-continue` without buffering the upload. Admit only that
  expectation, remove it on the upstream leg, and suppress libcurl's implicit
  `Expect` header. Emit one local `100` immediately after local
  `auth`/`preflight`/rewrite admission, before connecting upstream; omit it if
  body bytes already arrived. The bounded upload queue applies during upstream
  connection setup. Suppress any later upstream `100`, so the client receives
  at most one. Propagate other informational responses such as `103`. If the
  upstream sends a final response before the upload finishes, stop forwarding
  the upload and close the downstream connection after that response unless
  unread request bytes
  have been safely drained within a fixed bound.
- Support HTTP/1.1 chunked client uploads and request/response trailers.
  Decode inbound chunk framing before forwarding body bytes; send declared
  request trailers with libcurl's trailer callback and force chunked upstream
  HTTP/1.1 framing when trailers are declared. Select the HTTP/1.1 upstream
  pool for such requests at header admission; the public libcurl trailer
  callback is specified for HTTP chunked upload, so HTTP/2 request-trailer
  forwarding cannot be assumed. Relay permitted response trailers after the
  body. Forward a sanitized `Trailer` declaration if the upstream supplied
  one before final headers; a permitted undeclared response trailer is still
  forwarded in the downstream final chunk, since HTTP does not require the
  declaration. Do not report upload EOF to libcurl until the inbound chunk
  parser has validated the complete trailer block. Do not write the downstream final
  chunk until libcurl has delivered and validated upstream trailers.
  Enforce header/trailer count and byte limits. Reject forbidden trailer
  fields and undeclared request trailer fields, since an undeclared request
  trailer cannot be routed to the HTTP/1.1 pool at header admission. A trailer
  violation after headers aborts that connection; do not silently discard
  trailers.
- Preserve downstream connection reuse only when framing and body completion
  are unambiguous. An aborted stream or partially consumed request closes its
  downstream connection. An upstream `101` is valid only for an admitted
  WebSocket upgrade; an unsolicited `101` on an ordinary HTTP/SSE transfer is
  a `502` before downstream commitment.

The proxy route accepts downstream HTTP/1.1 only. Reject HTTP/1.0 at header
admission with `400` and an explanation that HTTP/1.1 is required: HTTP/1.0
cannot carry the required chunked response/trailer contract. Ordinary Kore
routes retain their HTTP/1.0 behavior. Route registration fails if the Kore
runtime is disabled. Unsupported methods and upgrade protocols receive
explicit errors; `CONNECT` is outside this route's scope.

### SSE

SSE is a normal HTTP response with `Content-Type: text/event-stream`. Forward
its bytes unchanged; do not parse events or wait for an entire event. Commit
and flush headers once the upstream final headers arrive, then flush each
available body chunk promptly. An upstream stream with unknown length also
uses immediate flushing. Backpressure may delay delivery when the client is
slow, but it never triggers full-response buffering. Keepalive comments are
ordinary bytes. A client disconnect cancels the upstream transfer.

### WebSocket upgrade

For an upgrade request, reject a framed request body at header admission,
then forward the sanitized opening handshake to the configured upstream over
HTTP or HTTPS using HTTP/1.1. For a connect-only TLS handle, disable ALPN so
another application protocol cannot be negotiated; validate an HTTP/1.1
response status line before upgrading. Preserve the client's WebSocket key,
offered subprotocols, and offered extensions; validate the upstream's `101`
status, upgrade tokens, accept value, and negotiated
selections before committing the downstream `101`. Revalidate GET and the
opening handshake after `rewrite`. Non-`101` responses follow
the ordinary HTTP response contract and may have a streamed body.

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

A Go `net/http` WebSocket backend using an HTTP/1.1 upgrader is compatible
with this route, even if its server also offers HTTP/2 for ordinary requests.
Go's default HTTP/1.x response writer permits connection hijacking; its
HTTP/2 response writer does not. Keep the proxy's upstream WebSocket leg on
HTTP/1.1. When an upgrader checks `Origin` against `Host`, configure the
backend's allowed public origins or explicitly preserve the validated public
Host in the proxy rewrite; the default outbound Host is the target authority.
Do not disable origin checks merely to make the proxy handshake pass.

## Kore transport integration

Implement one explicit proxy mode in Kore's HTTP connection lifecycle. The
mode owns the accepted request from header admission through normal completion
or raw upgrade. Ordinary Vectis handlers retain their current complete-body
contract. Do not add a second inbound HTTP request body parser, route matcher,
or outbound HTTP/TLS client to Vectis.

### Header-time admission and framing

After complete request headers, run the same ordered Vectis route selector
used by ordinary dispatch. It must distinguish a genuine WebSocket upgrade by
validated `Connection` and `Upgrade` tokens, not by path alone, and preserve
the established precedence of ordinary, static, and WebSocket routes. Reject
ambiguous framing, invalid headers, and malformed WebSocket key/version or
subprotocol offers before an upstream connection or local
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
stream sink. The parser recognizes fixed-length and chunked bodies and
trailers independently of the method; the proxy sink accepts framed bodies
on any supported method. The sink interface reports `ACCEPT`, `PAUSE`, or
`ERROR`, including how many bytes were accepted; a partial chunk remains
owned by the parser until resumed. A proxy pause stops application ingress
without suppressing write progress, control
events, or TLS progress that requires opposite-direction readiness. Linux's
current whole-fd epoll disable is inadequate; implement independent logical
read/write interest with the corresponding epoll and kqueue behavior. A
resume on edge-triggered epoll must actively drain already-ready bytes so it
does not wait for a new edge. Ordinary requests must preserve their existing
body limits and handler semantics.

`Expect: 100-continue` is resolved at header admission. Send the local `100`
after policy accepts, without waiting for upstream connection setup; this
avoids depending on a libcurl connection callback and lets the bounded upload
queue absorb initial body chunks. Do not wait for body bytes before connecting
upstream. The proxy suppresses upstream `100` and libcurl's default expect
timer. An early final response suppresses the upload and applies the
drain-or-close rule.

### Proxy-owned outbound HTTP transport

Create proxy-specific libcurl multi transports per Kore worker after fork,
with isolated `auto` and forced-HTTP/1.1 connection pools, socket/timer
integration, and easy-handle lifecycle. Keep HTTP/1.1 WebSocket connect-only
handles in the forced-HTTP/1.1 transport. This isolates proxy protocol and
timeout rules from Kore's current buffer-oriented curl wrapper, whose
completion path removes and frees easy handles. Use direct header, upload,
download, and trailer callbacks. An upload callback pauses when its bounded
queue is empty; a download callback pauses
when the downstream writer reaches its high-water mark. Account for bytes
libcurl may retain while a callback is paused. Resume on actual consumption,
never on a polling timer.

Use libcurl upload mode with a known length or a streamed unknown length when
a framed body is present, then set the validated method string. On HTTP/1.1,
the unknown length uses chunked framing; on HTTP/2, it ends with the DATA
stream. Do not mistake `CURLOPT_CUSTOMREQUEST` for upload or HEAD behavior.
Disable automatic `Expect: 100-continue` generation and own interim-response
timing as above.
Do not configure libcurl to continue sending an upload after an early final
error; cancel that upload and apply the downstream drain-or-close policy.

Require a libcurl build with asynchronous DNS capability, or prove equivalent
nonblocking resolution for every configured resolver path before enabling the
proxy. For the `auto` pool use `CURL_HTTP_VERSION_2TLS`; for the HTTP/1.1 pool
use `CURL_HTTP_VERSION_1_1`. Set `CURLMOPT_PIPELINING` to `CURLPIPE_NOTHING`
and `CURLMOPT_MAX_CONCURRENT_STREAMS` to one; test that no connection carries
concurrent streams, including under reuse. Do not register a server-push
callback. Verify the actual connection protocol: setting
`CURLOPT_HTTP_VERSION` alone may permit reuse of a connection
opened with another version. Keep both proxy pools separate from Kore's
ordinary curl pool. Disable automatic redirects, auth, decompression,
cookies, implicit environment proxies, and retries. An established SSE
transfer is governed by idle and optional route total timeouts, not Kore's
default short curl transfer timeout.

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
abort the downstream stream rather than attempting a second response. Feed
HTTP bytes through Kore's send queue and completion callbacks, with an explicit
queue high-water mark; retain its HSTS policy, access logging, response count,
connection close rules, and request lifetime/accounting. The response hook
cannot set `Content-Length`, `Transfer-Encoding`, `Connection`, or `Trailer`;
the writer computes them after the hook's status decision. For a body-bearing
response in the `auto` pool, commit downstream HTTP/1.1 chunked framing even
when the upstream advertises `Content-Length`, since the upstream may be
HTTP/2 with trailers at end-of-stream. The writer must still validate the
upstream length. A bodyless final status cancels or drains the upstream body
within a fixed bound. The writer holds the terminating chunk until all
upstream trailers arrive; an active SSE stream uses proxy write-progress
limits rather than
Kore's ordinary whole response write deadline.

### Raw WebSocket handoff

For WebSocket, use a proxy-owned connect-only curl easy handle on the proxy
multi to establish the upstream TCP/TLS connection. Retain that handle for
the tunnel lifetime; do not pass it through Kore's normal curl completion
cleanup. Disable ALPN on the raw TLS connection and require an HTTP/1.1
response line; `CURLINFO_HTTP_VERSION` does not establish the protocol of a
connect-only transfer. Write and parse the bounded HTTP/1.1 upgrade exchange
explicitly, validate the upstream selection, then hand the accepted Kore
connection to a raw relay after the downstream `101`. A non-`101` reply uses
an upstream-only incremental HTTP/1.1 response parser, including chunked
bodies, trailers, and interim `1xx` blocks, to feed the same downstream
response writer; it is not treated as tunnel bytes. Preserve bytes prefetched
beyond either handshake under an explicit handoff buffer limit; pause client
reads if an early frame reaches that limit. After
connect-only completion, transfer the upstream fd's readiness ownership from
the libcurl multi socket watcher to a raw watcher; the easy handle remains
attached to the multi for the whole tunnel. Drain `curl_easy_recv` and
`curl_easy_send` within per-event work budgets until `CURLE_AGAIN` or queue
limits, including bytes already decrypted inside TLS.
Arm the raw watcher for pending reads and writes without leaving a writable
fd spinning after a TLS-only `CURLE_AGAIN` with no application-byte progress.
The relay copies bounded byte chunks in both directions with independent
ingress pause/resume. Kore's WebSocket message API remains for application
WebSockets and is not used by transparent proxy tunnels.

### Ownership, shutdown, and resource limits

One proxy exchange owns the request, accepted connection, easy handle, queues,
timers, and optional raw tunnel. Cancellation is idempotent; no callback can
refer to the exchange after final cleanup. Specify the state transitions for
headers pending, streaming, upload complete, upgrade, cancellation, and worker
shutdown. A downstream disconnect cancels the upstream. Before downstream
headers, upstream DNS, connect, TLS, or protocol failure returns `502`; an
upstream deadline returns `504`; proxy resource exhaustion returns `503`.
Preflight rejection uses the status selected by the application.
An error after headers closes the downstream connection. A raw WebSocket leg
reaching EOF flushes only bytes already accepted into its bounded queue up to
the close deadline, then closes both legs; it does not synthesize WebSocket
frames. Do not reuse a downstream keepalive connection until its request body
is fully consumed and response framing is complete.

Apply per-worker and per-route limits to accepted and upstream sockets,
headers/trailers, upload bytes, each application queue, curl/TLS retained
buffers, and outstanding exchanges. Connect, no-progress, idle, and optional
total timers have distinct meanings. Intentional downstream backpressure
pauses the upstream read-progress clock while retaining a downstream
write/idle deadline; an upstream that stops producing bytes while the
downstream is ready remains subject to the upstream idle deadline. Backpressure
does not disable peer liveness or worker shutdown. Kore currently skips its
connection idle timer while an HTTP request is attached, and ordinary
body/minimum-rate timers may conflict with deliberate pauses, so the proxy
owns its stream timeouts explicitly. Stop or worker exit tears down both legs
within the existing graceful-shutdown deadline.

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

Treat six feasibility checks as decision gates before committing to the
full implementation: (1) curl multi and the chosen TLS build support
nonblocking DNS and bounded paused transfers; (2) a retained connect-only
easy handle supports the intended TLS WebSocket relay and single-owner fd
watcher handoff under the worker event loop; (3) Kore's Linux and BSD
readiness paths can pause one direction while
allowing writes and TLS handshake progress; (4) libcurl sends framed GET,
OPTIONS, and HEAD uploads while applying the correct response-body rule,
without blocking or buffering the entire upload; (5) libcurl delivers a
response body while a request upload is still paused or active, and can stop
an early-final upload cleanly; (6) the pinned libcurl build sustains HTTP/2
uploads, downloads, and SSE with multiplexing disabled and a measured stable
memory envelope under paused slow-consumer load. A failed gate requires
revisiting the transport design or dependency, not substituting a worker
thread per stream, full-body
buffer, or hidden spool file.

## Security and operational policy

- Verify upstream TLS certificates and hostnames by default. Use the URL host
  for SNI; allow an explicit CA bundle and client certificate. Never convert
  `https` to `http` after a connection failure. Require TLS 1.2 or newer on
  the HTTP/2-capable upstream pool; libcurl's HTTP/2 version preference alone
  does not enforce the protocol's TLS minimum.
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
| Policy unit tests | Target/raw-path/raw-query joining and escaping, including dot segments and repeated query fields; origin-form validation for `CURLOPT_REQUEST_TARGET`; Host and forwarding policy; hop-by-hop token removal; duplicate headers; allowed-target selection; malformed framing and handshake rejection; proxy and ordinary route precedence. |
| Parser and readiness integration | Initial read containing headers plus body, exactly at and beyond declared length; pipelined next request in the same read; split and malformed chunk boundaries; conflicting lengths; trailers; pausing midway through a chunk; resume without a new epoll edge and with pending writes; TLS `WANT_READ`/`WANT_WRITE` progress on Linux and BSD. Assert byte ownership and no desynchronization or spin. |
| HTTP integration | GET, HEAD, OPTIONS, POST, PUT, PATCH and error statuses, including framed bodies on normally bodyless methods and a `400` for downstream HTTP/1.0; fixed-length and chunked uploads; chunked and fixed-length responses; HEAD/`304` representation length without body bytes; declared request trailers, rejection of undeclared request trailers, and relay of permitted undeclared response trailers, verifying upload EOF and final-chunk ordering; exactly one local `100` even when upstream also sends `100`, bounded upload while upstream connects, `100` followed by `502` on connection failure, upstream `103`, and early-final replies; response hook status/bodyless/framing decisions; redirects and repeated `Set-Cookie`; TLS termination to both HTTP and verified HTTPS upstreams. Assert upstream receives the expected bytes and metadata. |
| HTTP/2 upstream integration | A local HTTPS upstream offers `h2` and `http/1.1`, then `h2` only: ordinary HTTP and SSE negotiate `h2`, while forced-HTTP/1.1 routes and WebSocket handshakes stay on HTTP/1.1 and fail with `502` against an `h2`-only peer. Verify HTTP/1.1 fallback still produces downstream chunked framing in the auto pool, TLS 1.2 minimum, no h2c, no concurrent streams per connection, server push refusal, `:path`/`:authority` rewrites, known/unknown upload length, early response, HEAD, and selected HTTP/1.1 pool for request trailers. Verify an HTTP/2 response with both `Content-Length` and undeclared trailers is sent downstream with chunked framing, intact trailer fields, and declared-length validation. Under many slow downstream readers and concurrent long-lived SSE streams, assert both the per-transfer application queue limit and the separately budgeted libcurl/TLS worker-memory envelope. Repeat with much larger response sizes and durations; memory must not track payload size. |
| Streaming integration | Upstream emits the first chunk, then waits before finishing; client must receive that chunk before upstream completion. Request chunks must arrive upstream before client EOF. Repeat with slow upstream, slow downstream, simultaneous upload/download, and a response that starts while upload is active. Assert bounded application and libcurl queue depths, active backpressure, and no spool files or full-body allocations. Use multi-gigabyte logical generators in the opt-in stress run. |
| SSE integration | Headers and first event arrive before upstream completion; periodic comments and events arrive at their production cadence; idle timeout behavior is explicit; downstream disconnect cancels upstream promptly. |
| WebSocket integration | Successful HTTP/1.1 `ws`/`wss`, selected subprotocol, offered extension pass-through, fragmented messages larger than Kore's normal frame limit, interleaved ping/pong, close code/reason, non-`101` rejection with a streamed body, and client/server disconnect. Assert `modify_response` is bypassed for `101` and applied to non-`101` rejection. Include handshake and first frame in one read on either leg; reject upgrade requests with framed bodies; exceed the bounded pre-upgrade buffer with a paused client; reject `Upgrade: h2c`; test TLS upstream that offers HTTP/2 and verify HTTP/1.1 selection; verify exact byte relay after the handshake, including masking. Test TLS `CURLE_AGAIN` without socket-watcher spin, one readiness owner after connect-only completion, and non-`101` chunked responses with trailers. Run a Go `net/http` upgrader fixture with public `Origin`/rewritten `Host`, subprotocol negotiation, and sustained bidirectional traffic. |
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
readers/writers; and HTTPS/WSS upstreams. Measure HTTPS upstreams using both
HTTP/1.1 and HTTP/2 with multiplexing disabled. Repeat a long-lived soak with
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
   connection, TLS, curl, and header state. For HTTP/2, establish a numerical
   per-connection reserve under worst-case paused transfers and use it to
   admit or reject new exchanges before opening upstream connections. The
   aggregate worker memory and descriptor limits must remain below the
   deployment budget at the configured concurrency. Record the measured
   envelope, maximum open descriptors, and their constituent budgets in the
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
[`CURLMOPT_PIPELINING`](https://curl.se/libcurl/c/CURLMOPT_PIPELINING.html)
disables multiplexing when set to `CURLPIPE_NOTHING`;
[`CURLMOPT_MAX_CONCURRENT_STREAMS`](https://curl.se/libcurl/c/CURLMOPT_MAX_CONCURRENT_STREAMS.html)
caps concurrent HTTP/2 streams per connection; and
[`CURLOPT_BUFFERSIZE`](https://curl.se/libcurl/c/CURLOPT_BUFFERSIZE.html)
controls callback buffer sizing, not the HTTP/2 flow-control window;
[`CURLOPT_HEADERFUNCTION`](https://curl.se/libcurl/c/CURLOPT_HEADERFUNCTION.html)
reports response header blocks, including interim responses, and trailers;
[`CURLOPT_TRAILERFUNCTION`](https://curl.se/libcurl/c/CURLOPT_TRAILERFUNCTION.html)
sends request trailers; and
[`CURLOPT_CONNECT_ONLY`](https://curl.se/libcurl/c/CURLOPT_CONNECT_ONLY.html)
requires a multi-owned handle to remain attached while raw send/receive runs.
[`curl_easy_recv`](https://curl.se/libcurl/c/curl_easy_recv.html) can return
cached TLS bytes without new socket readiness; and
[`curl_easy_send`](https://curl.se/libcurl/c/curl_easy_send.html) can return
`CURLE_AGAIN` after making only internal TLS progress.
[`CURLOPT_HTTP_VERSION`](https://curl.se/libcurl/c/CURLOPT_HTTP_VERSION.html)
does not by itself guarantee that a reused connection uses that version;
[`CURLOPT_CUSTOMREQUEST`](https://curl.se/libcurl/c/CURLOPT_CUSTOMREQUEST.html)
changes only the sent method string, not libcurl's request behavior;
[`CURLOPT_REQUEST_TARGET`](https://curl.se/libcurl/c/CURLOPT_REQUEST_TARGET.html)
sends a caller-supplied request target verbatim;
[`CURLOPT_SSL_ENABLE_ALPN`](https://curl.se/libcurl/c/CURLOPT_SSL_ENABLE_ALPN.html)
can disable ALPN for the connect-only TLS tunnel;
[`CURLINFO_HTTP_VERSION`](https://curl.se/libcurl/c/CURLINFO_HTTP_VERSION.html)
reports the version of an HTTP transfer, not a protocol guarantee for a raw
connect-only socket;
[`CURLOPT_KEEP_SENDING_ON_ERROR`](https://curl.se/libcurl/c/CURLOPT_KEEP_SENDING_ON_ERROR.html)
governs upload behavior after an early upstream error;
[`curl_version_info`](https://curl.se/libcurl/c/curl_version_info.html)
exposes asynchronous DNS capability; and
[`CURLOPT_EXPECT_100_TIMEOUT_MS`](https://curl.se/libcurl/c/CURLOPT_EXPECT_100_TIMEOUT_MS.html)
documents libcurl's fallback expect timer.
The [libcurl WebSocket interface](https://curl.se/libcurl/c/libcurl-ws.html)
does not support extensions, which is why transparent upgrades use a raw
tunnel instead of its frame API.
The distinct HTTP/2 WebSocket handshake is defined by
[RFC 8441](https://www.rfc-editor.org/rfc/rfc8441.html).
Go's [`http.Hijacker`](https://pkg.go.dev/net/http#Hijacker) documents the
HTTP/1.x and HTTP/2 response-writer distinction. The
[`gorilla/websocket` upgrader](https://github.com/gorilla/websocket/blob/main/server.go)
illustrates the HTTP/1.1 handshake and its default origin check.
The downstream bodyless-response and representation-length rules follow
[HTTP Semantics](https://www.rfc-editor.org/rfc/rfc9110.html) and
[HTTP/1.1 framing](https://www.rfc-editor.org/rfc/rfc9112.html). The
[HTTP/2 message format](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.8)
allows a content length and later trailer fields on the same response.
