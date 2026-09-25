# Reverse Proxy Design and Verification Spec

Status: architecture selected for implementation, 2026-09-25. Route
registration, target construction, worker curl integration, and bounded live
HTTP/SSE request and response streaming are implemented. A successful
HTTP/1.1 WebSocket upgrade now uses a retained connect-only transfer and a
bounded raw relay; a cleartext live test covers coalesced handshake/frame bytes
and 1 MiB streaming in both directions. Non-`101` replies now use an
incremental HTTP/1.1 rejection path; a cleartext live test covers a 1 MiB
fixed-length rejection that starts before the origin finishes sending, chunked
trailers, and `103` followed by a close-delimited final response. A verified
local WSS route now relays 128 KiB frames in both directions, rejects an
untrusted peer, and shares its optional CA bundle with ordinary HTTPS. TLS
retry, backpressure, and shutdown cases need production-route tests.
The C and Lua request rewrite hooks now select a configured target and edit
method, raw path/query, Host, and bounded end-to-end headers before either
upstream transport starts. C and Lua final-response hooks edit downstream
status and bounded end-to-end headers before commitment for HTTP, SSE, and
non-`101` WebSocket rejections; successful upgrades bypass the hook. C and Lua
preflight hooks now answer at headers time, and gateway-error hooks can answer
before HTTP or WebSocket response headers are sent. Admission failures outside
preflight still use fixed local responses; extending the error hook to every
uncommitted failure remains open. A Kore send-queue test covers the replacement
decision and local reply bytes before any write attempt. A Linux production-route
test holds the queued upstream 200 without writing it, closes the upstream,
and verifies a complete 502 on the wire and a single 5xx metric.

HTTP/SSE now enforces the route's no-progress idle deadline: a stalled
upstream receives a local `504` before commitment, while an idle committed
stream closes without a final success chunk. A live SSE fixture also stays
open beyond the deadline when periodic events make progress. A provisional
shared 16-exchange cap now rejects excess HTTP and WebSocket requests with
`503` at header admission, before allocating proxy exchange buffers; a live
test holds sixteen streams and verifies both rejections without an extra
upstream connection. A pinned Release production-route HTTP/2 slow-reader
smoke now measures worker RSS and descriptors at both the minimum and maximum
route chunk settings. The mixed-protocol aggregate worker allowance remains
open.

The [transport feasibility audit](reverse-proxy-feasibility-audit.md) records the
evidence behind this choice and the checks required during implementation.

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

The installed libvectis C headers and the proxy route API must compile under
strict C89. Kore and libcurl types stay out of the public proxy API; their
language mode must not leak into installed headers or consumer compile flags.
The installed SDK consumer is a C89 compilation and link gate for this API.
The private vendored Kore runtime uses GNU99; the embedded CLI and dependency
modules use C99. Project-owned `libvectis` sources compile in strict C89 mode
except for the private Kore bridge translation unit, which includes Kore's C99
headers and is compiled as GNU99. This source-level exception does not impose
its language mode on installed headers or downstream consumers.

The C registration is `app->proxy_route(app, &config, &error)` with a
corresponding Lua `app:proxy(opts)`. The current route configuration fields and
planned callback phases have the following contract:

