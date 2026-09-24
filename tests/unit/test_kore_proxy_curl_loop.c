#include <arpa/inet.h>
#include <assert.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#if defined(VECTIS_PROXY_SHARED_MULTI)
#include <nghttp2/nghttp2.h>
#endif
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <kore/http.h>
#include <kore/kore.h>
#include <vectis/vectis.h>

#define RELAY_BUFFER_SIZE 8192
#define RELAY_PAYLOAD_SIZE (1024 * 1024)
#define RELAY_INITIAL_BYTES 32768
#define SSE_CHUNK_SIZE 4096
#define SSE_BODY_SIZE (1024 * 1024)
#if defined(VECTIS_PROXY_SHARED_MULTI)
#define H2_SCALE_CONNECTIONS 16
#define H2_SCALE_BODY_SIZE (16u * 1024u * 1024u)
#endif
#define HTTP_BODY_BUFFER_SIZE 16384
#define UPLOAD_BODY_SIZE (1024 * 1024)
#define CHUNK_LINE_SIZE 128

enum upload_chunk_phase {
  UPLOAD_CHUNK_SIZE,
  UPLOAD_CHUNK_DATA,
  UPLOAD_CHUNK_DATA_CR,
  UPLOAD_CHUNK_DATA_LF,
  UPLOAD_CHUNK_TRAILERS,
  UPLOAD_CHUNK_COMPLETE
};

