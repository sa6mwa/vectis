# Reverse proxy transport feasibility audit

Status: source audit plus targeted live transport probes, 2026-09-24. The
pre-body takeover is a candidate, not a complete implementation choice. The
probes do not implement a production proxy route. The intended feature
contract is in [reverse-proxy-spec.md](reverse-proxy-spec.md).

## Decision

The candidate reduces changes to Kore's ordinary body path by letting Kore
parse the request line and headers, then handing only selected proxy requests
to a proxy-owned connection handler. Live probes now establish handoff,
queued-response ordering, bounded 1 MiB cleartext and double-TLS relays,
coupled HTTP/1.1 fixed-length and chunked upload/download overlap with
downstream TLS, SSE delivery over HTTP/1.1 and HTTP/2 upstreams, and a
coupled HTTP/1.1 WebSocket upgrade over verified upstream TLS. The leading
output choice uses one bounded Kore send netbuf at a time. It is **not yet a
proven complete proxy architecture**: general HTTP framing and header translation,
WebSocket rejection and long-lived close behavior, all cleanup paths, and the
release HTTP/2 memory envelope remain open.

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
connection in libcurl multi, then sends an HTTP/1.1 WebSocket upgrade request
and masked frame through the still-attached easy handle. The local server
receives both exactly and returns a valid `101` plus an immediate frame in one
TLS write; the client receives both exactly. This passes with certificate and
hostname verification enabled. It proves the upstream TLS and raw-byte
transport mechanism, not full proxy handshake validation. [Libcurl documents
that a connect-only multi easy must remain
attached](https://curl.se/libcurl/c/CURLOPT_CONNECT_ONLY.html)
and that `curl_easy_recv()` must be drained before waiting for another socket
edge because TLS data can remain inside libcurl
([receive API](https://curl.se/libcurl/c/curl_easy_recv.html)).

The HTTPS connect-only test also retains the tunnel easy in one multi while a
regular HTTPS request to the **same origin** completes on another easy. The
fixture accepts a second TLS connection, checks the HTTP request and body,
then resumes the original raw WebSocket exchange. Twenty repetitions pass;
the fixture observes two TLS handshakes. This establishes that this bundled
libcurl keeps a completed connect-only connection unavailable for an ordinary
same-origin transfer even when both handles share a multi. It does not prove
simultaneous active HTTP transfers, socket-callback reentrancy, or worker
shutdown in a shared multi. The later worker-shared probe below exercises
those cases for HTTP/1.1 and an HTTP/2 SSE download. Concurrent HTTP/2
transfers and cancellation within that worker loop remain open.

The [HTTP/1.1 duplex test](../tests/unit/test_proxy_curl_duplex.c) pauses the
upload callback after its first four bytes. A local server sends a final
chunked response and its first body chunk while the upload is paused, then
reads the remaining upload bytes and sends the final response chunk. The
pinned debug libcurl 8.22.0 delivered `pong` before upload resume and
completed `pongdone` after it. This proves response progress during a paused
HTTP/1.1 upload in this build; it does not prove coupled event-loop integration
or cancellation behavior.

The [HTTP/2 duplex probes](../tests/unit/test_proxy_curl_http2_duplex.c) use
an h2-only local nghttp2 server over certificate- and hostname-verified TLS.
The client sends an eight-byte body from a read callback in four variants:
POST and body-bearing GET, each with a known or unknown body length. GET uses
`CURLOPT_UPLOAD` plus `CURLOPT_CUSTOMREQUEST`. It pauses after `ping`. The
server verifies `:method` and `:path`, waits until it has received those four
request bytes, and produces `pong`. Before the upload resumes, libcurl
delivers that response DATA frame to its write callback while the transfer
remains active. After resume, the server receives `rest`, observes request
end-of-stream, and finishes the response with `done`. Both known-length
requests have `Content-Length: 8`; both unknown-length requests have no
`Content-Length`. None carries `Transfer-Encoding` on the HTTP/2 leg.
The client verifies HTTP/2 negotiation, status 200, the full `pongdone`
response, and successful completion. All four variants passed twenty serial
repetitions. This proves small known- and unknown-length HTTP/2 uploads,
including a body-bearing GET, can overlap an early response in the pinned
bundle. Large concurrent duplex streams, HTTP/2 trailers, cancellation, and
the coupled Kore worker loop remain open.

The same four variants now connect to a URL with an intentionally different
path and query, then set `CURLOPT_REQUEST_TARGET` to a raw path containing
`%2F` and repeated query fields. They also set a rewritten `Host` header.
The nghttp2 peer observes the exact raw target in `:path` and the rewritten
host in `:authority`, while TLS still verifies the configured URL's
`localhost` certificate. Twenty serial runs of all four variants pass.
This establishes the required HTTP/2 request-target and authority mapping
in the pinned build; policy validation of untrusted targets and hosts still
belongs to the proxy implementation.

The [HTTP/1.1 chunked-trailer probe](../tests/unit/test_proxy_curl_chunked_trailers.c)
sets an unknown upload size, pauses after its first chunk, and uses libcurl's
trailer callback after the resumed final chunk. The local server observes the
exact chunk framing and `X-Trace` request trailer. It sends the first response
chunk before the upload resumes, then a second response chunk and `X-Final`
trailer. The client receives the first body bytes before upload resume and
the response trailer through the header callback. Twenty serial repetitions
pass with the pinned libcurl 8.22.0. This proves the documented callback
composition for a small HTTP/1.1 transfer, not Kore ingress framing, general
trailer policy, HTTP/2 request trailers, or large-stream memory bounds.

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

A second build of the same live fixture,
[`vectis_unit_kore_proxy_curl_loop_shared`](../tests/unit/CMakeLists.txt),
uses one worker-owned libcurl multi with a shared socket callback, timer,
watcher registry, and completion dispatch by `CURLOPT_PRIVATE`. It retains a
verified HTTPS WebSocket connect-only easy while an SSE transfer completes,
then finishes the raw tunnel. It also overlaps a 1 MiB SSE download with a
1 MiB POST upload and records at least two simultaneously active curl
transfers. On worker stop, it cancels the active stalled TLS transfer, removes
the shared watchers and timer, and frees the multi. Both this build and the
original per-exchange build passed ten serial repetitions. This proves that
the worker-owned event and lifetime model can run these HTTP/1.1 cases on
Kore's Linux epoll loop. The same worker-shared multi now receives a 1 MiB SSE
body from an h2-only local TLS server. The fixture generates bytes on demand,
curl verifies the certificate and hostname, ALPN selects `h2`, and
`CURLINFO_HTTP_VERSION` reports HTTP/2. The slow downstream client checks every
byte and the normal bounded Kore output queue limits still pass. Ten serial
repetitions and the full project gate pass. The fixture must stay open until
the downstream finishes: closing it immediately after generating the final
HTTP/2 DATA frame caused a TCP reset from unread client control frames, which
the test caught as a truncated response. This proves one HTTP/2 SSE transfer
through the worker loop, not a production libcurl memory cap. The spec's
separate `auto` and forced-HTTP/1.1 pools, connection reuse under load, and
BSD kqueue behavior remain
open.

The worker-shared probe now also runs sixteen concurrent HTTP/2 SSE downloads
of 16 MiB each through sixteen separate verified TLS connections. Each
downstream client reads headers, stops reading for four seconds, then resumes
and verifies every byte of its response. All sixteen curl download callbacks
pause, and the local nghttp2 server stops generating at roughly 110 MiB of
the 256 MiB total before clients resume. After the first two seconds of the
hold, upstream generated bytes and downstream chunk counts stay unchanged;
worker pump calls also stay flat over the following two seconds. The worker's
current RSS rises about 5 MiB from its first admitted scale request and stays
level while paused. The probe samples worker RSS in each download callback,
enforces a 32 MiB regression ceiling for the sixteen transfers, and keeps
Kore's output queue to one bounded netbuf per exchange. Five serial runs pass;
all 256 MiB arrive after resume. This proves coupled worker backpressure and
a measured memory envelope for this pinned Linux debug fixture. It does not
cover adverse headers, reuse under load, other target bundles, or a production
admission reserve.

A separate worker-loop probe receives one 4 KiB chunk from a live HTTP/2
response, resets the downstream socket, and checks that the proxy cancels its
easy handle. The HTTP/2 peer observes `RST_STREAM` promptly. The proxy then
completes a second SSE request through the same HTTP/2 session and TLS
connection; the fixture accepts only one upstream socket and verifies two
request stream IDs. Five serial runs passed. This establishes cancellation
and sequential reuse of one HTTP/2 connection.

The worker-shared probe also starts sixteen separate verified HTTP/2 TLS
connections and holds all downstream readers until every curl download
callback has paused. It then resets all sixteen downstream sockets. Every
exchange is canceled, and each HTTP/2 peer receives `RST_STREAM` before
closing; five serial runs pass. The fixture defers its body producer after
2 MiB per connection so it can keep reading control frames. An earlier
fixture version blocked in `SSL_write()` and could not observe resets until
its write timeout; this is a server-side observation limit, not evidence
that the worker failed to cancel. The worker's RSS rises about 3 MiB over
this batch in the pinned Linux debug build, below the probe's 32 MiB
regression ceiling. This validates simultaneous cancellation with
multiplexing disabled, but does not establish behavior for multiplexed
streams, connection reuse under load, or a production memory allowance.

Libcurl's [total connection limit](https://curl.se/libcurl/c/CURLMOPT_MAX_TOTAL_CONNECTIONS.html)
queues excess easy handles internally, while its [multi connection cache](https://curl.se/libcurl/c/CURLMOPT_MAXCONNECTS.html)
grows with added handles by default. A connection limit alone therefore does
not bound outstanding proxy exchanges or retained idle TLS connections. The
spec now requires Vectis admission before `curl_multi_add_handle()` and
explicit active/cache limits on both proxy pools. A worker probe with a
synthetic sixteen-exchange and sixteen-connection cap now pauses all sixteen
HTTP/2 transfers, returns local `503` for a seventeenth request before
creating an easy handle, and checks that upstream sees no seventeenth
request. After all sixteen cancellations, its reservations return to zero;
new requests to the original origin and a fresh origin are admitted and
reach their peers. Five serial runs pass. This proves the header-time
admission mechanism for one pinned Linux debug pool; a production numerical
allowance across protocol pools and release bundles remains open.

The admission probe also exposed a connection-reuse race: after canceled
streams caused the fixture to close their upstream TLS sockets, the next
same-origin request sometimes used a stale cached socket and failed before
headers. With one HTTP/2 stream per connection, the probe now sets
[`CURLOPT_FORBID_REUSE`](https://curl.se/libcurl/c/CURLOPT_FORBID_REUSE.html)
before removing a canceled easy handle. The peer then sees either
`RST_STREAM` or TCP closure, and same-origin recovery passes five serial
runs. Libcurl documents this option for closing a connection after use;
whether setting it at cancellation closes reliably in every pinned release
bundle still needs validation. Normally completed responses retain reuse.

Source inspection exposed a watcher lifetime difference that the Linux
probe had missed. [`bsd.c`](../vendor/kore/upstream/src/bsd.c) returns separate
read and write kqueue results with the same event pointer in one batch, while
the probe freed that pointer immediately on libcurl's `CURL_POLL_REMOVE`.
Its `kore_platform_disable_read()` call also deletes only the BSD read
filter, leaving a possible write registration. The probe now marks a removed
watcher retired, ignores a deliberately injected late callback, and frees
it on a one-shot timer after Kore finishes the current event batch. Both
per-exchange and worker-shared variants pass. Kore defers connection removal
until after the event batch; the probe now guards a second downstream
event after disconnect. The production path needs a proxy-local Linux/kqueue
interest adapter that removes both BSD filters and uses the same deferred
watcher lifetime. Native kqueue execution is still required before the
design is proven on Darwin or BSD.

The cleanup harness needed a separate ownership fix. Vectis calls
`setpgid()` for its Kore runtime, so the test runner and Kore parent/workers
are in different process groups. Stopping only the runner group left Kore
alive and made CTest wait on its inherited output pipe. The wrapper now keeps
its session leader alive while it verifies and stops the recorded Kore group,
then stops its own group. The shared worker probe's normal, forced-exit, and
timeout cases pass under CTest; each forced-cleanup case also passed ten
serial repetitions. This establishes test containment; production app-stop
behavior is covered by separate runtime checks and remains part of the proxy
lifecycle gate.

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

An additional repeated run exposed a takeover assumption: Vectis configures
Kore's header receive buffer to 64 KiB by default, so bytes already read after
the request headers can exceed the 8 KiB relay queue. The probe now borrows
that live Kore request buffer and feeds it upstream in 8 KiB pieces before
reading more from the downstream socket. The test sends a 32 KiB initial
payload with the request headers; repeated runs observed more than 8 KiB and
up to 32 KiB at handoff, and passed with an 8 KiB relay queue high-water. The
source buffer is retained by the sleeping request until the tunnel consumes
it. The 64 KiB Kore receive allocation belongs in the per-exchange memory
budget even though the relay makes no second full-size copy. This does not
yet prove general HTTP body framing or pipelined suffix handling.

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
frames, full HTTP framing, or HTTP/2 flow control.

The same [Kore worker-loop probe](../tests/unit/test_kore_proxy_curl_loop.c)
now runs a normal libcurl HTTP/1.1 transfer for a chunked SSE response. The
upstream fixture sends response headers, then waits until the downstream
client has received them before producing any body bytes. The client then
verifies a 1 MiB `text/event-stream` body incrementally while reading slowly.
The proxy probe holds at most one 16 KiB libcurl callback chunk and one
bounded Kore send netbuf; in one run it saw 262 downstream chunks, 55 libcurl
receive pauses, 460 response-pump calls, and an 8 KiB maximum Kore queue.
Ten serial repetitions pass. This establishes early header flush, incremental
body delivery, pause/resume, and a bounded application queue for this
cleartext HTTP/1.1 SSE fixture. It does not establish general header/status
translation, trailers, request upload framing, TLS on this HTTP transfer,
or production idle-timeout policy.

An added idle SSE fixture sends one chunk, waits without sending another,
then observes whether closing the downstream with TCP RST cancels libcurl and
closes its upstream socket. Before the change, the worker removed the idle
downstream fd from epoll; the upstream stayed open until the fixture deadline,
then its close caused a partial-transfer assertion. The probe now keeps an
edge-triggered `EPOLLRDHUP` watcher only while there is no pending output,
retains level-triggered output readiness, and checks `SO_ERROR` before
cancelling. It separately has the client close its write side after receiving
SSE response headers; the 1 MiB stream still completes. Five serial worker
runs and the forked failure/timeout cleanup tests pass. This establishes
idle cleartext RST cancellation and a post-header TCP half-close in the Linux
probe. It does not establish downstream TLS close behavior, a reset while a
Kore netbuf is queued, or idle timeout policy.

An isolated ordinary GET and the SSE fixture both failed when the client
half-closed its write side immediately after sending request headers. Kore's
Linux event mapping treats `RDHUP` as an error even when request bytes are
readable, so its connection event handler can disconnect before parsing them.
Patch [`0031`](../vendor/kore/patches/0031-kore-drain-readable-bytes-before-read-half-close.patch)
passes through an epoll read event with `RDHUP` unless it also has `EPOLLERR`
or `EPOLLHUP`. On kqueue it passes through a read `EV_EOF` only if unread
bytes remain and `fflags` reports no socket error, as described by the
[FreeBSD kqueue manual](https://man.freebsd.org/cgi/man.cgi?query=kqueue&sektion=2).
The takeover clears Kore's current read flag so its receive loop stops after
the pre-body callback and the proxy handler owns later EOF processing. With
that patch applied from the tracked series, the ordinary GET and both SSE
half-close timings pass in five serial Linux runs; the direct I/O and ordinary
pipelining probes also pass. The BSD branch is source justified but still
needs execution on a kqueue host. This change is shared event classification,
not a proxy route parser or a second transport loop.

The worker-loop probe now also forces two upstream TCP resets. A server
that resets before sending response headers produces one complete local
`502` with `Content-Length: 11` and a downstream close. A second server
waits until the client has received response headers and the first chunk,
then resets; the proxy closes downstream without a terminal chunk. In both
cases libcurl reports failure, the proxy removes its handle and watchers,
and the client observes the correct framing outcome. Five serial Linux runs
pass. This proves cancellation after an upstream transfer error in those
two phases. Those two tests alone do not prove a reset while Kore has queued
but unwritten response bytes. That boundary needs explicit output accounting
so a `502` is sent only when it cannot conflict with a partially written
response.

The same two failure phases now run against certificate- and
hostname-verified HTTPS upstreams. Each TLS fixture completes its handshake
and receives the proxy request; one sends headers and a partial chunked SSE
body before closing the TCP socket with RST, while the other resets before
response headers. The downstream receives the first chunk then closure
without a terminal chunk, or exactly one local `502`, respectively. Libcurl
reports an upstream failure, its certificate verification result is zero,
and both worker multi modes pass. The first test run exposed a fixture
lifetime error: its shared `SSL_CTX` had been freed before these new servers
started. Moving that fixture cleanup after both exchanges resolved the crash.
One full-suite run also exposed an existing upload fixture race: the client
could receive the early response before the server recorded its ready flag.
Both fixed-length and chunked upload checks now wait for that flag before
asserting it, preserving their early-response and incomplete-upload checks.
Other TLS failure points remain open.

A further fixture makes the upstream send valid `200` headers, waits until
the proxy has queued its sole header netbuf without attempting any write,
then resets the upstream socket. The probe removes the unwritten netbuf,
clears Kore's send timeout state, and sends exactly one complete local `502`
over both cleartext and certificate-verified downstream TLS. Both worker
multi modes pass. This establishes that the queued-but-unwritten boundary can
be handled through Kore's existing `net_remove_netbuf()` API. A real proxy
must track the first header write attempt explicitly: `SSL_write()` may leave
ciphertext pending even if Kore's netbuf offset stays zero, so that attempt
must conservatively commit the response. Kore's
[`kore_tls_write()`](../vendor/kore/upstream/src/tls_openssl.c) also marks
`NETBUF_MUST_RESEND` on TLS `WANT_READ`/`WANT_WRITE`; removing that netbuf
does not immediately discard it. A second downstream TLS fixture now injects
`WANT_READ` from the first response-header `SSL_write()`, then holds the
resulting retry state until the upstream resets. At cancellation the header
netbuf still has `s_off == 0` and `NETBUF_MUST_RESEND`; the proxy closes the
downstream connection without queuing a replacement `502`. The TLS client
receives at most a prefix of the original `200` header before closure. The
first attempt at this probe let a normal readiness event resume the write
before the upstream reset; the test-only hold makes the intended interleaving
deterministic. This proves the conservative write-attempt boundary for that
injected Linux TLS state. Natural TLS retry timing and other callback phases
remain open.

A queued-response SSE fixture holds its first body netbuf with a test-only
readiness gate. The client resets after the worker reports that queue state;
Kore's disconnect callback confirms the netbuf is still queued, libcurl is
cancelled once, and the upstream observes its socket close. A second fixture
injects one write error while the body netbuf is queued. It exercises the
`net_send_flush()` failure branch, confirms that the queued netbuf remains
present through the disconnect callback, and observes the same upstream
closure. Five serial Linux runs pass for both paths. An earlier version
relied on socket backpressure to leave a netbuf queued; it passed several
runs but later missed that exact pause within its deadline, so it was not a
deterministic lifecycle test. Natural backpressure is covered by the separate
slow-peer SSE and relay probes. These two fixtures establish cleartext
pending-netbuf cancellation and write-error cleanup; they do not execute a
real TCP reset inside the write call.

The same held-netbuf SSE fixture now runs over a certificate- and
hostname-verified downstream TLS connection. After receiving the response
headers, the TLS client releases the first upstream chunk, waits until the
worker has queued its body netbuf, and closes the socket with TCP RST without
sending TLS `close_notify`. Kore's disconnect callback confirms both TLS
ownership and a still-queued netbuf. The proxy cancels the libcurl transfer
once, and the upstream observes closure. Five serial Linux runs pass. This
proves cancellation of a pending Kore TLS output netbuf after an abrupt
downstream transport failure. That reset path does not force a failure inside
`SSL_write()` or prove an orderly TLS shutdown while a response is queued.

The write-error fixture also runs through a verified downstream TLS listener.
Its test-only filter BIO fails the first body write without a retry flag, so
Kore's actual `kore_tls_write()` calls `SSL_write()` and receives an error.
Both per-exchange and worker-shared curl variants reach the `net_send_flush()`
failure branch, disconnect with that netbuf pending, cancel the upstream once,
and observe the upstream close. This proves cleanup for a queued TLS response
when `SSL_write()` fails through its BIO. It does not reproduce every natural
kernel or TLS protocol error.

The worker-loop probe also runs a fixed-length 1 MiB POST through a normal
libcurl HTTP/1.1 upload callback. It borrows any post-header bytes already in
Kore's receive buffer, then reads further bytes into an 8 KiB queue only when
libcurl has consumed the preceding chunk. The upstream verifies every byte
incrementally and sends a first response chunk after 8 KiB, while the client
is still sending. The client receives that chunk before its upload completes,
then receives the final response after the upstream has read the full body.
The same POST now passes through a downstream HTTPS listener as well as the
cleartext listener. The TLS client sends 16 KiB records, forcing an 8 KiB
application read to leave plaintext in OpenSSL. A one-shot continuation
drains `SSL_pending()` bytes after libcurl consumes the prior queue; it fired
64 times in one passing run. The scheduler also follows `SSL_read()` retry
direction and Kore's queued TLS writer retry direction. The upstream sends
the first response while the TLS client is still uploading. The test asserts
upload callback pauses and an 8 KiB maximum input queue. An initial
cleartext run exposed a lifecycle bug: a client
write-side close can occur while bytes remain queued in the kernel. Treating
that event as a full disconnect cancelled libcurl with 16 KiB still unread.
The probe now drains the declared length before completing the response. This
proves fixed-length duplex transport through cleartext and downstream TLS in
the Kore worker. It does not prove chunked ingress framing, downstream upload
cancellation, or response status/header translation.

A test-only [OpenSSL filter BIO](https://docs.openssl.org/3.6/man3/BIO_meth_new/)
on the accepted downstream TLS connection now forces one `SSL_read()` to
return `WANT_WRITE` and the first queued body `SSL_write()` to return
`WANT_READ`. It forwards all other encrypted bytes through socket BIOs and
uses [BIO retry flags](https://docs.openssl.org/3.6/man3/BIO_set_flags/) to
make OpenSSL report the requested direction. The live 1 MiB upload observes
both retry states in the Kore worker, observes a fresh writable event after
the read retry and a fresh readable event after the write retry, and receives
its early and final response with the existing bounded queues. Both
per-exchange and worker-shared curl modes pass three serial debug runs and
the focused address/undefined-sanitizer run. The sanitizer run disables its
freed-allocation quarantine so the existing HTTP/2 live-RSS assertion in the
shared fixture can measure the paused transfer; with default quarantine that
assertion fails before reaching this TLS case. These forced downstream HTTP
upload cases do not prove natural retry frequency, WebSocket tunnel retries,
or kqueue scheduling.

The worker-loop probe also runs a WebSocket-style HTTP/1.1 upgrade across a
cleartext downstream and certificate-verified HTTPS upstream. The takeover
captures a masked client frame in the initial post-header bytes and holds it
while sending a rewritten opening handshake. The upstream fixture checks that
no frame is available before it sends `101` and an immediate server frame in
one TLS write. The probe validates the complete upstream `101`, upgrade and
accept headers, and selected subprotocol before forwarding any response
bytes; it then relays both early frames and a later frame in each direction
through the same bounded Kore/libcurl tunnel. The client verifies all bytes
and EOF, and the worker reports one successful upgrade and tunnel cleanup.
This proves the event-loop composition and boundary-byte ownership for this
fixed handshake. The probe deliberately uses fixed handshake fields and
assertions: it does not prove a production parser, rewrite policy, general
rejection handling, extension negotiation, large-fragment backpressure, long-lived
idle/close semantics, or downstream TLS for the upgrade route. The early
client frame is an adversarial handoff fixture; production clients should
wait for `101` before sending frames.

The pinned [libcurl OpenSSL send path](https://github.com/curl/curl/blob/curl-8_22_0/lib/vtls/openssl.c)
sets an internal receive need and returns `CURLE_AGAIN` when `SSL_write()`
reports `WANT_READ`; its receive path likewise returns `CURLE_AGAIN` when
`SSL_read()` reports `WANT_WRITE`. The public
[`curl_easy_send()`](https://curl.se/libcurl/c/curl_easy_send.html) and
[`curl_easy_recv()`](https://curl.se/libcurl/c/curl_easy_recv.html) interfaces
do not expose that internal direction. A raw watcher therefore needs combined
socket interest while an operation is pending, no repeated edge registration
for an unchanged interest mask, and a cancellable delayed retry after a
no-progress callback. Otherwise a TLS-only retry can stall or spin on a
writable fd.

The disposable worker-loop probe now injects one `CURLE_AGAIN` at each public
send/receive call boundary of a verified-TLS WebSocket tunnel. It withholds
send progress until a fresh upstream read event and receive progress until a
fresh upstream write event. A peer releases one extra WebSocket frame only
after the send retry is armed. Both per-exchange and worker-shared curl modes
observe the two wakeups, preserve exact frame bytes and ordering, finish the
tunnel, and keep the retry tunnel below 100 upstream callbacks. Three serial
runs of each mode pass. This tests the scheduler against the **observable
public libcurl outcome**; it does not force OpenSSL itself to return
cross-direction retries. A test-only query for `CURLINFO_TLS_SSL_PTR` after
connect-only completion returned a null internal pointer in both modes, so
direct BIO injection at that point is unavailable through that public
handle.

The retry tunnel now also has a no-edge fixture. After libcurl reads one
six-byte final WebSocket frame, the test wrapper holds that bounded chunk and
returns `CURLE_AGAIN` to model internally cached bytes unavailable on that
call. It releases the chunk only on a one-shot 25 ms Kore worker timer; no
new upstream application data is sent. Both worker modes deliver the frame
and close cleanly, with one injected pause and one timer firing each. Before
that final exchange, the tunnel remains idle for 1.5 seconds with at most
two extra upstream callbacks and then resumes. Three serial debug runs per
mode and the focused ASan run pass. This proves that an edge-triggered
interest mask plus a bounded delayed retry can make progress without a
writable-fd spin in the simulated public API state. The wrapper, not libcurl,
creates that state; actual OpenSSL transitions, much longer idle periods,
and native kqueue execution remain open.

A second verified-TLS WebSocket exchange now resets the cleartext downstream
socket immediately after the no-edge timer is armed. The first run exposed a
real gap in the disposable relay: its raw read branch asserted on
`ECONNRESET` instead of cancelling, and its event callback did not inspect a
socket error before pumping. With both paths cancelling through Kore's
disconnect callback, the upstream TLS fixture observes closure, the retry
timer is removed once before its one-second due time, and the earlier
successful tunnel's timer remains the only one to fire after waiting beyond
that due time. Three serial debug runs in each curl mode and focused ASan
pass. This proves the timer's normal disconnect lifetime for this Linux
probe; other callback phases, abrupt upstream errors, and native kqueue
batch lifetime remain open.

The worker-loop probe also sends that early client frame to a WebSocket route
whose verified HTTPS upstream returns `403` and a 1 MiB body. The upstream
splits its response header across TLS writes. The proxy waits for the
complete header, selects the non-upgrade path, discards the held early frame,
and relays the response through one bounded Kore send netbuf while a slow
client checks every byte. The upstream sees no frame before or after the
rejection. Ten serial repetitions of the coupled sequence pass; five
additional repetitions explicitly assert that the split header was observed.
This proves the connect-only socket can return to normal HTTP response
delivery after a rejected upgrade. The probe recognizes one fixed `403`
header; a general bounded HTTP/1.1 status/header parser, hop-by-hop header
translation, response rewrite policy, other rejection statuses, and premature
upstream close remain open.

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
consumer. A local four-transfer memory measurement exists below, but no
release-wide production allowance has been established yet. If that allowance
cannot satisfy the streaming memory budget, the architecture needs a
different HTTP/2 client transport or a narrower supported contract.

The Linux [HTTPS HTTP/2 pause probes](../tests/unit/test_proxy_curl_http2_pause.c)
use a local nghttp2 server over certificate-verified TLS. ALPN selects `h2`;
the client reports HTTP/2, disables multiplexing, and limits concurrent
streams to one per connection. Separate TLS connections carry either four
64 MiB responses or sixteen 16 MiB responses from the same origin. Each
response is generated incrementally. Every download callback pauses after
its first chunk. During the two-second pause the server generated about
15 MiB in the four-connection case and 60 MiB in the sixteen-connection
case, both below half the total response volume. In representative runs,
whole-process RSS moved from 9.0 to 11.1 MiB with four connections and
from 6.7 to 13.0 MiB with sixteen. RSS stayed level during the pause, then
all 256 MiB of body bytes were verified after resume. Both variants passed
five serial repetitions. The test enforces a whole-process sampled-peak
RSS delta of at most 4 MiB per transfer and at most 4 MiB additional growth
after all transfers pause. It samples current RSS in each download callback
and at the pause, wait, completion, and cleanup boundaries. It separately
checks growth in Linux `ru_maxrss` from its pre-transfer value. The latter
can already exceed current RSS before the test begins, so its absolute value
is not a valid transfer peak baseline.
The server and client share the process, so this includes both sides. These
are regression ceilings for the pinned build, not an enforceable libcurl
allocation bound or a production per-connection allowance. Large headers,
adversarial compression, connection reuse, much longer idle periods, and
worker-integrated cancellation still need admission and stress tests before
that allowance is final. Automatic decompression must stay disabled for the
proxy because
[libcurl can cache uncompressed data while paused](https://curl.se/libcurl/c/curl_easy_pause.html).

A cancellation variant now pauses sixteen 16 MiB HTTP/2 responses, lets the
upstream run for two seconds, then removes all easy handles without resuming
them. It verifies each transfer still had zero delivered body bytes, all
sixteen TLS connections and server threads close within ten seconds, and
the process descriptor count returns from 8 to 5 in the local fixture.
Five serial runs pass. In those runs, the server generated about 60 MiB in
total while process RSS rose roughly 6 MiB above baseline and stayed flat
during the pause; post-cleanup RSS was about 4.5 MiB above baseline. This
proves cancellation progress and a measured regression envelope in the pinned
Linux bundle. The server is in the same process, so it still cannot establish
a client-only reserve.

The same sixteen-connection probe now also runs with its nghttp2/TLS server
in a separate child process. The child exits if its controller dies, and
the controller waits for its termination after every run. Five resume and
five cancellation runs pass. In those runs, client-process RSS peaked at
most 4.3 MiB above its pre-transfer baseline while the separate server
generated about 60 MiB before resume or cancellation. Client RSS stayed
level during each two-second pause; all 256 MiB arrived after resume, or
all sixteen transfers cancelled before any body bytes were delivered. The
client descriptor count returned from 7 to 5. The test enforces a wider
16 MiB aggregate client RSS regression ceiling for sixteen connections.
That ceiling is not a hard libcurl allocation cap. It measures the pinned
Linux debug bundle under this one header pattern and connection lifetime.

The same isolated sixteen-connection test was compiled against the pinned
x86_64 Linux GNU release `c.pkt.systems` archive, then run with resume and
cancellation. Both pass: the release client RSS rose about 4.1 MiB above its
pre-transfer value and stayed level while paused, and all sixteen connections
closed after cancellation. This also executes the release archive's libcurl
feature checks for asynchronous DNS, HTTP/2, and TLS. The first manual release
run exposed a probe error: the process began with a roughly 230 MiB inherited
`ru_maxrss` high-water mark despite only about 13 MiB of current RSS, so an
absolute high-water assertion failed despite no further high-water growth. The
callback sampling and high-water-delta checks above correct that false
failure. They do not make the 16 MiB regression ceiling a hard allocation
bound or prove other target archives.

The distinct pinned x86_64 Linux musl `c.pkt.systems-0.10.0` archive was
downloaded into the ignored build directory and verified against the SHA-256
in [`scripts/deps.sh`](../scripts/deps.sh). A static musl build of the same
isolated sixteen-connection probe passed five separate-server resume runs
and five cancellation runs. Its client baseline was about 5.7 MiB RSS and
peak about 9.9 MiB; the pause stayed level, all 256 MiB arrived on resume,
and cancellation returned the client's descriptor count from six to four.
The probe executed its runtime asynchronous-DNS, HTTP/2, and TLS feature
assertions against this musl archive. This is a second release-bundle
measurement, not a hard libcurl allocation cap or a coupled Kore worker
allowance. Close-on-cancel in the musl Kore worker remains open.

The pinned aarch64 and armhf Linux archives, both GNU and musl, were each
downloaded into ignored build directories and verified against their SHA-256
entries in `scripts/deps.sh`. The same isolated sixteen-connection probe was
cross-compiled as a static binary for each archive and executed with QEMU user
emulation. One resume run and one cancellation run passed per archive: all
sixteen 16 MiB responses arrived after resume, no body was delivered on
cancellation, the client descriptor count returned from six to four, and the
runtime libcurl checks reported asynchronous DNS, HTTP/2, and TLS. RSS was
stable during each two-second pause. QEMU's host process RSS includes emulator
memory, so these runs establish functional pause/cancel behavior and feature
availability, not a native ARM memory allowance or timing bound. The GNU
static builds also emitted glibc warnings about runtime shared-library
requirements for dynamically loaded facilities. Native ARM execution and
other proxy behaviors on these archives remain open.

The [tagged libcurl 8.22.0 HTTP/2 source](https://github.com/curl/curl/blob/curl-8_22_0/lib/http2.c)
sets a 64 KiB initial stream window, a 10 MiB maximum stream window, and a
16 KiB chunk pool with room for up to 10 MiB of network input. It sets the
stream's local receive window to zero while output is paused. Those limits
explain why pausing can halt ingress, but they do not cover all nghttp2,
OpenSSL, header, allocator, or connection-reuse allocations. The same source
sets a much larger connection-level flow-control window. We infer that the
one-stream-per-connection invariant is essential to the proposed bounded
resource policy. A client-only measurement in a Kore worker, with header and
reuse stress, is still required before choosing a numerical admission
reserve.

The HTTP/2 probe asserts that runtime libcurl reports `AsynchDNS`, `HTTP2`,
and `SSL`. The host-debug and x86_64 Linux GNU release presets in
[`scripts/deps.sh`](../scripts/deps.sh) select the same
`c.pkt.systems-0.10.0-x86_64-linux-gnu` archive by SHA-256; both were
executed above. The x86_64 musl and four ARM Linux archives were executed
separately for the isolated HTTP/2 probe. The Darwin archive and other proxy
behaviors on all target archives still need explicit checks before a proxy
route is available on those targets.

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
The probe also takes over `Content-Length: 4` GET and OPTIONS requests even
though Kore marks those methods complete before its ordinary body branch.
Each fixture sends the four body bytes and an ordinary GET in one write;
both cleartext and downstream TLS return two ordered responses with the
expected body. Five serial supervised runs pass. This proves method-independent
fixed-length byte ownership at the hook, not outbound forwarding or general
framing validation.

The same hook can queue one local `HTTP/1.1 100 Continue` after accepting
headers. A client that waits for it then sends the four-byte body plus an
ordinary pipelined GET; both final `200` responses arrive in order over
cleartext and TLS. A separate test sends `Expect: 100-continue` with body and
the next GET already in the initial write to a locally forbidden path. The
hook emits only `403`, closes the connection, and never dispatches the
following GET. These are test-only admission and close-policy probes. A
production proxy must still validate expectation tokens, suppress upstream
`100`, coordinate libcurl upload setup, and handle early final responses.
In the TLS rejection, `SSL_read()` reports `SSL_ERROR_ZERO_RETURN` for the
peer's `close_notify` before the raw socket necessarily reaches EOF. The
client probe treats that TLS signal, together with `Connection: close`, as
closure of the HTTP exchange rather than requiring immediate fd EOF.

The header reader originally matched the first four bytes of a candidate
`Host` field name. A `Hostile: attacker.invalid` field before the real `Host`
therefore selected the wrong authority before the pre-body hook; the live
framed GET/OPTIONS probe failed over downstream TLS. Patch
[`0032`](../vendor/kore/patches/0032-kore-match-host-header-name-exactly.patch)
requires the exact `Host:` field name. The same cleartext and TLS fixture now
passes with that unrelated field ahead of `Host`.

The live hook also sees both `Content-Length` fields in a conflicting
duplicate pair, `Content-Length` alongside `Transfer-Encoding`, a second
`Host` field, and the HTTP/1.0 version flag. For each case, a test-only
callback checks the exact header instances and emits a marked `400` before
Kore's method-based body path. Cleartext and TLS pass five serial supervised
runs. The callback is a visibility probe, not the production framing parser;
generic syntax, value, and trailer validation remain unproved. The test must
compile with `KORE_USE_CURL` to match Kore's `struct http_request` layout when
it enumerates the header list.

The same probe exposed a separate header-count boundary. Kore passed at most
24 nonempty lines, including the request line, to its split array; an extra
`Transfer-Encoding` field after 22 padding fields and `Host` disappeared
without an overflow indication. The live pre-body hook then returned `200`
for a request whose final framing field was invisible. Patch
[`0033`](../vendor/kore/patches/0033-kore-validate-request-header-block.patch)
counts nonempty lines before the destructive split and rejects any excess
with `400`. A boundary probe proves that the largest accepted header block
reaches the hook intact and the next field is rejected before the hook on
cleartext and TLS. This changes ordinary Kore requests at that limit from
silent truncation to rejection; it does not implement the proxy's complete
framing or header policy.

The same destructive split hid everything after an embedded NUL in a field
value, and treated a bare CR as a field separator. Live probes confirmed both
cases could reach the hook and produce `200`. Patch `0033` now rejects either
byte pattern before splitting, on cleartext and TLS. [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html#section-5.5)
requires rejection or replacement of NUL in a field value before forwarding;
the patch chooses rejection. The remaining proxy admission checks still need
to validate framing values and hop-by-hop field semantics.

The pre-body view also retains the raw path `/raw-target/a%2Fb/%2e/c` and
query `q=1&q=2&plus=%2B&empty=` byte-for-byte over cleartext and downstream
TLS. This gives the proxy enough inbound data to construct an origin-form
upstream target without reconstructing escaping or repeated query fields
from Vectis's decoded route parameters. Target validation, base-path joining,
and forwarding to libcurl still need their own integrated tests.

That visibility does not imply a proxy route can admit the same request today:
an ordinary regex route on `/ordinary-raw-target/.*` returns `400` for
escaped slash (`%2F`), percent (`%25`), colon (`%3A`), and parent segment
(`%2e%2e`) targets over both cleartext and TLS. The bridge's shared decoder
rejects escaped slash; ordinary path validation rejects the other decoded
forms. A proxy selector that simply calls the existing decoded-path query
would therefore discard some syntactically valid raw targets before the
director can see them. The least intrusive candidate is proxy-specific
matching over a validated raw segment view while leaving ordinary route
validation and registration order intact. Its overlap
semantics and security checks need executable proof; widening the shared
ordinary decoder would change unrelated routes.

Source inspection narrows that candidate. The existing body-policy query
validates the decoded path before scanning routes in registration order; its
matcher treats a parameter as one slash-delimited segment and compares regexes
against the supplied path. Registration forbids percent escapes in literal or
parameter route patterns, while regex patterns can contain them. Therefore a
raw fallback can scan only proxy routes after ordinary selection rejects the
path, and treat `%2F` as part of one segment. The ordinary selector has a
static-site trailing-slash exception to its path validator, so that exception
and the static `405` check must run before deciding whether fallback is
eligible. Proxy parameter captures would contain escaped bytes in this
fallback. Running it after a valid
decoded-path `404` would change route precedence and is excluded. The current
decoder reports malformed escapes and disallowed escaped bytes through the
same error status, so a raw fallback needs an independent, strict validator;
it cannot infer safety from the decoder's error code.

The new private selector probe tests a less intrusive representation: a
proxy route can be registered as an ordinary handler with a private marker
function. The existing body-policy scan now reports its selected handler and
userdata, so normal-path admission needs no new route kind or matcher. A
separate raw fallback filters the same registry to marker handlers and reuses
its method, parameter, and regex matcher after strict path validation. It
returns its
matched userdata. A two-proxy-route test distinguishes their configurations
without a second match. The validator rejects invalid percent triplets,
controls, backslashes, dot traversal after one decode, and percent triplets
produced by a second decode. This internal probe does not
add public proxy registration or connect upstream.

The focused [route-selection test](../tests/unit/test_proxy_route_selection.c)
passes with ordinary regex and marker parameter routes in both registration
orders. Normal decoded paths select the first registered route; an encoded
slash stays inside one raw proxy parameter; encoded percent and colon retain
their escaped spelling. Malformed and unsafe paths fail, including
double-encoded traversal. A static directory keeps its trailing-slash path
exception and its `405`/`Allow` result for a disallowed method. The
[live pre-body probe](../tests/unit/test_kore_prebody_handoff.c) uses Kore's
actual decoder and that registry over cleartext and TLS. It confirms normal
first-match behavior, encoded-unreserved decoding, raw fallback admission,
and rejection of malformed escapes in both route orders. It now also checks
an application WebSocket route overlapping both marker and ordinary routes:
valid upgrades reach the existing `101` handshake, while missing upgrade
headers and `h2c` attempts reach its `400` response, over cleartext and TLS.
The first combined probe showed that the WebSocket matcher returns a
path-validation error for escaped targets that raw proxy fallback should
consider. The corrected selector validates the decoded path separately and
skips WebSocket matching only for an invalid ordinary path. It enters raw
fallback only for that condition or decode failure. Errors from a valid-path
WebSocket lookup cannot become raw fallback. A static directory registered
before the overlapping ordinary and proxy routes serves its file over
cleartext and TLS. POST gets
`405` with `Allow: GET, HEAD`, while an escaped path under that prefix enters
proxy-only raw fallback. The unit test still covers the static trailing-slash
path exception. The live probe now also registers a proxy marker and a
streaming-upload route on overlapping POST paths in both registration orders
in each app. A four-byte body reaches the earlier live-upload route's write
callback, which checks bytes incrementally, and its finish response; the
earlier marker takes over at headers time.
An escaped POST target enters proxy-only raw fallback under that prefix.
These test-only selections compose; production admission and complete header
framing remain open.
The body-policy query now optionally retains captured parameters in a
caller-owned request. The focused test proves that a failed parameter route
does not leak its capture into the following winner, while the live pre-body
probe observes the winning parameter after decoded-path selection over TLS.
Production admission must carry that request through `preflight` and
`rewrite` without repeating route matching.

The [Kore worker curl-loop probe](../tests/unit/test_kore_proxy_curl_loop.c)
now covers the next transport boundary. Its pre-body hook receives
`/sse/a%2Fb/%25/%3A?x=1&x=2&raw=%2F`, retains the escaped path and repeated
query fields, prepends `/upstream`, and passes the resulting origin-form
target through `CURLOPT_REQUEST_TARGET`. A dedicated HTTP/1.1 upstream checks
the exact request line, while the downstream receives a complete 1 MiB SSE
body through bounded queues. Both per-transfer and worker-shared curl multi
variants pass on Linux. This proves raw-target transport through the composed
hook and curl path for the fixture; it does not prove proxy route admission,
raw-path validation, arbitrary director rewrites, or HTTP/2 upstream target
forwarding in the worker loop.

The same handoff probe now accepts a 1 MiB chunked POST with no
`Content-Length`, before Kore's ordinary `411` path. Its temporary framer
validates each body byte without storing the body, uses later receive windows
no larger than 8 KiB, parses bounded size and trailer lines, and restores
Kore's reader for a following ordinary GET. A second fixture puts a complete
small chunked body, its
trailer, and that GET in one TCP write; the framer asserts that the borrowed
post-header view contains the pipelined suffix. The small fixture also passes
in one downstream TLS write. Ten serial repetitions pass under the forked
test supervisor; an intentional early-exit case also verifies its forked
parent and worker cleanup. This proves byte ownership and replay for these
chunked cases, including the transition from borrowed bytes to later socket
reads.

The framer above is a probe sink; it does not feed libcurl. A separate
[Kore worker and libcurl multi probe](../tests/unit/test_kore_proxy_curl_loop.c)
now composes the two halves over cleartext and TLS downstream HTTP/1.1. It
accepts a 1 MiB chunked POST, incrementally decodes through one 8 KiB raw
buffer and one 8 KiB decoded buffer, pauses curl's upload read callback when
no decoded bytes are available, resumes after later downstream input, and
forwards the declared `X-Trace` trailer using libcurl's trailer callback.
Its upstream fixture verifies every body byte, chunked framing, and trailer.
It sends an early chunked response after the first 8 KiB; the downstream
client verifies that `pong` arrives before its upload finishes and then
receives `done` after the trailer. Both per-transfer and worker-shared curl
multi variants pass.
The test measures a maximum 8 KiB decoded queue, at least one curl upload
pause on each transfer, and repeated full-buffer read suppression under
16 KiB cleartext client chunks. The downstream TLS client also streams
1 MiB with a declared trailer and observes the early response before upload
completion. A second worker fixture keeps the downstream connection alive
after the curl response drains, restores Kore's ordinary reader, and serves
a following `/health` request on the same socket. It covers both a complete
trailer and next request in one write, where the decoder saves a 60 byte
suffix, and a next request sent separately while the upstream holds its
final response, where no suffix is saved. Both cases pass over cleartext and
downstream TLS in the per-transfer and worker-shared multi configurations;
five serial runs of each configuration pass. The test asserts one restoration
per case, exactly two ordered responses, and a bounded saved suffix.
Malformed framing rejection, broader request/response framing policies,
adversarial TLS record boundaries and retry directions, and cleanup with a
saved suffix during cancellation remain gates.

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

Route selection needs a Vectis-specific resolution step before the proxy
claims the connection. The bridge currently calls
[`vectis_internal_match_websocket()`](../src/vectis.c) first for every GET;
that matcher uses method and path, not the incoming upgrade tokens. Kore's
application WebSocket handshake then accepts or rejects the request. For
remaining requests, `vectis_internal_route_body_policy()` scans every route
kind in registration order. The bridge consumes a selected live-upload route
before ordinary dispatch, which scans handler kinds only. A live runtime test
now covers both registration orders for a regex buffered handler overlapping
a literal live-upload route: the earlier handler returns its buffered result,
and the earlier live-upload route returns its streamed result. Two runtime
fixtures register an ordinary handler before an overlapping application
WebSocket route: a valid upgrade receives `101`, while missing upgrade
headers and an `h2c` upgrade attempt take the WebSocket handshake path and
receive `400`. Exact duplicate method/path-kind/path registrations are
rejected, but regex and literal paths may overlap. The marker-route candidate
preserves this precedence by checking application WebSocket routes first,
then using the body-policy query's selected handler to identify a proxy
marker. It needs neither a new route kind nor a rewrite of ordinary dispatch.
Static directory routes register for all methods; a static winner must
continue through the allowed-method check to preserve its 405/`Allow`
outcome. The live marker overlap passes in both registration orders; composed
production admission is still open.

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
interest helper and native test. Kore's Linux backend updates one epoll
registration with the combined read/write mask; its BSD backend adds or
deletes separate `EVFILT_READ` and `EVFILT_WRITE` filters and can invoke the
connection callback once per filter in a single wait cycle. The production
scheduler must map the same proxy read/write interest state to those distinct
operations, including the case where neither direction is armed. The Linux
probe's `EPOLL*` masks cannot serve as that portable boundary. Its `EV_EOF`
handling must also preserve data reported with a read EOF, as the Linux
`EPOLLRDHUP` path now does. Source inspection establishes the required
translation, but this Linux host cannot execute the native kqueue path.

The downstream TLS upload now forces both cross-direction retry states and
exercises continuation after OpenSSL holds decrypted bytes. The earlier
queue-drain probe covers a buffered response and a cleartext live stream
completion callback, but not stream abort during takeover. The bounded
Kore-output path passes the predecessor/keepalive sequence over cleartext and
TLS. Queued TLS output cancellation and a fault-injected failure inside
`SSL_write()` pass. Worker shutdown with an active libcurl handshake and
sustained cleartext and double-TLS exchanges pass; cleartext SSE delivery and
idle RST cancellation pass. Tunnel TLS retries and general HTTP framing
remain open.

An assertion failure in a probe previously left its Kore parent and worker
alive after the controller exited. The two Linux proxy probes now run through
[a supervisor](../scripts/run_forked_kore_test.sh) in separate temporary build
directories. It verifies the executable recorded in `kore.pid`, stops that
exact parent and its known workers on exit, and removes the generated files.
Dedicated tests force an immediate controller exit and a timeout after Kore
starts; both pass, and the recorded parent/worker PIDs and temporary
directories are gone afterward. Normal runs of both probes also pass through
the supervisor. This covers those two probes; the broader Kore runtime test
harness and production proxy cancellation paths still need their own proof.

## Findings from the current source

| Boundary | Evidence and consequence | Feasibility |
| --- | --- | --- |
| Header selection | Vectis registers one catch-all Kore route in [`vectis_kore_bridge.c`](../src/vectis_kore_bridge.c); its own route selection runs later. [`http_header_recv()`](../vendor/kore/upstream/src/http.c) has a point after the header list is built and before method-based body checks. Patch `0030` adds an optional callback there. Patch `0032` makes the initial Host selection exact. The existing `on_headers` hook runs after initial body delivery and is skipped by some zero-length paths. | Hook timing passes a live probe. A valid `Hostile` field before `Host` no longer prevents TLS admission. Duplicate `Content-Length`, `Content-Length` plus `Transfer-Encoding`, a second `Host`, and HTTP/1.0 are visible at the hook and receive marked local `400` responses in a test-only callback. The bridge gives application WebSocket routes priority for every GET; body policy scans all route kinds in registration order and handles a selected live upload before ordinary dispatch. Live overlap tests pass in both registration orders for buffered versus live-upload routes and confirm WebSocket priority over an earlier ordinary handler. Exact duplicate routes conflict, but regex/literal overlaps are allowed. The body-policy result now reports the winning handler; a private marker route passes focused selection and live pre-body overlap probes. The combined live marker and application WebSocket overlap now passes over cleartext and TLS, including malformed handshakes; live static overlap now preserves file serving, 405/Allow, and proxy-only raw fallback; test-only marker versus live-upload POST selection now passes in both registration orders on cleartext and TLS, including the live body callback; production admission and framing remain open. |
| Connection handoff | [`kore_connection_event()`](../vendor/kore/upstream/src/connection.c) calls the replaceable `connection->handle`. [`net_recv_flush()`](../vendor/kore/upstream/src/net.c) invokes the receive callback while processing an event. The handoff probe replaces both connection and receive handlers and owns the initial suffix. Patch `0031` lets readable bytes reach the parser when an orderly write half-close accompanies them. | Coalesced and split input passes on Linux, including downstream TLS. A 1 MiB chunked POST and a same-write chunked body with a pipelined GET also pass, including the latter over TLS. Direct-I/O ordinary HTTP takeover restores Kore's event handler and serves two subsequent ordinary requests. Ordinary GET and SSE half-close with headers pass on Linux. The composed worker probe covers bounded chunked upload with curl backpressure, and replays a following ordinary GET after the live curl transfer in both same-write and split-write cases over cleartext and TLS. Full framing policy, kqueue execution, and cleanup races remain open. |
| Body framing and pipelining | Kore's ordinary request path is method-based and has no incoming chunked decoder. Before patch `0029`, `http_header_recv()` could drop bytes after a header-only request and pass surplus across a fixed body boundary to `http_body_update()`. | Patch `0029` covers ordinary byte boundaries. Pre-body takeover receives framed GET and OPTIONS bodies followed by an ordinary GET over cleartext and TLS. A bounded chunked probe validates 1 MiB incrementally, parses a trailer, and replays a following GET from borrowed and later socket bytes. A separate Kore worker probe couples 1 MiB chunked ingress to libcurl upload and trailer output with an early response and two 8 KiB buffers over cleartext and downstream TLS in both curl multi modes; a cleartext burst fills the decoded queue. The live curl worker restores Kore and serves an ordered ordinary GET from either a saved same-write suffix or later unread bytes. The TLS upload also passes forced cross-direction `SSL_read` and `SSL_write` retries. General framing policy, malformed-input handling, and TLS retry behavior in other phases remain unproved. |
| Backpressure and TLS | [`net_recv_flush()`](../vendor/kore/upstream/src/net.c) has no pause outcome, so taken-over input uses direct nonblocking fd/`SSL *` reads. Output copies one bounded chunk into Kore's send queue and uses its TLS writer; `SSL_want()` identifies queued output's retry direction. The worker probe measures an 8 KiB scratch queue plus one 8 KiB Kore netbuf. [OpenSSL permits either retry direction](https://docs.openssl.org/3.6/man3/SSL_write/). | Slow cleartext, HTTPS-upstream, and double-TLS relays pass on Linux. A filter BIO forces `SSL_read`/`WANT_WRITE` and `SSL_write`/`WANT_READ` in the coupled HTTP upload. Tunnel retries and kqueue remain open. |
| TLS buffered data | [OpenSSL documents](https://docs.openssl.org/3.0/man3/SSL_pending/) processed and unprocessed records that can remain after the socket stops reporting readable. An edge-triggered handler must drain buffered application data when capacity resumes, but it must not spin on a record that cannot yet produce application bytes. | The downstream TLS upload probe drains pending plaintext through a one-shot continuation after each bounded consumer advance and now exercises both forced cross-direction retry states. Adversarial partial records and other TLS phases remain open. |
| EOF and half-close | [`net_read()`](../vendor/kore/upstream/src/net.c) disconnects the whole connection on a zero-byte read; `kore_tls_read()` treats `SSL_ERROR_ZERO_RETURN` as an error. The direct-I/O probe keeps writing after TCP EOF and after a TLS client `close_notify`. The curl-loop probe distinguishes an idle SSE TCP RST from a post-header write half-close using `SO_ERROR` while retaining an idle epoll watcher. | Cleartext and TLS direct-I/O half-close exchanges pass. The SSE probe passes both cleartext half-close timings, idle RST cancellation, and abrupt downstream TLS cancellation with queued output. Plain TCP and verified HTTPS upstream resets before and after response commitment pass. Pre-header TLS half-close, other TLS upstream error phases, and close deadlines remain open. |
| Response output | `net_send_stream()` callbacks need an independent lifetime because Kore can invoke them during connection removal after freeing `hdlr_extra`. `net_send_queue()` instead copies a bounded chunk and needs no proxy completion callback. The proxy detects queue drain in its connection event handler before resuming upstream reads. Its first flush is deferred beyond the pre-body hook to avoid reentering a predecessor stream callback. | One 8 KiB Kore netbuf and one 8 KiB scratch buffer suffice for the 1 MiB slow-peer probe over cleartext and double TLS. The queue-based writer preserves a live streamed predecessor, its own response, and an ordinary successor over cleartext and TLS. An upstream reset while the proxy's sole header netbuf is queued but unwritten removes that netbuf and returns one `502` over cleartext and TLS downstreams. A reset after an injected first-header TLS `WANT_READ` with zero netbuf offset instead closes downstream without a replacement response. A reset with a queued cleartext or TLS body netbuf, a fault-injected cleartext writer error, and a fault-injected BIO failure inside TLS `SSL_write` cancel cleanly. Natural TLS write errors and other callback phases remain open. |
| Request/accounting lifetime | A taken-over GET may already have `HTTP_REQUEST_COMPLETE`. [`http_request_sleep()`](../vendor/kore/upstream/src/http.c) prevents normal dispatch, and connection removal wakes attached requests for deletion. A sleeping SSE request still counts against `http_request_limit` and retains the header allocation; Vectis defaults the header limit to 64 KiB and the request limit to max connections. | Ownership path exists; admission and memory measurements must include long-lived request/header objects. Any early release needs its own logging, timeout, and cleanup proof. |
| Timers and shutdown | [`kore_connection_check_timeout()`](../vendor/kore/upstream/src/connection.c) still enforces the header timer after pre-body takeover unless the proxy clears it. Worker teardown runs before [`kore_connection_cleanup()`](../vendor/kore/upstream/src/worker.c). Vectis exposes the worker teardown hook through its static-runtime symbol table. | A one-second timer killed an idle takeover before the fix; clearing `http_timeout` preserved it. Active downstream connection was observed at teardown and disconnected once. A stalled upstream TLS handshake was cancelled with its curl handle, Kore socket watchers, and deadline timer before event-loop cleanup. TCP upstream resets before and after response commitment, queued output abort over cleartext and TLS, and fault-injected cleartext and TLS write errors pass worker-loop probes. Other callback phases and timeout policies remain open. |
| Upstream transport | The local debug bundle has libcurl 8.22.0 with asynchronous DNS, HTTP/2, and TLS. Kore's wrapper buffers responses and removes completed easy handles, so the proxy needs its own multi transport. [Libcurl requires](https://curl.se/libcurl/c/CURLOPT_CONNECT_ONLY.html) a connect-only WebSocket handle to remain in its multi while raw send/receive uses its socket. | Plain TCP and verified HTTPS connect-only, same-origin ordinary HTTPS on a multi retaining a connect-only tunnel, early response during paused HTTP/1.1 and HTTP/2 uploads, Linux Kore worker-loop connect-only/tunnel handoff, worker-shared HTTP/1.1 multi with overlapping SSE/upload and retained WebSocket tunnel, one completed HTTP/2 SSE download and sixteen concurrently paused and resumed HTTP/2 SSE downloads in that worker multi, 1 MiB coupled cleartext and double-TLS relays, a fixed split-header WebSocket `403` with a 1 MiB body, and standalone four- and sixteen-connection paused/resumed HTTPS HTTP/2 probes pass. A downstream reset of a live HTTP/2 stream sends `RST_STREAM`; a second SSE response completes on that same TLS connection in the worker loop. Sixteen paused HTTP/2 transfers on separate TLS connections also cancel with one `RST_STREAM` each under the default reuse policy. With close-on-cancel, all sixteen cancel by `RST_STREAM` or TCP closure, a seventeenth exchange receives `503` before curl admission, and same-origin and fresh-origin follow-on requests succeed. Separate protocol pools in Kore, general HTTP framing, other WebSocket rejection cases, other target bundles, multiplexed HTTP/2 cancellation, reuse under load after normal completion, and a production HTTP/2 memory allowance remain open. |

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