| Field or hook | Contract |
| --- | --- |
| `path`, `methods`, `path_kind` | Use Vectis route matching and ordering. WebSocket upgrade requires GET. |
| `target` | Required configured `http://` or `https://` base URL. Its scheme and authority are fixed for the route. |
| `upstream_http_version` | `auto` by default: prefer HTTP/2 over HTTPS for ordinary HTTP/SSE, with HTTP/1.1 fallback; use HTTP/1.1 for cleartext HTTP and every WebSocket upgrade. `http1` forces HTTP/1.1 for the whole route. Neither mode uses h2c. |
| `tls_ca_pem` | Optional copied PEM CA bundle for HTTPS and WSS origin verification. Omit it to use libcurl's default trust store. The bundle replaces that store for the route; peer and hostname checks stay enabled. Limit: 256 KiB. |
| `tls_client_cert_pem`, `tls_client_key_pem` | Optional copied PEM client certificate chain and private key for HTTPS and WSS upstream authentication. Set both or neither; each is limited to 256 KiB. They are passed to libcurl from memory without staging files. |
| `preflight(in)` | Optional admission decision at headers time, including authentication policy. It may proxy or send a local response before any upstream transfer. It sees headers and route metadata only; it cannot consume the body. |
| `rewrite(in, out)` | Optional synchronous, borrowed callback. `in` is immutable inbound metadata; `out` is sanitized mutable outbound metadata. It may select an explicitly configured target, change method/path/query/Host and edit end-to-end headers. |
| `modify_response(response)` | Optional status/header decision after final upstream headers and before downstream headers are committed, for ordinary HTTP/SSE and non-`101` WebSocket rejections. A successful WebSocket `101` bypasses this hook so the validated handshake cannot be altered. The hook does not receive a materialized body. The transport owns framing fields and validates bodyless final statuses. |
| `on_error(error)` | Optional local error response while headers are uncommitted. It receives a borrowed failure cause and default 502/504 status. Later errors abort the stream and are logged. |
| `connect_timeout`, `idle_timeout`, `total_timeout`, `buffer_limit` | Explicit per-route resource policy. Total timeout defaults to disabled for an established SSE or WebSocket stream; connect and idle limits remain active. |

For C, `preflight` and `on_error` receive an opaque local-response builder.
`vectis_proxy_local_respond()` sets a final status and copies at most 64 KiB of
body; `vectis_proxy_local_add_header()` copies validated end-to-end headers,
including repeated `Set-Cookie`. A preflight callback that leaves the status
unset forwards the request. An error callback that leaves it unset uses the
default gateway response. Invalid preflight edits produce local `500`; invalid
error-hook edits fall back to the default `502` or `504`. Every local response
closes the downstream connection after its body, including when the request
body is unread. Proxied bodies and WebSocket messages remain chunk streamed.
The installed C API stays strict C89.

Lua `preflight(in)` returns `nil` or `true` to forward, or a table such as
`{status=401, body="denied", headers={{name="Set-Cookie", value="a=1"}}}` to
answer locally. Lua `on_error(failure)` receives `status`, `code`, and
`message` fields and returns the same response table shape, or `nil`/`true`
for the default gateway response. Lua body strings may contain NUL; the same
64 KiB limit applies. Callback errors in preflight produce `500`; callback
errors in `on_error` use the default gateway response.

The normal flow is: match route; copy and validate inbound metadata; run
`preflight` for admission; sanitize outbound metadata; apply default target/path
rewrite; run `rewrite`;
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
For paths accepted by the existing Vectis decoder, route matching uses its
validated decoded path; forwarding and rewriting start from a separately
retained, validated raw path and raw query. The proxy must not reconstruct the
upstream target from decoded route parameters or parsed query pairs, which
can change escaping or repeated query fields. Build
the outbound origin-form request target from those validated raw components
and pass it with `CURLOPT_REQUEST_TARGET`; the configured URL still selects
the connection authority and TLS peer. Reject absolute-form targets, fragments,
control characters, and ambiguous escaping before passing that target to
libcurl, which sends it verbatim.

