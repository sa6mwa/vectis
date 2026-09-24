# Reverse proxy transport feasibility audit

Status: source audit, 2026-09-24. The pre-body takeover is a candidate, not a
proven implementation choice. No proxy transport code or prototype is included
in this audit. The intended feature contract is in
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

## Findings from the current source

| Boundary | Evidence and consequence | Feasibility |
| --- | --- | --- |
| Header selection | Vectis registers one catch-all Kore route in [`vectis_kore_bridge.c`](../src/vectis_kore_bridge.c); its own route selection runs later. [`http_header_recv()`](../vendor/kore/upstream/src/http.c) has a point after the header list is built and before method-based body checks. A new optional callback can return continue, reject, or takeover there. The existing `on_headers` hook runs after initial body delivery and is skipped by some zero-length paths. | Source supports a narrow hook; callback ordering and route precedence need integration proof. |
| Connection handoff | [`kore_connection_event()`](../vendor/kore/upstream/src/connection.c) calls the replaceable `connection->handle`. [`net_recv_flush()`](../vendor/kore/upstream/src/net.c) invokes the receive callback while processing an event. A takeover can install a proxy handler and receive callback, but the current event invocation must finish safely and any already-read suffix must remain owned. | Plausible with explicit handoff state; unproven under coalesced reads. |
| Body framing and pipelining | Kore's ordinary request path is method-based and has no incoming chunked decoder. `http_header_recv()` gives the whole receive buffer to the request, then can drop bytes after a header-only request. It passes all initial surplus to `http_body_update()`, whose remaining-length subtraction can underflow when the read crosses a fixed body boundary. | Proxy can own its fixed/chunked body framer. Two targeted shared byte-boundary fixes are still required for ordinary-to-proxy pipelining and return to keepalive. |
| Backpressure and TLS | [`net_recv_flush()`](../vendor/kore/upstream/src/net.c) has no pause outcome. A proxy handler can gate reads and resume by explicitly draining. Linux [`kore_platform_disable_read()`](../vendor/kore/upstream/src/linux.c) deletes the entire epoll registration, so it cannot be used while writes must progress. [`kore_tls_read()` and `kore_tls_write()`](../vendor/kore/upstream/src/tls_openssl.c) collapse `WANT_READ` and `WANT_WRITE` into the same success result and clear only the operation's event flag. [OpenSSL permits either retry direction](https://docs.openssl.org/3.0/man3/SSL_get_error/) for either operation. | Current API is insufficient for correct cross-direction TLS retries. A targeted retry-direction result, plus tests on Linux and BSD, is required. |
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
