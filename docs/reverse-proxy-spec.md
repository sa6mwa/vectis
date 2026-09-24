# Reverse Proxy Design and Verification Spec

Status: proposed; this document specifies a future Vectis feature. No proxy
route or transport described here is implemented yet. The pre-body takeover
is a candidate pending the [transport feasibility audit](reverse-proxy-feasibility-audit.md)
and its executable gates.

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
Keep any bytes received after the client's request headers in a bounded
takeover buffer until the upstream `101` passes validation, then relay them
unchanged. Keep any bytes received after the upstream `101` header boundary
in the bounded upstream buffer and relay them after the response headers.
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

Add an optional pre-body takeover point to Kore's header parser. Kore still
parses the request line and headers for every connection. Only a selected
proxy request hands its accepted connection to a proxy-owned handler before
Kore's method-based body path runs. Ordinary Kore body delivery, Vectis's
buffered/spooled or live-upload handlers, and their existing dispatch remain
on the legacy path, with targeted shared byte-boundary fixes for pipelining.
The proxy owns only its request-body framing, response framing, and tunnel
after takeover. Do not add a second request-line/header parser or outbound
HTTP/TLS client to Vectis.

### Audited boundary in the current code

Vectis registers one catch-all Kore route per domain, with
`vectis_kore_body_chunk` and `vectis_kore_request_free`; actual Vectis route
selection occurs in the bridge, often after the body is complete. The bridge
already selects some upload body policy on the first body callback and handles
WebSocket routes before ordinary dispatch. Header-time proxy admission must
therefore be a Vectis selector on that catch-all route, not a new independent
Kore route. The pre-body selector should only take over when a proxy route
wins under the defined Vectis precedence; otherwise it leaves existing
selection and dispatch untouched. Share the same route-matching rules with
later dispatch so two code paths cannot disagree. Keep proxy state separate
from the bridge's current body-state type because its `on_free` hook assumes
that type.

The existing bridge asks for an application WebSocket route before ordinary
dispatch for every GET, regardless of whether `Connection` and `Upgrade`
tokens form a valid upgrade. Preserve that precedence: if an application
WebSocket route matches, return `CONTINUE` and let its current handshake path
produce the response, including malformed-handshake responses. Only after
that check may a proxy route be considered. For remaining requests, resolve
the first matching route in registration order using the same method, path,
parameter, and regex matcher used by body policy. Take over only when that
winner is a proxy route. An ordinary handler, static handler, or live upload
winner continues through its existing path. Extend the internal body-policy
query to report the winning route kind along with its policy; its existing
first-match scan already covers handlers, static routes, and live uploads.
The pre-body hook uses that result after the application WebSocket check.
Ordinary dispatch and upload handling keep their current selection paths.
Evaluate proxy WebSocket mode only for a validated HTTP/1.1 upgrade; a proxy
route may still handle an ordinary GET through its HTTP mode. Return path
validation and allocation errors locally before contacting upstream. Preserve
the existing static directory 405/`Allow` decision when its all-method route
wins an overlap; that check runs before ordinary body handling today.

Exact duplicate method/path-kind/path registrations already conflict, but
literal, parameter, and regex patterns can overlap. Test both registration
orders for overlaps so header-time admission and later route dispatch agree.
The current body-policy return value alone cannot identify a proxy winner:
it contains only policy and a live-upload flag. Extend this internal result
with the winning route kind; do not duplicate its method, path, parameter,
and regex matching loop in the pre-body hook. The bridge already consumes a
selected live-upload route before ordinary handler dispatch. Live tests cover
both registration orders for overlapping buffered and live-upload routes,
plus application WebSocket priority over an earlier ordinary handler. A
proxy-specific overlap and takeover test remains required once the proxy
route kind exists. This adds no further Kore transport surface.

