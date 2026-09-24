# Reverse proxy transport feasibility audit

Status: source audit plus targeted live transport probes, 2026-09-24. The
pre-body takeover is a candidate, not a complete implementation choice. The
probes do not implement an upstream proxy. The intended feature contract is in
[reverse-proxy-spec.md](reverse-proxy-spec.md).

## Decision

The candidate reduces changes to Kore's ordinary body path by letting Kore
parse the request line and headers, then handing only selected proxy requests
to a proxy-owned connection handler. Live probes now establish handoff,
queued-response ordering, bounded 1 MiB cleartext and double-TLS relays,
libcurl HTTP/1.1 upload/download overlap, and verified HTTPS connect-only
transport. The leading output choice uses one bounded Kore send netbuf at a
time. It is **not yet a proven complete proxy architecture**: framed
HTTP/SSE, WebSocket handshakes, all cleanup paths, and the HTTP/2 memory
envelope remain open.

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
confirms the basic handle lifetime contract in the local build. A separate
[HTTPS connect-only test](../tests/unit/test_proxy_curl_connect_only_tls.c)
creates a local TLS server with a trusted in-memory certificate, disables
ALPN as required for the raw HTTP/1.1 WebSocket handshake, completes the
connection in libcurl multi, then sends and receives bytes through the
still-attached easy handle and its active socket. This passes with certificate
and hostname verification enabled. It proves the TLS transport mechanism, not
a WebSocket handshake or coupled bounded tunnel. [Libcurl documents that a
connect-only multi easy must remain attached](https://curl.se/libcurl/c/CURLOPT_CONNECT_ONLY.html)
and that `curl_easy_recv()` must be drained before waiting for another socket
edge because TLS data can remain inside libcurl
([receive API](https://curl.se/libcurl/c/curl_easy_recv.html)).

The [HTTP/1.1 duplex test](../tests/unit/test_proxy_curl_duplex.c) pauses the
upload callback after its first four bytes. A local server sends a final
chunked response and its first body chunk while the upload is paused, then
reads the remaining upload bytes and sends the final response chunk. The
pinned debug libcurl 8.22.0 delivered `pong` before upload resume and
completed `pongdone` after it. This proves response progress during a paused
HTTP/1.1 upload in this build; it does not prove the event-loop integration,
HTTP/2 duplex, or cancellation behavior.

The Linux [socket handoff test](../tests/unit/test_proxy_curl_socket_handoff.c)
drives connect-only setup through `curl_multi_socket_action()` with a socket
callback and timer callback on an epoll loop. In the pinned build, libcurl
reports `CURL_POLL_REMOVE` when setup completes, while the easy handle remains
attached and the active socket stays usable. The test then registers the same
fd under an application-owned watcher and exchanges bytes through
`curl_easy_send()`/`curl_easy_recv()`. Five serial repetitions pass. This
proves the local epoll watcher transition in isolation. The coupled Kore
worker-loop probe below also exercises HTTPS setup and TLS watcher handoff.

The [Kore worker-loop probe](../tests/unit/test_kore_proxy_curl_loop.c) now
drives a libcurl multi connect-only transfer through Kore's epoll event and
timer APIs from a taken-over request. It then hands the connected socket to a
raw tunnel watcher, exchanges `ping`/`pong` with an upstream, and sends a
direct downstream response. The client receives the exact response, and the
app stops with one disconnect. Five serial repetitions passed. The pinned
build registered two libcurl setup sockets in one run, so the probe uses one
Kore event object per socket with `curl_multi_assign()`, removes both watchers,
and leaves the completed easy attached until the tunnel closes. An earlier
single-watcher attempt spun, showing that this ownership rule is necessary.
The test also had to start its local upstream server thread after Vectis's
single-threaded startup declaration phase. This proves the worker-loop API
composition for a small plain-TCP exchange. The probe now also opens a second
taken-over request whose upstream accepts TCP but stalls the TLS handshake.
When Vectis stops the worker, the registered worker teardown callback cancels
that active libcurl multi/easy, removes its Kore socket watchers, and cancels
its timer before Kore closes the event loop and later removes the downstream
connection. The client connection and stalled upstream server both close, and
the test records one worker cancellation, two active libcurl watchers and one
armed deadline timer at teardown, and two total downstream disconnects.
Five serial repetitions pass. This confirms the shutdown ordering and exposes
the required per-worker active-exchange registry; cancellation during every
callback phase and a complete WebSocket handshake are still open.

The same worker-loop probe now also connects a second cleartext upstream and
relays a generated 1 MiB body while a client concurrently sends and reads
slowly. The upstream echoes incrementally, the client verifies every byte
without materializing the body, and each direction has a fixed 8 KiB
application queue. The probe checks exact completion, actual read gating,
queue high-water at or below 8 KiB, upstream write half-close after downstream
EOF, response EOF, and worker cleanup. Twenty serial repetitions pass. The
test exposed three integration rules that small exchanges missed: suppress
downstream reads until connect-only setup completes; mark relay completion
before asking Kore to disconnect because a readiness callback may recur; and
remove a watcher entirely when neither direction needs readiness, otherwise
level-triggered HUP or writable readiness can spin. It also exposed an
independent-timer ownership error: a libcurl timer update must not cancel the
exchange deadline, while exchange cancellation must cancel both timers.
The probe now also connects to a certificate-verified HTTPS upstream through
the same worker-loop libcurl multi, first with a cleartext client and then
with a certificate-verified downstream TLS client. In each case an incremental
TLS echo server and slow client exchange the exact generated 1 MiB body. The
double-TLS client observes server `close_notify`; the upstream handle stays
attached through the transfer. Output uses `net_send_queue()` with at most
one 8 KiB Kore netbuf, in addition to the fixed 8 KiB scratch buffer. It
pauses upstream reads while the netbuf drains. Twenty serial repetitions of
the cleartext, HTTPS-upstream, and double-TLS sequence pass. The test also
checks that the total relay pump count stays below 5,000 and the TLS relay
count below 2,000. These bounds cover this Linux fixture, not arbitrary
production workloads or libcurl/TLS internal allocations. The TLS upstream
fixture closes after a known byte count; it does not prove WebSocket close
frames, HTTP framing, SSE, or HTTP/2 flow control.

This output choice came from a failed direct-TLS experiment in the same
worker loop. Level-triggered writable interest generated more than 150,000
callbacks in one 1 MiB run while `SSL_write()` repeatedly returned
`WANT_WRITE`; a stable edge-triggered watcher avoided that spin but stalled
on a later repetition. Smaller direct TLS writes still spun. Kore's bounded
send queue and TLS writer completed the same fixture without either failure
in the repetitions above. A proxy-specific direct TLS output scheduler would
be more complex, and there is no need for it if the queue-based path passes
the remaining lifecycle and platform gates.

The [libcurl pause contract](https://curl.se/libcurl/c/curl_easy_pause.html)
allows up to an HTTP/2 stream flow-control window of internal receive caching
when a transfer is paused, with a documented 10 MiB default window example.
Setting `CURLMOPT_PIPELINING` to `CURLPIPE_NOTHING` disables multiplexing
([option contract](https://curl.se/libcurl/c/CURLMOPT_PIPELINING.html)); it
does not establish an 8 KiB libcurl memory ceiling. The proposal therefore
requires an explicit per-connection HTTP/2 memory allowance and per-worker
admission limit, validated against the pinned binary under a paused slow
consumer. No numerical allowance has been measured yet. If that allowance
cannot satisfy the streaming memory budget, the architecture needs a
different HTTP/2 client transport or a narrower supported contract.

The Linux [HTTPS HTTP/2 pause probe](../tests/unit/test_proxy_curl_http2_pause.c)
uses a local nghttp2 server over certificate-verified TLS. ALPN selects `h2`;
the client reports HTTP/2 and disables multiplexing on its multi handle. The
server generates a 64 MiB response incrementally while libcurl's first body
callback pauses reception. In one run, the server had generated about 3.9 MiB
when the test ended, while total process RSS moved from 9.3 MiB before the
transfer to 10.3 MiB after pause and stayed there for another two seconds.
The test enforces a 32 MiB RSS-delta ceiling and a 4 MiB post-pause growth
ceiling; five serial repetitions passed. The server and client share the
process, so this is a conservative whole-process measurement. It shows a
stable, bounded paused single-stream case in the pinned debug bundle, not a
general hard libcurl allocation bound. Several concurrent streams, release
bundles, large headers, compressed responses, and resume/cancellation still
need an admission and stress test before the HTTP/2 allowance is final.

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

## Transport candidate under test

The direct-output candidate gave the proxy exchange its own downstream
writer on Kore's accepted fd or `SSL *`. It remains a useful feasibility
probe, but the sustained coupled relay found a simpler output boundary: copy
one bounded chunk into Kore's existing `net_send_queue()`. Kore handles TLS
output, and the proxy waits for that queue to drain before asking libcurl for
another chunk. There is
no stream completion callback or proxy state reference in a Kore netbuf. The
proxy still reads request bytes directly from the accepted fd or `SSL *` to
control ingress backpressure. Kore owns accept, TLS handshake, output
encryption, the worker event loop, and connection destruction. The proxy
temporarily owns the connection event callback and restores Kore's handler
after it has fully consumed the request and drained its response. It waits
for any earlier ordinary response already queued on that connection before
adding proxy bytes, preserving pipeline order.

The direct-output probe initially looked simpler than using
`net_send_stream()` for proxy output. In
[`connection_remove()`](../vendor/kore/upstream/src/connection.c), Kore frees
`connection->hdlr_extra` before removing queued send buffers; their stream
completion callbacks also run during removal in
[`net_remove_netbuf()`](../vendor/kore/upstream/src/net.c). A callback referring
to proxy state in `hdlr_extra` would use freed memory unless the proxy adds a
separate reference-counted lifetime or cancellation protocol. A bounded
`net_send_queue()` call copies the chunk instead and needs no completion
callback. Kore allocates at most 8 KiB for that netbuf in this probe. The
proxy's separate 8 KiB scratch buffer remains accounted. The proxy still
calls `SSL_get_error()` directly after `SSL_read()` for input and uses
`SSL_want()` after Kore's `net_send_flush()` to arm the queued output's retry
direction. [OpenSSL documents the retry direction and stable write argument
rule](https://docs.openssl.org/3.6/man3/SSL_write/).

The [Linux direct-I/O probe](../tests/unit/test_kore_proxy_direct_io.c) now
proves the cleartext part on a running Kore listener. It temporarily replaces
`connection->evt.handle`, changes only that connection to level-triggered
read/write interest through Kore's existing epoll scheduling function, and
keeps an 8 KiB application output queue. With a slow reader it relayed 1 MiB
byte-for-byte, observed 106 write pauses in one run, never queued more than
8 KiB, and completed after the client write-half-closed. It saw one read EOF
and one disconnect, with 108 event callbacks and no writable-fd spin. Ten
serial repetitions passed. This establishes the Linux plain-TCP pause/resume
and half-close mechanism without another Kore patch.

The same probe now uses Kore's accepted `SSL *` directly for a 256 KiB TLS
relay with a slow reader. One run observed 13 TLS write pauses, an 8 KiB
maximum application queue, one read EOF, and one disconnect, with the payload
preserved exactly. The first TLS run delivered all bytes but ended with an
unexpected EOF at the client: Kore's connection cleanup did not complete the
nonblocking TLS closure for this exchange. Explicitly sending the server
`close_notify` from the proxy adapter after its output queue drained made the
client observe `SSL_ERROR_ZERO_RETURN`. This closure is part of the proxy
transport. The probe uses `SSL_get_error()` for retry direction and holds the
write buffer unchanged across a retry. Five serial repetitions of the
TLS-inclusive test passed. The probe now precedes the taken-over request with
an ordinary 128 KiB response in the same client write. Both cleartext and TLS
runs observe Kore's earlier send queue still populated at takeover, drain it,
and only then emit the direct response. The client verifies the entire earlier
body and subsequent takeover header in order. This exercises the queue-drain
barrier without another Kore patch; both modes reported one queued predecessor
and one drain event in the final run.

The probe now also pipelines a 128 KiB ordinary
`vectis_response_stream_source()` response immediately before a taken-over
request. The client verifies every chunk and final chunk of the streamed
response, then receives the direct `101` in order; three serial repetitions
pass, with one takeover and no duplicate dispatch. Kore may finish draining
the previous stream's queue in the same event that invokes the pre-body hook,
so the proxy must wait for the queue to empty without assuming that its own
event handler performs every drain. The first version of this test mistakenly
used `vectis_response_source()`, which materializes the entire response in a
temporary file; it was changed to the live source API to test the actual
stream callback boundary.

The probe now also checks the selected bounded Kore output queue after a live
128 KiB streamed predecessor. It sends the streamed request, a taken-over
`200`, and an ordinary 128 KiB successor in one client write. The client
verifies exact ordering and bytes over cleartext and TLS; the connection
returns to Kore for the successor. Ten serial repetitions pass. The first
version flushed the prior queue synchronously inside the pre-body hook.
When the hook ran inside the predecessor stream's completion callback, that
flush reentered netbuf removal, emitted a duplicate terminal chunk, and
crashed the worker. Deferring the first flush and proxy enqueue to the next
readiness event resolved it. This non-reentrancy rule is required for the
final handoff.

A separate ordinary-HTTP takeover mode in the same probe queues its response
through `net_send_queue()` and returns ownership to Kore after that queue
drains. It marks the
sleeping request for deletion, restores Kore's connection and event handlers,
rearms edge-triggered interest, and replays bounded bytes that arrived beyond
the proxy request. On one connection, the client receives the proxy's `200`
response, an ordinary 128 KiB response coalesced in the same write, and a
second ordinary 128 KiB response sent later. The sequence passes over both
cleartext and downstream TLS in three serial repetitions. This establishes
that the bounded Kore output path can preserve keepalive reuse without
extending Kore's
ordinary transport API. The probe only covers a header-only proxy request and
a small initial pipeline suffix; the real framer must hold arbitrary bounded
body and suffix chunks until a safe return point.

The same probe now stops a running app while a second taken-over cleartext
connection remains open. Its worker teardown callback sees that live direct
connection, then Kore's later connection cleanup invokes its disconnect
callback once; the app waits for the worker to exit. This exposed a required
static-runtime integration: Kore discovers `kore_worker_teardown` by name, but
Vectis only resolves names present in its
[runtime symbol table](../src/vectis_kore_bridge.c). The symbol is now listed
and a private probe callback confirms the teardown order. The eventual proxy
worker cleanup must use that hook to remove curl easy handles and socket
watchers, cancel timers, and mark active exchanges before Kore frees accepted
connections. The current probe has no upstream handle to cancel.

A short-header-timeout variant exposed another takeover invariant. Kore invokes
the pre-body hook before it clears `connection->http_timeout` for an ordinary
header-only request. With a one-second header limit, an otherwise healthy
idle taken-over connection was closed after roughly 1.6 seconds. The probe
now sets `http_timeout` to zero at takeover and leaves a direct connection
idle past that limit; the socket remains open. Proxy-owned connect, idle, and
optional total deadlines must replace the inherited Kore header/body timer
for the entire HTTP/SSE/WebSocket exchange.

The full candidate is **not proved**. Kqueue still needs an equivalent
interest helper and test. The TLS probes cover ordinary retry directions;
neither has forced a cross-direction retry. The per-event budget has not
exercised continuation after OpenSSL holds decrypted bytes. The earlier
queue-drain probe covers a buffered response and a cleartext live stream
completion callback, but not stream abort during takeover. The new bounded
Kore-output path passes the predecessor/keepalive sequence over cleartext and
TLS, but TLS queue abort during disconnect remains open. Worker shutdown with
an active libcurl handshake and sustained cleartext and double-TLS exchanges
pass; HTTP-framed streaming remains open.

An assertion failure in a probe can leave the already started Kore parent and
worker alive after the test process exits. Two such orphaned probe instances
were found and terminated by their exact PIDs during this audit. Successful
probe runs stop their apps, but failure-path process cleanup is not yet proven.
The eventual proxy tests need a failure-safe supervisor or equivalent teardown
so a failed assertion or timeout cannot strand workers or a generated pid file.

## Findings from the current source

| Boundary | Evidence and consequence | Feasibility |
| --- | --- | --- |
| Header selection | Vectis registers one catch-all Kore route in [`vectis_kore_bridge.c`](../src/vectis_kore_bridge.c); its own route selection runs later. [`http_header_recv()`](../vendor/kore/upstream/src/http.c) has a point after the header list is built and before method-based body checks. Patch `0030` adds an optional callback there. The existing `on_headers` hook runs after initial body delivery and is skipped by some zero-length paths. | Hook timing passes a live probe; actual Vectis proxy route precedence remains untested. |
| Connection handoff | [`kore_connection_event()`](../vendor/kore/upstream/src/connection.c) calls the replaceable `connection->handle`. [`net_recv_flush()`](../vendor/kore/upstream/src/net.c) invokes the receive callback while processing an event. The handoff probe replaces both connection and receive handlers and owns the initial suffix. | Coalesced and split input passes on Linux, including downstream TLS. Direct-I/O ordinary HTTP takeover also restores Kore's event handler and serves two subsequent ordinary requests over cleartext and TLS. Bounded request-body framing and cleanup races remain open. |
| Body framing and pipelining | Kore's ordinary request path is method-based and has no incoming chunked decoder. Before patch `0029`, `http_header_recv()` could drop bytes after a header-only request and pass surplus across a fixed body boundary to `http_body_update()`. | Patch `0029` and the live test cover ordinary byte boundaries. A proxy-owned fixed/chunked framer and its bounds remain unproved. |
| Backpressure and TLS | [`net_recv_flush()`](../vendor/kore/upstream/src/net.c) has no pause outcome, so taken-over input uses direct nonblocking fd/`SSL *` reads. Output copies one bounded chunk into Kore's send queue and uses its TLS writer; `SSL_want()` identifies queued output's retry direction. The worker probe measures an 8 KiB scratch queue plus one 8 KiB Kore netbuf. [OpenSSL permits either retry direction](https://docs.openssl.org/3.6/man3/SSL_write/). | Slow cleartext, HTTPS-upstream, and double-TLS relays pass on Linux. Forced cross-direction retries and kqueue remain open. |
| TLS buffered data | [OpenSSL documents](https://docs.openssl.org/3.0/man3/SSL_pending/) processed and unprocessed records that can remain after the socket stops reporting readable. An edge-triggered handler must drain buffered application data when capacity resumes, but it must not spin on a record that cannot yet produce application bytes. | Requires a bounded work budget and explicit continuation scheduling, not readiness alone. |
| EOF and half-close | [`net_read()`](../vendor/kore/upstream/src/net.c) disconnects the whole connection on a zero-byte read; `kore_tls_read()` treats `SSL_ERROR_ZERO_RETURN` as an error. The direct-I/O probe keeps writing after TCP EOF and after a TLS client `close_notify`. | One cleartext and one TLS half-close exchange pass. Early upstream final responses, resets, and close deadlines remain open. |
| Response output | `net_send_stream()` callbacks need an independent lifetime because Kore can invoke them during connection removal after freeing `hdlr_extra`. `net_send_queue()` instead copies a bounded chunk and needs no proxy completion callback. The proxy detects queue drain in its connection event handler before resuming upstream reads. Its first flush is deferred beyond the pre-body hook to avoid reentering a predecessor stream callback. | One 8 KiB Kore netbuf and one 8 KiB scratch buffer suffice for the 1 MiB slow-peer probe over cleartext and double TLS. The queue-based writer preserves a live streamed predecessor, its own response, and an ordinary successor over cleartext and TLS. Abort coverage remains open. |
| Request/accounting lifetime | A taken-over GET may already have `HTTP_REQUEST_COMPLETE`. [`http_request_sleep()`](../vendor/kore/upstream/src/http.c) prevents normal dispatch, and connection removal wakes attached requests for deletion. A sleeping SSE request still counts against `http_request_limit` and retains the header allocation; Vectis defaults the header limit to 64 KiB and the request limit to max connections. | Ownership path exists; admission and memory measurements must include long-lived request/header objects. Any early release needs its own logging, timeout, and cleanup proof. |
| Timers and shutdown | [`kore_connection_check_timeout()`](../vendor/kore/upstream/src/connection.c) still enforces the header timer after pre-body takeover unless the proxy clears it. Worker teardown runs before [`kore_connection_cleanup()`](../vendor/kore/upstream/src/worker.c). Vectis exposes the worker teardown hook through its static-runtime symbol table. | A one-second timer killed an idle takeover before the fix; clearing `http_timeout` preserved it. Active downstream connection was observed at teardown and disconnected once. A stalled upstream TLS handshake was cancelled with its curl handle, Kore socket watchers, and deadline timer before event-loop cleanup. Other callback phases and timeout policies remain open. |
| Upstream transport | The local debug bundle has libcurl 8.22.0 with asynchronous DNS, HTTP/2, and TLS. Kore's wrapper buffers responses and removes completed easy handles, so the proxy needs its own multi transport. [Libcurl requires](https://curl.se/libcurl/c/CURLOPT_CONNECT_ONLY.html) a connect-only WebSocket handle to remain in its multi while raw send/receive uses its socket. | Plain TCP and verified HTTPS connect-only, paused-upload/concurrent-download HTTP/1.1, Linux Kore worker-loop connect-only/tunnel handoff, 1 MiB coupled cleartext and double-TLS relays, and a stable single-stream HTTPS HTTP/2 pause pass. Complete WebSocket handshake/tunnel, release-bundle features, and concurrent HTTP/2 memory remain open. |

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
   reset, cancellation inside a queued-write readiness callback, worker stop,
   and repeated connection reuse. Assert exactly one cleanup of each request,
   connection, timer, curl handle, and buffer.
4. **Upstream gates:** Verify asynchronous DNS in every release bundle, then
   prove paused upload with concurrent download, connect-only TLS socket
   handoff, and a stable HTTP/2 memory envelope under slow downstream readers.

Until these proofs pass, the pre-body takeover should remain a candidate. A
failure in the first three gates requires revisiting the Kore transport
boundary; it must not be hidden by full-body buffering, an unbounded queue, or
a worker thread per stream.
