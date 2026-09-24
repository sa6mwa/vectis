#include <arpa/inet.h>
#include <assert.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <pthread.h>
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
#define HTTP_BODY_BUFFER_SIZE 16384

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
static const char ws_response[] =
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
    "Sec-WebSocket-Protocol: chat\r\n\r\n";
static const unsigned char ws_client_early[] = {
    0x81, 0x84, 0x11, 0x22, 0x33, 0x44, 'p' ^ 0x11, 'i' ^ 0x22,
    'n' ^ 0x33, 'g' ^ 0x44};
static const unsigned char ws_client_later[] = {
    0x81, 0x84, 0x55, 0x66, 0x77, 0x88, 'm' ^ 0x55, 'o' ^ 0x66,
    'r' ^ 0x77, 'e' ^ 0x88};
static const unsigned char ws_server_early[] = {0x81, 0x04, 'p', 'o', 'n', 'g'};
static const unsigned char ws_server_later[] = {0x81, 0x04, 'd', 'o', 'n', 'e'};

struct echo_server {
  int listener;
  unsigned short port;
  pthread_t thread;
  SSL_CTX *tls_ctx;
  volatile int allow_body;
};

struct loop_metrics {
  unsigned connect_done;
  unsigned curl_watch_removed;
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
  unsigned ws_upgraded;
  unsigned http_body_pauses;
  unsigned http_chunks;
  unsigned http_headers_ready;
  unsigned http_pump_calls;
  size_t http_max_kore_queued;
  unsigned http_max_kore_buffers;
};

struct proxy_state;

struct curl_watch {
  struct kore_event evt;
  struct proxy_state *state;
  curl_socket_t fd;
};

struct proxy_state {
  struct kore_event tunnel_event;
  struct connection *downstream;
  struct kore_timer *timer;
  struct kore_timer *deadline_timer;
  struct kore_timer *relay_continue_timer;
  CURLM *multi;
  CURL *easy;
  curl_socket_t upstream_fd;
  unsigned curl_watch_count;
  int tunnel_watched;
  int downstream_watched;
  int downstream_interest;
  int phase;
  int running;
  size_t response_sent;
  int relay_mode;
  int relay_tls;
  int ws_mode;
  int ws_upgraded;
  int http_mode;
  int http_status_seen;
  int http_headers_ready;
  int http_headers_sent;
  int http_transfer_done;
  int http_paused;
  int http_final_sent;
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

static struct loop_metrics *metrics;
static unsigned short upstream_port;
static unsigned short stalled_port;
static unsigned short relay_port;
static unsigned short relay_tls_port;
static unsigned short ws_port;
static unsigned short sse_port;
static char tls_cert_path[128];
static char tls_key_path[128];

extern void vectis_kore_set_prebody_probe(
    int (*probe)(struct http_request *, const void *, size_t));
extern void vectis_kore_set_worker_teardown_probe(void (*probe)(void));

static void drive_curl(struct proxy_state *state, curl_socket_t fd, int flags);
static void curl_event(void *arg, int error);
static void downstream_event(void *arg, int error);
static void relay_pump(struct proxy_state *state);
static void http_pump(struct proxy_state *state);

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
  assert(poll(&watcher, 1, 50) == 0);
  memcpy(response, ws_response, sizeof(ws_response) - 1);
  memcpy(response + sizeof(ws_response) - 1,
      ws_server_early, sizeof(ws_server_early));
  assert(SSL_write(ssl, response, sizeof(response)) ==
      (int)sizeof(response));
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
  assert(SSL_shutdown(ssl) >= 0);
  SSL_free(ssl);
  assert(close(fd) == 0);
  return NULL;
}