static const char relay_response[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 1048576\r\nConnection: close\r\n\r\n";
static const char ws_upstream_request[] =
    "GET /chat HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Protocol: chat\r\n\r\n";
static const char ws_client_request[] =
    "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Protocol: chat\r\n\r\n";
static const char ws_retry_client_request[] =
    "GET /ws-retry HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Protocol: chat\r\n\r\n";
static const char ws_retry_abort_client_request[] =
    "GET /ws-retry-abort HTTP/1.1\r\nHost: localhost\r\n"
    "Upgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Protocol: chat\r\n\r\n";
static const char ws_reject_client_request[] =
    "GET /ws-reject HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Protocol: chat\r\n\r\n";
static const char ws_response[] =
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
    "Sec-WebSocket-Protocol: chat\r\n\r\n";
static const char ws_reject_response[] =
    "HTTP/1.1 403 Forbidden\r\nContent-Length: 1048576\r\n"
    "Connection: close\r\n\r\n";
static const unsigned char ws_client_early[] = {
    0x81, 0x84, 0x11, 0x22, 0x33, 0x44, 'p' ^ 0x11, 'i' ^ 0x22,
    'n' ^ 0x33, 'g' ^ 0x44};
static const unsigned char ws_client_later[] = {
    0x81, 0x84, 0x55, 0x66, 0x77, 0x88, 'm' ^ 0x55, 'o' ^ 0x66,
    'r' ^ 0x77, 'e' ^ 0x88};
static const unsigned char ws_server_early[] = {0x81, 0x04, 'p', 'o', 'n', 'g'};
static const unsigned char ws_server_later[] = {0x81, 0x04, 'd', 'o', 'n', 'e'};
static const unsigned char ws_server_retry[] = {0x81, 0x04, 'm', 'o', 'r', 'e'};

struct echo_server {
  int listener;
  unsigned short port;
  pthread_t thread;
  SSL_CTX *tls_ctx;
  volatile int allow_body;
  int ws_retry_mode;
  int ws_retry_abort_mode;
  int tls_reset_before_mode;
  int queue_abort_mode;
  int hold_final;
  volatile int release_final;
  size_t h2_body_size;
  int h2_abort_mode;
  int h2_cancel_mode;
};

struct loop_metrics {
  unsigned connect_done;
  unsigned curl_watch_removed;
  unsigned retired_watch_callbacks_ignored;
  unsigned tunnel_done;
  unsigned downstream_done;
  unsigned disconnects;
  unsigned max_curl_watchers;
  unsigned pending_started;
  unsigned stall_accepted;
  unsigned worker_cancelled;
  unsigned active_watchers_at_teardown;
  unsigned timers_at_teardown;
  unsigned relay_done;
  unsigned tls_connect_done;
  unsigned relay_read_pauses;
  unsigned relay_write_pauses;
  unsigned relay_pump_calls;
  unsigned downstream_tls_read_want_write;
  unsigned downstream_tls_write_want_read;
  unsigned downstream_tls_write_pauses;
  unsigned downstream_tls_close_notify;
  unsigned tls_pump_calls;
  unsigned tls_up_send_again;
  unsigned tls_up_recv_again;
  unsigned tls_down_write_want_write;
  unsigned tls_down_read_want_read;
  unsigned tls_tunnel_events;
  unsigned tls_downstream_events;
  unsigned relay_continuations;
  size_t relay_max_queued;
  size_t relay_max_kore_queued;
  size_t relay_max_initial_bytes;
  unsigned relay_max_kore_buffers;
  unsigned http_done;
  unsigned http_abort_cancelled;
  unsigned http_abort_upstream_closed;
  unsigned http_half_closed;
  unsigned http_upstream_failed;
  unsigned http_tls_upstream_failed;
  unsigned http_local_errors;
  unsigned http_queue_held;
  unsigned http_queue_abort_cancelled;
  unsigned http_queue_abort_upstream_closed;
  unsigned http_queue_pending_at_disconnect;
  unsigned http_tls_queue_cancelled;
  unsigned http_write_failures;
  unsigned http_write_fail_cancelled;
  unsigned http_write_fail_pending_at_disconnect;
  unsigned tls_write_call_failures;
  unsigned tls_read_want_write_injected;
  unsigned tls_read_want_write_observed;
  unsigned tls_read_write_wakeups;
  unsigned tls_write_want_read_injected;
  unsigned tls_write_want_read_observed;
  unsigned tls_write_read_wakeups;
  unsigned upload_done;
  unsigned chunked_upload_done;
  unsigned chunked_trailer_called;
  unsigned chunked_upload_pauses;
  unsigned chunked_input_pauses;
  unsigned chunked_pipeline_restored;
  unsigned chunked_pipeline_empty_restored;
  size_t chunked_pipeline_max;
  unsigned upload_pauses;
  unsigned upload_tls_pending_resumes;
  size_t upload_max_queued;
  unsigned ws_upgraded;
  unsigned ws_rejected;
  unsigned ws_reject_header_fragments;
  unsigned ws_retry_send_injected;
  unsigned ws_retry_recv_injected;
  unsigned ws_retry_read_wakeups;
  unsigned ws_retry_write_wakeups;
  unsigned ws_retry_tunnel_events;
  unsigned ws_retry_cached_injected;
  unsigned ws_retry_timer_fired;
  unsigned ws_retry_timer_armed;
  unsigned ws_retry_timer_cancelled;
  unsigned ws_retry_upstream_closed;
  unsigned ws_retry_idle_events;
  unsigned http_body_pauses;
  unsigned http_chunks;
  unsigned http_headers_ready;
  unsigned http_pump_calls;
  size_t http_max_kore_queued;
  unsigned http_max_kore_buffers;
#if defined(VECTIS_PROXY_SHARED_MULTI)
  unsigned shared_running_max;
#endif
  unsigned h2_negotiated;
  unsigned h2_requests;
  unsigned h2_completed;
  size_t h2_generated;
  size_t h2_down_received;
  unsigned h2_down_done;
#if defined(VECTIS_PROXY_SHARED_MULTI)
  unsigned h2_scale_admitted;
  unsigned h2_scale_paused;
  unsigned h2_scale_headers;
  unsigned h2_scale_done;
  unsigned h2_abort_rst_seen;
  unsigned h2_abort_tcp_closed;
  unsigned h2_abort_terminal;
  unsigned h2_abort_cancelled;
  unsigned h2_abort_reuse_done;
  unsigned h2_cancel_headers;
  unsigned h2_cancel_paused;
  unsigned h2_cancel_cancelled;
  unsigned h2_cancel_rst_seen;
  unsigned h2_cancel_tcp_closed;
  unsigned h2_cancel_terminal;
  unsigned h2_cancel_admitted;
  unsigned h2_cancel_rejected;
  unsigned h2_cancel_curl_adds;
  unsigned h2_cancel_active;
  unsigned h2_cancel_active_at_reject;
  pid_t h2_scale_worker_pid;
  unsigned long h2_scale_worker_baseline_kb;
  unsigned long h2_scale_worker_peak_kb;
  size_t h2_scale_received;
#endif
};

struct proxy_state;

struct curl_watch {
  struct kore_event evt;
  struct proxy_state *state;
  curl_socket_t fd;
  struct curl_watch *retired_next;
  int retired;
};

struct proxy_state {
  struct kore_event tunnel_event;
  struct connection *downstream;
  struct kore_timer *timer;
  struct kore_timer *deadline_timer;
  struct kore_timer *relay_continue_timer;
  struct kore_timer *relay_retry_timer;
  struct kore_timer *upload_continue_timer;
  CURLM *multi;
  CURL *easy;
  curl_socket_t upstream_fd;
  unsigned curl_watch_count;
  int tunnel_watched;
  int tunnel_interest;
  int downstream_watched;
  int downstream_interest;
  int phase;
  int running;
  size_t response_sent;
  int relay_mode;
  int relay_tls;
  int ws_mode;
  int ws_upgraded;
  int ws_reject_mode;
  int ws_rejected;
  int ws_retry_mode;
  int ws_retry_abort_mode;
  int ws_retry_send_injected;
  int ws_retry_recv_injected;
  int ws_retry_read_seen;
  int ws_retry_write_seen;
  int ws_retry_cached_once;
  int ws_retry_cache_pending;
  int ws_retry_cache_release;
  size_t ws_retry_cache_length;
  int http_mode;
  int h2_mode;
  int h2_scale_mode;
  int h2_scale_pause_seen;
  int h2_abort_mode;
  int h2_cancel_mode;
  int h2_cancel_pause_seen;
  int h2_cancel_reserved;
  int http_abort_mode;
  int http_tls_reset_mode;
  int http_queue_abort_mode;
  int http_queue_held;
  int http_write_fail_mode;
  int http_body_queued;
  int tls_write_error_once;
  int tls_retry_read_mode;
  int tls_retry_write_mode;
  int tls_retry_read_once;
  int tls_retry_write_once;
  BIO *tls_retry_rbio;
  BIO *tls_retry_wbio;
  int downstream_half_closed;
  int upload_mode;
  int chunked_upload_mode;
  int chunked_keepalive_mode;
  int upload_paused;
  int upload_read_wait;
  size_t upload_remaining;
  unsigned char upload_buffer[RELAY_BUFFER_SIZE];
  size_t upload_length;
  size_t upload_offset;
  enum upload_chunk_phase chunk_phase;
  size_t chunk_remaining;
  size_t chunk_total;
  size_t chunk_raw_length;
  size_t chunk_raw_offset;
  size_t chunk_line_length;
  int chunk_line_cr;
  int chunk_trailer_seen;
  char chunk_line[CHUNK_LINE_SIZE];
  unsigned char chunk_raw[RELAY_BUFFER_SIZE];
  unsigned char *chunk_pipeline;
  size_t chunk_pipeline_length;
  struct http_request *request;
  struct curl_slist *upload_headers;
  char curl_error[CURL_ERROR_SIZE];
  int http_status_seen;
  int http_headers_ready;
  int http_headers_sent;
  int http_transfer_done;
  int http_paused;
  int http_final_sent;
  int http_error_pending;
  int http_error_sent;
  size_t http_body_length;
  unsigned char http_body[HTTP_BODY_BUFFER_SIZE];
  unsigned char http_frame[HTTP_BODY_BUFFER_SIZE + 32];
  unsigned char to_upstream[RELAY_BUFFER_SIZE];
  const unsigned char *initial_data;
  size_t initial_length;
  size_t initial_offset;
  size_t to_upstream_offset;
  size_t to_upstream_length;
  unsigned char to_downstream[RELAY_BUFFER_SIZE];
  size_t to_downstream_length;
  int downstream_eof;
  int upstream_eof;
  int upstream_write_closed;
  int relay_finished;
  int relay_closing;
  int down_read_wait;
  int close_wait;
};

#if defined(VECTIS_PROXY_SHARED_MULTI)
struct worker_curl_loop {
  CURLM *multi;
  struct kore_timer *timer;
  unsigned watch_count;
  int running;
};

static struct worker_curl_loop worker_curl;
#endif

static struct loop_metrics *metrics;
static BIO_METHOD *tls_retry_bio_method;
#if defined(VECTIS_PROXY_SHARED_MULTI)
static volatile int h2_scale_release;
static volatile int h2_cancel_release;
#endif
static struct curl_watch *retired_watches;
static struct kore_timer *retired_watches_timer;
static unsigned short upstream_port;
static unsigned short stalled_port;
static unsigned short relay_port;
static unsigned short relay_tls_port;
static unsigned short ws_port;
static unsigned short ws_retry_port;
static unsigned short ws_retry_abort_port;
static unsigned short ws_reject_port;
static unsigned short sse_port;
static unsigned short sse_abort_port;
static unsigned short sse_queue_abort_port;
static unsigned short sse_write_fail_port;
static unsigned short sse_reset_port;
static unsigned short sse_reset_before_port;
static unsigned short sse_tls_reset_port;
static unsigned short sse_tls_reset_before_port;
static unsigned short upload_port;
static unsigned short upload_chunked_port;
#if defined(VECTIS_PROXY_SHARED_MULTI)
static unsigned short h2_port;
static unsigned short h2_scale_port;
static unsigned short h2_abort_port;
static unsigned short h2_cancel_port;
static unsigned short h2_recovery_port;
#endif
static char tls_cert_path[128];
static char tls_key_path[128];

extern void vectis_kore_set_prebody_probe(
    int (*probe)(struct http_request *, const void *, size_t));
extern void vectis_kore_set_worker_teardown_probe(void (*probe)(void));

static void drive_curl(struct proxy_state *state, curl_socket_t fd, int flags);
static void curl_event(void *arg, int error);
static void downstream_event(void *arg, int error);
static void relay_pump(struct proxy_state *state);
static int http_pump(struct proxy_state *state);
static void http_input_pump(struct proxy_state *state);
static void proxy_restore_http(struct proxy_state *state);
static unsigned char relay_byte(size_t offset);

static int
probe_downstream_write(struct connection *connection, size_t length,
    size_t *written)
{
  struct proxy_state *state;

  state = (struct proxy_state *)connection->hdlr_extra;
  if (state->http_write_fail_mode && state->http_body_queued &&
      connection->tls == NULL) {
    metrics->http_write_failures++;
    errno = EPIPE;
    return KORE_RESULT_ERROR;
  }
  if (connection->tls != NULL) {
    int result;

    result = kore_tls_write(connection, length, written);
    if (state->http_write_fail_mode && result == KORE_RESULT_ERROR)
      metrics->tls_write_call_failures++;
    return result;
  }
  return net_write(connection, length, written);
}

static int
retry_bio_create(BIO *bio)
{
  BIO_set_init(bio, 1);
  BIO_set_data(bio, NULL);
  return 1;
}

static int
retry_bio_destroy(BIO *bio)
{
  BIO_set_data(bio, NULL);
  BIO_set_init(bio, 0);
  return 1;
}

static int
retry_bio_read(BIO *bio, char *data, size_t length, size_t *read)
{
  struct proxy_state *state;
  int result;

  state = (struct proxy_state *)BIO_get_data(bio);
  *read = 0;
  BIO_clear_retry_flags(bio);
  if (state != NULL && state->tls_retry_read_once) {
    state->tls_retry_read_once = 0;
    metrics->tls_read_want_write_injected++;
    BIO_set_retry_write(bio);
    return 0;
  }
  result = BIO_read_ex(BIO_next(bio), data, length, read);
  if (!result)
    BIO_copy_next_retry(bio);
  return result;
}

static int
retry_bio_write(BIO *bio, const char *data, size_t length,
    size_t *written)
{
  struct proxy_state *state;
  int result;

  state = (struct proxy_state *)BIO_get_data(bio);
  *written = 0;
  BIO_clear_retry_flags(bio);
  if (state != NULL && state->tls_write_error_once &&
      state->http_body_queued) {
    state->tls_write_error_once = 0;
    metrics->http_write_failures++;
    errno = EIO;
    return 0;
  }
  if (state != NULL && state->tls_retry_write_once &&
      state->http_body_queued) {
    state->tls_retry_write_once = 0;
    metrics->tls_write_want_read_injected++;
    BIO_set_retry_read(bio);
    return 0;
  }
  result = BIO_write_ex(BIO_next(bio), data, length, written);
  if (!result)
    BIO_copy_next_retry(bio);
  return result;
}

static long
retry_bio_ctrl(BIO *bio, int command, long number, void *pointer)
{
  return BIO_ctrl(BIO_next(bio), command, number, pointer);
}

static void
install_tls_retry_bios(struct proxy_state *state)
{
  BIO *read_socket;
  BIO *write_socket;
  BIO *read_filter;
  BIO *write_filter;

  assert(state->downstream->tls != NULL);
  assert(tls_retry_bio_method != NULL);
  read_socket = BIO_new_socket(state->downstream->fd, BIO_NOCLOSE);
  write_socket = BIO_new_socket(state->downstream->fd, BIO_NOCLOSE);
  read_filter = BIO_new(tls_retry_bio_method);
  write_filter = BIO_new(tls_retry_bio_method);
  assert(read_socket != NULL && write_socket != NULL);
  assert(read_filter != NULL && write_filter != NULL);
  BIO_set_data(read_filter, state);
  BIO_set_data(write_filter, state);
  assert(BIO_push(read_filter, read_socket) == read_filter);
  assert(BIO_push(write_filter, write_socket) == write_filter);
  SSL_set_bio(state->downstream->tls, read_filter, write_filter);
  state->tls_retry_rbio = read_filter;
  state->tls_retry_wbio = write_filter;
}

static void
init_tls_retry_bio_method(void)
{
  int type;

  type = BIO_get_new_index();
  assert(type >= 0);
  tls_retry_bio_method = BIO_meth_new(type | BIO_TYPE_FILTER,
      "vectis proxy TLS retry probe");
  assert(tls_retry_bio_method != NULL);
  assert(BIO_meth_set_create(tls_retry_bio_method,
      retry_bio_create) == 1);
  assert(BIO_meth_set_destroy(tls_retry_bio_method,
      retry_bio_destroy) == 1);
  assert(BIO_meth_set_read_ex(tls_retry_bio_method,
      retry_bio_read) == 1);
  assert(BIO_meth_set_write_ex(tls_retry_bio_method,
      retry_bio_write) == 1);
  assert(BIO_meth_set_ctrl(tls_retry_bio_method,
      retry_bio_ctrl) == 1);
}

static void
upload_continue(void *arg, u_int64_t now)
{
  struct proxy_state *state;

  (void)now;
  state = (struct proxy_state *)arg;
  state->upload_continue_timer = NULL;
  metrics->upload_tls_pending_resumes++;
  http_input_pump(state);
}

static void
relay_continue(void *arg, u_int64_t now)
{
  struct proxy_state *state;

  (void)now;
  state = (struct proxy_state *)arg;
  state->relay_continue_timer = NULL;
  metrics->relay_continuations++;
  relay_pump(state);
}

static void
relay_retry_cached(void *arg, u_int64_t now)
{
  struct proxy_state *state;

  (void)now;
  state = (struct proxy_state *)arg;
  assert(state->ws_retry_cache_pending);
  state->relay_retry_timer = NULL;
  state->ws_retry_cache_release = 1;
  metrics->ws_retry_timer_fired++;
  relay_pump(state);
}

static void *
echo_main(void *arg)
{
  struct echo_server *server;
  char input[4];
  int fd;

  server = (struct echo_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  assert(recv(fd, input, sizeof(input), MSG_WAITALL) == 4);
  assert(memcmp(input, "ping", 4) == 0);
  assert(send(fd, "pong", 4, MSG_NOSIGNAL) == 4);
  assert(close(fd) == 0);
  return NULL;
}

static void *
relay_echo_main(void *arg)
{
  struct echo_server *server;
  unsigned char bytes[4096];
  size_t received;
  size_t offset;
  ssize_t amount;
  int fd;

  server = (struct echo_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  received = 0;
  for (;;) {
    amount = recv(fd, bytes, sizeof(bytes), 0);
    assert(amount >= 0);
    if (amount == 0)
      break;
    received += (size_t)amount;
    assert(received <= RELAY_PAYLOAD_SIZE);
    offset = 0;
    while (offset < (size_t)amount) {
      ssize_t written;

      written = send(fd, bytes + offset, (size_t)amount - offset,
          MSG_NOSIGNAL);
      assert(written > 0);
      offset += (size_t)written;
    }
  }
  assert(received == RELAY_PAYLOAD_SIZE);
  assert(close(fd) == 0);
  return NULL;
}

static void *
relay_tls_main(void *arg)
{
  struct echo_server *server;
  unsigned char bytes[4096];
  size_t received;
  size_t offset;
  SSL *ssl;
  int amount;
  int fd;

  server = (struct echo_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  ssl = SSL_new(server->tls_ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_accept(ssl) == 1);
  received = 0;
  while (received < RELAY_PAYLOAD_SIZE) {
    amount = SSL_read(ssl, bytes, sizeof(bytes));
    assert(amount > 0);
    received += (size_t)amount;
    assert(received <= RELAY_PAYLOAD_SIZE);
    offset = 0;
    while (offset < (size_t)amount) {
      int written;

      written = SSL_write(ssl, bytes + offset, amount - (int)offset);
      assert(written > 0);
      offset += (size_t)written;
    }
  }
  assert(SSL_shutdown(ssl) >= 0);
  SSL_free(ssl);
  assert(close(fd) == 0);
  return NULL;
}

static void *
ws_tls_main(void *arg)
{
  struct echo_server *server;
  unsigned char request[sizeof(ws_upstream_request) - 1 +
      sizeof(ws_client_early)];
  unsigned char response[sizeof(ws_response) - 1 +
      sizeof(ws_server_early)];
  SSL *ssl;
  struct pollfd watcher;
  size_t used;
  int fd;
  int got;
  int attempt;
  int retry_target;
  int ssl_error;
  struct timeval timeout;

  server = (struct echo_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  ssl = SSL_new(server->tls_ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_accept(ssl) == 1);
  used = 0;
  while (used < sizeof(ws_upstream_request) - 1) {
    got = SSL_read(ssl, request + used,
        (int)(sizeof(ws_upstream_request) - 1 - used));
    assert(got > 0);
    used += (size_t)got;
  }
  assert(memcmp(request, ws_upstream_request,
      sizeof(ws_upstream_request) - 1) == 0);
  watcher.fd = fd;
  watcher.events = POLLIN;
  watcher.revents = 0;
  assert(SSL_pending(ssl) == 0);
  assert(SSL_has_pending(ssl) == 0);
  assert(poll(&watcher, 1, 50) == 0);
  memcpy(response, ws_response, sizeof(ws_response) - 1);
  memcpy(response + sizeof(ws_response) - 1,
      ws_server_early, sizeof(ws_server_early));
  assert(SSL_write(ssl, response, sizeof(response)) ==
      (int)sizeof(response));
  if (server->ws_retry_mode) {
    retry_target = server->ws_retry_abort_mode ? 2 : 1;
    for (attempt = 0; attempt < 5000 &&
        __sync_fetch_and_add(&metrics->ws_retry_send_injected, 0) <
            (unsigned)retry_target;
        attempt++)
      usleep(1000u);
    assert(__sync_fetch_and_add(&metrics->ws_retry_send_injected, 0) ==
        (unsigned)retry_target);
    assert(SSL_write(ssl, ws_server_retry,
        sizeof(ws_server_retry)) == (int)sizeof(ws_server_retry));
  }
  used = 0;
  while (used < sizeof(ws_client_early)) {
    got = SSL_read(ssl, request + used,
        (int)(sizeof(ws_client_early) - used));
    assert(got > 0);
    used += (size_t)got;
  }
  assert(memcmp(request, ws_client_early,
      sizeof(ws_client_early)) == 0);
  used = 0;
  while (used < sizeof(ws_client_later)) {
    got = SSL_read(ssl, request + used,
        (int)(sizeof(ws_client_later) - used));
    assert(got > 0);
    used += (size_t)got;
  }
  assert(memcmp(request, ws_client_later,
      sizeof(ws_client_later)) == 0);
  assert(SSL_write(ssl, ws_server_later, sizeof(ws_server_later)) ==
      (int)sizeof(ws_server_later));
  if (server->ws_retry_abort_mode) {
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
        &timeout, sizeof(timeout)) == 0);
    got = SSL_read(ssl, request, sizeof(request));
    assert(got <= 0);
    ssl_error = SSL_get_error(ssl, got);
    assert(ssl_error == SSL_ERROR_ZERO_RETURN ||
        ssl_error == SSL_ERROR_SYSCALL ||
        ssl_error == SSL_ERROR_SSL);
    if (ssl_error == SSL_ERROR_SYSCALL)
      assert(errno != EAGAIN && errno != EWOULDBLOCK);
    __sync_fetch_and_add(&metrics->ws_retry_upstream_closed, 1);
    SSL_free(ssl);
    assert(close(fd) == 0);
    return NULL;
  }
  assert(SSL_shutdown(ssl) >= 0);
  SSL_free(ssl);
  assert(close(fd) == 0);
  return NULL;
}

static void *
ws_reject_main(void *arg)
{
  struct echo_server *server;
  unsigned char request[sizeof(ws_upstream_request) - 1];
  unsigned char body[4096];
  struct pollfd watcher;
  SSL *ssl;
  size_t used;
  size_t sent;
  size_t index;
  int fd;
  int got;

  server = (struct echo_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  ssl = SSL_new(server->tls_ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_accept(ssl) == 1);
  used = 0;
  while (used < sizeof(request)) {
    got = SSL_read(ssl, request + used,
        (int)(sizeof(request) - used));
    assert(got > 0);
    used += (size_t)got;
  }
  assert(memcmp(request, ws_upstream_request,
      sizeof(request)) == 0);
  watcher.fd = fd;
  watcher.events = POLLIN;
  watcher.revents = 0;
  assert(SSL_pending(ssl) == 0);
  assert(SSL_has_pending(ssl) == 0);
  assert(poll(&watcher, 1, 50) == 0);
  assert(SSL_write(ssl, ws_reject_response, 7) == 7);
  usleep(20000u);
  assert(SSL_write(ssl, ws_reject_response + 7,
      (int)(sizeof(ws_reject_response) - 1 - 7)) ==
      (int)(sizeof(ws_reject_response) - 1 - 7));
  for (sent = 0; sent < RELAY_PAYLOAD_SIZE; sent += sizeof(body)) {
    for (index = 0; index < sizeof(body); index++)
      body[index] = relay_byte(sent + index);
    assert(SSL_write(ssl, body, (int)sizeof(body)) ==
        (int)sizeof(body));
  }
  assert(SSL_pending(ssl) == 0);
  assert(SSL_has_pending(ssl) == 0);
  assert(poll(&watcher, 1, 50) == 0);
  assert(SSL_shutdown(ssl) >= 0);
  SSL_free(ssl);
  assert(close(fd) == 0);
  return NULL;
}

struct relay_sender {
  int fd;
  size_t start;
  unsigned delay_us;
  volatile size_t progress;
};

static unsigned char
relay_byte(size_t offset)
{
  return (unsigned char)((offset * 73u + 19u) & 0xffu);
}

static void *
relay_send_main(void *arg)
{
  struct relay_sender *sender;
  unsigned char bytes[4096];
  size_t offset;
  size_t used;
  ssize_t amount;

  sender = (struct relay_sender *)arg;
  offset = sender->start;
  while (offset < RELAY_PAYLOAD_SIZE) {
    for (used = 0; used < sizeof(bytes); used++)
      bytes[used] = relay_byte(offset + used);
    used = 0;
    while (used < sizeof(bytes)) {
      amount = send(sender->fd, bytes + used, sizeof(bytes) - used,
          MSG_NOSIGNAL);
      assert(amount > 0);
      used += (size_t)amount;
    }
    offset += sizeof(bytes);
    __sync_lock_test_and_set(&sender->progress, offset);
    if (sender->delay_us != 0)
      usleep(sender->delay_us);
  }
  assert(shutdown(sender->fd, SHUT_WR) == 0);
  return NULL;
}

static void
send_all(int fd, const void *data, size_t length)
{
  const unsigned char *bytes;
  size_t offset;
  ssize_t sent;

  bytes = (const unsigned char *)data;
  offset = 0;
  while (offset < length) {
    sent = send(fd, bytes + offset, length - offset, MSG_NOSIGNAL);
    assert(sent > 0);
    offset += (size_t)sent;
  }
}

static void *
upload_main(void *arg)
{
  static const char early[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
      "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "4\r\npong\r\n";
  static const char final[] = "4\r\ndone\r\n0\r\n\r\n";
  struct echo_server *server;
  struct timeval timeout;
  unsigned char input[4096];
  unsigned char *body;
  size_t received;
  size_t used;
  size_t index;
  ssize_t got;
  int fd;
  int early_sent;

  server = (struct echo_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  used = 0;
  body = NULL;
  while (body == NULL) {
    assert(used < sizeof(input) - 1);
    got = recv(fd, input + used, sizeof(input) - 1 - used, 0);
    assert(got > 0);
    used += (size_t)got;
    input[used] = '\0';
    body = (unsigned char *)strstr((char *)input, "\r\n\r\n");
  }
  assert(strstr((char *)input, "POST /upload HTTP/1.1\r\n") != NULL ||
      strstr((char *)input,
      "POST /upload-tls-retry HTTP/1.1\r\n") != NULL);
  assert(strstr((char *)input, "Content-Length: 1048576\r\n") != NULL);
  body += 4;
  received = 0;
  early_sent = 0;
  for (;;) {
    for (index = 0; index < used - (size_t)(body - input); index++) {
      assert(received < UPLOAD_BODY_SIZE);
      assert(body[index] == relay_byte(received));
      received++;
    }
    if (!early_sent && received >= RELAY_BUFFER_SIZE) {
      assert(received < UPLOAD_BODY_SIZE);
      send_all(fd, early, sizeof(early) - 1);
      __sync_lock_test_and_set(&server->allow_body, 1);
      early_sent = 1;
    }
    if (received == UPLOAD_BODY_SIZE)
      break;
    got = recv(fd, input, sizeof(input), 0);
    if (got <= 0)
      fprintf(stderr, "upload fixture stopped: got=%zd errno=%d "
          "received=%zu early=%d pauses=%u queued=%zu complete=%u "
          "http_done=%u\n",
          got, errno, received, early_sent, metrics->upload_pauses,
          metrics->upload_max_queued, metrics->upload_done,
          metrics->http_done);
    assert(got > 0);
    body = input;
    used = (size_t)got;
  }
  assert(early_sent);
  send_all(fd, final, sizeof(final) - 1);
  assert(close(fd) == 0);
  return NULL;
}

static void
upload_read_exact(int fd, unsigned char *data, size_t length)
{
  size_t offset;
  ssize_t got;

  offset = 0;
  while (offset < length) {
    got = recv(fd, data + offset, length - offset, 0);
    assert(got > 0);
    offset += (size_t)got;
  }
}

static void
upload_read_line(int fd, char *line, size_t length)
{
  size_t used;
  ssize_t got;

  used = 0;
  for (;;) {
    assert(used < length - 1);
    got = recv(fd, line + used, 1, 0);
    assert(got == 1);
    used++;
    if (used >= 2 && line[used - 2] == '\r' &&
        line[used - 1] == '\n') {
      line[used] = '\0';
      return;
    }
  }
}

static void *
upload_chunked_main(void *arg)
{
  static const char early[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
      "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "4\r\npong\r\n";
  static const char final[] = "4\r\ndone\r\n0\r\n\r\n";
  struct echo_server *server;
  struct timeval timeout;
  unsigned char data[4096];
  char line[256];
  char *end;
  size_t total;
  size_t chunk_size;
  size_t amount;
  size_t n;
  int fd;
  int saw_transfer;
  int saw_trailer;
  int early_sent;
  int attempt;

  server = (struct echo_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  upload_read_line(fd, line, sizeof(line));
  assert(strcmp(line, "POST /upload-chunked HTTP/1.1\r\n") == 0);
  saw_transfer = 0;
  saw_trailer = 0;
  for (;;) {
    upload_read_line(fd, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
    if (strcmp(line, "Transfer-Encoding: chunked\r\n") == 0)
      saw_transfer = 1;
    if (strcmp(line, "Trailer: X-Trace\r\n") == 0)
      saw_trailer = 1;
    assert(strcmp(line, "Expect: 100-continue\r\n") != 0);
  }
  assert(saw_transfer && saw_trailer);
  total = 0;
  early_sent = 0;
  for (;;) {
    upload_read_line(fd, line, sizeof(line));
    errno = 0;
    chunk_size = strtoul(line, &end, 16);
    assert(errno == 0 && strcmp(end, "\r\n") == 0);
    if (chunk_size == 0)
      break;
    assert(chunk_size <= UPLOAD_BODY_SIZE - total);
    while (chunk_size != 0) {
      amount = chunk_size;
      if (amount > sizeof(data))
        amount = sizeof(data);
      upload_read_exact(fd, data, amount);
      for (n = 0; n < amount; n++)
        assert(data[n] == relay_byte(total + n));
      total += amount;
      chunk_size -= amount;
      if (!early_sent && total >= RELAY_BUFFER_SIZE) {
        assert(total < UPLOAD_BODY_SIZE);
        send_all(fd, early, sizeof(early) - 1);
        __sync_lock_test_and_set(&server->allow_body, 1);
        early_sent = 1;
      }
    }
    upload_read_exact(fd, data, 2);
    assert(memcmp(data, "\r\n", 2) == 0);
  }
  assert(total == UPLOAD_BODY_SIZE && early_sent);
  upload_read_line(fd, line, sizeof(line));
  assert(strcmp(line, "X-Trace: done\r\n") == 0);
  upload_read_line(fd, line, sizeof(line));
  assert(strcmp(line, "\r\n") == 0);
  if (server->hold_final) {
    for (attempt = 0; attempt < 10000 &&
        __sync_fetch_and_add(&server->release_final, 0) == 0;
        attempt++)
      usleep(1000u);
    assert(__sync_fetch_and_add(&server->release_final, 0) == 1);
  }
  send_all(fd, final, sizeof(final) - 1);
  assert(close(fd) == 0);
  return NULL;
}

static unsigned char
sse_byte(size_t offset)
{
  static const char prefix[] = "data: ";
  size_t within;

  within = offset % SSE_CHUNK_SIZE;
  if (within < sizeof(prefix) - 1)
    return (unsigned char)prefix[within];
  if (within >= SSE_CHUNK_SIZE - 2)
    return '\n';
  return 'x';
}

#if defined(VECTIS_PROXY_SHARED_MULTI)
struct h2_fixture_connection {
  SSL *ssl;
  size_t generated;
  size_t body_size;
  int abort_mode;
  int cancel_mode;
  int cancel_rst_seen;
  int cancel_tcp_closed;
  int32_t aborted_stream_id;
  int32_t reused_stream_id;
  size_t reuse_generated;
};

struct h2_connection_args {
  struct echo_server *server;
  int fd;
  unsigned done_goal;
};

static int
h2_select_alpn(SSL *ssl, const unsigned char **out, unsigned char *outlen,
    const unsigned char *in, unsigned int inlen, void *arg)
{
  unsigned int offset;
  unsigned int length;

  (void)ssl;
  (void)arg;
  offset = 0;
  while (offset < inlen) {
    length = in[offset++];
    if (length > inlen - offset)
      break;
    if (length == 2 && memcmp(in + offset, "h2", 2) == 0) {
      *out = in + offset;
      *outlen = 2;
      __sync_fetch_and_add(&metrics->h2_negotiated, 1);
      return SSL_TLSEXT_ERR_OK;
    }
    offset += length;
  }
  return SSL_TLSEXT_ERR_NOACK;
}

static ssize_t
h2_send(nghttp2_session *session, const uint8_t *data, size_t length,
    int flags, void *arg)
{
  struct h2_fixture_connection *connection;
  int sent;

  (void)session;
  (void)flags;
  connection = (struct h2_fixture_connection *)arg;
  sent = SSL_write(connection->ssl, data, (int)length);
  if (sent != (int)length && connection->cancel_mode) {
    fprintf(stderr, "h2 cancel send: sent=%d ssl_error=%d errno=%d "
        "generated=%zu cancelled=%u rst=%u\n", sent,
        SSL_get_error(connection->ssl, sent), errno, connection->generated,
        metrics->h2_cancel_cancelled, metrics->h2_cancel_rst_seen);
    if ((errno == ECONNRESET || errno == EPIPE) &&
        !connection->cancel_tcp_closed) {
      connection->cancel_tcp_closed = 1;
      __sync_fetch_and_add(&metrics->h2_cancel_tcp_closed, 1);
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
  }
  assert(sent == (int)length);
  return sent;
}

static ssize_t
h2_body(nghttp2_session *session, int32_t stream_id, uint8_t *data,
    size_t length, uint32_t *flags, nghttp2_data_source *source, void *arg)
{
  struct h2_fixture_connection *connection;
  size_t index;

  (void)session;
  (void)source;
  connection = (struct h2_fixture_connection *)arg;
  if (connection->cancel_mode) {
    if (connection->generated == 2u * 1024u * 1024u)
      return NGHTTP2_ERR_DEFERRED;
    if (length > 2u * 1024u * 1024u - connection->generated)
      length = 2u * 1024u * 1024u - connection->generated;
  } else if (connection->abort_mode) {
    if (stream_id == connection->aborted_stream_id) {
      if (connection->generated == SSE_CHUNK_SIZE)
        return NGHTTP2_ERR_DEFERRED;
      if (length > SSE_CHUNK_SIZE)
        length = SSE_CHUNK_SIZE;
    } else {
      assert(stream_id == connection->reused_stream_id);
      if (length > SSE_BODY_SIZE - connection->reuse_generated)
        length = SSE_BODY_SIZE - connection->reuse_generated;
    }
  } else if (length > connection->body_size - connection->generated) {
    length = connection->body_size - connection->generated;
  }
  assert(length > 0);
  for (index = 0; index < length; index++)
    data[index] = sse_byte((connection->abort_mode &&
        stream_id == connection->reused_stream_id ?
        connection->reuse_generated : connection->generated) + index);
  connection->generated += length;
  __sync_fetch_and_add(&metrics->h2_generated, length);
  if (connection->abort_mode &&
      stream_id == connection->reused_stream_id)
    connection->reuse_generated += length;
  if ((connection->abort_mode &&
      connection->reuse_generated == SSE_BODY_SIZE) ||
      (!connection->abort_mode &&
      connection->generated == connection->body_size))
    *flags |= NGHTTP2_DATA_FLAG_EOF;
  return (ssize_t)length;
}

static int
h2_request(nghttp2_session *session, const nghttp2_frame *frame,
    void *arg)
{
  static uint8_t status_name[] = ":status";
  static uint8_t status_value[] = "200";
  static uint8_t type_name[] = "content-type";
  static uint8_t type_value[] = "text/event-stream";
  nghttp2_nv headers[] = {
    {status_name, status_value, sizeof(status_name) - 1,
      sizeof(status_value) - 1, NGHTTP2_NV_FLAG_NONE},
    {type_name, type_value, sizeof(type_name) - 1,
      sizeof(type_value) - 1, NGHTTP2_NV_FLAG_NONE}
  };
  nghttp2_data_provider provider;
  struct h2_fixture_connection *connection;

  connection = (struct h2_fixture_connection *)arg;
  if (connection->cancel_mode && frame->hd.type == NGHTTP2_RST_STREAM) {
    connection->cancel_rst_seen = 1;
    __sync_fetch_and_add(&metrics->h2_cancel_rst_seen, 1);
    return 0;
  }
  if (connection->abort_mode && frame->hd.type == NGHTTP2_RST_STREAM) {
    __sync_fetch_and_add(&metrics->h2_abort_rst_seen, 1);
    return 0;
  }
  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST)
    return 0;
  if (connection->abort_mode) {
    if (connection->aborted_stream_id == 0)
      connection->aborted_stream_id = frame->hd.stream_id;
    else {
      assert(connection->reused_stream_id == 0);
      assert(metrics->h2_abort_rst_seen == 1);
      connection->reused_stream_id = frame->hd.stream_id;
    }
  }
  __sync_fetch_and_add(&metrics->h2_requests, 1);
  memset(&provider, 0, sizeof(provider));
  provider.read_callback = h2_body;
  return nghttp2_submit_response(session, frame->hd.stream_id,
      headers, sizeof(headers) / sizeof(headers[0]), &provider);
}

static void *
h2_connection_main(void *arg)
{
  struct h2_connection_args *args;
  struct h2_fixture_connection connection;
  nghttp2_session_callbacks *callbacks;
  nghttp2_session *session;
  struct timeval timeout;
  sigset_t blocked;
  unsigned char input[16384];
  int received;
  int send_result;

  args = (struct h2_connection_args *)arg;
  assert(sigemptyset(&blocked) == 0);
  assert(sigaddset(&blocked, SIGPIPE) == 0);
  assert(pthread_sigmask(SIG_BLOCK, &blocked, NULL) == 0);
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;
  assert(setsockopt(args->fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  assert(setsockopt(args->fd, SOL_SOCKET, SO_SNDTIMEO,
      &timeout, sizeof(timeout)) == 0);
  memset(&connection, 0, sizeof(connection));
  connection.body_size = args->server->h2_body_size;
  connection.abort_mode = args->server->h2_abort_mode;
  connection.cancel_mode = args->server->h2_cancel_mode;
  connection.ssl = SSL_new(args->server->tls_ctx);
  assert(connection.ssl != NULL);
  assert(SSL_set_fd(connection.ssl, args->fd) == 1);
  assert(SSL_accept(connection.ssl) == 1);
  assert(nghttp2_session_callbacks_new(&callbacks) == 0);
  nghttp2_session_callbacks_set_send_callback(callbacks, h2_send);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, h2_request);
  assert(nghttp2_session_server_new(&session, callbacks, &connection) == 0);
  nghttp2_session_callbacks_del(callbacks);
  assert(nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE,
      NULL, 0) == 0);
  assert(nghttp2_session_send(session) == 0);
  while ((!connection.abort_mode && !connection.cancel_mode &&
      connection.generated < connection.body_size) ||
      (connection.cancel_mode && !connection.cancel_rst_seen) ||
      (connection.abort_mode &&
      (__sync_fetch_and_add(&metrics->h2_abort_rst_seen, 0) == 0 ||
      connection.reuse_generated < SSE_BODY_SIZE))) {
    received = SSL_read(connection.ssl, input, sizeof(input));
    if ((connection.abort_mode || connection.cancel_mode) && received <= 0) {
      fprintf(stderr, "h2 abort upstream read: result=%d ssl_error=%d "
          "errno=%d generated=%zu\n", received,
          SSL_get_error(connection.ssl, received), errno,
          connection.generated);
      if (received == 0 || errno == ECONNRESET || errno == EPIPE) {
        if (connection.cancel_mode && !connection.cancel_tcp_closed) {
          connection.cancel_tcp_closed = 1;
          __sync_fetch_and_add(&metrics->h2_cancel_tcp_closed, 1);
        } else if (connection.abort_mode) {
          __sync_fetch_and_add(&metrics->h2_abort_tcp_closed, 1);
        }
      }
      break;
    }
    assert(received > 0);
    assert(nghttp2_session_mem_recv(session, input,
        (size_t)received) == received);
    send_result = nghttp2_session_send(session);
    if (connection.cancel_mode && connection.cancel_tcp_closed)
      break;
    assert(send_result == 0);
  }
  if (connection.cancel_mode) {
    assert(connection.cancel_rst_seen == 1 ||
        connection.cancel_tcp_closed == 1);
    __sync_fetch_and_add(&metrics->h2_cancel_terminal, 1);
    nghttp2_session_del(session);
    SSL_free(connection.ssl);
    assert(close(args->fd) == 0);
    return NULL;
  }
  if (connection.abort_mode) {
    assert(connection.generated == SSE_CHUNK_SIZE + SSE_BODY_SIZE);
    assert(connection.reuse_generated == SSE_BODY_SIZE);
    assert(metrics->h2_abort_rst_seen == 1);
    __sync_fetch_and_add(&metrics->h2_abort_terminal, 1);
    for (received = 0; received < 3000 &&
        __sync_fetch_and_add(&metrics->h2_abort_reuse_done, 0) == 0;
        received++)
      usleep(10000u);
    assert(metrics->h2_abort_reuse_done == 1);
    nghttp2_session_del(session);
    assert(SSL_shutdown(connection.ssl) >= 0);
    SSL_free(connection.ssl);
    assert(close(args->fd) == 0);
    return NULL;
  }
  __sync_fetch_and_add(&metrics->h2_completed, 1);
  for (received = 0; received < 3000 &&
      __sync_fetch_and_add(args->done_goal == 1 ?
          &metrics->h2_down_done : &metrics->h2_scale_done, 0) <
          args->done_goal;
      received++)
    usleep(10000u);
  assert(__sync_fetch_and_add(args->done_goal == 1 ?
      &metrics->h2_down_done : &metrics->h2_scale_done, 0) ==
      args->done_goal);
  nghttp2_session_del(session);
  assert(SSL_shutdown(connection.ssl) >= 0);
  SSL_free(connection.ssl);
  assert(close(args->fd) == 0);
  return NULL;
}

static void *
h2_main(void *arg)
{
  struct echo_server *server;
  struct h2_connection_args connection;

  server = (struct echo_server *)arg;
  connection.server = server;
  connection.fd = accept(server->listener, NULL, NULL);
  assert(connection.fd >= 0);
  connection.done_goal = 1;
  h2_connection_main(&connection);
  return NULL;
}

static void *
h2_scale_main(void *arg)
{
  struct echo_server *server;
  struct h2_connection_args scale_args[H2_SCALE_CONNECTIONS];
  pthread_t threads[H2_SCALE_CONNECTIONS];
  int i;

  server = (struct echo_server *)arg;
  for (i = 0; i < H2_SCALE_CONNECTIONS; i++) {
    scale_args[i].server = server;
    scale_args[i].fd = accept(server->listener, NULL, NULL);
    assert(scale_args[i].fd >= 0);
    scale_args[i].done_goal = H2_SCALE_CONNECTIONS;
    assert(pthread_create(&threads[i], NULL, h2_connection_main,
        &scale_args[i]) == 0);
  }
  for (i = 0; i < H2_SCALE_CONNECTIONS; i++)
    assert(pthread_join(threads[i], NULL) == 0);
  return NULL;
}

static unsigned long
process_rss_kb(pid_t pid)
{
  char path[64];
  char line[256];
  FILE *file;
  unsigned long amount;

  assert(snprintf(path, sizeof(path), "/proc/%ld/status",
      (long)pid) > 0);
  file = fopen(path, "r");
  assert(file != NULL);
  amount = 0;
  while (fgets(line, sizeof(line), file) != NULL) {
    if (sscanf(line, "VmRSS: %lu kB", &amount) == 1)
      break;
  }
  assert(fclose(file) == 0);
  assert(amount > 0);
  return amount;
}
#endif

static void *
sse_main(void *arg)
{
  static const char header[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
      "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  struct echo_server *server;
  unsigned char body[SSE_CHUNK_SIZE];
  char chunk_header[32];
  char request[1024];
  size_t used;
  size_t offset;
  ssize_t got;
  int attempt;
  int length;
  int fd;

  server = (struct echo_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  used = 0;
  request[0] = '\0';
  while (strstr(request, "\r\n\r\n") == NULL) {
    assert(used < sizeof(request) - 1);
    got = recv(fd, request + used, sizeof(request) - 1 - used, 0);
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
  }
  assert(strstr(request, "GET / HTTP/1.1\r\n") == request);
  send_all(fd, header, sizeof(header) - 1);
  for (attempt = 0; attempt < 500 &&
      __sync_fetch_and_add(&server->allow_body, 0) == 0; attempt++)
    usleep(10000u);
  assert(__sync_fetch_and_add(&server->allow_body, 0) == 1);
  length = snprintf(chunk_header, sizeof(chunk_header), "%x\r\n",
      SSE_CHUNK_SIZE);
  assert(length > 0 && (size_t)length < sizeof(chunk_header));
  for (offset = 0; offset < SSE_BODY_SIZE; offset += sizeof(body)) {
    size_t i;

    for (i = 0; i < sizeof(body); i++)
      body[i] = sse_byte(offset + i);
    send_all(fd, chunk_header, (size_t)length);
    send_all(fd, body, sizeof(body));
    send_all(fd, "\r\n", 2);
  }
  send_all(fd, "0\r\n\r\n", 5);
  assert(close(fd) == 0);
  return NULL;
}

static void *
sse_abort_main(void *arg)
{
  static const char header[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
      "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  struct echo_server *server;
  struct timeval timeout;
  char request[1024];
  char byte;
  size_t used;
  ssize_t got;
  int attempt;
  int fd;

  server = (struct echo_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  used = 0;
  request[0] = '\0';
  while (strstr(request, "\r\n\r\n") == NULL) {
    assert(used < sizeof(request) - 1);
    got = recv(fd, request + used, sizeof(request) - 1 - used, 0);
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
  }
  assert(strstr(request, "GET / HTTP/1.1\r\n") == request);
  send_all(fd, header, sizeof(header) - 1);
  if (server->queue_abort_mode) {
    for (attempt = 0; attempt < 500 &&
        __sync_fetch_and_add(&server->allow_body, 0) == 0; attempt++)
      usleep(10000u);
    assert(__sync_fetch_and_add(&server->allow_body, 0) == 1);
  }
  send_all(fd, "4\r\npong\r\n", 9);
  timeout.tv_sec = 3;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  got = recv(fd, &byte, 1, 0);
  if (got == 0 || (got < 0 && errno == ECONNRESET)) {
    if (server->queue_abort_mode)
      metrics->http_queue_abort_upstream_closed++;
    else
      metrics->http_abort_upstream_closed++;
  }
  assert(close(fd) == 0);
  return NULL;
}

static void *
sse_reset_main(void *arg)
{
  static const char header[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
      "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  struct echo_server *server;
  struct linger reset;
  char request[1024];
  size_t used;
  ssize_t got;
  int attempt;
  int fd;

  server = (struct echo_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  used = 0;
  request[0] = '\0';
  while (strstr(request, "\r\n\r\n") == NULL) {
    assert(used < sizeof(request) - 1);
    got = recv(fd, request + used, sizeof(request) - 1 - used, 0);
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
  }
  assert(strstr(request, "GET / HTTP/1.1\r\n") == request);
  send_all(fd, header, sizeof(header) - 1);
  send_all(fd, "4\r\npong\r\n", 9);
  for (attempt = 0; attempt < 500 &&
      __sync_fetch_and_add(&server->allow_body, 0) == 0; attempt++)
    usleep(10000u);
  assert(__sync_fetch_and_add(&server->allow_body, 0) == 1);
  reset.l_onoff = 1;
  reset.l_linger = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_LINGER,
      &reset, sizeof(reset)) == 0);
  assert(close(fd) == 0);
  return NULL;
}

static void *
sse_reset_before_main(void *arg)
{
  struct echo_server *server;
  struct linger reset;
  char request[1024];
  size_t used;
  ssize_t got;
  int fd;

  server = (struct echo_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  used = 0;
  request[0] = '\0';
  while (strstr(request, "\r\n\r\n") == NULL) {
    assert(used < sizeof(request) - 1);
    got = recv(fd, request + used, sizeof(request) - 1 - used, 0);
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
  }
  assert(strstr(request, "GET / HTTP/1.1\r\n") == request);
  reset.l_onoff = 1;
  reset.l_linger = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_LINGER,
      &reset, sizeof(reset)) == 0);
  assert(close(fd) == 0);
  return NULL;
}

static void *
sse_tls_reset_main(void *arg)
{
  static const char header[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
      "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  struct echo_server *server;
  struct linger reset;
  char request[1024];
  SSL *ssl;
  size_t used;
  int got;
  int attempt;
  int fd;

  server = (struct echo_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  ssl = SSL_new(server->tls_ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_accept(ssl) == 1);
  used = 0;
  request[0] = '\0';
  while (strstr(request, "\r\n\r\n") == NULL) {
    assert(used < sizeof(request) - 1);
    got = SSL_read(ssl, request + used,
        (int)(sizeof(request) - 1 - used));
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
  }
  assert(strstr(request, "GET / HTTP/1.1\r\n") == request);
  if (!server->tls_reset_before_mode) {
    assert(SSL_write(ssl, header, sizeof(header) - 1) ==
        (int)(sizeof(header) - 1));
    assert(SSL_write(ssl, "4\r\npong\r\n", 9) == 9);
    for (attempt = 0; attempt < 500 &&
        __sync_fetch_and_add(&server->allow_body, 0) == 0; attempt++)
      usleep(10000u);
    assert(__sync_fetch_and_add(&server->allow_body, 0) == 1);
  }
  SSL_free(ssl);
  reset.l_onoff = 1;
  reset.l_linger = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_LINGER,
      &reset, sizeof(reset)) == 0);
  assert(close(fd) == 0);
  return NULL;
}

static void *
stall_main(void *arg)
{
  struct echo_server *server;
  struct pollfd watch;
  char bytes[4096];
  int fd;

  server = (struct echo_server *)arg;
  watch.fd = server->listener;
  watch.events = POLLIN;
  assert(poll(&watch, 1, 5000) > 0);
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  metrics->stall_accepted++;
  while (recv(fd, bytes, sizeof(bytes), 0) > 0) {
  }
  assert(close(fd) == 0);
  return NULL;
}

static unsigned short
available_port(void)
{
  struct sockaddr_in addr;
  socklen_t size;
  int fd;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  size = sizeof(addr);
  assert(getsockname(fd, (struct sockaddr *)&addr, &size) == 0);
  assert(close(fd) == 0);
  return ntohs(addr.sin_port);
}

static void
prepare_echo(struct echo_server *server)
{
  struct sockaddr_in addr;
  socklen_t size;

  server->tls_ctx = NULL;
  server->allow_body = 0;
  server->ws_retry_mode = 0;
  server->ws_retry_abort_mode = 0;
  server->tls_reset_before_mode = 0;
  server->queue_abort_mode = 0;
  server->hold_final = 0;
  server->release_final = 0;
  server->h2_body_size = 0;
  server->h2_abort_mode = 0;
  server->h2_cancel_mode = 0;
  server->listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(server->listener >= 0);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(server->listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  assert(listen(server->listener, 16) == 0);
  size = sizeof(addr);
  assert(getsockname(server->listener, (struct sockaddr *)&addr, &size) == 0);
  server->port = ntohs(addr.sin_port);
}

static void
reap_retired_watches(void *arg, u_int64_t now)
{
  struct curl_watch *watch;

  (void)arg;
  (void)now;
  retired_watches_timer = NULL;
  while ((watch = retired_watches) != NULL) {
    retired_watches = watch->retired_next;
    free(watch);
  }
}

static int
socket_change(CURL *easy, curl_socket_t fd, int what,
    void *arg, void *socket_arg)
{
#if !defined(VECTIS_PROXY_SHARED_MULTI)
  struct proxy_state *state;
#endif
  struct curl_watch *watch;
  int interest;

  (void)easy;
#if defined(VECTIS_PROXY_SHARED_MULTI)
  assert(arg == &worker_curl);
#else
  state = (struct proxy_state *)arg;
#endif
  watch = (struct curl_watch *)socket_arg;
  if (what == CURL_POLL_REMOVE) {
    if (watch != NULL) {
      kore_platform_disable_read((int)fd);
#if defined(VECTIS_PROXY_SHARED_MULTI)
      assert(worker_curl.watch_count > 0);
      worker_curl.watch_count--;
#else
      assert(state->curl_watch_count > 0);
      state->curl_watch_count--;
#endif
      metrics->curl_watch_removed++;
      watch->retired = 1;
      watch->retired_next = retired_watches;
      retired_watches = watch;
      if (retired_watches_timer == NULL)
        retired_watches_timer = kore_timer_add(reap_retired_watches,
            1, NULL, KORE_TIMER_ONESHOT);
      /* Model a second readiness result already in a kqueue batch. */
      curl_event(&watch->evt, 0);
    }
    return 0;
  }
  if (watch == NULL) {
    watch = calloc(1, sizeof(*watch));
    assert(watch != NULL);
    watch->evt.type = KORE_TYPE_CONNECTION;
    watch->evt.handle = curl_event;
#if defined(VECTIS_PROXY_SHARED_MULTI)
    watch->state = NULL;
#else
    watch->state = state;
#endif
    watch->fd = fd;
#if defined(VECTIS_PROXY_SHARED_MULTI)
    assert(curl_multi_assign(worker_curl.multi, fd, watch) == CURLM_OK);
    worker_curl.watch_count++;
    if (worker_curl.watch_count > metrics->max_curl_watchers)
      metrics->max_curl_watchers = worker_curl.watch_count;
#else
    assert(curl_multi_assign(state->multi, fd, watch) == CURLM_OK);
    state->curl_watch_count++;
    if (state->curl_watch_count > metrics->max_curl_watchers)
      metrics->max_curl_watchers = state->curl_watch_count;
#endif
  }
  interest = (what & CURL_POLL_IN ? EPOLLIN : 0) |
      (what & CURL_POLL_OUT ? EPOLLOUT : 0);
  kore_platform_event_schedule((int)fd, interest, 0, &watch->evt);
  return 0;
}

static void
timeout_event(void *arg, u_int64_t now)
{
#if !defined(VECTIS_PROXY_SHARED_MULTI)
  struct proxy_state *state;
#endif

  (void)now;
#if defined(VECTIS_PROXY_SHARED_MULTI)
  assert(arg == &worker_curl);
  worker_curl.timer = NULL;
  drive_curl(NULL, CURL_SOCKET_TIMEOUT, 0);
#else
  state = (struct proxy_state *)arg;
  state->timer = NULL;
  drive_curl(state, CURL_SOCKET_TIMEOUT, 0);
#endif
}

static void
deadline_expired(void *arg, u_int64_t now)
{
  (void)arg;
  (void)now;
  assert(0 && "probe deadline fired before worker cancellation");
}

static int
timer_change(CURLM *multi, long timeout_ms, void *arg)
{
#if !defined(VECTIS_PROXY_SHARED_MULTI)
  struct proxy_state *state;
#endif

  (void)multi;
#if defined(VECTIS_PROXY_SHARED_MULTI)
  assert(arg == &worker_curl);
  if (worker_curl.timer != NULL) {
    kore_timer_remove(worker_curl.timer);
    worker_curl.timer = NULL;
  }
  if (timeout_ms >= 0)
    worker_curl.timer = kore_timer_add(timeout_event,
        (u_int64_t)(timeout_ms > 0 ? timeout_ms : 1), &worker_curl,
        KORE_TIMER_ONESHOT);
#else
  state = (struct proxy_state *)arg;
  if (state->timer != NULL) {
    kore_timer_remove(state->timer);
    state->timer = NULL;
  }
  if (timeout_ms >= 0)
    state->timer = kore_timer_add(timeout_event,
        (u_int64_t)(timeout_ms > 0 ? timeout_ms : 1), state,
        KORE_TIMER_ONESHOT);
#endif
  return 0;
}

static void
relay_send(struct proxy_state *state, const void *bytes, size_t length,
    size_t *written, CURLcode *result)
{
  /* Model libcurl's TLS WANT_READ as a public CURLE_AGAIN result. */
  *written = 0;
  if (state->ws_retry_mode && state->ws_upgraded &&
      !state->ws_retry_send_injected) {
    state->ws_retry_send_injected = 1;
    metrics->ws_retry_send_injected++;
  }
  if (state->ws_retry_mode && state->ws_retry_send_injected &&
      !state->ws_retry_read_seen) {
    *result = CURLE_AGAIN;
    return;
  }
  *result = curl_easy_send(state->easy, bytes, length, written);
}

static void
relay_recv(struct proxy_state *state, void *bytes, size_t length,
    size_t *read, CURLcode *result)
{
  /* Model libcurl's TLS WANT_WRITE without reaching into its SSL object. */
  *read = 0;
  if (state->ws_retry_mode && !state->ws_retry_recv_injected &&
      state->to_upstream_offset >= sizeof(ws_upstream_request) - 1) {
    state->ws_retry_recv_injected = 1;
    metrics->ws_retry_recv_injected++;
  }
  if (state->ws_retry_mode && state->ws_retry_recv_injected &&
      !state->ws_retry_write_seen) {
    *result = CURLE_AGAIN;
    return;
  }
  if (state->ws_retry_mode && state->ws_retry_send_injected &&
      !state->ws_retry_read_seen) {
    *result = CURLE_AGAIN;
    return;
  }
  if (state->ws_retry_cache_pending) {
    if (!state->ws_retry_cache_release) {
      *result = CURLE_AGAIN;
      return;
    }
    assert(length >= state->ws_retry_cache_length);
    assert(bytes == state->to_downstream);
    *read = state->ws_retry_cache_length;
    state->ws_retry_cache_pending = 0;
    state->ws_retry_cache_length = 0;
    *result = CURLE_OK;
    return;
  }
  *result = curl_easy_recv(state->easy, bytes, length, read);
  if (state->ws_retry_mode && state->ws_upgraded &&
      !state->ws_retry_cached_once && *result == CURLE_OK &&
      *read == sizeof(ws_server_later) &&
      memcmp(bytes, ws_server_later, sizeof(ws_server_later)) == 0) {
    /* Keep one chunk in the existing scratch buffer until the timer wakes. */
    assert(bytes == state->to_downstream);
    state->ws_retry_cache_length = *read;
    state->ws_retry_cache_pending = 1;
    state->ws_retry_cached_once = 1;
    metrics->ws_retry_cached_injected++;
    *read = 0;
    *result = CURLE_AGAIN;
  }
}

static void
relay_pump(struct proxy_state *state)
{
  struct connection *downstream;
  size_t amount;
  size_t send_limit;
  ssize_t sent;
  CURLcode code;
  int step;
  int progress;
  int down_events;
  int up_events;
  int ssl_error;

  downstream = state->downstream;
  if (state->relay_finished)
    return;
  metrics->relay_pump_calls++;
  if (downstream->tls != NULL)
    metrics->tls_pump_calls++;
  if (state->relay_closing)
    goto finish;
  for (step = 0; step < 64; step++) {
    progress = 0;
    if (state->to_upstream_offset < state->to_upstream_length &&
        (!state->ws_mode || state->ws_upgraded ||
            state->to_upstream_offset < sizeof(ws_upstream_request) - 1)) {
      send_limit = state->to_upstream_length;
      if (state->ws_mode && !state->ws_upgraded)
        send_limit = sizeof(ws_upstream_request) - 1;
      relay_send(state,
          state->to_upstream + state->to_upstream_offset,
          send_limit - state->to_upstream_offset, &amount, &code);
      assert(code == CURLE_OK || code == CURLE_AGAIN);
      if (code == CURLE_OK && amount > 0) {
        state->to_upstream_offset += amount;
        progress = 1;
        if (state->to_upstream_offset == state->to_upstream_length) {
          state->to_upstream_offset = 0;
          state->to_upstream_length = 0;
        }
      } else {
        metrics->relay_write_pauses++;
        if (downstream->tls != NULL)
          metrics->tls_up_send_again++;
      }
    }
    if (TAILQ_EMPTY(&downstream->send_queue) &&
        state->to_downstream_length != 0 &&
        (!state->ws_mode || state->ws_upgraded || state->ws_rejected)) {
      struct netbuf *buffer;
      size_t queued;
      unsigned count;

      net_send_queue(downstream, state->to_downstream,
          state->to_downstream_length);
      state->to_downstream_length = 0;
      queued = 0;
      count = 0;
      TAILQ_FOREACH(buffer, &downstream->send_queue, list) {
        queued += buffer->b_len - buffer->s_off;
        count++;
      }
      if (queued > metrics->relay_max_kore_queued)
        metrics->relay_max_kore_queued = queued;
      if (count > metrics->relay_max_kore_buffers)
        metrics->relay_max_kore_buffers = count;
      progress = 1;
    }
    if (!TAILQ_EMPTY(&downstream->send_queue)) {
      downstream->evt.flags |= KORE_EVENT_WRITE;
      assert(net_send_flush(downstream) == KORE_RESULT_OK);
      if (!TAILQ_EMPTY(&downstream->send_queue)) {
        if (downstream->tls != NULL)
          metrics->downstream_tls_write_pauses++;
        metrics->relay_write_pauses++;
        metrics->relay_read_pauses++;
        goto schedule;
      }
      progress = 1;
    }
    if (state->to_upstream_length == 0 && !state->downstream_eof) {
      if (state->initial_offset < state->initial_length) {
        amount = state->initial_length - state->initial_offset;
        if (amount > sizeof(state->to_upstream))
          amount = sizeof(state->to_upstream);
        memcpy(state->to_upstream,
            state->initial_data + state->initial_offset, amount);
        state->initial_offset += amount;
        state->to_upstream_length = amount;
        progress = 1;
        if (amount > metrics->relay_max_queued)
          metrics->relay_max_queued = amount;
        continue;
      } else if (downstream->tls != NULL) {
        ERR_clear_error();
        sent = SSL_read(downstream->tls, state->to_upstream,
            (int)sizeof(state->to_upstream));
      } else {
        sent = recv(downstream->fd, state->to_upstream,
            sizeof(state->to_upstream), 0);
      }
      if (sent > 0) {
        state->down_read_wait = 0;
        state->to_upstream_length = (size_t)sent;
        progress = 1;
        if ((size_t)sent > metrics->relay_max_queued)
          metrics->relay_max_queued = (size_t)sent;
      } else if (downstream->tls != NULL) {
        ssl_error = SSL_get_error(downstream->tls, (int)sent);
        if (ssl_error == SSL_ERROR_ZERO_RETURN) {
          state->downstream_eof = 1;
          state->down_read_wait = 0;
          progress = 1;
        } else {
          assert(ssl_error == SSL_ERROR_WANT_READ ||
              ssl_error == SSL_ERROR_WANT_WRITE);
          state->down_read_wait = ssl_error == SSL_ERROR_WANT_READ
              ? EPOLLIN : EPOLLOUT;
          if (ssl_error == SSL_ERROR_WANT_WRITE)
            metrics->downstream_tls_read_want_write++;
          else
            metrics->tls_down_read_want_read++;
        }
      } else if (sent == 0) {
        state->downstream_eof = 1;
        progress = 1;
      } else {
        if (errno == ECONNRESET || errno == EPIPE) {
          kore_connection_disconnect(downstream);
          return;
        }
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
      }
    } else if (state->to_upstream_length != 0) {
      metrics->relay_read_pauses++;
    }
    if (state->downstream_eof && state->to_upstream_length == 0 &&
        !state->upstream_write_closed) {
      if (!state->relay_tls)
        assert(shutdown((int)state->upstream_fd, SHUT_WR) == 0);
      state->upstream_write_closed = 1;
      progress = 1;
    }
    if ((state->to_downstream_length == 0 ||
        (state->ws_mode && !state->ws_upgraded)) &&
        TAILQ_EMPTY(&downstream->send_queue) && !state->upstream_eof) {
      assert(state->to_downstream_length < sizeof(state->to_downstream));
      relay_recv(state,
          state->to_downstream + state->to_downstream_length,
          sizeof(state->to_downstream) - state->to_downstream_length,
          &amount, &code);
      if (code != CURLE_OK && code != CURLE_AGAIN)
        fprintf(stderr, "relay recv failed: %d %s phase=%d eof=%d\n",
            (int)code, curl_easy_strerror(code), state->phase,
            state->downstream_eof);
      assert(code == CURLE_OK || code == CURLE_AGAIN);
      if (code == CURLE_OK && amount > 0) {
        state->to_downstream_length += amount;
        progress = 1;
        if (amount > metrics->relay_max_queued)
          metrics->relay_max_queued = amount;
        if (state->ws_mode && !state->ws_upgraded &&
            !state->ws_rejected) {
          size_t prefix;

          if (state->ws_reject_mode) {
            prefix = state->to_downstream_length;
            if (prefix > sizeof(ws_reject_response) - 1)
              prefix = sizeof(ws_reject_response) - 1;
            assert(memcmp(state->to_downstream,
                ws_reject_response, prefix) == 0);
            if (state->to_downstream_length <
                sizeof(ws_reject_response) - 1)
              metrics->ws_reject_header_fragments++;
            else {
              state->ws_rejected = 1;
              state->downstream_eof = 1;
              state->upstream_write_closed = 1;
              state->to_upstream_length = 0;
              state->to_upstream_offset = 0;
              metrics->ws_rejected++;
            }
          } else {
            prefix = state->to_downstream_length;
            if (prefix > sizeof(ws_response) - 1)
              prefix = sizeof(ws_response) - 1;
            assert(memcmp(state->to_downstream, ws_response, prefix) == 0);
            if (state->to_downstream_length >= sizeof(ws_response) - 1) {
              state->ws_upgraded = 1;
              metrics->ws_upgraded++;
            }
          }
        }
      } else if (code == CURLE_OK) {
        state->upstream_eof = 1;
        progress = 1;
      } else if (downstream->tls != NULL) {
        metrics->tls_up_recv_again++;
      }
    } else if (state->to_downstream_length != 0) {
      metrics->relay_read_pauses++;
    }
    if (!progress)
      break;
  }
  if (state->upstream_eof && state->to_downstream_length == 0 &&
      TAILQ_EMPTY(&downstream->send_queue))
    state->relay_closing = 1;
  if (state->relay_closing) {
finish:
    if (downstream->tls != NULL) {
      int result;

      ERR_clear_error();
      result = SSL_shutdown(downstream->tls);
      if (result < 0) {
        ssl_error = SSL_get_error(downstream->tls, result);
        assert(ssl_error == SSL_ERROR_WANT_READ ||
            ssl_error == SSL_ERROR_WANT_WRITE);
        state->close_wait = ssl_error == SSL_ERROR_WANT_READ
            ? EPOLLIN : EPOLLOUT;
        if (state->tunnel_watched) {
          kore_platform_disable_read((int)state->upstream_fd);
          state->tunnel_watched = 0;
          state->tunnel_interest = 0;
        }
        down_events = state->close_wait | EPOLLET;
        if (!state->downstream_watched ||
            state->downstream_interest != down_events)
          kore_platform_event_schedule(downstream->fd,
              down_events, 0, downstream);
        state->downstream_watched = 1;
        state->downstream_interest = down_events;
        return;
      }
      metrics->downstream_tls_close_notify++;
    }
    state->relay_finished = 1;
    metrics->relay_done++;
    kore_connection_disconnect(downstream);
    return;
  }
  if (step == 64 && state->relay_continue_timer == NULL)
    state->relay_continue_timer = kore_timer_add(relay_continue, 0,
        state, KORE_TIMER_ONESHOT);
  if (state->ws_retry_cache_pending && !state->ws_retry_cache_release &&
      state->relay_retry_timer == NULL) {
    state->relay_retry_timer = kore_timer_add(relay_retry_cached,
        state->ws_retry_abort_mode ? 1000 : 25,
        state, KORE_TIMER_ONESHOT);
    metrics->ws_retry_timer_armed++;
  }
schedule:
  down_events = 0;
  up_events = 0;
  if (!TAILQ_EMPTY(&downstream->send_queue)) {
    if (downstream->tls != NULL &&
        SSL_want(downstream->tls) == SSL_READING) {
      down_events |= EPOLLIN;
      metrics->downstream_tls_write_want_read++;
    } else {
      down_events |= EPOLLOUT;
      metrics->tls_down_write_want_write++;
    }
  } else if (state->to_downstream_length != 0 &&
      (!state->ws_mode || state->ws_upgraded || state->ws_rejected)) {
    down_events |= EPOLLOUT;
  }
  if (state->to_upstream_length == 0 && !state->downstream_eof &&
      state->initial_offset == state->initial_length)
    down_events |= state->down_read_wait != 0
        ? state->down_read_wait : (int)(EPOLLIN | EPOLLRDHUP);
  if (state->to_upstream_length != 0 &&
      (!state->ws_mode || state->ws_upgraded ||
          state->to_upstream_offset < sizeof(ws_upstream_request) - 1))
    up_events |= EPOLLOUT;
  if ((state->to_downstream_length == 0 ||
      (state->ws_mode && !state->ws_upgraded)) &&
      TAILQ_EMPTY(&downstream->send_queue) && !state->upstream_eof)
    up_events |= EPOLLIN;
  if (state->ws_retry_mode &&
      ((state->ws_retry_send_injected && !state->ws_retry_read_seen) ||
      (state->ws_retry_recv_injected && !state->ws_retry_write_seen)))
    up_events |= EPOLLIN | EPOLLOUT;
  assert(down_events != 0 || up_events != 0);
  if (down_events != 0) {
    down_events |= EPOLLET;
    if (!state->downstream_watched ||
        state->downstream_interest != down_events)
      kore_platform_event_schedule(downstream->fd,
          down_events, 0, downstream);
    state->downstream_watched = 1;
    state->downstream_interest = down_events;
  } else if (state->downstream_watched) {
    kore_platform_disable_read(downstream->fd);
    state->downstream_watched = 0;
    state->downstream_interest = 0;
  }
  if (up_events != 0) {
    if (!state->ws_retry_mode || !state->tunnel_watched ||
        state->tunnel_interest != up_events)
      kore_platform_event_schedule((int)state->upstream_fd,
          up_events | (state->ws_retry_mode ? EPOLLET : 0),
          0, &state->tunnel_event);
    state->tunnel_watched = 1;
    state->tunnel_interest = up_events;
  } else if (state->tunnel_watched) {
    kore_platform_disable_read((int)state->upstream_fd);
    state->tunnel_watched = 0;
    state->tunnel_interest = 0;
  }
}

static void
tunnel_event(void *arg, int error)
{
  struct proxy_state *state;
  CURLcode code;
  char reply[4];
  size_t amount;

  state = (struct proxy_state *)((char *)arg -
      offsetof(struct proxy_state, tunnel_event));
  if (state->ws_retry_mode) {
    metrics->ws_retry_tunnel_events++;
    if (state->ws_retry_send_injected && !state->ws_retry_read_seen &&
        (state->tunnel_event.flags & KORE_EVENT_READ)) {
      state->ws_retry_read_seen = 1;
      metrics->ws_retry_read_wakeups++;
    }
    if (state->ws_retry_recv_injected && !state->ws_retry_write_seen &&
        (state->tunnel_event.flags & KORE_EVENT_WRITE)) {
      state->ws_retry_write_seen = 1;
      metrics->ws_retry_write_wakeups++;
    }
  }
  state->tunnel_event.flags = 0;
  if (state->relay_mode) {
    if (state->downstream->tls != NULL)
      metrics->tls_tunnel_events++;
    relay_pump(state);
    return;
  }
  if (error) {
    kore_connection_disconnect(state->downstream);
    return;
  }
  amount = 0;
  if (state->phase == 1) {
    code = curl_easy_send(state->easy, "ping", 4, &amount);
    if (code == CURLE_AGAIN)
      return;
    assert(code == CURLE_OK && amount == 4);
    state->phase = 2;
    kore_platform_event_schedule((int)state->upstream_fd,
        EPOLLIN, 0, &state->tunnel_event);
  }
  if (state->phase != 2)
    return;
  code = curl_easy_recv(state->easy, reply, sizeof(reply), &amount);
  if (code == CURLE_AGAIN)
    return;
  assert(code == CURLE_OK && amount == 4);
  assert(memcmp(reply, "pong", 4) == 0);
  metrics->tunnel_done++;
  kore_platform_disable_read((int)state->upstream_fd);
  state->tunnel_watched = 0;
  state->phase = 3;
  kore_platform_event_schedule(state->downstream->fd,
      EPOLLOUT, 0, state->downstream);
}

static void
http_schedule(struct proxy_state *state)
{
  int events;

  events = 0;
  if (state->http_queue_held) {
    assert(state->http_queue_abort_mode);
    assert(!TAILQ_EMPTY(&state->downstream->send_queue));
    events = EPOLLRDHUP | EPOLLET;
  } else if (!TAILQ_EMPTY(&state->downstream->send_queue)) {
    if (state->downstream->tls != NULL &&
        SSL_want(state->downstream->tls) == SSL_READING) {
      if (state->tls_retry_write_mode)
        metrics->tls_write_want_read_observed++;
      events |= EPOLLIN;
    } else {
      events |= EPOLLOUT;
    }
  }
  if (!state->http_queue_held &&
      TAILQ_EMPTY(&state->downstream->send_queue) &&
      ((state->http_headers_ready && !state->http_headers_sent) ||
      (state->http_headers_sent && state->http_body_length != 0) ||
      (state->http_transfer_done && !state->http_final_sent) ||
      state->http_final_sent || state->http_paused ||
      state->http_error_pending))
    events |= EPOLLOUT;
  if (state->upload_mode && !state->http_transfer_done &&
      state->initial_offset == state->initial_length &&
      (!state->chunked_upload_mode ||
      state->chunk_raw_offset == state->chunk_raw_length) &&
      state->upload_length == 0 &&
      (state->chunked_upload_mode ?
      state->chunk_phase != UPLOAD_CHUNK_COMPLETE :
      state->upload_remaining != 0))
    events |= state->upload_read_wait != 0
        ? state->upload_read_wait : (int)(EPOLLIN | EPOLLRDHUP);
  if (events == 0)
    events = EPOLLRDHUP | EPOLLET;
  else if (!state->downstream_half_closed)
    events |= EPOLLRDHUP;
  if (!(events & EPOLLET) || !state->downstream_watched ||
      state->downstream_interest != events) {
    kore_platform_event_schedule(state->downstream->fd,
        events, 0, state->downstream);
  }
  state->downstream_watched = 1;
  state->downstream_interest = events;
}

static void
chunked_decode(struct proxy_state *state, const unsigned char *data,
    size_t length, size_t *consumed)
{
  unsigned long parsed;
  size_t index;
  size_t amount;
  char *end;

  index = 0;
  while (index < length &&
      state->upload_length < sizeof(state->upload_buffer) &&
      state->chunk_phase != UPLOAD_CHUNK_COMPLETE) {
    if (state->chunk_phase == UPLOAD_CHUNK_DATA) {
      amount = length - index;
      if (amount > state->chunk_remaining)
        amount = state->chunk_remaining;
      if (amount > sizeof(state->upload_buffer) - state->upload_length)
        amount = sizeof(state->upload_buffer) - state->upload_length;
      memcpy(state->upload_buffer + state->upload_length,
          data + index, amount);
      state->upload_length += amount;
      state->chunk_total += amount;
      assert(state->chunk_total <= UPLOAD_BODY_SIZE);
      state->chunk_remaining -= amount;
      index += amount;
      if (state->chunk_remaining == 0)
        state->chunk_phase = UPLOAD_CHUNK_DATA_CR;
      continue;
    }
    if (state->chunk_phase == UPLOAD_CHUNK_DATA_CR) {
      assert(data[index++] == '\r');
      state->chunk_phase = UPLOAD_CHUNK_DATA_LF;
      continue;
    }
    if (state->chunk_phase == UPLOAD_CHUNK_DATA_LF) {
      assert(data[index++] == '\n');
      state->chunk_phase = UPLOAD_CHUNK_SIZE;
      continue;
    }
    if (state->chunk_line_cr) {
      assert(data[index++] == '\n');
      state->chunk_line_cr = 0;
      state->chunk_line[state->chunk_line_length] = '\0';
      if (state->chunk_phase == UPLOAD_CHUNK_SIZE) {
        assert(state->chunk_line_length > 0);
        errno = 0;
        parsed = strtoul(state->chunk_line, &end, 16);
        assert(errno == 0 && *end == '\0');
        assert(parsed <= UPLOAD_BODY_SIZE - state->chunk_total);
        state->chunk_remaining = (size_t)parsed;
        state->chunk_phase = parsed == 0 ?
            UPLOAD_CHUNK_TRAILERS : UPLOAD_CHUNK_DATA;
      } else {
        assert(state->chunk_phase == UPLOAD_CHUNK_TRAILERS);
        if (state->chunk_line_length == 0) {
          assert(state->chunk_total == UPLOAD_BODY_SIZE);
          assert(state->chunk_trailer_seen == 1);
          state->chunk_phase = UPLOAD_CHUNK_COMPLETE;
        } else {
          assert(strcmp(state->chunk_line, "X-Trace: done") == 0);
          state->chunk_trailer_seen++;
        }
      }
      state->chunk_line_length = 0;
    } else if (data[index] == '\r') {
      state->chunk_line_cr = 1;
      index++;
    } else {
      assert(state->chunk_line_length < sizeof(state->chunk_line) - 1);
      state->chunk_line[state->chunk_line_length++] = (char)data[index++];
    }
  }
  if (state->chunked_keepalive_mode &&
      state->chunk_phase == UPLOAD_CHUNK_COMPLETE && index < length) {
    assert(state->chunk_pipeline == NULL);
    state->chunk_pipeline_length = length - index;
    assert(state->chunk_pipeline_length <= http_header_max);
    state->chunk_pipeline = kore_malloc(state->chunk_pipeline_length);
    memcpy(state->chunk_pipeline, data + index,
        state->chunk_pipeline_length);
    if (state->chunk_pipeline_length > metrics->chunked_pipeline_max)
      metrics->chunked_pipeline_max = state->chunk_pipeline_length;
  }
  *consumed = index;
}

static int
chunked_trailer(struct curl_slist **list, void *arg)
{
  struct proxy_state *state;

  state = (struct proxy_state *)arg;
  assert(state->chunk_phase == UPLOAD_CHUNK_COMPLETE);
  assert(state->chunk_trailer_seen == 1);
  *list = curl_slist_append(NULL, "X-Trace: done");
  assert(*list != NULL);
  metrics->chunked_trailer_called++;
  return CURL_TRAILERFUNC_OK;
}

static size_t
http_upload(char *buffer, size_t size, size_t count, void *arg)
{
  struct proxy_state *state;
  size_t amount;

  state = (struct proxy_state *)arg;
  if (state->chunked_upload_mode) {
    amount = size * count;
    if (amount > state->upload_length - state->upload_offset)
      amount = state->upload_length - state->upload_offset;
    if (amount > 0) {
      memcpy(buffer, state->upload_buffer + state->upload_offset, amount);
      state->upload_offset += amount;
      if (state->upload_offset == state->upload_length) {
        state->upload_offset = 0;
        state->upload_length = 0;
        if (state->chunk_phase != UPLOAD_CHUNK_COMPLETE &&
            state->upload_continue_timer == NULL)
          state->upload_continue_timer = kore_timer_add(upload_continue,
              0, state, KORE_TIMER_ONESHOT);
      }
      http_schedule(state);
      return amount;
    }
    if (state->chunk_phase == UPLOAD_CHUNK_COMPLETE) {
      metrics->chunked_upload_done++;
      return 0;
    }
    state->upload_paused = 1;
    metrics->upload_pauses++;
    metrics->chunked_upload_pauses++;
    http_schedule(state);
    return CURL_READFUNC_PAUSE;
  }
  if (state->upload_remaining == 0)
    return 0;
  amount = size * count;
  if (amount > RELAY_BUFFER_SIZE)
    amount = RELAY_BUFFER_SIZE;
  if (amount > state->upload_remaining)
    amount = state->upload_remaining;
  if (state->initial_offset < state->initial_length) {
    if (amount > state->initial_length - state->initial_offset)
      amount = state->initial_length - state->initial_offset;
    memcpy(buffer, state->initial_data + state->initial_offset, amount);
    state->initial_offset += amount;
  } else if (state->upload_offset < state->upload_length) {
    if (amount > state->upload_length - state->upload_offset)
      amount = state->upload_length - state->upload_offset;
    memcpy(buffer, state->upload_buffer + state->upload_offset, amount);
    state->upload_offset += amount;
    if (state->upload_offset == state->upload_length) {
      state->upload_offset = 0;
      state->upload_length = 0;
      if (state->downstream->tls != NULL &&
          SSL_pending(state->downstream->tls) > 0 &&
          state->upload_remaining > amount &&
          state->upload_continue_timer == NULL)
        state->upload_continue_timer = kore_timer_add(upload_continue, 0,
            state, KORE_TIMER_ONESHOT);
    }
  } else {
    state->upload_paused = 1;
    metrics->upload_pauses++;
    http_schedule(state);
    return CURL_READFUNC_PAUSE;
  }
  assert(amount > 0);
  state->upload_remaining -= amount;
  if (state->upload_remaining == 0)
    metrics->upload_done++;
  http_schedule(state);
  return amount;
}

static size_t
http_header(char *data, size_t size, size_t count, void *arg)
{
  struct proxy_state *state;
  size_t amount;

  state = (struct proxy_state *)arg;
  amount = size * count;
  if (!state->http_status_seen) {
    if (state->h2_mode) {
      assert(amount >= sizeof("HTTP/2 200") - 1);
      assert(memcmp(data, "HTTP/2 200", sizeof("HTTP/2 200") - 1) == 0);
    } else {
      assert(amount >= 13);
      assert(memcmp(data, "HTTP/1.1 200 ", 13) == 0);
    }
    state->http_status_seen = 1;
  } else if (amount == 2 && memcmp(data, "\r\n", 2) == 0) {
    state->http_headers_ready = 1;
    metrics->http_headers_ready++;
    http_schedule(state);
  }
  return amount;
}

static size_t
http_download(char *data, size_t size, size_t count, void *arg)
{
  struct proxy_state *state;
#if defined(VECTIS_PROXY_SHARED_MULTI)
  unsigned long worker_rss;
#endif
  size_t amount;

  state = (struct proxy_state *)arg;
#if defined(VECTIS_PROXY_SHARED_MULTI)
  if (state->h2_scale_mode) {
    worker_rss = process_rss_kb(getpid());
    if (worker_rss > metrics->h2_scale_worker_peak_kb)
      metrics->h2_scale_worker_peak_kb = worker_rss;
  }
#endif
  amount = size * count;
  assert(state->http_headers_ready);
  assert(amount <= sizeof(state->http_body));
  if (state->http_body_length != 0) {
    state->http_paused = 1;
    metrics->http_body_pauses++;
#if defined(VECTIS_PROXY_SHARED_MULTI)
    if (state->h2_scale_mode && !state->h2_scale_pause_seen) {
      state->h2_scale_pause_seen = 1;
      metrics->h2_scale_paused++;
    }
    if (state->h2_cancel_mode && !state->h2_cancel_pause_seen) {
      state->h2_cancel_pause_seen = 1;
      metrics->h2_cancel_paused++;
    }
#endif
    return CURL_WRITEFUNC_PAUSE;
  }
  memcpy(state->http_body, data, amount);
  state->http_body_length = amount;
  http_schedule(state);
  return amount;
}

static int
http_pump(struct proxy_state *state)
{
  static const char response_header[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
      "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  static const char keepalive_header[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
      "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n";
  static const char gateway_error[] =
      "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 11\r\n"
      "Connection: close\r\n\r\nbad gateway";
  struct connection *downstream;
  struct netbuf *buffer;
  size_t queued;
  size_t frame_length;
  unsigned count;
  int prefix_length;

  downstream = state->downstream;
  metrics->http_pump_calls++;
  if (state->http_queue_held) {
    http_schedule(state);
    return 1;
  }
  if (!TAILQ_EMPTY(&downstream->send_queue)) {
    downstream->evt.flags |= KORE_EVENT_WRITE;
    if (net_send_flush(downstream) != KORE_RESULT_OK) {
      kore_connection_disconnect(downstream);
      return 0;
    }
    if (!TAILQ_EMPTY(&downstream->send_queue)) {
      http_schedule(state);
      return 1;
    }
  }
  if (state->http_error_pending) {
    if (state->http_error_sent) {
      kore_connection_disconnect(downstream);
      return 0;
    }
    net_send_queue(downstream, gateway_error,
        sizeof(gateway_error) - 1);
    state->http_error_sent = 1;
    metrics->http_local_errors++;
  } else if (state->http_headers_ready && !state->http_headers_sent) {
    if (state->chunked_keepalive_mode)
      net_send_queue(downstream, keepalive_header,
          sizeof(keepalive_header) - 1);
    else
      net_send_queue(downstream, response_header,
          sizeof(response_header) - 1);
    state->http_headers_sent = 1;
  } else if (state->http_body_length != 0 &&
      state->http_headers_sent) {
    prefix_length = snprintf((char *)state->http_frame,
        sizeof(state->http_frame), "%zx\r\n", state->http_body_length);
    assert(prefix_length > 0);
    frame_length = (size_t)prefix_length + state->http_body_length + 2;
    assert(frame_length <= sizeof(state->http_frame));
    memcpy(state->http_frame + prefix_length, state->http_body,
        state->http_body_length);
    memcpy(state->http_frame + prefix_length + state->http_body_length,
        "\r\n", 2);
    net_send_queue(downstream, state->http_frame, frame_length);
    state->http_body_length = 0;
    state->http_body_queued = 1;
    if (state->http_queue_abort_mode) {
      state->http_queue_held = 1;
      metrics->http_queue_held++;
    }
    metrics->http_chunks++;
  } else if (state->http_transfer_done && !state->http_final_sent) {
    net_send_queue(downstream, "0\r\n\r\n", 5);
    state->http_final_sent = 1;
  } else if (state->http_final_sent) {
    metrics->http_done++;
    if (state->chunked_keepalive_mode)
      proxy_restore_http(state);
    else
      kore_connection_disconnect(downstream);
    return 0;
  }
  if (!TAILQ_EMPTY(&downstream->send_queue)) {
    queued = 0;
    count = 0;
    TAILQ_FOREACH(buffer, &downstream->send_queue, list) {
      queued += buffer->b_len - buffer->s_off;
      count++;
    }
    if (queued > metrics->http_max_kore_queued)
      metrics->http_max_kore_queued = queued;
    if (count > metrics->http_max_kore_buffers)
      metrics->http_max_kore_buffers = count;
    http_schedule(state);
    return 1;
  }
  if (state->http_paused) {
    state->http_paused = 0;
    assert(curl_easy_pause(state->easy, CURLPAUSE_CONT) == CURLE_OK);
    if (state->http_body_length != 0 || state->http_transfer_done) {
      http_schedule(state);
      return 1;
    }
  }
  http_schedule(state);
  return 1;
}

static void
http_input_pump(struct proxy_state *state)
{
  struct connection *downstream;
  const unsigned char *data;
  size_t available;
  size_t consumed;
  size_t limit;
  ssize_t got;
  int ssl_error;

  if (!state->upload_mode || state->http_transfer_done)
    return;
  if (state->chunked_upload_mode) {
    if (state->upload_length != 0 ||
        state->chunk_phase == UPLOAD_CHUNK_COMPLETE)
      return;
    if (state->initial_offset < state->initial_length) {
      data = state->initial_data + state->initial_offset;
      available = state->initial_length - state->initial_offset;
      chunked_decode(state, data, available, &consumed);
      state->initial_offset += consumed;
    } else if (state->chunk_raw_offset < state->chunk_raw_length) {
      data = state->chunk_raw + state->chunk_raw_offset;
      available = state->chunk_raw_length - state->chunk_raw_offset;
      chunked_decode(state, data, available, &consumed);
      state->chunk_raw_offset += consumed;
      if (state->chunk_raw_offset == state->chunk_raw_length) {
        state->chunk_raw_offset = 0;
        state->chunk_raw_length = 0;
      }
    } else {
      downstream = state->downstream;
      limit = sizeof(state->chunk_raw);
      if (downstream->tls != NULL) {
        ERR_clear_error();
        got = SSL_read(downstream->tls, state->chunk_raw, (int)limit);
      } else {
        got = recv(downstream->fd, state->chunk_raw, limit, 0);
      }
      if (got > 0) {
        state->upload_read_wait = 0;
        state->chunk_raw_length = (size_t)got;
        chunked_decode(state, state->chunk_raw,
            state->chunk_raw_length, &consumed);
        state->chunk_raw_offset = consumed;
        if (state->chunk_raw_offset == state->chunk_raw_length) {
          state->chunk_raw_offset = 0;
          state->chunk_raw_length = 0;
        }
      } else if (downstream->tls != NULL) {
        ssl_error = SSL_get_error(downstream->tls, (int)got);
        assert(ssl_error == SSL_ERROR_WANT_READ ||
            ssl_error == SSL_ERROR_WANT_WRITE);
        state->upload_read_wait = ssl_error == SSL_ERROR_WANT_READ
            ? EPOLLIN : EPOLLOUT;
      } else if (got == 0) {
        assert(0 && "downstream chunked upload closed before trailer");
      } else {
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
      }
    }
    if (state->upload_length > metrics->upload_max_queued)
      metrics->upload_max_queued = state->upload_length;
    if (state->upload_length == sizeof(state->upload_buffer))
      metrics->chunked_input_pauses++;
    if (state->upload_paused &&
        (state->upload_length != 0 ||
        state->chunk_phase == UPLOAD_CHUNK_COMPLETE)) {
      state->upload_paused = 0;
      assert(curl_easy_pause(state->easy, CURLPAUSE_CONT) == CURLE_OK);
    }
    http_schedule(state);
    return;
  }
  if (state->initial_offset < state->initial_length ||
      state->upload_length != 0 || state->upload_remaining == 0)
    return;
  downstream = state->downstream;
  limit = sizeof(state->upload_buffer);
  if (limit > state->upload_remaining)
    limit = state->upload_remaining;
  if (downstream->tls != NULL) {
    ERR_clear_error();
    got = SSL_read(downstream->tls, state->upload_buffer, (int)limit);
  } else {
    got = recv(downstream->fd, state->upload_buffer, limit, 0);
  }
  if (got > 0) {
    state->upload_read_wait = 0;
    state->upload_length = (size_t)got;
    if ((size_t)got > metrics->upload_max_queued)
      metrics->upload_max_queued = (size_t)got;
    if (state->upload_paused) {
      state->upload_paused = 0;
      assert(curl_easy_pause(state->easy, CURLPAUSE_CONT) == CURLE_OK);
    }
  } else if (downstream->tls != NULL) {
    ssl_error = SSL_get_error(downstream->tls, (int)got);
    assert(ssl_error == SSL_ERROR_WANT_READ ||
        ssl_error == SSL_ERROR_WANT_WRITE);
    if (state->tls_retry_read_mode &&
        ssl_error == SSL_ERROR_WANT_WRITE)
      metrics->tls_read_want_write_observed++;
    state->upload_read_wait = ssl_error == SSL_ERROR_WANT_READ
        ? EPOLLIN : EPOLLOUT;
  } else if (got == 0) {
    assert(0 && "downstream upload closed before Content-Length");
  } else {
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
  }
  http_schedule(state);
}

static void
curl_done(struct proxy_state *state, CURLMsg *msg)
{
  long verify_result;
#if defined(VECTIS_PROXY_SHARED_MULTI)
  long http_version;
#endif

  assert(state != NULL && msg != NULL && msg->msg == CURLMSG_DONE);
  assert(state->phase == 0);
  if (msg->data.result != CURLE_OK)
    fprintf(stderr, "curl transfer failed: %s detail=%s h2=%d "
        "h2_generated=%zu down_received=%zu "
        "phase=%d upload_left=%zu read_wait=%d pending=%d paused=%d\n",
        curl_easy_strerror(msg->data.result), state->curl_error,
        state->h2_mode, metrics->h2_generated,
        metrics->h2_down_received, state->phase,
        state->upload_remaining, state->upload_read_wait,
        state->downstream->tls != NULL
            ? SSL_pending(state->downstream->tls) : 0,
        state->upload_paused);
  if (msg->data.result != CURLE_OK && state->http_mode) {
    metrics->http_upstream_failed++;
    if (state->http_tls_reset_mode) {
      assert(curl_easy_getinfo(state->easy, CURLINFO_SSL_VERIFYRESULT,
          &verify_result) == CURLE_OK);
      assert(verify_result == 0);
      metrics->http_tls_upstream_failed++;
    }
    state->phase = 1;
    state->http_transfer_done = 1;
    if (!state->http_headers_sent) {
      state->http_error_pending = 1;
      http_schedule(state);
    } else {
      kore_connection_disconnect(state->downstream);
    }
    return;
  }
  assert(msg->data.result == CURLE_OK);
  if (state->http_mode) {
#if defined(VECTIS_PROXY_SHARED_MULTI)
    if (state->h2_mode) {
      assert(curl_easy_getinfo(state->easy, CURLINFO_HTTP_VERSION,
          &http_version) == CURLE_OK);
      assert(http_version == CURL_HTTP_VERSION_2_0);
      assert(curl_easy_getinfo(state->easy, CURLINFO_SSL_VERIFYRESULT,
          &verify_result) == CURLE_OK);
      assert(verify_result == 0);
    }
#endif
    state->http_transfer_done = 1;
    state->phase = 1;
    http_schedule(state);
    return;
  }
  assert(curl_easy_getinfo(state->easy, CURLINFO_ACTIVESOCKET,
      &state->upstream_fd) == CURLE_OK);
  assert(state->upstream_fd != CURL_SOCKET_BAD);
  if (state->relay_tls) {
    verify_result = -1;
    assert(curl_easy_getinfo(state->easy, CURLINFO_SSL_VERIFYRESULT,
        &verify_result) == CURLE_OK);
    assert(verify_result == 0);
    metrics->tls_connect_done++;
  }
#if !defined(VECTIS_PROXY_SHARED_MULTI)
  assert(state->curl_watch_count == 0);
#endif
  metrics->connect_done++;
  state->phase = 1;
  if (state->relay_mode) {
    relay_pump(state);
    return;
  }
  kore_platform_event_schedule((int)state->upstream_fd,
      EPOLLOUT, 0, &state->tunnel_event);
  state->tunnel_watched = 1;
}

static void
drive_curl(struct proxy_state *state, curl_socket_t fd, int flags)
{
  CURLMsg *msg;
  int pending;

#if defined(VECTIS_PROXY_SHARED_MULTI)
  (void)state;
  assert(worker_curl.multi != NULL);
  assert(curl_multi_socket_action(worker_curl.multi, fd,
      flags, &worker_curl.running) == CURLM_OK);
  if ((unsigned)worker_curl.running > metrics->shared_running_max)
    metrics->shared_running_max = (unsigned)worker_curl.running;
  while ((msg = curl_multi_info_read(worker_curl.multi, &pending)) != NULL) {
    struct proxy_state *owner;

    owner = NULL;
    assert(msg->msg == CURLMSG_DONE);
    assert(curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE,
        &owner) == CURLE_OK);
    assert(owner != NULL && owner->easy == msg->easy_handle);
    curl_done(owner, msg);
  }
#else
  assert(curl_multi_socket_action(state->multi, fd,
      flags, &state->running) == CURLM_OK);
  if (state->running != 0 || state->phase != 0)
    return;
  msg = curl_multi_info_read(state->multi, &pending);
  curl_done(state, msg);
#endif
}

static void
curl_event(void *arg, int error)
{
  struct curl_watch *watch;
#if !defined(VECTIS_PROXY_SHARED_MULTI)
  struct proxy_state *state;
#endif
  int flags;

  watch = (struct curl_watch *)arg;
  if (watch->retired) {
    metrics->retired_watch_callbacks_ignored++;
    return;
  }
#if !defined(VECTIS_PROXY_SHARED_MULTI)
  state = watch->state;
#endif
  flags = (watch->evt.flags & KORE_EVENT_READ ? CURL_CSELECT_IN : 0) |
      (watch->evt.flags & KORE_EVENT_WRITE ? CURL_CSELECT_OUT : 0) |
      (error ? CURL_CSELECT_ERR : 0);
  watch->evt.flags = 0;
#if defined(VECTIS_PROXY_SHARED_MULTI)
  drive_curl(NULL, watch->fd, flags);
#else
  drive_curl(state, watch->fd, flags);
#endif
}

static void
proxy_cancel(struct proxy_state *state)
{
  if (state->timer != NULL) {
    kore_timer_remove(state->timer);
    state->timer = NULL;
  }
  if (state->deadline_timer != NULL) {
    kore_timer_remove(state->deadline_timer);
    state->deadline_timer = NULL;
  }
  if (state->relay_continue_timer != NULL) {
    kore_timer_remove(state->relay_continue_timer);
    state->relay_continue_timer = NULL;
  }
  if (state->relay_retry_timer != NULL) {
    if (state->ws_retry_abort_mode)
      metrics->ws_retry_timer_cancelled++;
    kore_timer_remove(state->relay_retry_timer);
    state->relay_retry_timer = NULL;
  }
  if (state->upload_continue_timer != NULL) {
    kore_timer_remove(state->upload_continue_timer);
    state->upload_continue_timer = NULL;
  }
  if (state->tunnel_watched) {
    kore_platform_disable_read((int)state->upstream_fd);
    state->tunnel_watched = 0;
    state->tunnel_interest = 0;
  }
  if (state->easy != NULL) {
#if defined(VECTIS_PROXY_SHARED_MULTI)
    if (state->h2_cancel_mode && !state->http_transfer_done)
      assert(curl_easy_setopt(state->easy, CURLOPT_FORBID_REUSE,
          1L) == CURLE_OK);
#endif
    assert(curl_multi_remove_handle(state->multi,
        state->easy) == CURLM_OK);
    curl_easy_cleanup(state->easy);
    state->easy = NULL;
  }
#if defined(VECTIS_PROXY_SHARED_MULTI)
  if (state->h2_cancel_reserved) {
    assert(metrics->h2_cancel_active > 0);
    metrics->h2_cancel_active--;
    state->h2_cancel_reserved = 0;
  }
#endif
#if defined(VECTIS_PROXY_SHARED_MULTI)
  state->multi = NULL;
#else
  assert(state->curl_watch_count == 0);
  if (state->multi != NULL) {
    assert(curl_multi_cleanup(state->multi) == CURLM_OK);
    state->multi = NULL;
  }
#endif
  if (state->upload_headers != NULL) {
    curl_slist_free_all(state->upload_headers);
    state->upload_headers = NULL;
  }
  if (state->chunk_pipeline != NULL) {
    kore_free(state->chunk_pipeline);
    state->chunk_pipeline = NULL;
  }
}

static void
proxy_restore_http(struct proxy_state *state)
{
  struct connection *connection;
  struct http_request *request;

  assert(state->chunked_keepalive_mode);
  assert(state->http_transfer_done && state->http_final_sent);
  connection = state->downstream;
  request = state->request;
  assert(request != NULL);
  assert(TAILQ_EMPTY(&connection->send_queue));
  assert(connection->http_pipeline == NULL);
  connection->http_pipeline = state->chunk_pipeline;
  connection->http_pipeline_len = state->chunk_pipeline_length;
  state->chunk_pipeline = NULL;
  proxy_cancel(state);
  request->flags |= HTTP_REQUEST_DELETE;
  http_request_wakeup(request);
  connection->disconnect = NULL;
  connection->evt.handle = kore_connection_event;
  connection->handle = kore_connection_handle;
  connection->hdlr_extra = NULL;
  connection->flags &= ~CONN_IS_BUSY;
  kore_free(state);
  kore_platform_event_schedule(connection->fd,
      EPOLLIN | EPOLLRDHUP, 0, connection);
  if (connection->http_pipeline_len == 0)
    metrics->chunked_pipeline_empty_restored++;
  metrics->chunked_pipeline_restored++;
  http_start_recv(connection);
}

static void
downstream_disconnect(struct connection *connection)
{
  struct proxy_state *state;

  state = (struct proxy_state *)connection->hdlr_extra;
  metrics->disconnects++;
  if (state != NULL) {
    if (state->tls_retry_rbio != NULL)
      BIO_set_data(state->tls_retry_rbio, NULL);
    if (state->tls_retry_wbio != NULL)
      BIO_set_data(state->tls_retry_wbio, NULL);
#if defined(VECTIS_PROXY_SHARED_MULTI)
    if (state->h2_abort_mode && !state->http_transfer_done)
      metrics->h2_abort_cancelled++;
    if (state->h2_cancel_mode && state->multi != NULL)
      metrics->h2_cancel_cancelled++;
#endif
    if (state->http_abort_mode && state->multi != NULL)
      metrics->http_abort_cancelled++;
    if (state->http_queue_abort_mode && state->multi != NULL) {
      metrics->http_queue_abort_cancelled++;
      if (!TAILQ_EMPTY(&connection->send_queue))
        metrics->http_queue_pending_at_disconnect++;
      if (connection->tls != NULL &&
          !TAILQ_EMPTY(&connection->send_queue))
        metrics->http_tls_queue_cancelled++;
    }
    if (state->http_write_fail_mode && state->multi != NULL) {
      metrics->http_write_fail_cancelled++;
      if (!TAILQ_EMPTY(&connection->send_queue))
        metrics->http_write_fail_pending_at_disconnect++;
    }
    proxy_cancel(state);
  }
}

static void
worker_cancel(void)
{
  struct connection *connection;
  struct proxy_state *state;

#if defined(VECTIS_PROXY_SHARED_MULTI)
  metrics->active_watchers_at_teardown += worker_curl.watch_count;
#endif
  TAILQ_FOREACH(connection, &connections, list) {
    if (connection->evt.handle != downstream_event)
      continue;
    state = (struct proxy_state *)connection->hdlr_extra;
    if (state != NULL && state->multi != NULL) {
#if !defined(VECTIS_PROXY_SHARED_MULTI)
      metrics->active_watchers_at_teardown += state->curl_watch_count;
#endif
      if (state->timer != NULL || state->deadline_timer != NULL ||
          state->relay_continue_timer != NULL ||
          state->relay_retry_timer != NULL)
        metrics->timers_at_teardown++;
      proxy_cancel(state);
      metrics->worker_cancelled++;
    }
  }
#if defined(VECTIS_PROXY_SHARED_MULTI)
  if (worker_curl.timer != NULL) {
    kore_timer_remove(worker_curl.timer);
    worker_curl.timer = NULL;
  }
  if (worker_curl.multi != NULL) {
    assert(curl_multi_cleanup(worker_curl.multi) == CURLM_OK);
    worker_curl.multi = NULL;
  }
  assert(worker_curl.watch_count == 0);
#endif
  if (retired_watches_timer != NULL) {
    kore_timer_remove(retired_watches_timer);
    retired_watches_timer = NULL;
  }
  reap_retired_watches(NULL, 0);
}

static void
downstream_event(void *arg, int error)
{
  static const char response[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\npong";
  struct connection *connection;
  struct proxy_state *state;
  ssize_t sent;
  int socket_error;
  u_int32_t ready_flags;
  socklen_t error_length;

  connection = (struct connection *)arg;
  if (connection->state == CONN_STATE_DISCONNECTING)
    return;
  state = (struct proxy_state *)connection->hdlr_extra;
  ready_flags = connection->evt.flags;
  connection->evt.flags = 0;
  if (state->http_mode) {
    if (state->tls_retry_read_mode &&
        state->upload_read_wait == EPOLLOUT &&
        (ready_flags & KORE_EVENT_WRITE))
      metrics->tls_read_write_wakeups++;
    if (state->tls_retry_write_mode &&
        !TAILQ_EMPTY(&connection->send_queue) &&
        SSL_want(connection->tls) == SSL_READING &&
        (ready_flags & KORE_EVENT_READ))
      metrics->tls_write_read_wakeups++;
    if (error) {
      error_length = sizeof(socket_error);
      assert(getsockopt(connection->fd, SOL_SOCKET, SO_ERROR,
          &socket_error, &error_length) == 0);
      if (socket_error != 0) {
        kore_connection_disconnect(connection);
        return;
      }
      if (!state->downstream_half_closed) {
        state->downstream_half_closed = 1;
        metrics->http_half_closed++;
      }
    }
    if (!http_pump(state))
      return;
    if (state->upload_mode && !state->http_transfer_done)
      http_input_pump(state);
    return;
  }
  if (state->relay_mode) {
    if (error) {
      error_length = sizeof(socket_error);
      assert(getsockopt(connection->fd, SOL_SOCKET, SO_ERROR,
          &socket_error, &error_length) == 0);
      if (socket_error != 0) {
        kore_connection_disconnect(connection);
        return;
      }
    }
    if (connection->tls != NULL)
      metrics->tls_downstream_events++;
    if (state->phase != 0)
      relay_pump(state);
    if (connection->evt.handle == downstream_event)
      connection->evt.flags = 0;
    return;
  }
  if (error) {
    kore_connection_disconnect(connection);
    return;
  }
  if (state->phase != 3)
    return;
  sent = send(connection->fd, response + state->response_sent,
      sizeof(response) - 1 - state->response_sent, MSG_NOSIGNAL);
  if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return;
  assert(sent > 0);
  state->response_sent += (size_t)sent;
  if (state->response_sent == sizeof(response) - 1) {
    metrics->downstream_done++;
    kore_connection_disconnect(connection);
  }
}

static int
takeover(struct http_request *req, const void *data, size_t len)
{
  struct proxy_state *state;
  unsigned short target_port;
  char url[128];

  if (strcmp(req->path, "/curl") != 0 &&
      strcmp(req->path, "/relay") != 0 &&
      strcmp(req->path, "/relay-tls") != 0 &&
      strcmp(req->path, "/ws") != 0 &&
      strcmp(req->path, "/ws-retry") != 0 &&
      strcmp(req->path, "/ws-retry-abort") != 0 &&
      strcmp(req->path, "/ws-reject") != 0 &&
      strcmp(req->path, "/sse") != 0 &&
#if defined(VECTIS_PROXY_SHARED_MULTI)
      strcmp(req->path, "/sse-h2") != 0 &&
      strcmp(req->path, "/sse-h2-scale") != 0 &&
      strcmp(req->path, "/sse-h2-abort") != 0 &&
      strcmp(req->path, "/sse-h2-cancel") != 0 &&
      strcmp(req->path, "/sse-h2-recovery") != 0 &&
#endif
      strcmp(req->path, "/sse-abort") != 0 &&
      strcmp(req->path, "/sse-queue-abort") != 0 &&
      strcmp(req->path, "/sse-write-fail") != 0 &&
      strcmp(req->path, "/sse-reset") != 0 &&
      strcmp(req->path, "/sse-reset-before") != 0 &&
      strcmp(req->path, "/sse-reset-tls") != 0 &&
      strcmp(req->path, "/sse-reset-tls-before") != 0 &&
      strcmp(req->path, "/upload") != 0 &&
      strcmp(req->path, "/upload-tls-retry") != 0 &&
      strcmp(req->path, "/upload-chunked") != 0 &&
      strcmp(req->path, "/upload-chunked-keepalive") != 0 &&
      strcmp(req->path, "/pending") != 0)
    return KORE_RESULT_OK;
  assert(len == 0 || strcmp(req->path, "/relay") == 0 ||
      strcmp(req->path, "/relay-tls") == 0 ||
      strcmp(req->path, "/ws") == 0 ||
      strcmp(req->path, "/ws-retry") == 0 ||
      strcmp(req->path, "/ws-retry-abort") == 0 ||
      strcmp(req->path, "/ws-reject") == 0 ||
      strcmp(req->path, "/upload") == 0 ||
      strcmp(req->path, "/upload-tls-retry") == 0 ||
      strcmp(req->path, "/upload-chunked") == 0 ||
      strcmp(req->path, "/upload-chunked-keepalive") == 0);
#if defined(VECTIS_PROXY_SHARED_MULTI)
  if ((strcmp(req->path, "/sse-h2-cancel") == 0 ||
      strcmp(req->path, "/sse-h2-recovery") == 0) &&
      metrics->h2_cancel_active == H2_SCALE_CONNECTIONS) {
    metrics->h2_cancel_active_at_reject = metrics->h2_cancel_active;
    metrics->h2_cancel_rejected++;
    req->owner->flags |= CONN_CLOSE_EMPTY;
    http_response(req, HTTP_STATUS_SERVICE_UNAVAILABLE, "full", 4);
    return KORE_RESULT_ERROR;
  }
#endif
  state = kore_calloc(1, sizeof(*state));
  state->relay_mode = strcmp(req->path, "/relay") == 0 ||
      strcmp(req->path, "/relay-tls") == 0 ||
      strcmp(req->path, "/ws") == 0 ||
      strcmp(req->path, "/ws-retry") == 0 ||
      strcmp(req->path, "/ws-retry-abort") == 0 ||
      strcmp(req->path, "/ws-reject") == 0;
  state->ws_mode = strcmp(req->path, "/ws") == 0 ||
      strcmp(req->path, "/ws-retry") == 0 ||
      strcmp(req->path, "/ws-retry-abort") == 0 ||
      strcmp(req->path, "/ws-reject") == 0;
  state->ws_retry_abort_mode =
      strcmp(req->path, "/ws-retry-abort") == 0;
  state->ws_retry_mode = state->ws_retry_abort_mode ||
      strcmp(req->path, "/ws-retry") == 0;
  state->ws_reject_mode = strcmp(req->path, "/ws-reject") == 0;
  state->relay_tls = strcmp(req->path, "/relay-tls") == 0 ||
      state->ws_mode;
  state->chunked_keepalive_mode =
      strcmp(req->path, "/upload-chunked-keepalive") == 0;
  state->chunked_upload_mode =
      strcmp(req->path, "/upload-chunked") == 0 ||
      state->chunked_keepalive_mode;
  state->upload_mode = strcmp(req->path, "/upload") == 0 ||
      strcmp(req->path, "/upload-tls-retry") == 0 ||
      state->chunked_upload_mode;
  state->tls_retry_read_mode =
      strcmp(req->path, "/upload-tls-retry") == 0;
  state->tls_retry_write_mode = state->tls_retry_read_mode;
  state->tls_retry_read_once = state->tls_retry_read_mode;
  state->tls_retry_write_once = state->tls_retry_write_mode;
#if defined(VECTIS_PROXY_SHARED_MULTI)
  state->h2_scale_mode = strcmp(req->path, "/sse-h2-scale") == 0;
  state->h2_abort_mode = strcmp(req->path, "/sse-h2-abort") == 0;
  state->h2_cancel_mode = strcmp(req->path, "/sse-h2-cancel") == 0 ||
      strcmp(req->path, "/sse-h2-recovery") == 0;
  if (state->h2_cancel_mode) {
    state->h2_cancel_reserved = 1;
    metrics->h2_cancel_active++;
    metrics->h2_cancel_admitted++;
  }
  state->h2_mode = strcmp(req->path, "/sse-h2") == 0 ||
      state->h2_scale_mode || state->h2_abort_mode ||
      state->h2_cancel_mode;
  if (state->h2_scale_mode) {
    if (metrics->h2_scale_admitted == 0) {
      metrics->h2_scale_worker_pid = getpid();
      metrics->h2_scale_worker_baseline_kb =
          process_rss_kb(metrics->h2_scale_worker_pid);
      metrics->h2_scale_worker_peak_kb =
          metrics->h2_scale_worker_baseline_kb;
    }
    metrics->h2_scale_admitted++;
  }
#endif
  state->http_abort_mode = strcmp(req->path, "/sse-abort") == 0;
  state->http_tls_reset_mode =
      strcmp(req->path, "/sse-reset-tls") == 0 ||
      strcmp(req->path, "/sse-reset-tls-before") == 0;
  state->http_queue_abort_mode =
      strcmp(req->path, "/sse-queue-abort") == 0;
  state->http_write_fail_mode =
      strcmp(req->path, "/sse-write-fail") == 0;
  state->tls_write_error_once = state->http_write_fail_mode &&
      req->owner->tls != NULL;
  state->http_mode = strcmp(req->path, "/sse") == 0 ||
      state->h2_mode ||
      strcmp(req->path, "/sse-reset") == 0 ||
      strcmp(req->path, "/sse-reset-before") == 0 ||
      state->http_tls_reset_mode ||
      state->http_abort_mode ||
      state->http_queue_abort_mode ||
      state->http_write_fail_mode ||
      state->upload_mode;
  if (state->upload_mode) {
    assert(req->method == HTTP_METHOD_POST);
    if (!state->chunked_upload_mode)
      assert(len <= UPLOAD_BODY_SIZE);
    state->initial_data = (const unsigned char *)data;
    state->initial_length = len;
    if (!state->chunked_upload_mode)
      state->upload_remaining = UPLOAD_BODY_SIZE;
  }
  state->request = req;
  if (state->relay_mode) {
    int sndbuf;

    if (state->ws_mode) {
      assert(len == sizeof(ws_client_early));
      assert(memcmp(data, ws_client_early, len) == 0);
      memcpy(state->to_upstream, ws_upstream_request,
          sizeof(ws_upstream_request) - 1);
      memcpy(state->to_upstream + sizeof(ws_upstream_request) - 1,
          data, len);
      state->to_upstream_length = sizeof(ws_upstream_request) - 1 + len;
    } else {
      assert(len <= http_header_max);
      state->initial_data = (const unsigned char *)data;
      state->initial_length = len;
      if (len > metrics->relay_max_initial_bytes)
        metrics->relay_max_initial_bytes = len;
      memcpy(state->to_downstream, relay_response,
          sizeof(relay_response) - 1);
      state->to_downstream_length = sizeof(relay_response) - 1;
    }
    sndbuf = 4096;
    assert(setsockopt(req->owner->fd, SOL_SOCKET, SO_SNDBUF,
        &sndbuf, sizeof(sndbuf)) == 0);
  }
  state->downstream = req->owner;
  if (state->tls_retry_read_mode || state->tls_write_error_once)
    install_tls_retry_bios(state);
  state->downstream_watched = 1;
  state->downstream->http_timeout = 0;
  state->upstream_fd = CURL_SOCKET_BAD;
  state->tunnel_event.handle = tunnel_event;
  state->tunnel_event.type = KORE_TYPE_CONNECTION;
  state->downstream->hdlr_extra = state;
  state->downstream->disconnect = downstream_disconnect;
  state->downstream->evt.handle = downstream_event;
  if (state->http_write_fail_mode)
    state->downstream->write = probe_downstream_write;
  state->downstream->evt.flags &= ~KORE_EVENT_READ;
  state->downstream->flags |= CONN_IS_BUSY;
  http_request_sleep(req);
#if defined(VECTIS_PROXY_SHARED_MULTI)
  if (worker_curl.multi == NULL) {
    worker_curl.multi = curl_multi_init();
    assert(worker_curl.multi != NULL);
    assert(curl_multi_setopt(worker_curl.multi, CURLMOPT_SOCKETFUNCTION,
        socket_change) == CURLM_OK);
    assert(curl_multi_setopt(worker_curl.multi, CURLMOPT_SOCKETDATA,
        &worker_curl) == CURLM_OK);
    assert(curl_multi_setopt(worker_curl.multi, CURLMOPT_TIMERFUNCTION,
        timer_change) == CURLM_OK);
    assert(curl_multi_setopt(worker_curl.multi, CURLMOPT_TIMERDATA,
        &worker_curl) == CURLM_OK);
    assert(curl_multi_setopt(worker_curl.multi, CURLMOPT_PIPELINING,
        CURLPIPE_NOTHING) == CURLM_OK);
    assert(curl_multi_setopt(worker_curl.multi,
        CURLMOPT_MAX_CONCURRENT_STREAMS, 1L) == CURLM_OK);
    assert(curl_multi_setopt(worker_curl.multi,
        CURLMOPT_MAX_TOTAL_CONNECTIONS,
        (long)H2_SCALE_CONNECTIONS) == CURLM_OK);
    assert(curl_multi_setopt(worker_curl.multi,
        CURLMOPT_MAXCONNECTS,
        (long)H2_SCALE_CONNECTIONS) == CURLM_OK);
  }
  state->multi = worker_curl.multi;
#else
  state->multi = curl_multi_init();
#endif
  state->easy = curl_easy_init();
  assert(state->multi != NULL && state->easy != NULL);
  assert(curl_easy_setopt(state->easy, CURLOPT_ERRORBUFFER,
      state->curl_error) == CURLE_OK);
#if defined(VECTIS_PROXY_SHARED_MULTI)
  assert(curl_easy_setopt(state->easy, CURLOPT_PRIVATE, state) == CURLE_OK);
#else
  assert(curl_multi_setopt(state->multi, CURLMOPT_SOCKETFUNCTION,
      socket_change) == CURLM_OK);
  assert(curl_multi_setopt(state->multi, CURLMOPT_SOCKETDATA,
      state) == CURLM_OK);
  assert(curl_multi_setopt(state->multi, CURLMOPT_TIMERFUNCTION,
      timer_change) == CURLM_OK);
  assert(curl_multi_setopt(state->multi, CURLMOPT_TIMERDATA,
      state) == CURLM_OK);
#endif
  target_port = upstream_port;
  if (strcmp(req->path, "/pending") == 0)
    target_port = stalled_port;
  else if (state->ws_mode)
    target_port = state->ws_reject_mode ? ws_reject_port :
        state->ws_retry_abort_mode ? ws_retry_abort_port :
        state->ws_retry_mode ? ws_retry_port : ws_port;
  else if (strcmp(req->path, "/relay-tls") == 0)
    target_port = relay_tls_port;
  else if (state->upload_mode)
    target_port = state->chunked_upload_mode ?
        upload_chunked_port : upload_port;
  else if (state->http_abort_mode)
    target_port = sse_abort_port;
  else if (state->http_queue_abort_mode)
    target_port = sse_queue_abort_port;
  else if (state->http_write_fail_mode)
    target_port = sse_write_fail_port;
  else if (strcmp(req->path, "/sse-reset") == 0)
    target_port = sse_reset_port;
  else if (strcmp(req->path, "/sse-reset-before") == 0)
    target_port = sse_reset_before_port;
  else if (strcmp(req->path, "/sse-reset-tls") == 0)
    target_port = sse_tls_reset_port;
  else if (strcmp(req->path, "/sse-reset-tls-before") == 0)
    target_port = sse_tls_reset_before_port;
  else if (state->http_mode)
#if defined(VECTIS_PROXY_SHARED_MULTI)
    target_port = strcmp(req->path, "/sse-h2-recovery") == 0 ?
        h2_recovery_port :
        state->h2_cancel_mode ? h2_cancel_port :
        state->h2_abort_mode ? h2_abort_port :
        state->h2_scale_mode ? h2_scale_port :
        state->h2_mode ? h2_port : sse_port;
#else
    target_port = sse_port;
#endif
  else if (state->relay_mode)
    target_port = relay_port;
  assert(snprintf(url, sizeof(url), "%s://%s:%u/",
      strcmp(req->path, "/pending") == 0 || state->relay_tls ||
          state->h2_mode || state->http_tls_reset_mode
          ? "https" : "http",
      state->relay_tls || state->h2_mode || state->http_tls_reset_mode
          ? "localhost" : "127.0.0.1",
      (unsigned)target_port) > 0);
  assert(curl_easy_setopt(state->easy, CURLOPT_URL, url) == CURLE_OK);
  if (!state->http_mode)
    assert(curl_easy_setopt(state->easy, CURLOPT_CONNECT_ONLY,
        1L) == CURLE_OK);
  assert(curl_easy_setopt(state->easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK);
  if (state->http_mode) {
    assert(curl_easy_setopt(state->easy, CURLOPT_HEADERFUNCTION,
        http_header) == CURLE_OK);
    assert(curl_easy_setopt(state->easy, CURLOPT_HEADERDATA,
        state) == CURLE_OK);
    assert(curl_easy_setopt(state->easy, CURLOPT_WRITEFUNCTION,
        http_download) == CURLE_OK);
    assert(curl_easy_setopt(state->easy, CURLOPT_WRITEDATA,
        state) == CURLE_OK);
    assert(curl_easy_setopt(state->easy, CURLOPT_BUFFERSIZE,
        8192L) == CURLE_OK);
    assert(curl_easy_setopt(state->easy, CURLOPT_HTTP_VERSION,
        state->h2_mode ? CURL_HTTP_VERSION_2TLS :
        CURL_HTTP_VERSION_1_1) == CURLE_OK);
    assert(curl_easy_setopt(state->easy, CURLOPT_NOPROXY,
        "*") == CURLE_OK);
    if (state->upload_mode) {
      state->upload_headers = curl_slist_append(NULL, "Expect:");
      assert(state->upload_headers != NULL);
      if (state->chunked_upload_mode) {
        state->upload_headers = curl_slist_append(
            state->upload_headers, "Trailer: X-Trace");
        assert(state->upload_headers != NULL);
      }
      assert(curl_easy_setopt(state->easy, CURLOPT_HTTPHEADER,
          state->upload_headers) == CURLE_OK);
      assert(curl_easy_setopt(state->easy, CURLOPT_UPLOAD, 1L) == CURLE_OK);
      assert(curl_easy_setopt(state->easy, CURLOPT_CUSTOMREQUEST,
          "POST") == CURLE_OK);
      assert(curl_easy_setopt(state->easy, CURLOPT_REQUEST_TARGET,
          state->chunked_upload_mode ? "/upload-chunked" :
          "/upload") == CURLE_OK);
      assert(curl_easy_setopt(state->easy, CURLOPT_INFILESIZE_LARGE,
          state->chunked_upload_mode ? (curl_off_t)-1 :
          (curl_off_t)UPLOAD_BODY_SIZE) == CURLE_OK);
      assert(curl_easy_setopt(state->easy, CURLOPT_READFUNCTION,
          http_upload) == CURLE_OK);
      assert(curl_easy_setopt(state->easy, CURLOPT_READDATA,
          state) == CURLE_OK);
      if (state->chunked_upload_mode) {
        assert(curl_easy_setopt(state->easy, CURLOPT_TRAILERFUNCTION,
            chunked_trailer) == CURLE_OK);
        assert(curl_easy_setopt(state->easy, CURLOPT_TRAILERDATA,
            state) == CURLE_OK);
      }
    }
  }
  if (state->h2_mode || state->http_tls_reset_mode) {
    assert(curl_easy_setopt(state->easy, CURLOPT_CAINFO,
        tls_cert_path) == CURLE_OK);
    assert(curl_easy_setopt(state->easy, CURLOPT_SSL_VERIFYPEER,
        1L) == CURLE_OK);
    assert(curl_easy_setopt(state->easy, CURLOPT_SSL_VERIFYHOST,
        2L) == CURLE_OK);
  }
  if (state->relay_tls) {
    assert(curl_easy_setopt(state->easy, CURLOPT_CAINFO,
        tls_cert_path) == CURLE_OK);
    assert(curl_easy_setopt(state->easy, CURLOPT_SSL_VERIFYPEER,
        1L) == CURLE_OK);
    assert(curl_easy_setopt(state->easy, CURLOPT_SSL_VERIFYHOST,
        2L) == CURLE_OK);
    assert(curl_easy_setopt(state->easy, CURLOPT_SSL_ENABLE_ALPN,
        0L) == CURLE_OK);
    assert(curl_easy_setopt(state->easy, CURLOPT_HTTP_VERSION,
        CURL_HTTP_VERSION_1_1) == CURLE_OK);
    assert(curl_easy_setopt(state->easy, CURLOPT_NOPROXY,
        "*") == CURLE_OK);
  }
#if defined(VECTIS_PROXY_SHARED_MULTI)
  if (state->h2_cancel_mode)
    metrics->h2_cancel_curl_adds++;
#endif
  assert(curl_multi_add_handle(state->multi, state->easy) == CURLM_OK);
  state->running = 1;
  if (state->http_mode && !state->upload_mode) {
    http_schedule(state);
  } else if (state->upload_mode) {
    http_schedule(state);
  }
  drive_curl(state, CURL_SOCKET_TIMEOUT, 0);
  if (state->chunked_upload_mode &&
      state->upload_continue_timer == NULL)
    state->upload_continue_timer = kore_timer_add(upload_continue, 0,
        state, KORE_TIMER_ONESHOT);
  if (state->relay_mode && state->phase == 0) {
    kore_platform_disable_read(state->downstream->fd);
    state->downstream_watched = 0;
    state->downstream_interest = 0;
  }
  if (strcmp(req->path, "/pending") == 0) {
    state->deadline_timer = kore_timer_add(deadline_expired, 10000,
        state, KORE_TIMER_ONESHOT);
    metrics->pending_started++;
  }
  return KORE_RESULT_RETRY;
}

static vectis_status
health(vectis_app *app, vectis_request *request, vectis_response *response,
    void *userdata, vectis_error *error)
{
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 200, "text/plain", "ok", error);
}

static void
sse_read_exact(int fd, void *output, size_t length)
{
  unsigned char *bytes;
  size_t received;
  ssize_t got;

  bytes = (unsigned char *)output;
  received = 0;
  while (received < length) {
    got = recv(fd, bytes + received, length - received, 0);
    assert(got > 0);
    received += (size_t)got;
  }
}

static void
sse_read_line(int fd, char *line, size_t capacity)
{
  size_t used;

  used = 0;
  for (;;) {
    assert(used < capacity - 1);
    sse_read_exact(fd, line + used, 1);
    if (line[used++] == '\n') {
      line[used] = '\0';
      return;
    }
  }
}

static void
check_sse(unsigned short port, struct echo_server *server,
    int early_halfclose, int use_h2)
{
  static const char request[] =
      "GET /sse HTTP/1.1\r\nHost: localhost\r\n\r\n";
  static const char h2_request[] =
      "GET /sse-h2 HTTP/1.1\r\nHost: localhost\r\n\r\n";
#if defined(VECTIS_PROXY_SHARED_MULTI)
  static const char h2_reuse_request[] =
      "GET /sse-h2-abort HTTP/1.1\r\nHost: localhost\r\n\r\n";
#endif
  struct sockaddr_in addr;
  struct timeval timeout;
  unsigned char body[HTTP_BODY_BUFFER_SIZE];
  char line[256];
  char ending[2];
  size_t received;
  size_t chunk;
  size_t i;
  int saw_type;
  int saw_chunked;
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  if (use_h2 == 1)
    send_all(fd, h2_request, sizeof(h2_request) - 1);
#if defined(VECTIS_PROXY_SHARED_MULTI)
  else if (use_h2 == 2)
    send_all(fd, h2_reuse_request, sizeof(h2_reuse_request) - 1);
#endif
  else
    send_all(fd, request, sizeof(request) - 1);
  if (early_halfclose)
    assert(shutdown(fd, SHUT_WR) == 0);
  sse_read_line(fd, line, sizeof(line));
  assert(strstr(line, " 200 ") != NULL);
  saw_type = 0;
  saw_chunked = 0;
  for (;;) {
    sse_read_line(fd, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
    if (strstr(line, "Content-Type: text/event-stream") != NULL)
      saw_type = 1;
    if (strstr(line, "Transfer-Encoding: chunked") != NULL)
      saw_chunked = 1;
  }
  assert(saw_type && saw_chunked);
  if (!early_halfclose)
    assert(shutdown(fd, SHUT_WR) == 0);
  __sync_lock_test_and_set(&server->allow_body, 1);
  received = 0;
  for (;;) {
    sse_read_line(fd, line, sizeof(line));
    chunk = strtoul(line, NULL, 16);
    if (chunk == 0)
      break;
    assert(chunk <= sizeof(body));
    assert(chunk <= SSE_BODY_SIZE - received);
    sse_read_exact(fd, body, chunk);
    for (i = 0; i < chunk; i++)
      assert(body[i] == sse_byte(received + i));
    received += chunk;
    if (use_h2 == 1)
      metrics->h2_down_received = received;
    sse_read_exact(fd, ending, sizeof(ending));
    assert(memcmp(ending, "\r\n", 2) == 0);
    usleep(1000u);
  }
  assert(received == SSE_BODY_SIZE);
  sse_read_line(fd, line, sizeof(line));
  assert(strcmp(line, "\r\n") == 0);
  assert(recv(fd, ending, sizeof(ending), 0) == 0);
  if (use_h2 == 1)
    __sync_lock_test_and_set(&metrics->h2_down_done, 1);
#if defined(VECTIS_PROXY_SHARED_MULTI)
  else if (use_h2 == 2)
    __sync_lock_test_and_set(&metrics->h2_abort_reuse_done, 1);
#endif
  assert(close(fd) == 0);
}

#if defined(VECTIS_PROXY_SHARED_MULTI)
static void
check_h2_abort(unsigned short port)
{
  static const char request[] =
      "GET /sse-h2-abort HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct sockaddr_in addr;
  struct timeval timeout;
  struct linger reset;
  unsigned char body[HTTP_BODY_BUFFER_SIZE];
  char line[256];
  char ending[2];
  size_t chunk;
  size_t i;
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  send_all(fd, request, sizeof(request) - 1);
  sse_read_line(fd, line, sizeof(line));
  assert(strstr(line, " 200 ") != NULL);
  for (;;) {
    sse_read_line(fd, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
  }
  sse_read_line(fd, line, sizeof(line));
  chunk = strtoul(line, NULL, 16);
  assert(chunk > 0 && chunk <= sizeof(body));
  sse_read_exact(fd, body, chunk);
  for (i = 0; i < chunk; i++)
    assert(body[i] == sse_byte(i));
  sse_read_exact(fd, ending, sizeof(ending));
  assert(memcmp(ending, "\r\n", 2) == 0);
  reset.l_onoff = 1;
  reset.l_linger = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_LINGER,
      &reset, sizeof(reset)) == 0);
  assert(close(fd) == 0);
}

struct h2_scale_client {
  unsigned short port;
  int recovery_mode;
};

static void
check_h2_saturation(unsigned short port)
{
  static const char request[] =
      "GET /sse-h2-cancel HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct sockaddr_in addr;
  struct timeval timeout;
  char line[256];
  char bytes[1024];
  ssize_t got;
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  send_all(fd, request, sizeof(request) - 1);
  sse_read_line(fd, line, sizeof(line));
  assert(strstr(line, " 503 ") != NULL);
  while ((got = recv(fd, bytes, sizeof(bytes), 0)) > 0) {
  }
  assert(got == 0);
  assert(close(fd) == 0);
}

static void *
h2_scale_client_main(void *arg)
{
  static const char request[] =
      "GET /sse-h2-scale HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct h2_scale_client *client;
  struct sockaddr_in addr;
  struct timeval timeout;
  unsigned char body[HTTP_BODY_BUFFER_SIZE];
  char line[256];
  char ending[2];
  size_t received;
  size_t chunk;
  size_t i;
  int saw_type;
  int saw_chunked;
  int attempt;
  int fd;

  client = (struct h2_scale_client *)arg;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(client->port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 20;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  send_all(fd, request, sizeof(request) - 1);
  sse_read_line(fd, line, sizeof(line));
  assert(strstr(line, " 200 ") != NULL);
  saw_type = 0;
  saw_chunked = 0;
  for (;;) {
    sse_read_line(fd, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
    if (strstr(line, "Content-Type: text/event-stream") != NULL)
      saw_type = 1;
    if (strstr(line, "Transfer-Encoding: chunked") != NULL)
      saw_chunked = 1;
  }
  assert(saw_type && saw_chunked);
  __sync_fetch_and_add(&metrics->h2_scale_headers, 1);
  for (attempt = 0; attempt < 10000 &&
      __sync_fetch_and_add(&h2_scale_release, 0) == 0; attempt++)
    usleep(1000u);
  assert(__sync_fetch_and_add(&h2_scale_release, 0) == 1);
  received = 0;
  for (;;) {
    sse_read_line(fd, line, sizeof(line));
    chunk = strtoul(line, NULL, 16);
    if (chunk == 0)
      break;
    assert(chunk <= sizeof(body));
    assert(chunk <= H2_SCALE_BODY_SIZE - received);
    sse_read_exact(fd, body, chunk);
    for (i = 0; i < chunk; i++)
      assert(body[i] == sse_byte(received + i));
    received += chunk;
    sse_read_exact(fd, ending, sizeof(ending));
    assert(memcmp(ending, "\r\n", 2) == 0);
  }
  assert(received == H2_SCALE_BODY_SIZE);
  sse_read_line(fd, line, sizeof(line));
  assert(strcmp(line, "\r\n") == 0);
  assert(recv(fd, ending, sizeof(ending), 0) == 0);
  __sync_fetch_and_add(&metrics->h2_scale_received, received);
  __sync_fetch_and_add(&metrics->h2_scale_done, 1);
  assert(close(fd) == 0);
  return NULL;
}

static void *
h2_cancel_client_main(void *arg)
{
  static const char request[] =
      "GET /sse-h2-cancel HTTP/1.1\r\nHost: localhost\r\n\r\n";
  static const char recovery_request[] =
      "GET /sse-h2-recovery HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct h2_scale_client *client;
  struct sockaddr_in addr;
  struct timeval timeout;
  struct linger reset;
  char line[256];
  int attempt;
  int fd;

  client = (struct h2_scale_client *)arg;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(client->port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  if (client->recovery_mode)
    send_all(fd, recovery_request, sizeof(recovery_request) - 1);
  else
    send_all(fd, request, sizeof(request) - 1);
  sse_read_line(fd, line, sizeof(line));
  assert(strstr(line, " 200 ") != NULL);
  for (;;) {
    sse_read_line(fd, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
  }
  __sync_fetch_and_add(&metrics->h2_cancel_headers, 1);
  for (attempt = 0; attempt < 10000 &&
      __sync_fetch_and_add(&h2_cancel_release, 0) == 0; attempt++)
    usleep(1000u);
  assert(__sync_fetch_and_add(&h2_cancel_release, 0) == 1);
  reset.l_onoff = 1;
  reset.l_linger = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_LINGER,
      &reset, sizeof(reset)) == 0);
  assert(close(fd) == 0);
  return NULL;
}

struct concurrent_sse_client {
  unsigned short port;
  struct echo_server *server;
};

static void *
concurrent_sse_main(void *arg)
{
  struct concurrent_sse_client *client;

  client = (struct concurrent_sse_client *)arg;
  check_sse(client->port, client->server, 0, 0);
  return NULL;
}
#endif

static void
check_sse_abort(unsigned short port)
{
  static const char request[] =
      "GET /sse-abort HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct sockaddr_in addr;
  struct timeval timeout;
  struct linger reset;
  char line[256];
  char body[4];
  char ending[2];
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  send_all(fd, request, sizeof(request) - 1);
  sse_read_line(fd, line, sizeof(line));
  assert(strstr(line, " 200 ") != NULL);
  for (;;) {
    sse_read_line(fd, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
  }
  sse_read_line(fd, line, sizeof(line));
  assert(strcmp(line, "4\r\n") == 0);
  sse_read_exact(fd, body, sizeof(body));
  assert(memcmp(body, "pong", sizeof(body)) == 0);
  sse_read_exact(fd, ending, sizeof(ending));
  assert(memcmp(ending, "\r\n", sizeof(ending)) == 0);
  reset.l_onoff = 1;
  reset.l_linger = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_LINGER,
      &reset, sizeof(reset)) == 0);
  assert(close(fd) == 0);
}

static void
check_sse_queue_abort(unsigned short port, struct echo_server *server)
{
  static const char request[] =
      "GET /sse-queue-abort HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct sockaddr_in addr;
  struct timeval timeout;
  struct linger reset;
  char line[256];
  int attempt;
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  send_all(fd, request, sizeof(request) - 1);
  sse_read_line(fd, line, sizeof(line));
  assert(strstr(line, " 200 ") != NULL);
  for (;;) {
    sse_read_line(fd, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
  }
  __sync_lock_test_and_set(&server->allow_body, 1);
  for (attempt = 0; attempt < 500 &&
      __sync_fetch_and_add(&metrics->http_queue_held, 0) == 0;
      attempt++)
    usleep(10000u);
  assert(__sync_fetch_and_add(&metrics->http_queue_held, 0) == 1);
  reset.l_onoff = 1;
  reset.l_linger = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_LINGER,
      &reset, sizeof(reset)) == 0);
  assert(close(fd) == 0);
}

static void
check_tls_sse_queue_abort(unsigned short port, struct echo_server *server)
{
  static const char request[] =
      "GET /sse-queue-abort HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct sockaddr_in addr;
  struct timeval timeout;
  struct linger reset;
  SSL_CTX *ctx;
  SSL *ssl;
  char response[256];
  size_t used;
  int attempt;
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
      &timeout, sizeof(timeout)) == 0);
  ctx = SSL_CTX_new(TLS_client_method());
  assert(ctx != NULL);
  assert(SSL_CTX_load_verify_locations(ctx, tls_cert_path, NULL) == 1);
  ssl = SSL_new(ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
  assert(SSL_set1_host(ssl, "localhost") == 1);
  assert(SSL_connect(ssl) == 1);
  assert(SSL_get_verify_result(ssl) == X509_V_OK);
  assert(SSL_write(ssl, request, (int)(sizeof(request) - 1)) ==
      (int)(sizeof(request) - 1));
  used = 0;
  response[0] = '\0';
  while (strstr(response, "\r\n\r\n") == NULL) {
    assert(used < sizeof(response) - 1);
    assert(SSL_read(ssl, response + used, 1) == 1);
    response[++used] = '\0';
  }
  assert(strstr(response, "HTTP/1.1 200 OK\r\n") == response);
  assert(strstr(response, "Transfer-Encoding: chunked\r\n") != NULL);
  __sync_lock_test_and_set(&server->allow_body, 1);
  for (attempt = 0; attempt < 500 &&
      __sync_fetch_and_add(&metrics->http_queue_held, 0) < 2;
      attempt++)
    usleep(10000u);
  assert(__sync_fetch_and_add(&metrics->http_queue_held, 0) == 2);
  reset.l_onoff = 1;
  reset.l_linger = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_LINGER,
      &reset, sizeof(reset)) == 0);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  assert(close(fd) == 0);
}

static void
check_sse_write_fail(unsigned short port)
{
  static const char request[] =
      "GET /sse-write-fail HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct sockaddr_in addr;
  struct timeval timeout;
  char line[256];
  ssize_t got;
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  send_all(fd, request, sizeof(request) - 1);
  sse_read_line(fd, line, sizeof(line));
  assert(strstr(line, " 200 ") != NULL);
  for (;;) {
    sse_read_line(fd, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
  }
  got = recv(fd, line, sizeof(line), 0);
  assert(got == 0 || (got < 0 && errno == ECONNRESET));
  assert(close(fd) == 0);
}

static void
check_tls_sse_write_fail(unsigned short port)
{
  static const char request[] =
      "GET /sse-write-fail HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct sockaddr_in addr;
  struct timeval timeout;
  SSL_CTX *ctx;
  SSL *ssl;
  char response[256];
  size_t used;
  int fd;
  int got;
  int ssl_error;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
      &timeout, sizeof(timeout)) == 0);
  ctx = SSL_CTX_new(TLS_client_method());
  assert(ctx != NULL);
  assert(SSL_CTX_load_verify_locations(ctx, tls_cert_path, NULL) == 1);
  ssl = SSL_new(ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
  assert(SSL_set1_host(ssl, "localhost") == 1);
  assert(SSL_connect(ssl) == 1);
  assert(SSL_get_verify_result(ssl) == X509_V_OK);
  assert(SSL_write(ssl, request, (int)(sizeof(request) - 1)) ==
      (int)(sizeof(request) - 1));
  used = 0;
  response[0] = '\0';
  while (strstr(response, "\r\n\r\n") == NULL) {
    assert(used < sizeof(response) - 1);
    assert(SSL_read(ssl, response + used, 1) == 1);
    response[++used] = '\0';
  }
  assert(strstr(response, "HTTP/1.1 200 OK\r\n") == response);
  assert(strstr(response, "Transfer-Encoding: chunked\r\n") != NULL);
  got = SSL_read(ssl, response, sizeof(response));
  assert(got <= 0);
  ssl_error = SSL_get_error(ssl, got);
  assert(ssl_error != SSL_ERROR_WANT_READ &&
      ssl_error != SSL_ERROR_WANT_WRITE);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  assert(close(fd) == 0);
}

static void
check_sse_reset(unsigned short port, struct echo_server *server, int tls)
{
  static const char request[] =
      "GET /sse-reset HTTP/1.1\r\nHost: localhost\r\n\r\n";
  static const char tls_request[] =
      "GET /sse-reset-tls HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct sockaddr_in addr;
  struct timeval timeout;
  char line[256];
  char body[4];
  char ending[2];
  ssize_t got;
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  if (tls)
    send_all(fd, tls_request, sizeof(tls_request) - 1);
  else
    send_all(fd, request, sizeof(request) - 1);
  sse_read_line(fd, line, sizeof(line));
  assert(strstr(line, " 200 ") != NULL);
  for (;;) {
    sse_read_line(fd, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
  }
  sse_read_line(fd, line, sizeof(line));
  assert(strcmp(line, "4\r\n") == 0);
  sse_read_exact(fd, body, sizeof(body));
  assert(memcmp(body, "pong", sizeof(body)) == 0);
  sse_read_exact(fd, ending, sizeof(ending));
  assert(memcmp(ending, "\r\n", sizeof(ending)) == 0);
  __sync_lock_test_and_set(&server->allow_body, 1);
  got = recv(fd, line, sizeof(line), 0);
  assert(got == 0 || (got < 0 && errno == ECONNRESET));
  assert(close(fd) == 0);
}

static void
check_sse_reset_before(unsigned short port, int tls)
{
  static const char request[] =
      "GET /sse-reset-before HTTP/1.1\r\nHost: localhost\r\n\r\n";
  static const char tls_request[] =
      "GET /sse-reset-tls-before HTTP/1.1\r\nHost: localhost\r\n\r\n";
  static const char expected[] =
      "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 11\r\n"
      "Connection: close\r\n\r\nbad gateway";
  struct sockaddr_in addr;
  struct timeval timeout;
  char response[256];
  size_t used;
  ssize_t got;
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  if (tls)
    send_all(fd, tls_request, sizeof(tls_request) - 1);
  else
    send_all(fd, request, sizeof(request) - 1);
  used = 0;
  while (used < sizeof(response)) {
    got = recv(fd, response + used, sizeof(response) - used, 0);
    assert(got >= 0);
    if (got == 0)
      break;
    used += (size_t)got;
  }
  assert(used == sizeof(expected) - 1);
  assert(memcmp(response, expected, used) == 0);
  assert(close(fd) == 0);
}

static void
check_upload(unsigned short port, struct echo_server *server)
{
  static const char request[] =
      "POST /upload HTTP/1.1\r\nHost: localhost\r\n"
      "Content-Length: 1048576\r\n\r\n";
  struct sockaddr_in addr;
  struct timeval timeout;
  struct relay_sender sender;
  pthread_t thread;
  char line[256];
  char body[4];
  char ending[2];
  int fd;
  int saw_chunked;
  int attempt;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
      &timeout, sizeof(timeout)) == 0);
  send_all(fd, request, sizeof(request) - 1);
  sender.fd = fd;
  sender.start = 0;
  sender.delay_us = 2000;
  sender.progress = 0;
  assert(pthread_create(&thread, NULL, relay_send_main, &sender) == 0);
  sse_read_line(fd, line, sizeof(line));
  assert(strstr(line, " 200 ") != NULL);
  saw_chunked = 0;
  for (;;) {
    sse_read_line(fd, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
    if (strstr(line, "Transfer-Encoding: chunked") != NULL)
      saw_chunked = 1;
  }
  assert(saw_chunked);
  sse_read_line(fd, line, sizeof(line));
  assert(strcmp(line, "4\r\n") == 0);
  sse_read_exact(fd, body, sizeof(body));
  assert(memcmp(body, "pong", sizeof(body)) == 0);
  sse_read_exact(fd, ending, sizeof(ending));
  assert(memcmp(ending, "\r\n", 2) == 0);
  for (attempt = 0; attempt < 1000 &&
      __sync_fetch_and_add(&server->allow_body, 0) == 0; attempt++)
    usleep(1000u);
  assert(__sync_fetch_and_add(&server->allow_body, 0) == 1);
  assert(__sync_fetch_and_add(&sender.progress, 0) < UPLOAD_BODY_SIZE);
  sse_read_line(fd, line, sizeof(line));
  assert(strcmp(line, "4\r\n") == 0);
  sse_read_exact(fd, body, sizeof(body));
  assert(memcmp(body, "done", sizeof(body)) == 0);
  sse_read_exact(fd, ending, sizeof(ending));
  assert(memcmp(ending, "\r\n", 2) == 0);
  sse_read_line(fd, line, sizeof(line));
  assert(strcmp(line, "0\r\n") == 0);
  sse_read_line(fd, line, sizeof(line));
  assert(strcmp(line, "\r\n") == 0);
  assert(recv(fd, body, sizeof(body), 0) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(close(fd) == 0);
}

static void *
upload_chunked_send_main(void *arg)
{
  static const char final[] = "0\r\nX-Trace: done\r\n\r\n";
  struct relay_sender *sender;
  unsigned char data[16384];
  char line[32];
  size_t offset;
  size_t n;
  int length;

  sender = (struct relay_sender *)arg;
  for (offset = 0; offset < UPLOAD_BODY_SIZE; offset += sizeof(data)) {
    for (n = 0; n < sizeof(data); n++)
      data[n] = relay_byte(offset + n);
    length = snprintf(line, sizeof(line), "%zx\r\n", sizeof(data));
    assert(length > 0 && (size_t)length < sizeof(line));
    send_all(sender->fd, line, (size_t)length);
    send_all(sender->fd, data, sizeof(data));
    send_all(sender->fd, "\r\n", 2);
    __sync_lock_test_and_set(&sender->progress, offset + sizeof(data));
    usleep(2000u);
  }
  send_all(sender->fd, final, sizeof(final) - 1);
  assert(shutdown(sender->fd, SHUT_WR) == 0);
  return NULL;
}

static void
check_chunked_upload(unsigned short port, struct echo_server *server)
{
  static const char request[] =
      "POST /upload-chunked HTTP/1.1\r\nHost: localhost\r\n"
      "Transfer-Encoding: chunked\r\nTrailer: X-Trace\r\n\r\n";
  struct sockaddr_in addr;
  struct timeval timeout;
  struct relay_sender sender;
  pthread_t thread;
  char line[256];
  char body[4];
  char ending[2];
  int fd;
  int saw_chunked;
  int attempt;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
      &timeout, sizeof(timeout)) == 0);
  send_all(fd, request, sizeof(request) - 1);
  memset(&sender, 0, sizeof(sender));
  sender.fd = fd;
  assert(pthread_create(&thread, NULL,
      upload_chunked_send_main, &sender) == 0);
  sse_read_line(fd, line, sizeof(line));
  assert(strstr(line, " 200 ") != NULL);
  saw_chunked = 0;
  for (;;) {
    sse_read_line(fd, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
    if (strstr(line, "Transfer-Encoding: chunked") != NULL)
      saw_chunked = 1;
  }
  assert(saw_chunked);
  sse_read_line(fd, line, sizeof(line));
  assert(strcmp(line, "4\r\n") == 0);
  sse_read_exact(fd, body, sizeof(body));
  assert(memcmp(body, "pong", sizeof(body)) == 0);
  sse_read_exact(fd, ending, sizeof(ending));
  assert(memcmp(ending, "\r\n", 2) == 0);
  for (attempt = 0; attempt < 1000 &&
      __sync_fetch_and_add(&server->allow_body, 0) == 0; attempt++)
    usleep(1000u);
  assert(__sync_fetch_and_add(&server->allow_body, 0) == 1);
  assert(__sync_fetch_and_add(&sender.progress, 0) < UPLOAD_BODY_SIZE);
  sse_read_line(fd, line, sizeof(line));
  assert(strcmp(line, "4\r\n") == 0);
  sse_read_exact(fd, body, sizeof(body));
  assert(memcmp(body, "done", sizeof(body)) == 0);
  sse_read_exact(fd, ending, sizeof(ending));
  assert(memcmp(ending, "\r\n", 2) == 0);
  sse_read_line(fd, line, sizeof(line));
  assert(strcmp(line, "0\r\n") == 0);
  sse_read_line(fd, line, sizeof(line));
  assert(strcmp(line, "\r\n") == 0);
  assert(recv(fd, body, sizeof(body), 0) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(close(fd) == 0);
}

static void
pipeline_send(int fd, SSL *ssl, const void *data, size_t length)
{
  const unsigned char *bytes;
  size_t offset;
  size_t window;
  int sent;

  if (ssl == NULL) {
    send_all(fd, data, length);
    return;
  }
  bytes = (const unsigned char *)data;
  offset = 0;
  while (offset < length) {
    window = length - offset;
    if (window > 16384)
      window = 16384;
    sent = SSL_write(ssl, bytes + offset, (int)window);
    assert(sent > 0);
    offset += (size_t)sent;
  }
}

static void
check_chunked_keepalive(unsigned short port,
    struct echo_server *server, int tls)
{
  static const char request[] =
      "POST /upload-chunked-keepalive HTTP/1.1\r\nHost: localhost\r\n"
      "Transfer-Encoding: chunked\r\nTrailer: X-Trace\r\n\r\n";
  static const char trailer_and_next[] =
      "0\r\nX-Trace: done\r\n\r\n"
      "GET /health HTTP/1.1\r\nHost: localhost\r\n"
      "Connection: close\r\n\r\n";
  static const char trailer[] = "0\r\nX-Trace: done\r\n\r\n";
  static const char next_request[] =
      "GET /health HTTP/1.1\r\nHost: localhost\r\n"
      "Connection: close\r\n\r\n";
  struct sockaddr_in addr;
  struct timeval timeout;
  SSL_CTX *ctx;
  SSL *ssl;
  unsigned char data[16384];
  char line[32];
  char response[2048];
  char *first;
  char *second;
  char *marker;
  size_t offset;
  size_t n;
  size_t used;
  ssize_t got;
  int fd;
  int length;
  int ssl_error;
  int attempt;
  unsigned done_before;
  unsigned restored_before;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
      &timeout, sizeof(timeout)) == 0);
  ctx = NULL;
  ssl = NULL;
  if (tls) {
    ctx = SSL_CTX_new(TLS_client_method());
    assert(ctx != NULL);
    assert(SSL_CTX_load_verify_locations(ctx, tls_cert_path, NULL) == 1);
    ssl = SSL_new(ctx);
    assert(ssl != NULL);
    assert(SSL_set_fd(ssl, fd) == 1);
    assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
    assert(SSL_set1_host(ssl, "localhost") == 1);
    assert(SSL_connect(ssl) == 1);
    assert(SSL_get_verify_result(ssl) == X509_V_OK);
  }
  pipeline_send(fd, ssl, request, sizeof(request) - 1);
  for (offset = 0; offset < UPLOAD_BODY_SIZE; offset += sizeof(data)) {
    length = snprintf(line, sizeof(line), "%zx\r\n", sizeof(data));
    assert(length > 0 && (size_t)length < sizeof(line));
    for (n = 0; n < sizeof(data); n++)
      data[n] = relay_byte(offset + n);
    pipeline_send(fd, ssl, line, (size_t)length);
    pipeline_send(fd, ssl, data, sizeof(data));
    pipeline_send(fd, ssl, "\r\n", 2);
  }
  usleep(10000u);
  if (server->hold_final) {
    done_before = __sync_fetch_and_add(&metrics->chunked_upload_done, 0);
    restored_before = __sync_fetch_and_add(
        &metrics->chunked_pipeline_restored, 0);
    pipeline_send(fd, ssl, trailer, sizeof(trailer) - 1);
    for (attempt = 0; attempt < 10000 &&
        __sync_fetch_and_add(&metrics->chunked_upload_done, 0) ==
        done_before; attempt++)
      usleep(1000u);
    assert(__sync_fetch_and_add(&metrics->chunked_upload_done, 0) >
        done_before);
    assert(__sync_fetch_and_add(&metrics->chunked_pipeline_restored, 0) ==
        restored_before);
    pipeline_send(fd, ssl, next_request,
        sizeof(next_request) - 1);
    usleep(1000u);
    __sync_lock_test_and_set(&server->release_final, 1);
  } else {
    pipeline_send(fd, ssl, trailer_and_next,
        sizeof(trailer_and_next) - 1);
  }
  used = 0;
  response[0] = '\0';
  while (used < sizeof(response) - 1) {
    if (ssl == NULL) {
      got = recv(fd, response + used,
          sizeof(response) - 1 - used, 0);
    } else {
      ERR_clear_error();
      got = SSL_read(ssl, response + used,
          (int)(sizeof(response) - 1 - used));
      if (got <= 0) {
        ssl_error = SSL_get_error(ssl, (int)got);
        assert(ssl_error == SSL_ERROR_ZERO_RETURN);
        break;
      }
    }
    if (got < 0)
      fprintf(stderr, "pipeline recv: errno=%d bytes=%zu "
          "restored=%u saved=%zu eof=%u trailer=%u "
          "headers=%u pauses=%u server_early=%d response=%s\n", errno, used,
          metrics->chunked_pipeline_restored,
          metrics->chunked_pipeline_max, metrics->chunked_upload_done,
          metrics->chunked_trailer_called, metrics->http_headers_ready,
          metrics->chunked_upload_pauses, server->allow_body, response);
    assert(got >= 0);
    if (got == 0)
      break;
    used += (size_t)got;
    response[used] = '\0';
    second = strstr(response + 1, "HTTP/1.1 200 OK\r\n");
    if (second != NULL && strstr(second, "\r\n\r\nok") != NULL)
      break;
  }
  first = strstr(response, "HTTP/1.1 200 OK\r\n");
  assert(first == response);
  second = strstr(first + 1, "HTTP/1.1 200 OK\r\n");
  assert(second != NULL);
  assert(strstr(second + 1, "HTTP/1.1 200 OK\r\n") == NULL);
  marker = strstr(first, "Connection: keep-alive\r\n");
  assert(marker != NULL && marker < second);
  marker = strstr(first, "pong");
  assert(marker != NULL && marker < second);
  marker = strstr(first, "done");
  assert(marker != NULL && marker < second);
  marker = strstr(first, "0\r\n\r\n");
  assert(marker != NULL && marker < second);
  assert(strstr(second, "ok") != NULL);
  assert(__sync_fetch_and_add(&server->allow_body, 0) == 1);
  if (ssl != NULL)
    SSL_free(ssl);
  if (ctx != NULL)
    SSL_CTX_free(ctx);
  assert(close(fd) == 0);
}

static void
check_tls_upload(unsigned short port, struct echo_server *server,
    int inject_retry)
{
  static const char request[] =
      "POST /upload HTTP/1.1\r\nHost: localhost\r\n"
      "Content-Length: 1048576\r\n\r\n";
  static const char retry_request[] =
      "POST /upload-tls-retry HTTP/1.1\r\nHost: localhost\r\n"
      "Content-Length: 1048576\r\n\r\n";
  struct sockaddr_in addr;
  struct pollfd watch;
  struct timeval timeout;
  SSL_CTX *ctx;
  SSL *ssl;
  unsigned char output[16384];
  char response[512];
  size_t sent;
  size_t used;
  size_t index;
  int fd;
  int flags;
  int amount;
  int ssl_error;
  int events;
  int progress;
  int early_seen;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
      &timeout, sizeof(timeout)) == 0);
  ctx = SSL_CTX_new(TLS_client_method());
  assert(ctx != NULL);
  assert(SSL_CTX_load_verify_locations(ctx, tls_cert_path, NULL) == 1);
  ssl = SSL_new(ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
  assert(SSL_set1_host(ssl, "localhost") == 1);
  assert(SSL_connect(ssl) == 1);
  assert(SSL_get_verify_result(ssl) == X509_V_OK);
  if (inject_retry) {
    assert(SSL_write(ssl, retry_request,
        (int)(sizeof(retry_request) - 1)) ==
        (int)(sizeof(retry_request) - 1));
  } else {
    assert(SSL_write(ssl, request, (int)(sizeof(request) - 1)) ==
        (int)(sizeof(request) - 1));
  }
  flags = fcntl(fd, F_GETFL, 0);
  assert(flags >= 0);
  assert(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
  sent = 0;
  used = 0;
  early_seen = 0;
  memset(response, 0, sizeof(response));
  while (strstr(response, "0\r\n\r\n") == NULL) {
    progress = 0;
    events = 0;
    if (sent < UPLOAD_BODY_SIZE) {
      for (index = 0; index < sizeof(output); index++)
        output[index] = relay_byte(sent + index);
      ERR_clear_error();
      amount = SSL_write(ssl, output, (int)sizeof(output));
      if (amount > 0) {
        assert(amount == (int)sizeof(output));
        sent += (size_t)amount;
        progress = 1;
        usleep(1000u);
      } else {
        ssl_error = SSL_get_error(ssl, amount);
        assert(ssl_error == SSL_ERROR_WANT_READ ||
            ssl_error == SSL_ERROR_WANT_WRITE);
        events |= ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
      }
    }
    ERR_clear_error();
    amount = SSL_read(ssl, response + used,
        (int)(sizeof(response) - used - 1));
    if (amount > 0) {
      used += (size_t)amount;
      response[used] = '\0';
      progress = 1;
      if (!early_seen && strstr(response, "pong") != NULL) {
        assert(sent < UPLOAD_BODY_SIZE);
        early_seen = 1;
      }
    } else {
      ssl_error = SSL_get_error(ssl, amount);
      assert(ssl_error == SSL_ERROR_WANT_READ ||
          ssl_error == SSL_ERROR_WANT_WRITE);
      events |= ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
    }
    if (!progress) {
      assert(events != 0);
      watch.fd = fd;
      watch.events = (short)events;
      assert(poll(&watch, 1, 10000) > 0);
    }
  }
  assert(sent == UPLOAD_BODY_SIZE);
  assert(early_seen);
  assert(__sync_fetch_and_add(&server->allow_body, 0) == 1);
  assert(strstr(response, "HTTP/1.1 200 OK\r\n") == response);
  assert(strstr(response, "Transfer-Encoding: chunked\r\n") != NULL);
  assert(strstr(response, "done") != NULL);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  assert(close(fd) == 0);
}

static void
check_tls_chunked_upload(unsigned short port, struct echo_server *server)
{
  static const char request[] =
      "POST /upload-chunked HTTP/1.1\r\nHost: localhost\r\n"
      "Transfer-Encoding: chunked\r\nTrailer: X-Trace\r\n\r\n";
  static const char final[] = "0\r\nX-Trace: done\r\n\r\n";
  struct sockaddr_in addr;
  struct pollfd watch;
  struct timeval timeout;
  SSL_CTX *ctx;
  SSL *ssl;
  unsigned char output[8192 + 32];
  char response[512];
  size_t sent;
  size_t used;
  size_t index;
  size_t frame_length;
  int prefix_length;
  int fd;
  int flags;
  int amount;
  int ssl_error;
  int events;
  int progress;
  int early_seen;
  int final_sent;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
      &timeout, sizeof(timeout)) == 0);
  ctx = SSL_CTX_new(TLS_client_method());
  assert(ctx != NULL);
  assert(SSL_CTX_load_verify_locations(ctx, tls_cert_path, NULL) == 1);
  ssl = SSL_new(ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
  assert(SSL_set1_host(ssl, "localhost") == 1);
  assert(SSL_connect(ssl) == 1);
  assert(SSL_get_verify_result(ssl) == X509_V_OK);
  assert(SSL_write(ssl, request, (int)(sizeof(request) - 1)) ==
      (int)(sizeof(request) - 1));
  flags = fcntl(fd, F_GETFL, 0);
  assert(flags >= 0);
  assert(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
  sent = 0;
  used = 0;
  early_seen = 0;
  final_sent = 0;
  memset(response, 0, sizeof(response));
  while (strstr(response, "0\r\n\r\n") == NULL) {
    progress = 0;
    events = 0;
    if (sent < UPLOAD_BODY_SIZE || !final_sent) {
      if (sent < UPLOAD_BODY_SIZE) {
        prefix_length = snprintf((char *)output, sizeof(output),
            "%zx\r\n", (size_t)8192);
        assert(prefix_length > 0);
        frame_length = (size_t)prefix_length + 8192 + 2;
        assert(frame_length <= sizeof(output));
        for (index = 0; index < 8192; index++)
          output[(size_t)prefix_length + index] =
              relay_byte(sent + index);
        memcpy(output + (size_t)prefix_length + 8192, "\r\n", 2);
      } else {
        memcpy(output, final, sizeof(final) - 1);
        frame_length = sizeof(final) - 1;
      }
      ERR_clear_error();
      amount = SSL_write(ssl, output, (int)frame_length);
      if (amount > 0) {
        assert((size_t)amount == frame_length);
        if (sent < UPLOAD_BODY_SIZE)
          sent += 8192;
        else
          final_sent = 1;
        progress = 1;
        usleep(1000u);
      } else {
        ssl_error = SSL_get_error(ssl, amount);
        assert(ssl_error == SSL_ERROR_WANT_READ ||
            ssl_error == SSL_ERROR_WANT_WRITE);
        events |= ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
      }
    }
    ERR_clear_error();
    amount = SSL_read(ssl, response + used,
        (int)(sizeof(response) - used - 1));
    if (amount > 0) {
      used += (size_t)amount;
      response[used] = '\0';
      progress = 1;
      if (!early_seen && strstr(response, "pong") != NULL) {
        assert(sent < UPLOAD_BODY_SIZE);
        early_seen = 1;
      }
    } else {
      ssl_error = SSL_get_error(ssl, amount);
      assert(ssl_error == SSL_ERROR_WANT_READ ||
          ssl_error == SSL_ERROR_WANT_WRITE);
      events |= ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
    }
    if (!progress) {
      assert(events != 0);
      watch.fd = fd;
      watch.events = (short)events;
      assert(poll(&watch, 1, 10000) > 0);
    }
  }
  assert(sent == UPLOAD_BODY_SIZE && final_sent);
  assert(early_seen);
  assert(__sync_fetch_and_add(&server->allow_body, 0) == 1);
  assert(strstr(response, "HTTP/1.1 200 OK\r\n") == response);
  assert(strstr(response, "Transfer-Encoding: chunked\r\n") != NULL);
  assert(strstr(response, "done") != NULL);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  assert(close(fd) == 0);
}

static void
check_ws(unsigned short port, struct echo_server *sse_server,
    int retry, int abort_retry)
{
  struct sockaddr_in addr;
  struct linger reset;
  struct timeval timeout;
  unsigned char initial[sizeof(ws_retry_abort_client_request) - 1 +
      sizeof(ws_client_early)];
  unsigned char bytes[sizeof(ws_response) - 1 + sizeof(ws_server_early)];
  const char *request;
  size_t request_length;
  unsigned idle_before;
  unsigned idle_after;
  int fd;
  int attempt;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  request = abort_retry ? ws_retry_abort_client_request :
      retry ? ws_retry_client_request : ws_client_request;
  request_length = strlen(request);
  memcpy(initial, request, request_length);
  memcpy(initial + request_length,
      ws_client_early, sizeof(ws_client_early));
  send_all(fd, initial, request_length + sizeof(ws_client_early));
  sse_read_exact(fd, bytes, sizeof(bytes));
  assert(memcmp(bytes, ws_response, sizeof(ws_response) - 1) == 0);
  assert(memcmp(bytes + sizeof(ws_response) - 1,
      ws_server_early, sizeof(ws_server_early)) == 0);
  if (retry) {
    sse_read_exact(fd, bytes, sizeof(ws_server_retry));
    assert(memcmp(bytes, ws_server_retry, sizeof(ws_server_retry)) == 0);
    if (!abort_retry) {
      usleep(50000u);
      idle_before = __sync_fetch_and_add(
          &metrics->ws_retry_tunnel_events, 0);
      usleep(1500000u);
      idle_after = __sync_fetch_and_add(
          &metrics->ws_retry_tunnel_events, 0);
      assert(idle_after >= idle_before && idle_after <= idle_before + 2);
      metrics->ws_retry_idle_events = idle_after - idle_before;
    }
  }
  if (sse_server != NULL)
    check_sse(port, sse_server, 0, 0);
  send_all(fd, ws_client_later, sizeof(ws_client_later));
  if (abort_retry) {
    for (attempt = 0; attempt < 5000 &&
        __sync_fetch_and_add(&metrics->ws_retry_timer_armed, 0) < 2;
        attempt++)
      usleep(1000u);
    assert(__sync_fetch_and_add(&metrics->ws_retry_timer_armed, 0) == 2);
    reset.l_onoff = 1;
    reset.l_linger = 0;
    assert(setsockopt(fd, SOL_SOCKET, SO_LINGER,
        &reset, sizeof(reset)) == 0);
    assert(close(fd) == 0);
    return;
  }
  sse_read_exact(fd, bytes, sizeof(ws_server_later));
  assert(memcmp(bytes, ws_server_later,
      sizeof(ws_server_later)) == 0);
  assert(recv(fd, bytes, sizeof(bytes), 0) == 0);
  assert(close(fd) == 0);
}

static void
check_ws_reject(unsigned short port)
{
  struct sockaddr_in addr;
  struct timeval timeout;
  unsigned char initial[sizeof(ws_reject_client_request) - 1 +
      sizeof(ws_client_early)];
  unsigned char bytes[4096];
  size_t total;
  size_t index;
  size_t position;
  ssize_t got;
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  memcpy(initial, ws_reject_client_request,
      sizeof(ws_reject_client_request) - 1);
  memcpy(initial + sizeof(ws_reject_client_request) - 1,
      ws_client_early, sizeof(ws_client_early));
  send_all(fd, initial, sizeof(initial));
  total = 0;
  for (;;) {
    got = recv(fd, bytes, sizeof(bytes), 0);
    assert(got >= 0);
    if (got == 0)
      break;
    assert(total + (size_t)got <=
        sizeof(ws_reject_response) - 1 + RELAY_PAYLOAD_SIZE);
    for (index = 0; index < (size_t)got; index++) {
      position = total + index;
      if (position < sizeof(ws_reject_response) - 1)
        assert(bytes[index] ==
            (unsigned char)ws_reject_response[position]);
      else
        assert(bytes[index] ==
            relay_byte(position - (sizeof(ws_reject_response) - 1)));
    }
    total += (size_t)got;
    usleep(1000u);
  }
  assert(total == sizeof(ws_reject_response) - 1 + RELAY_PAYLOAD_SIZE);
  assert(close(fd) == 0);
}

static void
check_relay(unsigned short port, const char *path)
{
  struct sockaddr_in addr;
  struct timeval timeout;
  struct relay_sender sender;
  pthread_t thread;
  unsigned char bytes[4096];
  unsigned char initial[128 + RELAY_INITIAL_BYTES];
  char request[128];
  size_t total;
  size_t index;
  ssize_t amount;
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
      &timeout, sizeof(timeout)) == 0);
  amount = snprintf(request, sizeof(request),
      "GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n", path);
  assert(amount > 0 && (size_t)amount < sizeof(request));
  memcpy(initial, request, (size_t)amount);
  for (index = 0; index < RELAY_INITIAL_BYTES; index++)
    initial[(size_t)amount + index] = relay_byte(index);
  send_all(fd, initial, (size_t)amount + RELAY_INITIAL_BYTES);
  sender.fd = fd;
  sender.start = RELAY_INITIAL_BYTES;
  sender.delay_us = 0;
  sender.progress = RELAY_INITIAL_BYTES;
  assert(pthread_create(&thread, NULL, relay_send_main, &sender) == 0);
  total = 0;
  for (;;) {
    amount = recv(fd, bytes, sizeof(bytes), 0);
    assert(amount >= 0);
    if (amount == 0)
      break;
    assert(total + (size_t)amount <=
        sizeof(relay_response) - 1 + RELAY_PAYLOAD_SIZE);
    for (index = 0; index < (size_t)amount; index++) {
      size_t position;

      position = total + index;
      if (position < sizeof(relay_response) - 1)
        assert(bytes[index] == (unsigned char)relay_response[position]);
      else
        assert(bytes[index] ==
            relay_byte(position - (sizeof(relay_response) - 1)));
    }
    total += (size_t)amount;
    usleep(1000u);
  }
  assert(total == sizeof(relay_response) - 1 + RELAY_PAYLOAD_SIZE);
  assert(pthread_join(thread, NULL) == 0);
  assert(close(fd) == 0);
}

static void
check_tls_relay(unsigned short port)
{
  static const char request[] =
      "GET /relay-tls HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct sockaddr_in addr;
  struct pollfd watch;
  struct timeval timeout;
  SSL_CTX *ctx;
  SSL *ssl;
  unsigned char input[4096];
  unsigned char output[4096];
  size_t sent;
  size_t received;
  size_t output_offset;
  size_t index;
  int fd;
  int flags;
  int amount;
  int ssl_error;
  int events;
  int progress;
  int eof;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
      &timeout, sizeof(timeout)) == 0);
  ctx = SSL_CTX_new(TLS_client_method());
  assert(ctx != NULL);
  assert(SSL_CTX_load_verify_locations(ctx, tls_cert_path, NULL) == 1);
  ssl = SSL_new(ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
  assert(SSL_set1_host(ssl, "localhost") == 1);
  assert(SSL_connect(ssl) == 1);
  assert(SSL_get_verify_result(ssl) == X509_V_OK);
  assert(SSL_write(ssl, request, (int)(sizeof(request) - 1)) ==
      (int)(sizeof(request) - 1));
  flags = fcntl(fd, F_GETFL, 0);
  assert(flags >= 0);
  assert(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);

  sent = 0;
  received = 0;
  output_offset = sizeof(output);
  eof = 0;
  while (!eof) {
    progress = 0;
    events = 0;
    if (sent < RELAY_PAYLOAD_SIZE) {
      if (output_offset == sizeof(output)) {
        for (index = 0; index < sizeof(output); index++)
          output[index] = relay_byte(sent + index);
        output_offset = 0;
      }
      ERR_clear_error();
      amount = SSL_write(ssl, output + output_offset,
          (int)(sizeof(output) - output_offset));
      if (amount > 0) {
        output_offset += (size_t)amount;
        sent += (size_t)amount;
        progress = 1;
      } else {
        ssl_error = SSL_get_error(ssl, amount);
        assert(ssl_error == SSL_ERROR_WANT_READ ||
            ssl_error == SSL_ERROR_WANT_WRITE);
        events |= ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
        if (ssl_error == SSL_ERROR_WANT_WRITE)
          goto client_wait;
      }
    }
    ERR_clear_error();
    amount = SSL_read(ssl, input, sizeof(input));
    if (amount > 0) {
      assert(received + (size_t)amount <=
          sizeof(relay_response) - 1 + RELAY_PAYLOAD_SIZE);
      for (index = 0; index < (size_t)amount; index++) {
        size_t position;

        position = received + index;
        if (position < sizeof(relay_response) - 1)
          assert(input[index] ==
              (unsigned char)relay_response[position]);
        else
          assert(input[index] ==
              relay_byte(position - (sizeof(relay_response) - 1)));
      }
      received += (size_t)amount;
      progress = 1;
      usleep(1000u);
    } else {
      ssl_error = SSL_get_error(ssl, amount);
      if (ssl_error == SSL_ERROR_ZERO_RETURN) {
        eof = 1;
      } else {
        assert(ssl_error == SSL_ERROR_WANT_READ ||
            ssl_error == SSL_ERROR_WANT_WRITE);
        events |= ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
      }
    }
client_wait:
    if (!progress && !eof) {
      assert(events != 0);
      watch.fd = fd;
      watch.events = (short)events;
      assert(poll(&watch, 1, 10000) > 0);
    }
  }
  assert(sent == RELAY_PAYLOAD_SIZE);
  assert(received == sizeof(relay_response) - 1 + RELAY_PAYLOAD_SIZE);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  assert(close(fd) == 0);
}

int
main(void)
{
  static const char request[] =
      "GET /curl HTTP/1.1\r\nHost: localhost\r\n\r\n";
  static const char expected[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\npong";
  static const char pending_request[] =
      "GET /pending HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct echo_server upstream;
  struct echo_server relay;
  struct echo_server relay_tls;
  struct echo_server ws;
  struct echo_server ws_retry;
  struct echo_server ws_retry_abort;
  struct echo_server ws_reject;
  struct echo_server sse;
  struct echo_server sse_abort;
  struct echo_server sse_queue_abort;
  struct echo_server sse_write_fail;
  struct echo_server sse_reset;
  struct echo_server sse_reset_before;
  struct echo_server sse_tls_reset;
  struct echo_server sse_tls_reset_before;
  struct echo_server upload;
  struct echo_server upload_chunked;
  struct echo_server upload_tls;
  struct echo_server upload_chunked_tls;
  struct echo_server stalled;
#if defined(VECTIS_PROXY_SHARED_MULTI)
  struct echo_server h2;
  struct echo_server h2_scale;
  struct echo_server h2_abort;
  struct echo_server h2_cancel;
  struct echo_server h2_recovery;
#endif
  struct sockaddr_in addr;
  struct timeval timeout;
  vectis_app_config config;
  vectis_cert_bundle_config certs;
  vectis_route_config route;
  vectis_error error;
  vectis_app *app;
  unsigned short port;
  char received[128];
  size_t used;
  ssize_t got;
  unsigned chunked_pauses_before;
  int fd;
  int attempt;
#if defined(VECTIS_PROXY_SHARED_MULTI)
  struct concurrent_sse_client concurrent_sse;
  pthread_t concurrent_client;
  struct h2_scale_client h2_scale_clients[H2_SCALE_CONNECTIONS];
  pthread_t h2_scale_threads[H2_SCALE_CONNECTIONS];
  struct h2_scale_client h2_cancel_clients[H2_SCALE_CONNECTIONS];
  pthread_t h2_cancel_threads[H2_SCALE_CONNECTIONS];
  size_t h2_cancel_generated_before;
  unsigned long h2_cancel_rss_before;
  unsigned long h2_cancel_rss_after;
  unsigned long h2_scale_rss_at_pause;
  unsigned long h2_scale_rss_after_wait;
  unsigned h2_scale_pumps_before;
  unsigned h2_scale_pumps_at_pause;
  unsigned h2_scale_pumps_after_wait;
  size_t h2_scale_generated_at_pause;
  size_t h2_scale_generated_after_wait;
  unsigned h2_scale_chunks_at_pause;
  unsigned h2_scale_chunks_after_wait;
  size_t h2_scale_generated_after_idle;
  unsigned h2_scale_chunks_after_idle;
  unsigned h2_scale_pumps_after_idle;
  unsigned long h2_scale_rss_after_idle;
  unsigned long h2_scale_rss_after_completion;
  int i;
#endif

  metrics = mmap(NULL, sizeof(*metrics), PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(metrics != MAP_FAILED);
  memset(metrics, 0, sizeof(*metrics));
  assert(curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
  init_tls_retry_bio_method();
  prepare_echo(&upstream);
  prepare_echo(&relay);
  prepare_echo(&relay_tls);
  prepare_echo(&ws);
  prepare_echo(&ws_retry);
  ws_retry.ws_retry_mode = 1;
  prepare_echo(&ws_retry_abort);
  ws_retry_abort.ws_retry_mode = 1;
  ws_retry_abort.ws_retry_abort_mode = 1;
  prepare_echo(&ws_reject);
  prepare_echo(&sse);
  prepare_echo(&sse_abort);
  prepare_echo(&sse_queue_abort);
  sse_queue_abort.queue_abort_mode = 1;
  prepare_echo(&sse_write_fail);
  prepare_echo(&sse_reset);
  prepare_echo(&sse_reset_before);
  prepare_echo(&sse_tls_reset);
  prepare_echo(&sse_tls_reset_before);
  sse_tls_reset_before.tls_reset_before_mode = 1;
  prepare_echo(&upload);
  prepare_echo(&upload_chunked);
  prepare_echo(&upload_tls);
  prepare_echo(&upload_chunked_tls);
  prepare_echo(&stalled);
#if defined(VECTIS_PROXY_SHARED_MULTI)
  prepare_echo(&h2);
  prepare_echo(&h2_scale);
  prepare_echo(&h2_abort);
  prepare_echo(&h2_cancel);
  prepare_echo(&h2_recovery);
  h2_port = h2.port;
  h2_scale_port = h2_scale.port;
  h2_abort_port = h2_abort.port;
  h2_cancel_port = h2_cancel.port;
  h2_recovery_port = h2_recovery.port;
#endif
  upstream_port = upstream.port;
  relay_port = relay.port;
  relay_tls_port = relay_tls.port;
  ws_port = ws.port;
  ws_retry_port = ws_retry.port;
  ws_retry_abort_port = ws_retry_abort.port;
  ws_reject_port = ws_reject.port;
  sse_port = sse.port;
  sse_abort_port = sse_abort.port;
  sse_queue_abort_port = sse_queue_abort.port;
  sse_write_fail_port = sse_write_fail.port;
  sse_reset_port = sse_reset.port;
  sse_reset_before_port = sse_reset_before.port;
  sse_tls_reset_port = sse_tls_reset.port;
  sse_tls_reset_before_port = sse_tls_reset_before.port;
  upload_port = upload.port;
  upload_chunked_port = upload_chunked.port;
  stalled_port = stalled.port;
  assert(snprintf(tls_cert_path, sizeof(tls_cert_path),
      "vectis-curl-loop-%ld-cert.pem", (long)getpid()) > 0);
  assert(snprintf(tls_key_path, sizeof(tls_key_path),
      "vectis-curl-loop-%ld-key.pem", (long)getpid()) > 0);
  vectis_cert_bundle_config_init(&certs);
  certs.subject.common_name = "localhost";
  certs.dns_names = "localhost";
  certs.output_cert_path = tls_cert_path;
  certs.output_key_path = tls_key_path;
  certs.key_bits = 2048u;
  certs.valid_days = 1L;
  assert(vectis_cert_generate_bundle(&certs, &error) == VECTIS_OK);
  relay_tls.tls_ctx = SSL_CTX_new(TLS_server_method());
  assert(relay_tls.tls_ctx != NULL);
  assert(SSL_CTX_use_certificate_file(relay_tls.tls_ctx,
      tls_cert_path, SSL_FILETYPE_PEM) == 1);
  assert(SSL_CTX_use_PrivateKey_file(relay_tls.tls_ctx,
      tls_key_path, SSL_FILETYPE_PEM) == 1);
  assert(SSL_CTX_check_private_key(relay_tls.tls_ctx) == 1);
  ws.tls_ctx = relay_tls.tls_ctx;
  ws_retry.tls_ctx = relay_tls.tls_ctx;
  ws_retry_abort.tls_ctx = relay_tls.tls_ctx;
  ws_reject.tls_ctx = relay_tls.tls_ctx;
  sse_tls_reset.tls_ctx = relay_tls.tls_ctx;
  sse_tls_reset_before.tls_ctx = relay_tls.tls_ctx;
  port = available_port();
  vectis_kore_set_prebody_probe(takeover);
  vectis_kore_set_worker_teardown_probe(worker_cancel);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = port;
  config.server.worker_count = 1u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/health", health, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  if (app->start(app, &error) != VECTIS_OK) {
    fprintf(stderr, "curl loop startup: %s\n", error.message);
    assert(0);
  }
  if (getenv("VECTIS_KORE_PROBE_EXIT_AFTER_START") != NULL)
    _exit(71);
  if (getenv("VECTIS_KORE_PROBE_HANG_AFTER_START") != NULL)
    for (;;)
      pause();
  assert(pthread_create(&upstream.thread, NULL, echo_main, &upstream) == 0);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  for (attempt = 0; attempt < 100; attempt++) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
      break;
    assert(close(fd) == 0);
    usleep(10000u);
  }
  assert(attempt < 100);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  assert(send(fd, request, sizeof(request) - 1, MSG_NOSIGNAL) ==
      (ssize_t)(sizeof(request) - 1));
  used = 0;
  while (used < sizeof(received)) {
    got = recv(fd, received + used, sizeof(received) - used, 0);
    assert(got >= 0);
    if (got == 0)
      break;
    used += (size_t)got;
  }
  assert(used == sizeof(expected) - 1);
  assert(memcmp(received, expected, used) == 0);
  assert(close(fd) == 0);
  assert(pthread_join(upstream.thread, NULL) == 0);
  assert(close(upstream.listener) == 0);

  assert(pthread_create(&relay.thread, NULL, relay_echo_main, &relay) == 0);
  check_relay(port, "/relay");
  assert(pthread_join(relay.thread, NULL) == 0);
  assert(close(relay.listener) == 0);

  assert(pthread_create(&relay_tls.thread, NULL,
      relay_tls_main, &relay_tls) == 0);
  check_relay(port, "/relay-tls");
  assert(pthread_join(relay_tls.thread, NULL) == 0);
  assert(close(relay_tls.listener) == 0);

  assert(pthread_create(&ws.thread, NULL, ws_tls_main, &ws) == 0);
#if defined(VECTIS_PROXY_SHARED_MULTI)
  assert(pthread_create(&sse.thread, NULL, sse_main, &sse) == 0);
  check_ws(port, &sse, 0, 0);
  assert(pthread_join(sse.thread, NULL) == 0);
#else
  check_ws(port, NULL, 0, 0);
#endif
  assert(pthread_join(ws.thread, NULL) == 0);
  assert(close(ws.listener) == 0);
  assert(pthread_create(&ws_retry.thread, NULL,
      ws_tls_main, &ws_retry) == 0);
  check_ws(port, NULL, 1, 0);
  assert(pthread_join(ws_retry.thread, NULL) == 0);
  assert(close(ws_retry.listener) == 0);
  assert(metrics->ws_retry_send_injected == 1);
  assert(metrics->ws_retry_recv_injected == 1);
  assert(metrics->ws_retry_read_wakeups == 1);
  assert(metrics->ws_retry_write_wakeups == 1);
  assert(metrics->ws_retry_cached_injected == 1);
  assert(metrics->ws_retry_timer_fired == 1);
  assert(metrics->ws_retry_timer_armed == 1);
  assert(metrics->ws_retry_idle_events <= 2);
  assert(metrics->ws_retry_tunnel_events < 100);
  assert(pthread_create(&ws_retry_abort.thread, NULL,
      ws_tls_main, &ws_retry_abort) == 0);
  check_ws(port, NULL, 1, 1);
  assert(pthread_join(ws_retry_abort.thread, NULL) == 0);
  assert(close(ws_retry_abort.listener) == 0);
  for (attempt = 0; attempt < 500 &&
      __sync_fetch_and_add(&metrics->ws_retry_timer_cancelled, 0) == 0;
      attempt++)
    usleep(1000u);
  assert(metrics->ws_retry_timer_cancelled == 1);
  assert(metrics->ws_retry_upstream_closed == 1);
  usleep(1100000u);
  assert(metrics->ws_retry_timer_fired == 1);
  assert(metrics->ws_retry_timer_armed == 2);
  assert(metrics->ws_retry_cached_injected == 2);
  assert(metrics->ws_retry_tunnel_events < 200);
  assert(pthread_create(&ws_reject.thread, NULL,
      ws_reject_main, &ws_reject) == 0);
  check_ws_reject(port);
  assert(pthread_join(ws_reject.thread, NULL) == 0);
  assert(close(ws_reject.listener) == 0);
#if defined(VECTIS_PROXY_SHARED_MULTI)
  h2.tls_ctx = relay_tls.tls_ctx;
  h2.h2_body_size = SSE_BODY_SIZE;
  SSL_CTX_set_alpn_select_cb(h2.tls_ctx, h2_select_alpn, NULL);
  assert(pthread_create(&h2.thread, NULL, h2_main, &h2) == 0);
  check_sse(port, &h2, 0, 1);
  assert(pthread_join(h2.thread, NULL) == 0);
  assert(close(h2.listener) == 0);
  assert(metrics->h2_negotiated == 1);
  assert(metrics->h2_requests == 1);
  assert(metrics->h2_completed == 1);
  assert(metrics->h2_generated == SSE_BODY_SIZE);

  h2_scale.tls_ctx = relay_tls.tls_ctx;
  h2_scale.h2_body_size = H2_SCALE_BODY_SIZE;
  h2_scale_pumps_before = metrics->http_pump_calls;
  assert(h2_scale_pumps_before < 5000);
  assert(pthread_create(&h2_scale.thread, NULL,
      h2_scale_main, &h2_scale) == 0);
  for (i = 0; i < H2_SCALE_CONNECTIONS; i++) {
    h2_scale_clients[i].port = port;
    assert(pthread_create(&h2_scale_threads[i], NULL,
        h2_scale_client_main, &h2_scale_clients[i]) == 0);
  }
  for (attempt = 0; attempt < 10000 &&
      (__sync_fetch_and_add(&metrics->h2_scale_headers, 0) <
          H2_SCALE_CONNECTIONS ||
      __sync_fetch_and_add(&metrics->h2_scale_paused, 0) <
          H2_SCALE_CONNECTIONS); attempt++)
    usleep(1000u);
  assert(metrics->h2_scale_admitted == H2_SCALE_CONNECTIONS);
  assert(metrics->h2_scale_headers == H2_SCALE_CONNECTIONS);
  assert(metrics->h2_scale_paused == H2_SCALE_CONNECTIONS);
  assert(metrics->h2_scale_worker_pid > 0);
  h2_scale_pumps_at_pause = metrics->http_pump_calls;
  h2_scale_generated_at_pause =
      __sync_fetch_and_add(&metrics->h2_generated, 0) - SSE_BODY_SIZE;
  h2_scale_chunks_at_pause = metrics->http_chunks;
  h2_scale_rss_at_pause = process_rss_kb(metrics->h2_scale_worker_pid);
  usleep(2000000u);
  h2_scale_rss_after_wait = process_rss_kb(metrics->h2_scale_worker_pid);
  h2_scale_pumps_after_wait = metrics->http_pump_calls;
  h2_scale_generated_after_wait =
      __sync_fetch_and_add(&metrics->h2_generated, 0) - SSE_BODY_SIZE;
  h2_scale_chunks_after_wait = metrics->http_chunks;
  usleep(2000000u);
  h2_scale_rss_after_idle = process_rss_kb(metrics->h2_scale_worker_pid);
  h2_scale_pumps_after_idle = metrics->http_pump_calls;
  h2_scale_generated_after_idle =
      __sync_fetch_and_add(&metrics->h2_generated, 0) - SSE_BODY_SIZE;
  h2_scale_chunks_after_idle = metrics->http_chunks;
  fprintf(stderr, "worker h2 scale: baseline=%luKB paused=%luKB "
      "later=%lu,%luKB callback_peak=%luKB generated=%zu,%zu,%zu "
      "chunks=%u,%u,%u pumps=%u,%u,%u\n",
      metrics->h2_scale_worker_baseline_kb, h2_scale_rss_at_pause,
      h2_scale_rss_after_wait, h2_scale_rss_after_idle,
      metrics->h2_scale_worker_peak_kb,
      h2_scale_generated_at_pause, h2_scale_generated_after_wait,
      h2_scale_generated_after_idle, h2_scale_chunks_at_pause,
      h2_scale_chunks_after_wait, h2_scale_chunks_after_idle,
      h2_scale_pumps_at_pause, h2_scale_pumps_after_wait,
      h2_scale_pumps_after_idle);
  assert(h2_scale_rss_after_wait <= h2_scale_rss_at_pause + 4096u);
  assert(h2_scale_rss_after_idle <= h2_scale_rss_at_pause + 4096u);
  assert(h2_scale_generated_after_idle == h2_scale_generated_after_wait);
  assert(h2_scale_chunks_after_idle == h2_scale_chunks_after_wait);
  assert(h2_scale_pumps_after_idle - h2_scale_pumps_after_wait <= 64u);
  __sync_lock_test_and_set(&h2_scale_release, 1);
  for (i = 0; i < H2_SCALE_CONNECTIONS; i++)
    assert(pthread_join(h2_scale_threads[i], NULL) == 0);
  assert(pthread_join(h2_scale.thread, NULL) == 0);
  assert(close(h2_scale.listener) == 0);
  h2_scale_rss_after_completion =
      process_rss_kb(metrics->h2_scale_worker_pid);
  fprintf(stderr, "worker h2 scale complete: rss=%luKB "
      "callback_peak=%luKB received=%zu\n",
      h2_scale_rss_after_completion,
      metrics->h2_scale_worker_peak_kb, metrics->h2_scale_received);
  assert(metrics->h2_scale_done == H2_SCALE_CONNECTIONS);
  assert(metrics->h2_scale_received ==
      H2_SCALE_CONNECTIONS * H2_SCALE_BODY_SIZE);
  assert(metrics->h2_negotiated == 1 + H2_SCALE_CONNECTIONS);
  assert(metrics->h2_requests == 1 + H2_SCALE_CONNECTIONS);
  assert(metrics->h2_completed == 1 + H2_SCALE_CONNECTIONS);
  assert(metrics->h2_generated == SSE_BODY_SIZE +
      H2_SCALE_CONNECTIONS * H2_SCALE_BODY_SIZE);
  assert(metrics->h2_scale_worker_peak_kb <=
      metrics->h2_scale_worker_baseline_kb + 32768u);
  assert(h2_scale_rss_after_completion <=
      metrics->h2_scale_worker_baseline_kb + 32768u);
  assert(metrics->http_pump_calls - h2_scale_pumps_before < 200000u);

  h2_abort.tls_ctx = relay_tls.tls_ctx;
  h2_abort.h2_body_size = H2_SCALE_BODY_SIZE;
  h2_abort.h2_abort_mode = 1;
  assert(pthread_create(&h2_abort.thread, NULL,
      h2_main, &h2_abort) == 0);
  check_h2_abort(port);
  for (attempt = 0; attempt < 5000 &&
      (__sync_fetch_and_add(&metrics->h2_abort_cancelled, 0) == 0 ||
      __sync_fetch_and_add(&metrics->h2_abort_rst_seen, 0) == 0); attempt++)
    usleep(1000u);
  assert(metrics->h2_abort_cancelled == 1);
  assert(metrics->h2_abort_rst_seen == 1);
  check_sse(port, &h2_abort, 0, 2);
  for (attempt = 0; attempt < 5000 &&
      __sync_fetch_and_add(&metrics->h2_abort_terminal, 0) == 0;
      attempt++)
    usleep(1000u);
  fprintf(stderr, "h2 abort: cancelled=%u rst=%u tcp_closed=%u terminal=%u "
      "wait=%d generated=%zu\n", metrics->h2_abort_cancelled,
      metrics->h2_abort_rst_seen, metrics->h2_abort_tcp_closed,
      metrics->h2_abort_terminal, attempt,
      metrics->h2_generated - SSE_BODY_SIZE -
      H2_SCALE_CONNECTIONS * H2_SCALE_BODY_SIZE);
  assert(metrics->h2_abort_terminal == 1);
  assert(metrics->h2_abort_cancelled == 1);
  assert(metrics->h2_abort_reuse_done == 1);
  assert(metrics->h2_abort_tcp_closed == 0);
  assert(pthread_join(h2_abort.thread, NULL) == 0);
  assert(close(h2_abort.listener) == 0);
  assert(metrics->h2_negotiated == 2 + H2_SCALE_CONNECTIONS);
  assert(metrics->h2_requests == 3 + H2_SCALE_CONNECTIONS);
  assert(metrics->h2_completed == 1 + H2_SCALE_CONNECTIONS);
  assert(metrics->h2_generated == SSE_BODY_SIZE +
      H2_SCALE_CONNECTIONS * H2_SCALE_BODY_SIZE + SSE_CHUNK_SIZE +
      SSE_BODY_SIZE);

  h2_cancel.tls_ctx = relay_tls.tls_ctx;
  h2_cancel.h2_body_size = H2_SCALE_BODY_SIZE;
  h2_cancel.h2_cancel_mode = 1;
  h2_cancel_generated_before = metrics->h2_generated;
  h2_cancel_rss_before = process_rss_kb(metrics->h2_scale_worker_pid);
  assert(pthread_create(&h2_cancel.thread, NULL,
      h2_scale_main, &h2_cancel) == 0);
  for (i = 0; i < H2_SCALE_CONNECTIONS; i++) {
    h2_cancel_clients[i].port = port;
    h2_cancel_clients[i].recovery_mode = 0;
    assert(pthread_create(&h2_cancel_threads[i], NULL,
        h2_cancel_client_main, &h2_cancel_clients[i]) == 0);
  }
  for (attempt = 0; attempt < 10000 &&
      (__sync_fetch_and_add(&metrics->h2_cancel_headers, 0) <
          H2_SCALE_CONNECTIONS ||
      __sync_fetch_and_add(&metrics->h2_cancel_paused, 0) <
          H2_SCALE_CONNECTIONS); attempt++)
    usleep(1000u);
  fprintf(stderr, "worker h2 cancel ready: headers=%u paused=%u "
      "requests=%u generated=%zu wait=%d\n",
      metrics->h2_cancel_headers, metrics->h2_cancel_paused,
      metrics->h2_requests,
      metrics->h2_generated - h2_cancel_generated_before, attempt);
  assert(metrics->h2_cancel_headers == H2_SCALE_CONNECTIONS);
  assert(metrics->h2_cancel_paused == H2_SCALE_CONNECTIONS);
  check_h2_saturation(port);
  assert(metrics->h2_cancel_rejected == 1);
  assert(metrics->h2_cancel_active_at_reject == H2_SCALE_CONNECTIONS);
  assert(metrics->h2_cancel_active == H2_SCALE_CONNECTIONS);
  assert(metrics->h2_cancel_admitted == H2_SCALE_CONNECTIONS);
  assert(metrics->h2_cancel_curl_adds == H2_SCALE_CONNECTIONS);
  assert(metrics->h2_requests == 3 + 2 * H2_SCALE_CONNECTIONS);
  __sync_lock_test_and_set(&h2_cancel_release, 1);
  for (i = 0; i < H2_SCALE_CONNECTIONS; i++)
    assert(pthread_join(h2_cancel_threads[i], NULL) == 0);
  fprintf(stderr, "worker h2 cancel clients joined: cancelled=%u rst=%u "
      "terminal=%u\n", metrics->h2_cancel_cancelled,
      metrics->h2_cancel_rst_seen, metrics->h2_cancel_terminal);
  assert(pthread_join(h2_cancel.thread, NULL) == 0);
  h2_cancel_rss_after = process_rss_kb(metrics->h2_scale_worker_pid);
  fprintf(stderr, "worker h2 cancel: cancelled=%u rst=%u terminal=%u "
      "rss=%lu,%luKB generated=%zu\n", metrics->h2_cancel_cancelled,
      metrics->h2_cancel_rst_seen, metrics->h2_cancel_terminal,
      h2_cancel_rss_before, h2_cancel_rss_after,
      metrics->h2_generated - h2_cancel_generated_before);
  assert(metrics->h2_cancel_cancelled == H2_SCALE_CONNECTIONS);
  assert(metrics->h2_cancel_rst_seen + metrics->h2_cancel_tcp_closed ==
      H2_SCALE_CONNECTIONS);
  assert(metrics->h2_cancel_terminal == H2_SCALE_CONNECTIONS);
  assert(metrics->h2_cancel_active == 0);
  assert(h2_cancel_rss_after <= h2_cancel_rss_before + 32768u);
  assert(metrics->h2_generated - h2_cancel_generated_before <
      H2_SCALE_CONNECTIONS * H2_SCALE_BODY_SIZE);
  assert(metrics->h2_negotiated == 2 + 2 * H2_SCALE_CONNECTIONS);
  assert(metrics->h2_requests == 3 + 2 * H2_SCALE_CONNECTIONS);

  assert(pthread_create(&h2_cancel.thread, NULL,
      h2_main, &h2_cancel) == 0);
  h2_cancel_client_main(&h2_cancel_clients[0]);
  assert(pthread_join(h2_cancel.thread, NULL) == 0);
  assert(close(h2_cancel.listener) == 0);
  assert(metrics->h2_cancel_admitted == H2_SCALE_CONNECTIONS + 1);
  assert(metrics->h2_cancel_curl_adds == H2_SCALE_CONNECTIONS + 1);
  assert(metrics->h2_cancel_active == 0);
  h2_recovery.tls_ctx = relay_tls.tls_ctx;
  h2_recovery.h2_body_size = H2_SCALE_BODY_SIZE;
  h2_recovery.h2_cancel_mode = 1;
  assert(pthread_create(&h2_recovery.thread, NULL,
      h2_main, &h2_recovery) == 0);
  h2_cancel_clients[0].recovery_mode = 1;
  h2_cancel_client_main(&h2_cancel_clients[0]);
  assert(pthread_join(h2_recovery.thread, NULL) == 0);
  assert(close(h2_recovery.listener) == 0);
  assert(metrics->h2_cancel_admitted == H2_SCALE_CONNECTIONS + 2);
  assert(metrics->h2_cancel_curl_adds == H2_SCALE_CONNECTIONS + 2);
  assert(metrics->h2_cancel_cancelled == H2_SCALE_CONNECTIONS + 2);
  assert(metrics->h2_cancel_rst_seen + metrics->h2_cancel_tcp_closed ==
      H2_SCALE_CONNECTIONS + 2);
  assert(metrics->h2_cancel_terminal == H2_SCALE_CONNECTIONS + 2);
  assert(metrics->h2_cancel_active == 0);
  assert(metrics->h2_negotiated == 4 + 2 * H2_SCALE_CONNECTIONS);
  assert(metrics->h2_requests == 5 + 2 * H2_SCALE_CONNECTIONS);
#endif
#if defined(VECTIS_PROXY_SHARED_MULTI)
  __sync_lock_test_and_set(&sse.allow_body, 0);
#endif
  assert(pthread_create(&sse.thread, NULL, sse_main, &sse) == 0);
  check_sse(port, &sse, 0, 0);
  assert(pthread_join(sse.thread, NULL) == 0);
  __sync_lock_test_and_set(&sse.allow_body, 0);
  assert(pthread_create(&sse.thread, NULL, sse_main, &sse) == 0);
  check_sse(port, &sse, 1, 0);
  assert(pthread_join(sse.thread, NULL) == 0);
#if !defined(VECTIS_PROXY_SHARED_MULTI)
  assert(close(sse.listener) == 0);
#endif

  assert(pthread_create(&sse_abort.thread, NULL,
      sse_abort_main, &sse_abort) == 0);
  check_sse_abort(port);
  assert(pthread_join(sse_abort.thread, NULL) == 0);
  assert(close(sse_abort.listener) == 0);
  assert(metrics->http_abort_cancelled == 1);
  assert(metrics->http_abort_upstream_closed == 1);
  assert(metrics->http_half_closed > 0);

  assert(pthread_create(&sse_queue_abort.thread, NULL,
      sse_abort_main, &sse_queue_abort) == 0);
  check_sse_queue_abort(port, &sse_queue_abort);
  assert(pthread_join(sse_queue_abort.thread, NULL) == 0);
  assert(metrics->http_queue_abort_cancelled == 1);
  assert(metrics->http_queue_abort_upstream_closed == 1);
  assert(metrics->http_queue_pending_at_disconnect == 1);

  assert(pthread_create(&sse_write_fail.thread, NULL,
      sse_abort_main, &sse_write_fail) == 0);
  check_sse_write_fail(port);
  assert(pthread_join(sse_write_fail.thread, NULL) == 0);
  assert(metrics->http_write_failures == 1);
  assert(metrics->http_write_fail_cancelled == 1);
  assert(metrics->http_write_fail_pending_at_disconnect == 1);
  assert(metrics->http_abort_upstream_closed == 2);

  assert(pthread_create(&sse_reset.thread, NULL,
      sse_reset_main, &sse_reset) == 0);
  check_sse_reset(port, &sse_reset, 0);
  assert(pthread_join(sse_reset.thread, NULL) == 0);
  assert(close(sse_reset.listener) == 0);
  assert(metrics->http_upstream_failed == 1);

  assert(pthread_create(&sse_reset_before.thread, NULL,
      sse_reset_before_main, &sse_reset_before) == 0);
  check_sse_reset_before(port, 0);
  assert(pthread_join(sse_reset_before.thread, NULL) == 0);
  assert(close(sse_reset_before.listener) == 0);
  assert(metrics->http_upstream_failed == 2);
  assert(metrics->http_local_errors == 1);

  assert(pthread_create(&sse_tls_reset.thread, NULL,
      sse_tls_reset_main, &sse_tls_reset) == 0);
  check_sse_reset(port, &sse_tls_reset, 1);
  assert(pthread_join(sse_tls_reset.thread, NULL) == 0);
  assert(close(sse_tls_reset.listener) == 0);
  assert(metrics->http_upstream_failed == 3);
  assert(metrics->http_tls_upstream_failed == 1);

  assert(pthread_create(&sse_tls_reset_before.thread, NULL,
      sse_tls_reset_main, &sse_tls_reset_before) == 0);
  check_sse_reset_before(port, 1);
  assert(pthread_join(sse_tls_reset_before.thread, NULL) == 0);
  assert(close(sse_tls_reset_before.listener) == 0);
  assert(metrics->http_upstream_failed == 4);
  assert(metrics->http_tls_upstream_failed == 2);
  assert(metrics->http_local_errors == 2);
  SSL_CTX_free(relay_tls.tls_ctx);

  assert(pthread_create(&upload.thread, NULL,
      upload_main, &upload) == 0);
#if defined(VECTIS_PROXY_SHARED_MULTI)
  metrics->shared_running_max = 0;
  __sync_lock_test_and_set(&sse.allow_body, 0);
  assert(pthread_create(&sse.thread, NULL, sse_main, &sse) == 0);
  concurrent_sse.port = port;
  concurrent_sse.server = &sse;
  assert(pthread_create(&concurrent_client, NULL,
      concurrent_sse_main, &concurrent_sse) == 0);
  for (attempt = 0; attempt < 500 &&
      __sync_fetch_and_add(&sse.allow_body, 0) == 0; attempt++)
    usleep(1000u);
  assert(__sync_fetch_and_add(&sse.allow_body, 0) == 1);
#endif
  check_upload(port, &upload);
  assert(pthread_join(upload.thread, NULL) == 0);
  assert(close(upload.listener) == 0);
  chunked_pauses_before = metrics->chunked_upload_pauses;
  assert(pthread_create(&upload_chunked.thread, NULL,
      upload_chunked_main, &upload_chunked) == 0);
  check_chunked_upload(port, &upload_chunked);
  assert(metrics->chunked_upload_pauses > chunked_pauses_before);
  assert(pthread_join(upload_chunked.thread, NULL) == 0);
  __sync_lock_test_and_set(&upload_chunked.allow_body, 0);
  assert(pthread_create(&upload_chunked.thread, NULL,
      upload_chunked_main, &upload_chunked) == 0);
  check_chunked_keepalive(port, &upload_chunked, 0);
  assert(pthread_join(upload_chunked.thread, NULL) == 0);
  upload_chunked.hold_final = 1;
  __sync_lock_test_and_set(&upload_chunked.allow_body, 0);
  assert(pthread_create(&upload_chunked.thread, NULL,
      upload_chunked_main, &upload_chunked) == 0);
  check_chunked_keepalive(port, &upload_chunked, 0);
  assert(pthread_join(upload_chunked.thread, NULL) == 0);
  assert(close(upload_chunked.listener) == 0);
  assert(metrics->chunked_pipeline_restored == 2);
  assert(metrics->chunked_pipeline_empty_restored == 1);
  assert(metrics->chunked_pipeline_max > 0);
  assert(metrics->chunked_pipeline_max <= http_header_max);
#if defined(VECTIS_PROXY_SHARED_MULTI)
  assert(pthread_join(concurrent_client, NULL) == 0);
  assert(pthread_join(sse.thread, NULL) == 0);
  assert(close(sse.listener) == 0);
  assert(metrics->shared_running_max >= 2);
#endif

  assert(pthread_create(&stalled.thread, NULL, stall_main, &stalled) == 0);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  assert(send(fd, pending_request, sizeof(pending_request) - 1,
      MSG_NOSIGNAL) == (ssize_t)(sizeof(pending_request) - 1));
  for (attempt = 0; attempt < 500 &&
      (metrics->pending_started == 0 || metrics->stall_accepted == 0);
      attempt++)
    usleep(10000u);
  assert(metrics->pending_started == 1);
  assert(metrics->stall_accepted == 1);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  assert(close(fd) == 0);
  app->close(app);

  prepare_echo(&relay_tls);
  relay_tls_port = relay_tls.port;
  relay_tls.tls_ctx = SSL_CTX_new(TLS_server_method());
  assert(relay_tls.tls_ctx != NULL);
  assert(SSL_CTX_use_certificate_file(relay_tls.tls_ctx,
      tls_cert_path, SSL_FILETYPE_PEM) == 1);
  assert(SSL_CTX_use_PrivateKey_file(relay_tls.tls_ctx,
      tls_key_path, SSL_FILETYPE_PEM) == 1);
  port = available_port();
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_MANUAL;
  config.tls.bind = "127.0.0.1";
  config.tls.port = port;
  config.tls.domain = "localhost";
  config.tls.certificate_path = tls_cert_path;
  config.tls.private_key_path = tls_key_path;
  config.tls.ca_bundle_path = tls_cert_path;
  config.server.worker_count = 1u;
  upload_port = upload_tls.port;
  upload_chunked_port = upload_chunked_tls.port;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/health", health, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  if (app->start(app, &error) != VECTIS_OK) {
    fprintf(stderr, "TLS curl loop startup: %s\n", error.message);
    assert(0);
  }
  assert(pthread_create(&relay_tls.thread, NULL,
      relay_tls_main, &relay_tls) == 0);
  check_tls_relay(port);
  assert(pthread_join(relay_tls.thread, NULL) == 0);
  assert(close(relay_tls.listener) == 0);
  assert(pthread_create(&upload_tls.thread, NULL,
      upload_main, &upload_tls) == 0);
  check_tls_upload(port, &upload_tls, 0);
  assert(pthread_join(upload_tls.thread, NULL) == 0);
  __sync_lock_test_and_set(&upload_tls.allow_body, 0);
  assert(pthread_create(&upload_tls.thread, NULL,
      upload_main, &upload_tls) == 0);
  check_tls_upload(port, &upload_tls, 1);
  assert(pthread_join(upload_tls.thread, NULL) == 0);
  assert(metrics->tls_read_want_write_injected == 1);
  assert(metrics->tls_read_want_write_observed == 1);
  assert(metrics->tls_read_write_wakeups > 0);
  assert(metrics->tls_write_want_read_injected == 1);
  assert(metrics->tls_write_want_read_observed > 0);
  assert(metrics->tls_write_read_wakeups > 0);
  assert(close(upload_tls.listener) == 0);
  chunked_pauses_before = metrics->chunked_upload_pauses;
  assert(pthread_create(&upload_chunked_tls.thread, NULL,
      upload_chunked_main, &upload_chunked_tls) == 0);
  check_tls_chunked_upload(port, &upload_chunked_tls);
  assert(metrics->chunked_upload_pauses > chunked_pauses_before);
  assert(pthread_join(upload_chunked_tls.thread, NULL) == 0);
  __sync_lock_test_and_set(&upload_chunked_tls.allow_body, 0);
  assert(pthread_create(&upload_chunked_tls.thread, NULL,
      upload_chunked_main, &upload_chunked_tls) == 0);
  check_chunked_keepalive(port, &upload_chunked_tls, 1);
  assert(pthread_join(upload_chunked_tls.thread, NULL) == 0);
  upload_chunked_tls.hold_final = 1;
  __sync_lock_test_and_set(&upload_chunked_tls.allow_body, 0);
  assert(pthread_create(&upload_chunked_tls.thread, NULL,
      upload_chunked_main, &upload_chunked_tls) == 0);
  check_chunked_keepalive(port, &upload_chunked_tls, 1);
  assert(pthread_join(upload_chunked_tls.thread, NULL) == 0);
  assert(close(upload_chunked_tls.listener) == 0);
  assert(metrics->chunked_pipeline_restored == 4);
  assert(metrics->chunked_pipeline_empty_restored == 2);
  __sync_lock_test_and_set(&sse_queue_abort.allow_body, 0);
  assert(pthread_create(&sse_queue_abort.thread, NULL,
      sse_abort_main, &sse_queue_abort) == 0);
  check_tls_sse_queue_abort(port, &sse_queue_abort);
  assert(pthread_join(sse_queue_abort.thread, NULL) == 0);
  assert(close(sse_queue_abort.listener) == 0);
  assert(metrics->http_queue_abort_cancelled == 2);
  assert(metrics->http_queue_abort_upstream_closed == 2);
  assert(metrics->http_queue_pending_at_disconnect == 2);
  assert(metrics->http_tls_queue_cancelled == 1);
  assert(pthread_create(&sse_write_fail.thread, NULL,
      sse_abort_main, &sse_write_fail) == 0);
  check_tls_sse_write_fail(port);
  assert(pthread_join(sse_write_fail.thread, NULL) == 0);
  assert(close(sse_write_fail.listener) == 0);
  assert(metrics->http_write_failures == 2);
  assert(metrics->tls_write_call_failures == 1);
  assert(metrics->http_write_fail_cancelled == 2);
  assert(metrics->http_write_fail_pending_at_disconnect == 2);
  assert(metrics->http_abort_upstream_closed == 3);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  SSL_CTX_free(relay_tls.tls_ctx);
  vectis_kore_set_prebody_probe(NULL);
  vectis_kore_set_worker_teardown_probe(NULL);
  assert(pthread_join(stalled.thread, NULL) == 0);
  assert(close(stalled.listener) == 0);
  curl_global_cleanup();
  assert(remove(tls_cert_path) == 0);
  assert(remove(tls_key_path) == 0);
  fprintf(stderr, "relay metrics: done=%u max=%zu initial=%zu "
      "read_pauses=%u write_pauses=%u pump_calls=%u disconnects=%u\n",
      metrics->relay_done, metrics->relay_max_queued,
      metrics->relay_max_initial_bytes, metrics->relay_read_pauses,
      metrics->relay_write_pauses, metrics->relay_pump_calls,
      metrics->disconnects);
  fprintf(stderr, "TLS loop: pump=%u tunnel_events=%u downstream_events=%u "
      "up_send_again=%u up_recv_again=%u down_write_read=%u "
      "down_write_write=%u down_read_read=%u down_read_write=%u\n",
      metrics->tls_pump_calls, metrics->tls_tunnel_events,
      metrics->tls_downstream_events, metrics->tls_up_send_again,
      metrics->tls_up_recv_again,
      metrics->downstream_tls_write_want_read,
      metrics->tls_down_write_want_write,
      metrics->tls_down_read_want_read,
      metrics->downstream_tls_read_want_write);
  assert(metrics->connect_done == 8);
  assert(metrics->tls_connect_done == 6);
  assert(metrics->curl_watch_removed > 0);
  fprintf(stderr, "retired watchers: removed=%u ignored=%u\n",
      metrics->curl_watch_removed,
      metrics->retired_watch_callbacks_ignored);
  assert(metrics->retired_watch_callbacks_ignored >=
      metrics->curl_watch_removed);
  assert(metrics->tunnel_done == 1);
  assert(metrics->downstream_done == 1);
#if defined(VECTIS_PROXY_SHARED_MULTI)
  assert(metrics->disconnects == 32 + 2 * H2_SCALE_CONNECTIONS);
#else
  assert(metrics->disconnects == 25);
#endif
  assert(metrics->relay_done == 6);
  assert(metrics->ws_upgraded == 3);
  assert(metrics->ws_rejected == 1);
  assert(metrics->ws_reject_header_fragments > 0);
#if defined(VECTIS_PROXY_SHARED_MULTI)
  assert(metrics->http_done == 15 + H2_SCALE_CONNECTIONS);
  assert(metrics->http_headers_ready == 25 + 2 * H2_SCALE_CONNECTIONS);
  assert(metrics->h2_down_received == SSE_BODY_SIZE);
  assert(metrics->h2_down_done == 1);
#else
  assert(metrics->http_done == 11);
  assert(metrics->http_headers_ready == 18);
#endif
  assert(metrics->upload_done == 3);
  assert(metrics->chunked_upload_done == 6);
  assert(metrics->chunked_trailer_called == 6);
  assert(metrics->chunked_upload_pauses > 0);
  assert(metrics->chunked_input_pauses > 0);
  assert(metrics->upload_pauses > 0);
  assert(metrics->upload_max_queued <= RELAY_BUFFER_SIZE);
  assert(metrics->upload_tls_pending_resumes > 0);
  assert(metrics->http_chunks > 0);
  assert(metrics->http_body_pauses > 0);
#if !defined(VECTIS_PROXY_SHARED_MULTI)
  assert(metrics->http_pump_calls < 5000);
#endif
  assert(metrics->http_max_kore_queued <= HTTP_BODY_BUFFER_SIZE + 32);
  assert(metrics->http_max_kore_buffers == 1);
  fprintf(stderr, "SSE loop: chunks=%u pauses=%u pumps=%u max_queue=%lu\n",
      metrics->http_chunks, metrics->http_body_pauses,
      metrics->http_pump_calls,
      (unsigned long)metrics->http_max_kore_queued);
  fprintf(stderr, "upload loop: done=%u pauses=%u max_queue=%zu "
      "tls_pending_resumes=%u chunked_done=%u trailer=%u "
      "chunk_pauses=%u chunk_input_pauses=%u\n",
      metrics->upload_done, metrics->upload_pauses,
      metrics->upload_max_queued, metrics->upload_tls_pending_resumes,
      metrics->chunked_upload_done, metrics->chunked_trailer_called,
      metrics->chunked_upload_pauses, metrics->chunked_input_pauses);
  assert(metrics->downstream_tls_close_notify == 1);
  assert(metrics->downstream_tls_write_pauses > 0);
  assert(metrics->relay_max_queued <= RELAY_BUFFER_SIZE);
  assert(metrics->relay_max_initial_bytes > RELAY_BUFFER_SIZE);
  assert(metrics->relay_max_kore_queued <= RELAY_BUFFER_SIZE);
  assert(metrics->relay_max_kore_buffers == 1);
  assert(metrics->relay_pump_calls < 5000);
  assert(metrics->tls_pump_calls < 2000);
  assert(metrics->relay_read_pauses > 0);
  assert(metrics->worker_cancelled == 1);
  assert(metrics->active_watchers_at_teardown > 0);
  assert(metrics->timers_at_teardown == 1);
  assert(metrics->max_curl_watchers > 0);
  fprintf(stderr, "Kore curl loop: max_watchers=%u removed=%u "
      "active_at_teardown=%u timers_at_teardown=%u\n",
      metrics->max_curl_watchers, metrics->curl_watch_removed,
      metrics->active_watchers_at_teardown,
      metrics->timers_at_teardown);
  assert(munmap(metrics, sizeof(*metrics)) == 0);
  BIO_meth_free(tls_retry_bio_method);
  tls_retry_bio_method = NULL;
  return 0;
}