Kore's current request-body behavior is method-based: GET, HEAD, OPTIONS,
COPY, and MOVE are marked complete at request creation; most other methods
require `Content-Length`; there is no incoming chunked decoder. Its
`on_body_chunk` callback has only success/error, with no pause or partial
consume result. Its response stream helper accepts an already available
buffer, while Vectis's generated-response bridge can send bounded chunks and
resume from send-completion callbacks. That bridge's source treats zero bytes
as EOF, so an idle asynchronous SSE producer needs a new wakeup path, not a
call into the synchronous source API.

The minimal Kore surface is an optional pre-body callback, correct ownership
of bounded bytes read beyond headers or a fixed-length body, and a way for
the proxy connection handler to pause application reads while keeping send
and TLS progress. A targeted TLS readiness result may be needed so the proxy
handler knows when a read needs write readiness or a write needs read
readiness. Fixed-length and chunked request-body framing, trailer validation,
curl transfers, response queue policy, and raw tunnel state belong in the proxy
bridge. This intentionally duplicates only a small fixed-length body counter;
Kore's ordinary body delivery stays unchanged apart from the shared
byte-boundary corrections. The handoff and readiness edits still require
non-proxy regression coverage.

### Header-time admission and framing

After complete request headers, invoke the Vectis selector through a new
optional pre-body callback on the catch-all Kore route. Leave Kore's existing
`on_headers` timing and behavior intact for non-proxy requests. The selector
must distinguish a genuine WebSocket upgrade by validated `Connection` and
`Upgrade` tokens, not by path alone, and preserve the established precedence
of ordinary, static, upload, and application WebSocket routes. Reject ambiguous
framing, invalid headers,
and malformed WebSocket key/version or subprotocol offers for a selected proxy
before an upstream connection starts. Run proxy `auth`/`preflight`/`rewrite`
at this boundary; a locally rejected proxy request follows a defined
drain-or-close policy so unread body bytes cannot become the next request.
On `TAKEOVER`, clear Kore's inherited `connection->http_timeout` immediately:
the pre-body hook runs before Kore clears the header timer on its ordinary
header-only path. The proxy then owns connect, idle, write-progress, and
optional total deadlines through cancellable worker timers. A healthy SSE or
WebSocket connection must survive the ordinary header/body timeout.
At `TAKEOVER`, clear the current Kore read event flag after borrowing the
post-header bytes; otherwise `net_recv_flush()` may read EOF in the same
event after a client write half-close and disconnect the connection before
the proxy handler can serve its response. The shared event loop must pass
readable request bytes to Kore before treating an orderly read-half-close as
a disconnect. An actual socket error still cancels immediately.

Kore currently passes initial body bytes to `http_body_update()` before
`on_headers`, and some zero-length paths return before that hook. Insert the
new callback immediately after parsed request headers are recorded and
before method-based completion, `411` for missing `Content-Length`, `413` for
`http_body_max`, or initial body delivery. It returns `CONTINUE`, `TAKEOVER`,
or `REJECT`. `CONTINUE` follows Kore's existing path. `TAKEOVER` passes a
borrowed view of bytes already read beyond the headers, bounded by
`http_header_max`, to the proxy; Kore must not feed those bytes to its body
callback or discard them. The proxy consumes them in order, retaining any
surplus for a pipelined request or early WebSocket frame. `REJECT` sends a
local reply and closes or drains unconsumed input safely.
Keep the request that owns the borrowed receive buffer alive until all of
those bytes have been consumed or copied into bounded transport chunks. The
configured `http_header_max` allocation is part of each admitted exchange's
memory budget; the initial surplus may exceed an 8 KiB proxy queue and must
never be copied wholesale into that queue.

The shared header reader must also preserve bytes after a complete ordinary
header-only request, and the ordinary fixed-length path must consume no more
than its remaining body length before handing a following request back to the
header reader. Those two bounded byte-boundary fixes are required for an
ordinary request followed by a proxy request on a pipelined connection. They
must not change ordinary body limits, spooling, or callback timing.