struct relay_sender {
  int fd;
  size_t start;
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
  server->listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(server->listener >= 0);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(server->listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  assert(listen(server->listener, 1) == 0);
  size = sizeof(addr);
  assert(getsockname(server->listener, (struct sockaddr *)&addr, &size) == 0);
  server->port = ntohs(addr.sin_port);
}

static int
socket_change(CURL *easy, curl_socket_t fd, int what,
    void *arg, void *socket_arg)
{
  struct proxy_state *state;
  struct curl_watch *watch;
  int interest;

  (void)easy;
  state = (struct proxy_state *)arg;
  watch = (struct curl_watch *)socket_arg;
  if (what == CURL_POLL_REMOVE) {
    if (watch != NULL) {
      kore_platform_disable_read((int)fd);
      assert(state->curl_watch_count > 0);
      state->curl_watch_count--;
      metrics->curl_watch_removed++;
      free(watch);
    }
    return 0;
  }
  if (watch == NULL) {
    watch = calloc(1, sizeof(*watch));
    assert(watch != NULL);
    watch->evt.type = KORE_TYPE_CONNECTION;
    watch->evt.handle = curl_event;
    watch->state = state;
    watch->fd = fd;
    assert(curl_multi_assign(state->multi, fd, watch) == CURLM_OK);
    state->curl_watch_count++;
    if (state->curl_watch_count > metrics->max_curl_watchers)
      metrics->max_curl_watchers = state->curl_watch_count;
  }
  interest = (what & CURL_POLL_IN ? EPOLLIN : 0) |
      (what & CURL_POLL_OUT ? EPOLLOUT : 0);
  kore_platform_event_schedule((int)fd, interest, 0, &watch->evt);
  return 0;
}

static void
timeout_event(void *arg, u_int64_t now)
{
  struct proxy_state *state;

  (void)now;
  state = (struct proxy_state *)arg;
  state->timer = NULL;
  drive_curl(state, CURL_SOCKET_TIMEOUT, 0);
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
  struct proxy_state *state;

  (void)multi;
  state = (struct proxy_state *)arg;
  if (state->timer != NULL) {
    kore_timer_remove(state->timer);
    state->timer = NULL;
  }
  if (timeout_ms >= 0)
    state->timer = kore_timer_add(timeout_event,
        (u_int64_t)(timeout_ms > 0 ? timeout_ms : 1), state,
        KORE_TIMER_ONESHOT);
  return 0;
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
      code = curl_easy_send(state->easy,
          state->to_upstream + state->to_upstream_offset,
          send_limit - state->to_upstream_offset, &amount);
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
        (!state->ws_mode || state->ws_upgraded)) {
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
      code = curl_easy_recv(state->easy,
          state->to_downstream + state->to_downstream_length,
          sizeof(state->to_downstream) - state->to_downstream_length,
          &amount);
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
        if (state->ws_mode && !state->ws_upgraded) {
          size_t prefix;

          prefix = state->to_downstream_length;
          if (prefix > sizeof(ws_response) - 1)
            prefix = sizeof(ws_response) - 1;
          assert(memcmp(state->to_downstream, ws_response, prefix) == 0);
          if (state->to_downstream_length >= sizeof(ws_response) - 1) {
            state->ws_upgraded = 1;
            metrics->ws_upgraded++;
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
      (!state->ws_mode || state->ws_upgraded)) {
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
    kore_platform_event_schedule((int)state->upstream_fd, up_events, 0,
        &state->tunnel_event);
    state->tunnel_watched = 1;
  } else if (state->tunnel_watched) {
    kore_platform_disable_read((int)state->upstream_fd);
    state->tunnel_watched = 0;
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
  kore_platform_event_schedule(state->downstream->fd, EPOLLOUT, 0,
      state->downstream);
  state->downstream_watched = 1;
}

static size_t
http_header(char *data, size_t size, size_t count, void *arg)
{
  struct proxy_state *state;
  size_t amount;

  state = (struct proxy_state *)arg;
  amount = size * count;
  if (!state->http_status_seen) {
    assert(amount >= 13);
    assert(memcmp(data, "HTTP/1.1 200 ", 13) == 0);
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
  size_t amount;

  state = (struct proxy_state *)arg;
  amount = size * count;
  assert(state->http_headers_ready);
  assert(amount <= sizeof(state->http_body));
  if (state->http_body_length != 0) {
    state->http_paused = 1;
    metrics->http_body_pauses++;
    return CURL_WRITEFUNC_PAUSE;
  }
  memcpy(state->http_body, data, amount);
  state->http_body_length = amount;
  http_schedule(state);
  return amount;
}

static void
http_pump(struct proxy_state *state)
{
  static const char response_header[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
      "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  struct connection *downstream;
  struct netbuf *buffer;
  size_t queued;
  size_t frame_length;
  unsigned count;
  int prefix_length;

  downstream = state->downstream;
  metrics->http_pump_calls++;
  if (!TAILQ_EMPTY(&downstream->send_queue)) {
    downstream->evt.flags |= KORE_EVENT_WRITE;
    assert(net_send_flush(downstream) == KORE_RESULT_OK);
    if (!TAILQ_EMPTY(&downstream->send_queue)) {
      http_schedule(state);
      return;
    }
  }
  if (state->http_headers_ready && !state->http_headers_sent) {
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
    metrics->http_chunks++;
  } else if (state->http_transfer_done && !state->http_final_sent) {
    net_send_queue(downstream, "0\r\n\r\n", 5);
    state->http_final_sent = 1;
  } else if (state->http_final_sent) {
    metrics->http_done++;
    kore_connection_disconnect(downstream);
    return;
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
    return;
  }
  if (state->http_paused) {
    state->http_paused = 0;
    assert(curl_easy_pause(state->easy, CURLPAUSE_CONT) == CURLE_OK);
    if (state->http_body_length != 0 || state->http_transfer_done) {
      http_schedule(state);
      return;
    }
  }
  kore_platform_disable_read(downstream->fd);
  state->downstream_watched = 0;
}

static void
drive_curl(struct proxy_state *state, curl_socket_t fd, int flags)
{
  CURLMsg *msg;
  long verify_result;
  int pending;

  assert(curl_multi_socket_action(state->multi, fd,
      flags, &state->running) == CURLM_OK);
  if (state->running != 0 || state->phase != 0)
    return;
  msg = curl_multi_info_read(state->multi, &pending);
  assert(msg != NULL && msg->msg == CURLMSG_DONE);
  assert(msg->data.result == CURLE_OK);
  if (state->http_mode) {
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
  assert(state->curl_watch_count == 0);
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
curl_event(void *arg, int error)
{
  struct curl_watch *watch;
  struct proxy_state *state;
  int flags;

  watch = (struct curl_watch *)arg;
  state = watch->state;
  flags = (watch->evt.flags & KORE_EVENT_READ ? CURL_CSELECT_IN : 0) |
      (watch->evt.flags & KORE_EVENT_WRITE ? CURL_CSELECT_OUT : 0) |
      (error ? CURL_CSELECT_ERR : 0);
  watch->evt.flags = 0;
  drive_curl(state, watch->fd, flags);
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
  if (state->tunnel_watched) {
    kore_platform_disable_read((int)state->upstream_fd);
    state->tunnel_watched = 0;
  }
  if (state->easy != NULL) {
    assert(curl_multi_remove_handle(state->multi,
        state->easy) == CURLM_OK);
    curl_easy_cleanup(state->easy);
    state->easy = NULL;
  }
  assert(state->curl_watch_count == 0);
  if (state->multi != NULL) {
    assert(curl_multi_cleanup(state->multi) == CURLM_OK);
    state->multi = NULL;
  }
}

static void
downstream_disconnect(struct connection *connection)
{
  struct proxy_state *state;

  state = (struct proxy_state *)connection->hdlr_extra;
  metrics->disconnects++;
  if (state != NULL)
    proxy_cancel(state);
}

static void
worker_cancel(void)
{
  struct connection *connection;
  struct proxy_state *state;

  TAILQ_FOREACH(connection, &connections, list) {
    if (connection->evt.handle != downstream_event)
      continue;
    state = (struct proxy_state *)connection->hdlr_extra;
    if (state != NULL && state->multi != NULL) {
      metrics->active_watchers_at_teardown += state->curl_watch_count;
      if (state->timer != NULL || state->deadline_timer != NULL ||
          state->relay_continue_timer != NULL)
        metrics->timers_at_teardown++;
      proxy_cancel(state);
      metrics->worker_cancelled++;
    }
  }
}

static void
downstream_event(void *arg, int error)
{
  static const char response[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\npong";
  struct connection *connection;
  struct proxy_state *state;
  ssize_t sent;

  connection = (struct connection *)arg;
  state = (struct proxy_state *)connection->hdlr_extra;
  connection->evt.flags = 0;
  if (state->http_mode) {
    if (error)
      kore_connection_disconnect(connection);
    else
      http_pump(state);
    return;
  }
  if (state->relay_mode) {
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
      strcmp(req->path, "/sse") != 0 &&
      strcmp(req->path, "/pending") != 0)
    return KORE_RESULT_OK;
  assert(len == 0 || strcmp(req->path, "/relay") == 0 ||
      strcmp(req->path, "/relay-tls") == 0 ||
      strcmp(req->path, "/ws") == 0);
  state = kore_calloc(1, sizeof(*state));
  state->relay_mode = strcmp(req->path, "/relay") == 0 ||
      strcmp(req->path, "/relay-tls") == 0 ||
      strcmp(req->path, "/ws") == 0;
  state->ws_mode = strcmp(req->path, "/ws") == 0;
  state->relay_tls = strcmp(req->path, "/relay-tls") == 0 ||
      state->ws_mode;
  state->http_mode = strcmp(req->path, "/sse") == 0;
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
  state->downstream_watched = 1;
  state->downstream->http_timeout = 0;
  state->upstream_fd = CURL_SOCKET_BAD;
  state->tunnel_event.handle = tunnel_event;
  state->tunnel_event.type = KORE_TYPE_CONNECTION;
  state->downstream->hdlr_extra = state;
  state->downstream->disconnect = downstream_disconnect;
  state->downstream->evt.handle = downstream_event;
  state->downstream->flags |= CONN_IS_BUSY;
  http_request_sleep(req);
  state->multi = curl_multi_init();
  state->easy = curl_easy_init();
  assert(state->multi != NULL && state->easy != NULL);
  assert(curl_multi_setopt(state->multi, CURLMOPT_SOCKETFUNCTION,
      socket_change) == CURLM_OK);
  assert(curl_multi_setopt(state->multi, CURLMOPT_SOCKETDATA,
      state) == CURLM_OK);
  assert(curl_multi_setopt(state->multi, CURLMOPT_TIMERFUNCTION,
      timer_change) == CURLM_OK);
  assert(curl_multi_setopt(state->multi, CURLMOPT_TIMERDATA,
      state) == CURLM_OK);
  target_port = upstream_port;
  if (strcmp(req->path, "/pending") == 0)
    target_port = stalled_port;
  else if (state->ws_mode)
    target_port = ws_port;
  else if (strcmp(req->path, "/relay-tls") == 0)
    target_port = relay_tls_port;
  else if (state->http_mode)
    target_port = sse_port;
  else if (state->relay_mode)
    target_port = relay_port;
  assert(snprintf(url, sizeof(url), "%s://%s:%u/",
      strcmp(req->path, "/pending") == 0 || state->relay_tls
          ? "https" : "http",
      state->relay_tls ? "localhost" : "127.0.0.1",
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
        CURL_HTTP_VERSION_1_1) == CURLE_OK);
    assert(curl_easy_setopt(state->easy, CURLOPT_NOPROXY,
        "*") == CURLE_OK);
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
  assert(curl_multi_add_handle(state->multi, state->easy) == CURLM_OK);
  state->running = 1;
  if (state->http_mode) {
    kore_platform_disable_read(state->downstream->fd);
    state->downstream_watched = 0;
  }
  drive_curl(state, CURL_SOCKET_TIMEOUT, 0);
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
check_sse(unsigned short port, struct echo_server *server)
{
  static const char request[] =
      "GET /sse HTTP/1.1\r\nHost: localhost\r\n\r\n";
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
    sse_read_exact(fd, ending, sizeof(ending));
    assert(memcmp(ending, "\r\n", 2) == 0);
    usleep(1000u);
  }
  assert(received == SSE_BODY_SIZE);
  sse_read_line(fd, line, sizeof(line));
  assert(strcmp(line, "\r\n") == 0);
  assert(recv(fd, ending, sizeof(ending), 0) == 0);
  assert(close(fd) == 0);
}

static void
check_ws(unsigned short port)
{
  struct sockaddr_in addr;
  struct timeval timeout;
  unsigned char initial[sizeof(ws_client_request) - 1 +
      sizeof(ws_client_early)];
  unsigned char bytes[sizeof(ws_response) - 1 + sizeof(ws_server_early)];
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
  memcpy(initial, ws_client_request, sizeof(ws_client_request) - 1);
  memcpy(initial + sizeof(ws_client_request) - 1,
      ws_client_early, sizeof(ws_client_early));
  send_all(fd, initial, sizeof(initial));
  sse_read_exact(fd, bytes, sizeof(bytes));
  assert(memcmp(bytes, ws_response, sizeof(ws_response) - 1) == 0);
  assert(memcmp(bytes + sizeof(ws_response) - 1,
      ws_server_early, sizeof(ws_server_early)) == 0);
  send_all(fd, ws_client_later, sizeof(ws_client_later));
  sse_read_exact(fd, bytes, sizeof(ws_server_later));
  assert(memcmp(bytes, ws_server_later,
      sizeof(ws_server_later)) == 0);
  assert(recv(fd, bytes, sizeof(bytes), 0) == 0);
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
  struct echo_server sse;
  struct echo_server stalled;
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
  int fd;
  int attempt;

  metrics = mmap(NULL, sizeof(*metrics), PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(metrics != MAP_FAILED);
  memset(metrics, 0, sizeof(*metrics));
  assert(curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
  prepare_echo(&upstream);
  prepare_echo(&relay);
  prepare_echo(&relay_tls);
  prepare_echo(&ws);
  prepare_echo(&sse);
  prepare_echo(&stalled);
  upstream_port = upstream.port;
  relay_port = relay.port;
  relay_tls_port = relay_tls.port;
  ws_port = ws.port;
  sse_port = sse.port;
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
  check_ws(port);
  assert(pthread_join(ws.thread, NULL) == 0);
  assert(close(ws.listener) == 0);
  SSL_CTX_free(relay_tls.tls_ctx);

  assert(pthread_create(&sse.thread, NULL, sse_main, &sse) == 0);
  check_sse(port, &sse);
  assert(pthread_join(sse.thread, NULL) == 0);
  assert(close(sse.listener) == 0);

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
  assert(metrics->connect_done == 5);
  assert(metrics->tls_connect_done == 3);
  assert(metrics->curl_watch_removed > 0);
  assert(metrics->tunnel_done == 1);
  assert(metrics->downstream_done == 1);
  assert(metrics->disconnects == 7);
  assert(metrics->relay_done == 4);
  assert(metrics->ws_upgraded == 1);
  assert(metrics->http_done == 1);
  assert(metrics->http_headers_ready == 1);
  assert(metrics->http_chunks > 0);
  assert(metrics->http_body_pauses > 0);
  assert(metrics->http_pump_calls < 5000);
  assert(metrics->http_max_kore_queued <= HTTP_BODY_BUFFER_SIZE + 32);
  assert(metrics->http_max_kore_buffers == 1);
  fprintf(stderr, "SSE loop: chunks=%u pauses=%u pumps=%u max_queue=%lu\n",
      metrics->http_chunks, metrics->http_body_pauses,
      metrics->http_pump_calls,
      (unsigned long)metrics->http_max_kore_queued);
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
  return 0;
}
