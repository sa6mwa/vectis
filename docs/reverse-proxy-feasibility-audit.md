# Reverse proxy transport feasibility audit

Status: source audit plus targeted live transport probes, 2026-09-24. The
pre-body takeover is a candidate, not a complete implementation choice. The
probes do not implement an upstream proxy. The intended feature contract is in
[reverse-proxy-spec.md](reverse-proxy-spec.md).

## Decision

The candidate reduces changes to Kore's ordinary body path by letting Kore
parse the request line and headers, then handing only selected proxy requests
to a proxy-owned connection handler. It is plausible, but **not yet proven
feasible** for bounded full-duplex TLS traffic. The TLS retry signal, buffered
byte replay, EOF policy, and cleanup ordering require an executable spike
before committing to this architecture. Source inspection is insufficient for
those behaviors.

The alternative shared-framing approach avoids duplicating a fixed-length
body counter but touches more of Kore's normal request path. The pre-body
candidate is preferable only if the gates below pass without broadening its
Kore changes into a second event/HTTP stack.

## Executable finding: ordinary pipelining

The focused [Kore pipelining test](../tests/unit/test_kore_pipelining.c) sent
two ordinary GET requests in one TCP write to a running Vectis listener.
Before the fix, it received one response. Patch
[`0029`](../vendor/kore/patches/0029-kore-preserve-pipelined-request-bytes.patch)
now retains the bounded suffix from the initial header read, clamps the
initial body delivery to the declared `Content-Length`, and replays the suffix
after the preceding response starts the next receive. The live test receives
two responses for GET plus GET, fixed-length POST plus GET, and zero-length
POST plus GET. It closes the app and waits for its workers before exiting.
The existing HTTPS runtime test, header-limit test, and `kore_smoke` runtime
case also pass. This proves the ordinary byte-replay prerequisite on Linux;
it does not prove proxy takeover, TLS replay, or backpressure.

## Executable finding: libcurl connect-only ownership

The focused [connect-only test](../tests/unit/test_proxy_curl_connect_only.c)
uses the pinned debug libcurl build with a local TCP echo server. It keeps a
completed connect-only easy handle attached to a multi handle, obtains its
active socket, exchanges bytes with `curl_easy_send` and `curl_easy_recv`, and
checks that raw sending stops working after removing the easy handle. This
confirms the basic handle lifetime contract in the local build. It does not
yet prove HTTPS/WSS, socket-watcher transfer, or bounded relay behavior.

## Executable finding: pre-body handoff and replay

Patch [`0030`](../vendor/kore/patches/0030-kore-prebody-handoff-hook.patch)
adds a route-local callback after Kore has parsed headers but before its
method-based body rules or initial body delivery. The
[handoff probe](../tests/unit/test_kore_prebody_handoff.c) installs a temporary
connection handler and bounded receive buffer through that callback. On a
running listener it receives three ordered responses for ordinary GET, taken
over POST, ordinary GET sent in one TCP write. A split POST body followed by
an ordinary GET also produces two responses. Both the three-request sequence
and an upgrade-style request with an early masked WebSocket frame preserve
the exact initial bytes over cleartext and downstream TLS. The test stops the
app and its workers after each listener. Header-time rejection returns either
the default `400` or a callback-supplied `403` before request-body delivery.

The first split-body probe stalled after one response when takeover returned
from the active receive loop. With edge-triggered epoll, the next request was
already in the socket and no new edge arrived. Letting that same receive loop
continue into the newly installed callback resolved the stall. This is a
required handoff invariant: a takeover must install its receive state
atomically and drain the current event until the socket would block or a
bounded queue explicitly pauses it. The probe uses a private test callback
setter in the Vectis bridge; the eventual route selector must replace it.
The raw upgrade response is a byte-ownership probe, not a complete WebSocket
handshake or relay.

## Findings from the current source