For a taken-over request, validate all `Content-Length` instances and
`Transfer-Encoding` combinations strictly. Frame its body independently of
method with a proxy-owned fixed-length counter or incremental chunked decoder
and bounded trailer parser. The proxy framer reports consumed bytes and
`ACCEPT`, `PAUSE`, or `ERROR`; a partial input chunk remains owned until
resume. Do not use the existing binary `on_body_chunk` callback as a fake
pause mechanism or apply Kore's ordinary `http_body_max` to a bounded proxy
transfer. Non-proxy framing and body policy remain unchanged except for the
shared byte-boundary corrections above.
For a fixed-length upload, a client write-side close can be reported while
declared body bytes are still available in the socket or TLS buffers. Keep
draining until the declared length is consumed; only a read returning EOF
before that point is a framing error.

After takeover, install a proxy-specific connection event handler. The proxy
reads bounded request chunks directly from Kore's accepted fd or `SSL *`, but
uses Kore's existing send queue for downstream output. It copies at most one
bounded response chunk into `net_send_queue()` and waits for that netbuf to
drain before reading another upstream chunk. No proxy output uses
`net_send_stream()` or its completion callback. Kore retains connection
allocation, TLS handshake, output encryption, event-loop, and teardown
ownership. Drain any earlier Kore send queue before emitting proxy response
bytes. The pre-body hook can run while an earlier stream netbuf's completion
callback is still on the stack. Install the proxy state there, then defer the
first `net_send_flush()` and any proxy output enqueue until the next event;
otherwise a reentrant flush can process the predecessor netbuf twice. Gate
application reads when the upload queue is full and explicitly
drain on resume so an edge-triggered event is not lost. Track read/write
readiness together: Linux's `kore_platform_disable_read()` removes the entire
epoll registration and is appropriate only when neither direction needs a
wakeup. On BSD, Kore registers read and write as separate kqueue filters and
disables each direction independently. Keep one proxy-owned interest mask and
translate it through a small platform adapter that uses Kore's existing event
functions; do not add a new Kore transport hook just to schedule readiness.
The adapter must tolerate separate read and write callbacks from one kqueue
wait cycle and preserve readable bytes reported together with `EV_EOF`.
Re-register when a bounded queue becomes writable again. Handle
`SSL_read` `WANT_READ` and `WANT_WRITE` directly; for queued output, call
`net_send_flush()` and use `SSL_want()` to arm the required retry direction.
When a bounded TLS read leaves decrypted bytes inside OpenSSL, schedule a
continuation after the consumer drains that chunk: `SSL_pending()` bytes do
not necessarily produce another socket readiness notification.
When closing a downstream TLS connection, send `close_notify` after the final
queue drains; hand a reusable connection back to Kore without shutting TLS.
Prove bounded progress and no readiness spin on Linux and BSD. This output
choice remains subject to the
[feasibility audit](reverse-proxy-feasibility-audit.md).

Sleep the taken-over request so Kore's normal complete-body dispatch cannot
run it. Hold its lifetime through streaming and finalize it once after
completion or cancellation. Keep `CONN_IS_BUSY` set while the proxy exchange
is active; restore the ordinary connection handler and resume keepalive only
after both request consumption and response framing are complete. Replay any
bounded pipelined suffix before reading later socket bytes. On upgrade,
transfer state ownership from the request to the connection before freeing
the request.

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

Use a proxy-local readiness adapter for the platform-specific Kore event
calls. On Linux, one epoll registration carries the combined read/write
interest mask; removing it takes one `kore_platform_disable_read()` call.
On kqueue, read and write are independent filters; update or delete each
filter separately. Keep one event object per libcurl socket. A socket removal
must mark that object retired and unregister both filters, but defer freeing
it until after the current Kore event batch. Kqueue can return read and write
results for the same socket in one batch; a second result must find the
retired object and do nothing. Worker teardown drains the retired list after
event processing stops. A taken-over downstream connection likewise ignores
any later batch result after entering Kore's disconnecting state. This is
proxy-owned watcher lifetime management and requires no new generic Kore
event API; native kqueue execution remains a feasibility gate.