This route-selection rule has an unresolved feasibility gate. The current
Vectis decoder rejects escaped slashes, and its ordinary path validator can
reject other escaped forms before route matching. Kore's pre-body hook retains
the raw bytes. The candidate selector first uses the existing decoded-path
selection, including the static-site trailing-slash exception and static
`405` check. Only when ordinary decoding fails, or the existing selector
rejects the decoded path as invalid with no static-site exception, may it
validate the raw path separately and scan proxy routes in registration order.
A raw path that passes this second check must have origin-form syntax, valid
percent triplets, no raw or encoded controls or backslash, and no raw or
once-decoded dot segments. Reject a second percent triplet that appears only
after one decoding pass, so a backend cannot turn `%252e` or `%252f` into a
hidden traversal or separator. Malformed or unsafe paths must fail locally
before `preflight`, `rewrite`, or an upstream connection.
If no proxy route matches, preserve the ordinary rejection. The raw fallback
must never make an ordinary, upload, static, or application WebSocket route
handle a path it currently rejects.

The raw fallback is a distinct public matching case: an encoded slash stays
inside one raw segment, and a proxy parameter captures its escaped spelling.
Forwarding still uses the original raw target, not that parameter. A proxy
literal pattern cannot contain percent escapes under current route
registration rules, so escaped-path fallback depends on a parameter or regex
pattern. Tests must cover encoded percent and colon, mixed-case escapes, and
overlaps with every existing route kind. Public proxy registration and
production admission now use this fallback. Decoded-path proxy matches also
validate the original raw path before
`preflight`, so a raw fragment or control byte cannot bypass the proxy path
policy. Selector unit tests cover mixed-case escapes, escaped slash/percent/
colon, raw colon, dot segments, malformed escapes, and double-escape attempts;
the production listener rejects a raw fragment before invoking `preflight`.
The remaining route-overlap combinations still need live verification.

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
The HTTP/2-capable pool explicitly requires TLS 1.2 or newer, including when
ALPN falls back to HTTP/1.1. In `vectis_unit_proxy_curl_tls_floor`, a local
TLS 1.1 origin accepts both a forced-HTTP/1.1 request and an HTTP/2-preference
control request without the floor, then rejects the configured HTTP/2-capable
request.

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
  `1xx` and `204`. Treat [`205` as a no-content final status](https://www.rfc-editor.org/rfc/rfc9110.html#section-15.3.6):
  accept a zero declared length or empty chunked framing, reject nonzero
  content, and do not carry a body into a rewritten `205` response.
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
continuation frames. The pinned libcurl raw WebSocket mode passes opaque frames
with a caller-supplied key and extension header, but reports an upstream
non-`101` as `CURLE_HTTP_RETURNED_ERROR` without delivering its response body;
it also accepts a `101` with an invalid `Sec-WebSocket-Accept`. Its documented
WebSocket API does not support extension negotiation. The tunnel therefore
uses curl only to establish a raw TCP/TLS upstream connection, then sends and
reads the opening HTTP handshake and tunneled bytes through curl's connect-only
send/receive interface. The proxy validates the `101` itself and incrementally
parses and streams a non-`101` HTTP response. No blocking `curl_easy_perform()`
call runs in a Kore worker.

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

### Vendored Kore boundary

`vendor/kore/upstream/` is a disposable checkout. Every required change to
Kore itself belongs in a focused numbered patch in `vendor/kore/patches/series`;
the existing pipelining, pre-body hook, half-close, Host matching, and header
validation patches provide the current transport boundary. Keep the proxy
route, director and response hooks, framing, bounded queues, libcurl multi
integration, readiness adapter, and WebSocket tunnel in Vectis-owned source.
Use Kore's existing platform event calls from that adapter, including separate
read and write filter removal on kqueue. Add another Kore patch only when an
essential behavior cannot be expressed through the existing hook and event
surface, and cover ordinary non-proxy behavior with a regression test. Verify
the series by applying it to a clean pinned checkout with
`make verify-kore-patches`; never treat edits to the generated upstream
checkout as the source of truth.

Keep the Vectis implementation split along its ownership boundaries:

| Vectis source | Responsibility |
| --- | --- |
| `vectis_proxy_route.c` | Route configuration, target policy, and synchronous application hooks. |
| `vectis_proxy_select.c` | Header-time route precedence and strict proxy-only raw-path fallback. |
| `vectis_proxy_url.c` | Validated origin-form target and authority construction from raw request metadata. |
| `vectis_proxy_headers.c` | Inbound framing and header validation, hop-by-hop sanitization, forwarding metadata, and outbound request metadata. |
| `vectis_proxy_framing.c` | Bounded incremental body and trailer framing, with explicit consumed-byte and pause results. |
| `vectis_proxy_curl.c` | Per-worker libcurl multi pools, socket/timer readiness, admission, and easy-handle lifetime. |
| `vectis_proxy_events.c` | Linux and BSD readiness translation shared by curl sockets and taken-over connections. |
| `vectis_proxy_http.c` | Incremental upstream response status, header, body-length, and trailer validation. |
| `vectis_proxy_http_upstream.c` | Libcurl HTTP callbacks, one bounded pending download chunk, and pause/resume flow control. Upload callbacks still need production integration. |
| `vectis_proxy_ws_handshake.c`, `vectis_proxy_ws_wire.c` | HTTP/1.1 WebSocket handshake validation and bounded opening wire blocks. |
| `vectis_proxy_ws_rejection.c` | Incremental non-`101` response framing, body and trailer validation, and bounded downstream chunks. |
| `vectis_kore_proxy_ws.c` | Connect-only WebSocket lifecycle, Kore readiness, and opaque duplex relay. |
| `vectis_kore_proxy.c` | Header-time route selection, accepted-connection takeover, Kore send queue, TLS readiness, and connection restoration. |

Share private state through small `src/` headers. Keep Kore-specific types out
of the public API and avoid copying the test-only transport probes into one
production file.

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
tokens form a valid upgrade. Preserve that precedence for a valid ordinary
decoded path: if an application WebSocket route matches, return `CONTINUE`
and let its current handshake path produce the response, including
malformed-handshake responses. Validate the decoded path separately before
this lookup. If it is invalid, skip application WebSocket matching and let
the strict proxy raw-path fallback decide admission; do not treat an error
from a valid-path WebSocket lookup as fallback eligibility. For remaining
requests, resolve the first matching route in registration order using the
same method, path, parameter, and regex matcher used by body policy.
Represent a proxy route in
the existing handler registry with a private marker handler and route-owned
proxy configuration. Take over only when that marker wins. An ordinary
handler, static handler, or live upload winner continues through its existing
path. The internal body-policy query now reports the winning handler and
userdata pointers along with its policy; its existing first-match scan covers
handlers, static routes, and live uploads. The marker must return an explicit
error if normal dispatch reaches it, rather than accidentally serving a
buffered response.
The pre-body hook uses that result after the application WebSocket check.
Ordinary dispatch and upload handling keep their current selection paths.
The body-policy query now accepts an optional request in which it retains the
winning route's captured parameters for `preflight` and `rewrite`. Existing
callers can continue using its scratch request. The production pre-body
selector must pass an exchange-owned request and preserve it through both
hooks without repeating route matching.
Evaluate proxy WebSocket mode only for a validated HTTP/1.1 upgrade; a proxy
route may still handle an ordinary GET through its HTTP mode. Return path
validation and allocation errors locally before contacting upstream. Preserve
the existing static directory 405/`Allow` decision when its all-method route
wins an overlap; that check runs before ordinary body handling today.

Exact duplicate method/path-kind/path registrations already conflict, but
literal, parameter, and regex patterns can overlap. Test both registration
orders for overlaps so header-time admission and later route dispatch agree.
The raw fallback is eligible only after ordinary decoding fails or the
ordinary selector rejects the decoded path as invalid. A decoded path served
through the static-site trailing-slash exception stays on its current path.
Do not use the fallback merely because no ordinary route matched: a proxy
regex could acquire a valid decoded path that currently returns `404`.
This keeps normal-path precedence and static `405` intact.
The live pre-body probe now confirms that an earlier static directory serves
its file and produces `405` with `Allow` for POST over cleartext and TLS;
an escaped path under its prefix can still enter proxy-only raw fallback.
The selected handler pointer identifies a private proxy marker without a new
route kind or a duplicate normal-path matcher. Its userdata pointer identifies
the route-owned proxy configuration without another matching pass. Takeover
must retain that configuration until the exchange is destroyed, including
worker shutdown. The raw fallback validates the raw path and filters the same
registry to marker routes, using its existing method, parameter, and regex
matcher, and returns the matched userdata. The bridge already consumes a
selected live-upload route before ordinary handler dispatch. Live tests cover
both registration orders for overlapping buffered and live-upload routes,
plus application WebSocket priority over an earlier ordinary handler. A live
marker probe covers ordinary regex overlap in both registration orders and
escaped-path fallback over cleartext and TLS. It also covers WebSocket
priority, static file and `405` behavior, and both registration orders for
proxy-marker versus live-upload POST routes with a live body callback. The
production pre-body path now calls the shared selector and validates complete
request-header framing before proxy takeover. This adds no further Kore
transport surface.

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
must receive the complete parsed header list. Patch `0033` rejects header
blocks that exceed Kore's split array, contain an embedded NUL, or contain a
bare CR before the destructive split; this rejection applies to ordinary
requests too. The selector
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
event API. Native kqueue execution is an implementation verification gate.

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

The Linux production-route HTTP/2 memory smoke uses sixteen independent,
certificate-verified TLS connections to a local `h2` origin, one Vectis worker,
and downstream clients that stop reading after response headers. Each origin
offers a 64 MiB response, or 1 GiB total. The test samples worker RSS every
20 ms for four seconds, checks the later two-second interval for stable RSS
and limited upstream production, verifies a seventeenth request receives
`503` without opening an origin connection, then checks worker FD recovery.
It runs with both 8 KiB and 1 MiB route chunk limits.

| Pinned x86-64 Linux Release run, 2026-09-25 | 8 KiB chunks | 1 MiB chunks |
| --- | ---: | ---: |
| Worker RSS before first upstream connection | 7,408 KiB | 7,328 KiB |
| Sampled peak with sixteen exchanges | 16,740 KiB | 37,344 KiB |
| Worker RSS after closing clients | 16,492 KiB | 22,192 KiB |
| Worker FDs before / during / after | 14 / 48 / 16 | 14 / 48 / 16 |
| Origin bytes produced at four seconds | 87,228,416 | 96,092,160 |

These are observed values for this GET/SSE profile, not a libcurl allocator
bound. The executable regression ceiling is 64 MiB of aggregate additional
worker RSS for sixteen exchanges, equivalent to 4 MiB per exchange, plus no
more than 4 MiB growth after the first two seconds. It does not measure each
connection's allocation separately. The ASan build uses a 256 MiB aggregate
RSS ceiling for instrumentation redzones and quarantine while retaining the
same streaming, plateau, and teardown checks.
The Linux production-route WebSocket smoke holds sixteen HTTP/1.1 upgraded
connections with 1 MiB route chunk limits and idle clients for four seconds.
The pinned x86-64 Linux Release run measured 8,384 KiB worker RSS before the
first connection, 9,908 KiB at the handshakes and sampled peak, and 9,844 KiB
after teardown. Worker FDs were 14 / 48 / 16 before, during, and after. A
seventeenth HTTP request and a seventeenth WebSocket handshake each received
`503` without reaching the origin; after closing the sixteen tunnels, a new
WebSocket handshake succeeded. These measurements cover idle retained handles.

The same production test also sends one 4 MiB WebSocket frame from each of
the sixteen origins into a client with a small receive buffer that stops
reading after the handshake. All 64 MiB were accepted by the origin sockets;
the test samples worker RSS for four seconds, checks for a plateau after
two seconds, rejects both overflow request types, and verifies FD recovery
and slot reuse. In the pinned x86-64 Linux Release run, the worker measured
8,432 KiB before the first connection, 25,312 KiB at the sampled peak and
later plateau, and 18,144 KiB after teardown. The executable ceiling is
64 MiB of additional worker RSS for sixteen tunnels, with no more than
4 MiB growth after warmup; the ASan ceiling is 256 MiB. This is an aggregate
slow-reader test, not a measurement of the exact occupancy of each relay
buffer or a bound for every active WebSocket workload.

A separate bidirectional variant sends a masked 4 MiB client frame on each
of the sixteen tunnels while every origin also sends a 4 MiB frame. The
origins have small receive buffers and do not consume the client frames;
the clients have small receive buffers and do not consume the origin frames.
The test waits for bytes to reach all sixteen origins, then checks the same
four-second RSS plateau, `503` admission, slot reuse, and FD recovery. In a
local x86-64 Linux Debug run, the worker measured 8,560 KiB before the first
connection, 37,512 KiB at the sampled peak and later plateau, and 28,956 KiB
after teardown. Both client and origin sockets accepted 64 MiB of frame
payload. The ASan variant passed the same behavioral checks and its 256 MiB
aggregate RSS ceiling. These figures include allocator retention and do not
establish a per-tunnel allocator bound.

The mixed production-route smoke holds eight certificate-verified HTTP/2
slow-reader downloads and eight cleartext WebSocket tunnels in the same worker.
Each WebSocket origin sends one 4 MiB frame to a client that stops reading;
the HTTP/2 origins offer 64 MiB each with 1 MiB route chunk limits. The test
checks both overflow request types return `503` without reaching either
origin, samples the four-second RSS plateau, and checks descriptor recovery.
On the pinned x86-64 Linux Debug run, worker RSS was 7,460 KiB before the
first connection, 35,908 KiB at the sampled peak and later plateau, and
19,992 KiB after teardown. Worker FDs were 15 / 49 / 17 before, during, and
after. The origins produced 43,024,384 HTTP/2 bytes and 33,554,432
WebSocket payload bytes. The ASan run passed the same 256 MiB aggregate
ceiling and teardown checks. This exercises the shared sixteen-slot admission
cap under mixed load; it does not establish a deployment-wide worker budget.

The upload-pressure variant holds sixteen HTTP/2 POSTs, or eight HTTP/2 and
eight HTTP/1.1 POSTs in the same worker, against origins that stop reading
after the first request-body chunk. Each client generates a 64 MiB logical
body and stops sending on backpressure, without buffering that body in the
test process. Every origin receives body bytes before its client finishes.
The test samples the four-second worker RSS plateau, verifies `503` admission
without another origin connection, and checks FD recovery. In one x86-64
Linux Debug mixed-pool run with 1 MiB route chunk limits, worker RSS was
7,464 KiB at the preflight baseline, 41,896 KiB at headers and through the
sampled plateau, and 40,808 KiB after teardown; clients had sent 119,666,556
bytes in aggregate while the origins remained stalled. Both upload variants
also passed under ASan's 256 MiB aggregate ceiling. This measures a stalled
upload profile, not the allocator maximum for every concurrent workload.

The full admission reserve still needs combined upload and download pressure,
both idle caches, maximum permitted headers, CA bundles and client identities,
and a deployment worker-memory budget. The bidirectional WebSocket case is
covered separately above.
The existing 16-slot exchange cap remains provisional until that gate is
complete.

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
HTTP bytes through the bounded Kore queue. Queuing a proxy-owned header netbuf
does not commit it: if upstream fails before any write attempt, remove that
unwritten netbuf and send one local error response. Track the first write
attempt explicitly. After a write attempt, treat the response as committed,
even if Kore's netbuf offset is still zero: `SSL_write()` may have pending
ciphertext that cannot safely be replaced. The response hook
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
policy has targeted executable probes and must be verified in the production
path. A downstream reset can arrive as an event error or as `ECONNRESET` on
the next raw read; both paths must cancel the easy handle and delayed retry timer
before the exchange is freed.
The relay copies bounded byte chunks in both directions with independent
ingress pause/resume. Kore's WebSocket message API remains for application
WebSockets and is not used by transparent proxy tunnels.

### Ownership, shutdown, and resource limits

One proxy exchange owns the request, accepted connection, easy handle, queues,
timers, and optional raw tunnel. Cancellation is idempotent; no callback can
refer to the exchange after final cleanup. Specify the state transitions for
headers pending, streaming, upload complete, upgrade, cancellation, and worker
shutdown. A downstream disconnect cancels the upstream. Before a downstream
header write attempt, upstream DNS, connect, TLS, or protocol failure returns
`502`; an upstream deadline returns `504`; proxy resource exhaustion returns
`503`.
Preflight rejection uses the status selected by the application.
An error after a header write attempt closes the downstream connection. A raw
WebSocket leg reaching EOF flushes only bytes already accepted into its bounded
queue up to the close deadline, then closes both legs; it does not synthesize
WebSocket frames. Do not reuse a downstream keepalive connection until its
request body is fully consumed and response framing is complete.

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
8. A proxy route can admit and forward validated escaped raw paths, including
   an encoded slash, without changing ordinary route rejection behavior or
   registration order. Reject malformed escapes, controls, backslashes, and
   dot traversal before a director or upstream connection. Prove both route
   orders for overlapping proxy and ordinary patterns, plus raw parameter
   captures, and prove the raw target reaches the director and upstream intact
   after an allowed rewrite.

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
| Policy unit tests | Target/raw-path/raw-query joining and escaping, including dot segments and repeated query fields; origin-form validation for `CURLOPT_REQUEST_TARGET`; Host and forwarding policy; hop-by-hop token removal; duplicate headers; allowed-target selection; malformed proxy framing and handshake rejection; pre-body proxy selection agreeing with later non-proxy dispatch. Cover both registration orders for proxy literal versus ordinary regex, proxy regex versus static or live upload, and an application WebSocket route overlapping a proxy route. Test raw encoded slash, percent, colon, malformed escape, encoded dot traversal, raw parameter capture, valid decoded-path `404`, and the static-site trailing-slash exception. The application WebSocket route must keep its current priority for GET with valid, missing, or malformed upgrade headers; exact duplicate route registration must still fail. |
| Parser and readiness integration | For a proxy takeover, initial read containing headers plus body, bodyless or `Content-Length: 0` request followed by another request, exactly and beyond declared length, and an early WebSocket frame; split and malformed chunk boundaries; duplicate/conflicting lengths and `Content-Length`/`Transfer-Encoding` ambiguity; trailers; pausing midway through a chunk; resume without a new epoll edge and with pending writes; downstream TLS `WANT_READ`/`WANT_WRITE` progress on Linux and BSD. For `CONTINUE`, compare ordinary route, live-upload, and application WebSocket behavior to existing fixtures, including current body limits and dispatch timing; also send ordinary header-only and fixed-length requests immediately followed by a proxy request in one read. Assert byte ownership, exactly one dispatch per request, and no desynchronization, spin, or premature next-request read. |
| HTTP integration | GET, HEAD, OPTIONS, POST, PUT, PATCH and error statuses, including framed bodies on normally bodyless methods and a `400` for downstream HTTP/1.0; fixed-length and chunked uploads; chunked and fixed-length responses; HEAD/`304` representation length without body bytes; declared request trailers, rejection of undeclared request trailers, and relay of permitted undeclared response trailers, verifying upload EOF and final-chunk ordering; exactly one local `100` even when upstream also sends `100`, bounded upload while upstream connects, `100` followed by `502` on connection failure, upstream `103`, and early-final replies; response hook status/bodyless/framing decisions; exact upstream `4xx`/`5xx` status, headers, and body with Kore pretty errors enabled; redirects and repeated `Set-Cookie`; TLS termination to both HTTP and verified HTTPS upstreams, including a client-certificate-authenticated origin. Assert upstream receives the expected bytes and metadata. |
| HTTP/2 upstream integration | A local HTTPS upstream offers `h2` and `http/1.1`, then `h2` only: ordinary HTTP and SSE negotiate `h2`, while forced-HTTP/1.1 routes and WebSocket handshakes stay on HTTP/1.1 and fail with `502` against an `h2`-only peer. Verify HTTP/1.1 fallback still produces downstream chunked framing in the auto pool, TLS 1.2 minimum, no h2c, no concurrent streams per connection, server push refusal, `:path`/`:authority` rewrites, known/unknown upload length, early response, HEAD, and selected HTTP/1.1 pool for request trailers. Verify an HTTP/2 response with both `Content-Length` and undeclared trailers is sent downstream with chunked framing, intact trailer fields, and declared-length validation. Under many slow downstream readers and concurrent long-lived SSE streams, assert both the per-transfer application queue limit and the separately budgeted libcurl/TLS worker-memory envelope. At the configured active and idle limits, send one extra request and assert immediate `503`, no easy handle queued in libcurl, and bounded memory across both protocol pools and retained WebSocket tunnels. Repeat with much larger response sizes and durations; memory must not track payload size. |
| Streaming integration | Upstream emits the first chunk, then waits before finishing; client must receive that chunk before upstream completion. Request chunks must arrive upstream before client EOF. Repeat with slow upstream, slow downstream, simultaneous upload/download, and a response that starts while upload is active. Assert bounded application and libcurl queue depths, active backpressure, and no spool files or full-body allocations. Use multi-gigabyte logical generators in the opt-in stress run. |
| SSE integration | Headers and first event arrive before upstream completion; periodic comments and events arrive at their production cadence; idle timeout behavior is explicit; downstream reset during a producer idle period cancels upstream promptly without a new upstream event. A client write half-close after headers still permits response completion. Test a half-close coalesced with request headers separately at the pre-body boundary. Assert no writable or `RDHUP` busy loop. |
| WebSocket integration | Successful HTTP/1.1 `ws`/`wss`, selected subprotocol, offered extension pass-through, fragmented messages larger than Kore's normal frame limit, interleaved ping/pong, close code/reason, non-`101` rejection with a streamed body, and client/server disconnect. Assert `modify_response` is bypassed for `101` and applied to non-`101` rejection. Include handshake and first frame in one read on either leg; reject upgrade requests with framed bodies; exceed the bounded pre-upgrade buffer with a paused client; reject `Upgrade: h2c`; test TLS upstream that offers HTTP/2 and verify HTTP/1.1 selection; verify exact byte relay after the handshake, including masking. Test TLS `CURLE_AGAIN` without socket-watcher spin, one readiness owner after connect-only completion, and non-`101` chunked responses with trailers. Run a Go `net/http` upgrader fixture with public `Origin`/rewritten `Host`, subprotocol negotiation, and sustained bidirectional traffic. |
| Failure and lifecycle | DNS/connect/TLS failure, upstream reset before and after headers, malformed upstream headers, slowloris, callback rejection, partial request body, downstream reset while idle or with a queued response write, downstream TLS close, worker shutdown, app stop, and connection limits. Before any downstream header write attempt, remove an unwritten proxy-owned netbuf and send a complete local `502` for upstream transport failure; after a write attempt, end the downstream stream without a successful chunk terminator, including TLS `WANT_*` with zero netbuf offset. Cover the case where Kore has queued a response but has not attempted a write, and assert exactly one request/connection teardown, no orphan transfer, retained curl handle, leaked fd, hanging test process, or accidentally reusable connection with unread request bytes. |
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