| Boundary | Evidence and consequence | Feasibility |
| --- | --- | --- |
| Header selection | Vectis registers one catch-all Kore route in [`vectis_kore_bridge.c`](../src/vectis_kore_bridge.c); its own route selection runs later. [`http_header_recv()`](../vendor/kore/upstream/src/http.c) has a point after the header list is built and before method-based body checks. Patch `0030` adds an optional callback there. The existing `on_headers` hook runs after initial body delivery and is skipped by some zero-length paths. | Hook timing passes a live probe; actual Vectis proxy route precedence remains untested. |
| Connection handoff | [`kore_connection_event()`](../vendor/kore/upstream/src/connection.c) calls the replaceable `connection->handle`. [`net_recv_flush()`](../vendor/kore/upstream/src/net.c) invokes the receive callback while processing an event. The handoff probe replaces both connection and receive handlers and owns the initial suffix. | Coalesced and split input passes on Linux, including downstream TLS; bounded pauses and cleanup races remain open. |
| Body framing and pipelining | Kore's ordinary request path is method-based and has no incoming chunked decoder. Before patch `0029`, `http_header_recv()` could drop bytes after a header-only request and pass surplus across a fixed body boundary to `http_body_update()`. | Patch `0029` and the live test cover ordinary byte boundaries. A proxy-owned fixed/chunked framer and its bounds remain unproved. |
| Backpressure and TLS | [`net_recv_flush()`](../vendor/kore/upstream/src/net.c) has no pause outcome. A proxy handler can gate reads and resume by explicitly draining. Linux [`kore_platform_disable_read()`](../vendor/kore/upstream/src/linux.c) deletes the entire epoll registration, so it cannot be used while writes must progress. [`kore_tls_read()` and `kore_tls_write()`](../vendor/kore/upstream/src/tls_openssl.c) collapse `WANT_READ` and `WANT_WRITE` into the same success result and clear only the operation's event flag. [OpenSSL permits either retry direction](https://docs.openssl.org/3.0/man3/SSL_get_error/) for either operation. | Current API is insufficient for correct cross-direction TLS retries. Compare a proxy-only OpenSSL adapter with a targeted Kore TLS result extension, then prove readiness on Linux and BSD. |
| TLS buffered data | [OpenSSL documents](https://docs.openssl.org/3.0/man3/SSL_pending/) processed and unprocessed records that can remain after the socket stops reporting readable. An edge-triggered handler must drain buffered application data when capacity resumes, but it must not spin on a record that cannot yet produce application bytes. | Requires a bounded work budget and explicit continuation scheduling, not readiness alone. |
| EOF and half-close | [`net_read()`](../vendor/kore/upstream/src/net.c) disconnects the whole connection on a zero-byte read; `kore_tls_read()` treats `SSL_ERROR_ZERO_RETURN` as an error. A client TCP write-half-close after a complete upload can still expect a response. | Decide and prove the proxy EOF contract. Supporting half-close needs a proxy-specific read result or adapter; treating read EOF as full disconnect is insufficient for that case. |
| Response output | Vectis already drives bounded generated responses using [`net_send_stream()`](../vendor/kore/upstream/src/net.c) and send-completion callbacks in [`vectis_kore_bridge.c`](../src/vectis_kore_bridge.c). An asynchronous producer can reuse that queue mechanism with its own wakeup and high-water mark. | Mechanism exists; zero queued bytes must mean wait, and callbacks on cancellation must be tested. |
| Request/accounting lifetime | A taken-over GET may already have `HTTP_REQUEST_COMPLETE`. [`http_request_sleep()`](../vendor/kore/upstream/src/http.c) prevents normal dispatch, and connection removal wakes attached requests for deletion. A sleeping SSE request still counts against `http_request_limit` and retains the header allocation; Vectis defaults the header limit to 64 KiB and the request limit to max connections. | Ownership path exists; admission and memory measurements must include long-lived request/header objects. Any early release needs its own logging, timeout, and cleanup proof. |
| Timers and shutdown | [`kore_connection_check_timeout()`](../vendor/kore/upstream/src/connection.c) skips the normal idle check while a request remains attached and can enforce ordinary send timeouts on queued output. Worker teardown runs before [`kore_connection_cleanup()`](../vendor/kore/upstream/src/worker.c). | Proxy timers and one owner for cancellation are required. Cancel curl handles and timers before connection/request teardown, including worker stop. |
| Upstream transport | The local debug dependency bundle identifies libcurl 8.22.0 and OpenSSL 3.6.4. A direct `curl_version_info()` check of its shared library reports asynchronous DNS, HTTP/2, and TLS support. The existing Kore curl wrapper buffers responses and removes completed easy handles; proxy HTTP needs a separate multi transport. [Libcurl requires](https://curl.se/libcurl/c/CURLOPT_CONNECT_ONLY.html) a connect-only WebSocket handle to remain in its multi while raw send/receive uses its socket. | Local debug build clears the asynchronous-DNS feature check. Connect-only fd ownership, full duplex, and HTTP/2 memory envelope remain open, including for release bundles. |

## Minimum executable proof before architecture commitment

1. **Handoff and replay:** Add only the optional pre-body callback and a
   disposable proxy echo sink in an isolated spike. Send header plus body,
   header plus early WebSocket bytes, and ordinary then proxy then ordinary
   requests in one TCP write. Verify exact bytes, ordering, one dispatch per
   request, and no use-after-free. Repeat over downstream TLS.
2. **Backpressure and TLS:** Use a slow consumer and producer with bounded
   queues. Force read pause while writes are pending, then reverse it. Exercise
   both TLS retry directions and buffered TLS records; assert progress after
   resume without new socket edges, no busy loop, and stable memory. Run on
   epoll and kqueue.
3. **EOF and lifecycle:** Complete an upload, half-close the client write side,
   and verify the response can finish. Then test downstream reset, upstream
   reset, cancellation inside a send-completion callback, worker stop, and
   repeated connection reuse. Assert exactly one cleanup of each request,
   connection, timer, curl handle, and buffer.
4. **Upstream gates:** Verify asynchronous DNS in every release bundle, then
   prove paused upload with concurrent download, connect-only TLS socket
   handoff, and a stable HTTP/2 memory envelope under slow downstream readers.

Until these proofs pass, the pre-body takeover should remain a candidate. A
failure in the first three gates requires revisiting the Kore transport
boundary; it must not be hidden by full-body buffering, an unbounded queue, or
a worker thread per stream.