Use libcurl upload mode with a known length or a streamed unknown length when
a framed body is present, then set the validated method string. On HTTP/1.1,
the unknown length uses chunked framing; on HTTP/2, it ends with the DATA
stream. Do not mistake `CURLOPT_CUSTOMREQUEST` for upload or HEAD behavior.
Disable automatic `Expect: 100-continue` generation and own interim-response
timing as above.
Do not configure libcurl to continue sending an upload after an early final
error; cancel that upload and apply the downstream drain-or-close policy.
For HTTP/2, removing a live easy handle must cancel its stream promptly.
Libcurl can retain the underlying TLS connection after an `RST_STREAM`.
Because this design disables multiplexing, mark a downstream-canceled
HTTP/2 connection non-reusable before removing its easy handle. A peer may
observe `RST_STREAM` or TCP closure; do not wait for socket EOF to retire the
exchange. Allow reuse after a normally completed response, and account for
those retained idle connections in the worker budget until the multi closes
them. Validate close-on-cancel against the pinned libcurl build rather than
assuming that setting `CURLOPT_FORBID_REUSE` during cancellation has an
unconditional public API guarantee.

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

Set explicit `CURLMOPT_MAX_TOTAL_CONNECTIONS` and `CURLMOPT_MAXCONNECTS`
on each proxy pool. The former limits open connections but [queues extra
easy handles inside libcurl](https://curl.se/libcurl/c/CURLMOPT_MAX_TOTAL_CONNECTIONS.html);
the latter limits idle cached connections, whose [default capacity grows
with added easy handles](https://curl.se/libcurl/c/CURLMOPT_MAXCONNECTS.html).
Reserve an exchange and its connection/memory allowance in Vectis before
adding its easy handle to the multi; reject saturation with `503` at header
admission instead of building an unbounded libcurl waiting queue. Count
retained connect-only WebSocket handles, both protocol pools, and idle cached
connections against the worker budget. Release exchange reservations only
after their easy handles and owned buffers are retired. Choose the numerical
limits from the pinned-bundle memory gate; the sixteen-connection probe does
not establish a release-wide allowance.

### Downstream response writer

Use a proxy-owned asynchronous response controller attached to the Kore
connection. It copies one bounded chunk at a time into Kore's send queue
after any preceding response drains, then calls `net_send_flush()` on
readiness. The source pauses while that single chunk is pending and resumes
when the queue empties. The application scratch buffer and Kore's copied
netbuf are both counted in the per-exchange memory allowance. An empty
upstream queue means wait for a producer wakeup; it is not EOF. Emit proxy
response headers explicitly, including interim and WebSocket `101` headers.
The ordinary `http_response()` helper injects `Content-Length` and connection
fields and may synthesize a pretty error body for an upstream `4xx` or `5xx`,
so it cannot preserve the upstream
response contract reliably. Explicit emission must preserve Kore's HSTS,
response count, access logging, and close behavior. The controller handles
chunk framing, trailers, producer wakeup, and cancellation without requiring
a new generic Kore response API. A response may
begin before the request body ends. Once final headers are committed, failures
abort the downstream stream rather than attempting a second response. Write
HTTP bytes through the bounded Kore queue. The response hook
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
The pinned libcurl OpenSSL backend can return `CURLE_AGAIN` from
`curl_easy_send()` when `SSL_write()` wants a **read**, and from
`curl_easy_recv()` when `SSL_read()` wants a **write**. The public connect-only
API does not report that retry direction. While either raw operation is
pending after `CURLE_AGAIN`, arm both socket directions, preserve its input
buffer and offset, and avoid re-registering an unchanged edge-triggered mask
on every callback. After a readiness callback with no application progress,
use a bounded, cancellable delayed retry so an internal TLS transition with
no further socket edge cannot stall forever; measure its wakeup rate under an
idle tunnel and a forced cross-direction retry. Never use a continuous
level-triggered writable watcher as that retry mechanism. This raw-tunnel
policy requires an executable probe before architecture commitment. A
downstream reset can arrive as an event error or as `ECONNRESET` on the next
raw read; both paths must cancel the easy handle and delayed retry timer
before the exchange is freed.
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
[`net.c`](../vendor/kore/upstream/src/net.c),
[`linux.c`](../vendor/kore/upstream/src/linux.c),
[`bsd.c`](../vendor/kore/upstream/src/bsd.c),
[`tls_openssl.c`](../vendor/kore/upstream/src/tls_openssl.c),
[`worker.c`](../vendor/kore/upstream/src/worker.c),
[`curl.c`](../vendor/kore/upstream/src/curl.c), and
[`websocket.c`](../vendor/kore/upstream/src/websocket.c).

## Delivery gates and architecture risks

Deliver the transport foundation first: pre-body takeover, proxy-owned body
framing, read gating with TLS progress, proxy-owned curl multi, and
one-chunk-at-a-time Kore response output. Prove full-duplex
fixed-length and chunked HTTP streaming under slow peers before adding SSE
and raw upgrade. Then add
informational responses and trailers, followed by WebSocket tunneling. These
are implementation milestones, not exemptions from the wire contract above;
the proxy route is complete only when all required cases pass.

Treat these as feasibility gates before committing to the full implementation:

1. The catch-all route's optional pre-body callback selects a proxy for
   bodyless, zero-length, and body-bearing requests while `CONTINUE` preserves
   ordinary, static, upload, and application WebSocket behavior.
2. Kore can hand the bounded initial receive surplus to the proxy without
   duplicating request-line/header parsing or losing pipelined and early
   WebSocket bytes. Ordinary header-only and fixed-length requests followed
   by proxy requests in one read retain exact byte boundaries. Proxy-only
   pause and resume preserve downstream TLS progress on Linux and BSD, with
   no busy loop when both legs are half-closed or one queue is full.
3. Curl multi and the chosen TLS build provide nonblocking DNS and bounded
   paused transfers.
4. A retained connect-only easy handle supports the TLS WebSocket relay and
   single-owner fd watcher handoff under the worker event loop.
5. Libcurl sends framed GET, OPTIONS, and HEAD uploads while applying correct
   response-body rules, without blocking or buffering the entire upload.
6. Libcurl delivers a response body while request upload is paused or active
   and can stop an early-final upload cleanly.
7. The pinned libcurl build sustains HTTP/2 uploads, downloads, and SSE with
   multiplexing disabled and a measured stable memory envelope under paused
   slow-consumer load.

A failed gate requires revisiting the transport design or dependency, not
substituting a worker thread per stream, full-body buffer, or hidden spool
file.

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
| Policy unit tests | Target/raw-path/raw-query joining and escaping, including dot segments and repeated query fields; origin-form validation for `CURLOPT_REQUEST_TARGET`; Host and forwarding policy; hop-by-hop token removal; duplicate headers; allowed-target selection; malformed proxy framing and handshake rejection; pre-body proxy selection agreeing with later non-proxy dispatch. Cover both registration orders for proxy literal versus ordinary regex, proxy regex versus static or live upload, and an application WebSocket route overlapping a proxy route. The application WebSocket route must keep its current priority for GET with valid, missing, or malformed upgrade headers; exact duplicate route registration must still fail. |
| Parser and readiness integration | For a proxy takeover, initial read containing headers plus body, bodyless or `Content-Length: 0` request followed by another request, exactly and beyond declared length, and an early WebSocket frame; split and malformed chunk boundaries; duplicate/conflicting lengths and `Content-Length`/`Transfer-Encoding` ambiguity; trailers; pausing midway through a chunk; resume without a new epoll edge and with pending writes; downstream TLS `WANT_READ`/`WANT_WRITE` progress on Linux and BSD. For `CONTINUE`, compare ordinary route, live-upload, and application WebSocket behavior to existing fixtures, including current body limits and dispatch timing; also send ordinary header-only and fixed-length requests immediately followed by a proxy request in one read. Assert byte ownership, exactly one dispatch per request, and no desynchronization, spin, or premature next-request read. |
| HTTP integration | GET, HEAD, OPTIONS, POST, PUT, PATCH and error statuses, including framed bodies on normally bodyless methods and a `400` for downstream HTTP/1.0; fixed-length and chunked uploads; chunked and fixed-length responses; HEAD/`304` representation length without body bytes; declared request trailers, rejection of undeclared request trailers, and relay of permitted undeclared response trailers, verifying upload EOF and final-chunk ordering; exactly one local `100` even when upstream also sends `100`, bounded upload while upstream connects, `100` followed by `502` on connection failure, upstream `103`, and early-final replies; response hook status/bodyless/framing decisions; exact upstream `4xx`/`5xx` status, headers, and body with Kore pretty errors enabled; redirects and repeated `Set-Cookie`; TLS termination to both HTTP and verified HTTPS upstreams. Assert upstream receives the expected bytes and metadata. |
| HTTP/2 upstream integration | A local HTTPS upstream offers `h2` and `http/1.1`, then `h2` only: ordinary HTTP and SSE negotiate `h2`, while forced-HTTP/1.1 routes and WebSocket handshakes stay on HTTP/1.1 and fail with `502` against an `h2`-only peer. Verify HTTP/1.1 fallback still produces downstream chunked framing in the auto pool, TLS 1.2 minimum, no h2c, no concurrent streams per connection, server push refusal, `:path`/`:authority` rewrites, known/unknown upload length, early response, HEAD, and selected HTTP/1.1 pool for request trailers. Verify an HTTP/2 response with both `Content-Length` and undeclared trailers is sent downstream with chunked framing, intact trailer fields, and declared-length validation. Under many slow downstream readers and concurrent long-lived SSE streams, assert both the per-transfer application queue limit and the separately budgeted libcurl/TLS worker-memory envelope. At the configured active and idle limits, send one extra request and assert immediate `503`, no easy handle queued in libcurl, and bounded memory across both protocol pools and retained WebSocket tunnels. Repeat with much larger response sizes and durations; memory must not track payload size. |
| Streaming integration | Upstream emits the first chunk, then waits before finishing; client must receive that chunk before upstream completion. Request chunks must arrive upstream before client EOF. Repeat with slow upstream, slow downstream, simultaneous upload/download, and a response that starts while upload is active. Assert bounded application and libcurl queue depths, active backpressure, and no spool files or full-body allocations. Use multi-gigabyte logical generators in the opt-in stress run. |
| SSE integration | Headers and first event arrive before upstream completion; periodic comments and events arrive at their production cadence; idle timeout behavior is explicit; downstream reset during a producer idle period cancels upstream promptly without a new upstream event. A client write half-close after headers still permits response completion. Test a half-close coalesced with request headers separately at the pre-body boundary. Assert no writable or `RDHUP` busy loop. |
| WebSocket integration | Successful HTTP/1.1 `ws`/`wss`, selected subprotocol, offered extension pass-through, fragmented messages larger than Kore's normal frame limit, interleaved ping/pong, close code/reason, non-`101` rejection with a streamed body, and client/server disconnect. Assert `modify_response` is bypassed for `101` and applied to non-`101` rejection. Include handshake and first frame in one read on either leg; reject upgrade requests with framed bodies; exceed the bounded pre-upgrade buffer with a paused client; reject `Upgrade: h2c`; test TLS upstream that offers HTTP/2 and verify HTTP/1.1 selection; verify exact byte relay after the handshake, including masking. Test TLS `CURLE_AGAIN` without socket-watcher spin, one readiness owner after connect-only completion, and non-`101` chunked responses with trailers. Run a Go `net/http` upgrader fixture with public `Origin`/rewritten `Host`, subprotocol negotiation, and sustained bidirectional traffic. |
| Failure and lifecycle | DNS/connect/TLS failure, upstream reset before and after headers, malformed upstream headers, slowloris, callback rejection, partial request body, downstream reset while idle or with a queued response write, downstream TLS close, worker shutdown, app stop, and connection limits. Before any downstream response byte is committed, send a complete local `502` for upstream transport failure; after commitment, end the downstream stream without a successful chunk terminator. Cover the case where Kore has queued a response but has not written any byte, and assert exactly one request/connection teardown, no orphan transfer, retained curl handle, leaked fd, hanging test process, or accidentally reusable connection with unread request bytes. |
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
