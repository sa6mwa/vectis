#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <kore/http.h>
#include <kore/kore.h>
#include <lc/lc.h>
#include <vectis/vectis.h>

#define DIRECT_BUFFER_SIZE 8192
#define DIRECT_PAYLOAD_SIZE (1024 * 1024)
#define TLS_PAYLOAD_SIZE (256 * 1024)
#define PRIOR_PAYLOAD_SIZE (128 * 1024)

static unsigned char prior_payload[PRIOR_PAYLOAD_SIZE];
static const char queued_http_response[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\npong";

struct direct_metrics {
  unsigned events;
  unsigned continuations;
  unsigned disconnects;
  unsigned write_pauses;
  unsigned read_eofs;
  unsigned tls_read_want_write;
  unsigned tls_write_want_read;
  unsigned tls_read_want_read;
  unsigned tls_write_want_write;
  unsigned prior_queue_seen;
  unsigned prior_queue_drained;
  unsigned worker_teardown_active;
  unsigned worker_teardown_calls;
  unsigned stream_takeover_seen;
  unsigned queued_stream_takeover_seen;
  unsigned stream_probe_active;
  unsigned http_restored;
  unsigned http_queue_chunks;
  size_t max_queued;
};

struct direct_state {
  struct http_request *req;
  struct kore_timer *continuation;
  unsigned char output[DIRECT_BUFFER_SIZE];
  size_t offset;
  size_t length;
  int read_eof;
  int read_wait;
  int write_wait;
  int http_mode;
  int http_queued;
  size_t pipeline_len;
  unsigned char pipeline[256];
};

struct sender_args {
  int fd;
  unsigned char *bytes;
  size_t size;
};

static struct direct_metrics *metrics;

extern void vectis_kore_set_prebody_probe(
    int (*probe)(struct http_request *, const void *, size_t));
extern void vectis_kore_set_worker_teardown_probe(void (*probe)(void));

static void direct_pump(struct connection *c);

static void
direct_finish_http(struct connection *c, struct direct_state *state)
{
  state->req->flags |= HTTP_REQUEST_DELETE;
  http_request_wakeup(state->req);
  if (state->pipeline_len > 0) {
    assert(c->http_pipeline == NULL);
    c->http_pipeline = kore_malloc(state->pipeline_len);
    memcpy(c->http_pipeline, state->pipeline, state->pipeline_len);
    c->http_pipeline_len = state->pipeline_len;
  }
  c->hdlr_extra = NULL;
  c->disconnect = NULL;
  c->evt.handle = kore_connection_event;
  c->handle = kore_connection_handle;
  c->flags &= ~CONN_IS_BUSY;
  kore_free(state);
  metrics->http_restored++;
  kore_platform_event_all(c->fd, c);
  http_start_recv(c);
}

static void
direct_interest(struct connection *c, struct direct_state *state)
{
  int events;
  int result;
  int ssl_error;

  events = 0;
  if (state->http_mode && state->http_queued &&
      TAILQ_EMPTY(&c->send_queue) &&
      state->length == state->offset) {
    direct_finish_http(c, state);
    return;
  }
  if (!TAILQ_EMPTY(&c->send_queue)) {
    events = c->tls != NULL && SSL_want(c->tls) == SSL_READING
        ? EPOLLIN : EPOLLOUT;
  } else if (state->http_mode && !state->http_queued) {
    events = EPOLLOUT;
  } else if (state->length > state->offset) {
    if (state->write_wait == KORE_EVENT_READ)
      events |= EPOLLIN;
    else
      events |= EPOLLOUT;
  } else if (!state->read_eof) {
    if (state->read_wait == KORE_EVENT_WRITE)
      events |= EPOLLOUT;
    else
      events |= EPOLLIN | EPOLLRDHUP;
  } else {
    if (c->tls != NULL) {
      ERR_clear_error();
      result = SSL_shutdown(c->tls);
      if (result < 0) {
        ssl_error = SSL_get_error(c->tls, result);
        if (ssl_error == SSL_ERROR_WANT_READ ||
            ssl_error == SSL_ERROR_WANT_WRITE) {
          events = ssl_error == SSL_ERROR_WANT_READ ? EPOLLIN : EPOLLOUT;
          kore_platform_event_schedule(c->fd, events, 0, c);
          return;
        }
      }
    }
    kore_connection_disconnect(c);
    return;
  }
  kore_platform_event_schedule(c->fd, events, 0, c);
}

static void
direct_continue(void *arg, u_int64_t now)
{
  struct connection *c;
  struct direct_state *state;

  (void)now;
  c = (struct connection *)arg;
  state = (struct direct_state *)c->hdlr_extra;
  assert(state != NULL);
  metrics->continuations++;
  state->continuation = NULL;
  direct_pump(c);
}

static void
direct_disconnect(struct connection *c)
{
  struct direct_state *state;

  state = (struct direct_state *)c->hdlr_extra;
  metrics->disconnects++;
  if (state != NULL && state->continuation != NULL) {
    kore_timer_remove(state->continuation);
    state->continuation = NULL;
  }
}

static void
direct_pump(struct connection *c)
{
  struct direct_state *state;
  ssize_t amount;
  int ssl_error;
  int step;

  state = (struct direct_state *)c->hdlr_extra;
  assert(state != NULL);
  if (!TAILQ_EMPTY(&c->send_queue)) {
    if ((c->evt.flags & KORE_EVENT_WRITE) ||
        ((c->evt.flags & KORE_EVENT_READ) && c->tls != NULL &&
        SSL_want(c->tls) == SSL_READING)) {
      c->evt.flags |= KORE_EVENT_WRITE;
      if (!net_send_flush(c)) {
        kore_connection_disconnect(c);
        return;
      }
    }
    if (!TAILQ_EMPTY(&c->send_queue)) {
      direct_interest(c, state);
      return;
    }
    if (!state->http_queued)
      metrics->prior_queue_drained++;
  }
  if (state->http_mode) {
    if (!state->http_queued) {
      net_send_queue(c, queued_http_response,
          sizeof(queued_http_response) - 1);
      state->http_queued = 1;
      metrics->http_queue_chunks++;
    }
    direct_interest(c, state);
    return;
  }
  for (step = 0; step < 64; step++) {
    if (state->length > state->offset) {
      if (c->tls != NULL) {
        ERR_clear_error();
        amount = SSL_write(c->tls, state->output + state->offset,
            (int)(state->length - state->offset));
      } else {
        amount = send(c->fd, state->output + state->offset,
            state->length - state->offset, MSG_NOSIGNAL);
      }
      if (amount > 0) {
        state->offset += (size_t)amount;
        state->write_wait = 0;
        if (state->offset == state->length) {
          state->offset = 0;
          state->length = 0;
        }
        continue;
      }
      if (c->tls != NULL) {
        ssl_error = SSL_get_error(c->tls, (int)amount);
        if (ssl_error == SSL_ERROR_WANT_READ ||
            ssl_error == SSL_ERROR_WANT_WRITE) {
          state->write_wait = ssl_error == SSL_ERROR_WANT_READ
              ? KORE_EVENT_READ : KORE_EVENT_WRITE;
          if (ssl_error == SSL_ERROR_WANT_READ)
            metrics->tls_write_want_read++;
          else
            metrics->tls_write_want_write++;
          metrics->write_pauses++;
          break;
        }
        kore_connection_disconnect(c);
        return;
      }
      if (amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        state->write_wait = KORE_EVENT_WRITE;
        metrics->write_pauses++;
        break;
      }
      if (amount < 0 && errno == EINTR)
        continue;
      kore_connection_disconnect(c);
      return;
    }
    if (state->read_eof)
      break;
    if (c->tls != NULL) {
      ERR_clear_error();
      amount = SSL_read(c->tls, state->output,
          (int)sizeof(state->output));
    } else {
      amount = recv(c->fd, state->output, sizeof(state->output), 0);
    }
    if (amount > 0) {
      state->length = (size_t)amount;
      state->read_wait = 0;
      if (state->length > metrics->max_queued)
        metrics->max_queued = state->length;
      continue;
    }
    if (c->tls != NULL) {
      ssl_error = SSL_get_error(c->tls, (int)amount);
      if (ssl_error == SSL_ERROR_ZERO_RETURN) {
        state->read_eof = 1;
        metrics->read_eofs++;
        break;
      }
      if (ssl_error == SSL_ERROR_WANT_READ ||
          ssl_error == SSL_ERROR_WANT_WRITE) {
        state->read_wait = ssl_error == SSL_ERROR_WANT_WRITE
            ? KORE_EVENT_WRITE : KORE_EVENT_READ;
        if (ssl_error == SSL_ERROR_WANT_WRITE)
          metrics->tls_read_want_write++;
        else
          metrics->tls_read_want_read++;
        break;
      }
      kore_connection_disconnect(c);
      return;
    }
    if (amount == 0) {
      state->read_eof = 1;
      metrics->read_eofs++;
      break;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      state->read_wait = KORE_EVENT_READ;
      break;
    }
    if (errno == EINTR)
      continue;
    kore_connection_disconnect(c);
    return;
  }
  if (step == 64 && !state->read_eof && state->continuation == NULL)
    state->continuation = kore_timer_add(direct_continue, 0, c,
        KORE_TIMER_ONESHOT);
  direct_interest(c, state);
}

static void
direct_event(void *arg, int error)
{
  struct connection *c;

  c = (struct connection *)arg;
  metrics->events++;
  (void)error;
  direct_pump(c);
  if (c->evt.handle == direct_event)
    c->evt.flags = 0;
}

static void
direct_worker_teardown(void)
{
  struct connection *c;

  metrics->worker_teardown_calls++;
  TAILQ_FOREACH(c, &connections, list) {
    if (c->evt.handle == direct_event)
      metrics->worker_teardown_active++;
  }
}

static int
direct_prebody(struct http_request *req, const void *data, size_t len)
{
  static const char header[] =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";
  struct direct_state *state;
  struct connection *c;
  int sndbuf;

  if (strcmp(req->path, "/direct") != 0 &&
      strcmp(req->path, "/direct-http") != 0)
    return KORE_RESULT_OK;
  assert(len <= DIRECT_BUFFER_SIZE - (sizeof(header) - 1));
  c = req->owner;
  c->http_timeout = 0;
  if (!TAILQ_EMPTY(&c->send_queue))
    metrics->prior_queue_seen++;
  if (metrics->stream_probe_active) {
    if (strcmp(req->path, "/direct-http") == 0)
      metrics->queued_stream_takeover_seen++;
    else
      metrics->stream_takeover_seen++;
  }
  sndbuf = 4096;
  assert(setsockopt(c->fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0);
  state = kore_calloc(1, sizeof(*state));
  state->req = req;
  state->http_mode = strcmp(req->path, "/direct-http") == 0;
  if (state->http_mode) {
    assert(len <= sizeof(state->pipeline));
    if (len > 0)
      memcpy(state->pipeline, data, len);
    state->pipeline_len = len;
  } else {
    memcpy(state->output, header, sizeof(header) - 1);
    state->length = sizeof(header) - 1;
  }
  if (!state->http_mode && len > 0) {
    memcpy(state->output + state->length, data, len);
    state->length += len;
  }
  c->hdlr_extra = state;
  c->disconnect = direct_disconnect;
  c->evt.handle = direct_event;
  c->flags |= CONN_IS_BUSY;
  http_request_sleep(req);
  direct_interest(c, state);
  return KORE_RESULT_RETRY;
}

static void *
send_main(void *arg)
{
  struct sender_args *sender;
  size_t sent;
  ssize_t amount;

  sender = (struct sender_args *)arg;
  sent = 0;
  while (sent < sender->size) {
    amount = send(sender->fd, sender->bytes + sent,
        sender->size - sent, MSG_NOSIGNAL);
    assert(amount > 0);
    sent += (size_t)amount;
  }
  assert(shutdown(sender->fd, SHUT_WR) == 0);
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

static int
connect_local(unsigned short port)
{
  struct sockaddr_in addr;
  int attempt;
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  for (attempt = 0; attempt < 100; attempt++) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
      return fd;
    assert(close(fd) == 0);
    usleep(10000u);
  }
  assert(0 && "Vectis listener did not start");
  return -1;
}

static vectis_status
health(vectis_app *app, vectis_request *request, vectis_response *response,
    void *userdata, vectis_error *error)
{
  vectis_bytes body;

  (void)app;
  (void)request;
  (void)userdata;
  body.data = prior_payload;
  body.size = sizeof(prior_payload);
  return vectis_response_bytes(response, 200, "application/octet-stream",
      body, error);
}

static vectis_status
stream_response(vectis_app *app, vectis_request *request,
    vectis_response *response, void *userdata, vectis_error *error)
{
  lc_source *source;
  vectis_status result;

  (void)app;
  (void)request;
  (void)userdata;
  source = NULL;
  assert(lc_source_from_memory(prior_payload, sizeof(prior_payload),
      &source, NULL) == LC_OK);
  result = vectis_response_stream_source(response, 200,
      "application/octet-stream", source, error);
  if (result != VECTIS_OK)
    lc_source_close(source);
  return result;
}

static void
read_exact(int fd, void *buffer, size_t length)
{
  size_t used;
  ssize_t got;

  used = 0;
  while (used < length) {
    got = recv(fd, (char *)buffer + used, length - used, 0);
    assert(got > 0);
    used += (size_t)got;
  }
}

static void
read_line(int fd, char *out, size_t capacity)
{
  size_t used;

  used = 0;
  for (;;) {
    assert(used < capacity - 1);
    read_exact(fd, out + used, 1);
    used++;
    out[used] = '\0';
    if (used >= 2 && out[used - 2] == '\r' && out[used - 1] == '\n')
      return;
  }
}

static void
check_stream_then_direct(unsigned short port)
{
  static const char request[] =
      "GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n"
      "GET /direct HTTP/1.1\r\nHost: localhost\r\n"
      "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";
  struct timeval timeout;
  unsigned char buffer[DIRECT_BUFFER_SIZE];
  char line[256];
  size_t received;
  size_t chunk;
  int fd;

  fd = connect_local(port);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
      sizeof(timeout)) == 0);
  assert(send(fd, request, sizeof(request) - 1, MSG_NOSIGNAL) ==
      (ssize_t)(sizeof(request) - 1));
  read_line(fd, line, sizeof(line));
  assert(strstr(line, " 200 ") != NULL);
  for (;;) {
    read_line(fd, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
    if (strncasecmp(line, "transfer-encoding:", 18) == 0)
      assert(strstr(line, "chunked") != NULL);
  }
  received = 0;
  for (;;) {
    read_line(fd, line, sizeof(line));
    chunk = (size_t)strtoul(line, NULL, 16);
    if (chunk == 0)
      break;
    assert(chunk <= sizeof(buffer));
    assert(chunk <= sizeof(prior_payload) - received);
    read_exact(fd, buffer, chunk);
    assert(memcmp(buffer, prior_payload + received, chunk) == 0);
    received += chunk;
    read_exact(fd, line, 2);
    assert(memcmp(line, "\r\n", 2) == 0);
  }
  assert(received == sizeof(prior_payload));
  read_line(fd, line, sizeof(line));
  assert(strcmp(line, "\r\n") == 0);
  read_line(fd, line, sizeof(line));
  assert(strstr(line, " 101 ") != NULL);
  for (;;) {
    read_line(fd, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
  }
  assert(shutdown(fd, SHUT_WR) == 0);
  assert(recv(fd, line, sizeof(line), 0) == 0);
  assert(close(fd) == 0);
}

static void
check_http_restore_response(int fd, size_t body_size)
{
  char line[256];
  unsigned char buffer[DIRECT_BUFFER_SIZE];
  size_t received;
  size_t chunk;

  read_line(fd, line, sizeof(line));
  assert(strstr(line, " 200 ") != NULL);
  for (;;) {
    read_line(fd, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
  }
  received = 0;
  while (received < body_size) {
    chunk = body_size - received;
    if (chunk > sizeof(buffer))
      chunk = sizeof(buffer);
    read_exact(fd, buffer, chunk);
    if (body_size == 4)
      assert(memcmp(buffer, "pong" + received, chunk) == 0);
    else
      assert(memcmp(buffer, prior_payload + received, chunk) == 0);
    received += chunk;
  }
}

static void
check_http_restore(unsigned short port)
{
  static const char pipelined[] =
      "GET /direct-http HTTP/1.1\r\nHost: localhost\r\n\r\n"
      "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
  static const char later[] =
      "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct timeval timeout;
  int fd;

  fd = connect_local(port);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  assert(send(fd, pipelined, sizeof(pipelined) - 1, MSG_NOSIGNAL) ==
      (ssize_t)(sizeof(pipelined) - 1));
  check_http_restore_response(fd, 4);
  check_http_restore_response(fd, PRIOR_PAYLOAD_SIZE);
  assert(send(fd, later, sizeof(later) - 1, MSG_NOSIGNAL) ==
      (ssize_t)(sizeof(later) - 1));
  check_http_restore_response(fd, PRIOR_PAYLOAD_SIZE);
  assert(close(fd) == 0);
}

static void
check_stream_then_queued_http(unsigned short port)
{
  static const char request[] =
      "GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n"
      "GET /direct-http HTTP/1.1\r\nHost: localhost\r\n\r\n"
      "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct timeval timeout;
  unsigned char buffer[DIRECT_BUFFER_SIZE];
  char line[256];
  size_t received;
  size_t chunk;
  int fd;

  fd = connect_local(port);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
      sizeof(timeout)) == 0);
  assert(send(fd, request, sizeof(request) - 1, MSG_NOSIGNAL) ==
      (ssize_t)(sizeof(request) - 1));
  read_line(fd, line, sizeof(line));
  assert(strstr(line, " 200 ") != NULL);
  for (;;) {
    read_line(fd, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
  }
  received = 0;
  for (;;) {
    read_line(fd, line, sizeof(line));
    chunk = (size_t)strtoul(line, NULL, 16);
    if (chunk == 0)
      break;
    assert(chunk <= sizeof(buffer));
    assert(chunk <= sizeof(prior_payload) - received);
    read_exact(fd, buffer, chunk);
    assert(memcmp(buffer, prior_payload + received, chunk) == 0);
    received += chunk;
    read_exact(fd, line, 2);
    assert(memcmp(line, "\r\n", 2) == 0);
  }
  assert(received == sizeof(prior_payload));
  read_line(fd, line, sizeof(line));
  assert(strcmp(line, "\r\n") == 0);
  check_http_restore_response(fd, 4);
  check_http_restore_response(fd, PRIOR_PAYLOAD_SIZE);
  assert(close(fd) == 0);
}

static void
check_tls_relay(unsigned short port, const unsigned char *payload)
{
  static const char request[] =
      "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"
      "GET /direct HTTP/1.1\r\nHost: localhost\r\n"
      "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";
  SSL_CTX *ctx;
  SSL *ssl;
  char response_header[256];
  unsigned char recvbuf[DIRECT_BUFFER_SIZE];
  size_t header_used;
  size_t sent;
  size_t received;
  size_t chunk;
  int fd;
  int amount;
  int sndbuf;
  int rcvbuf;
  size_t prior_read;
  struct timeval timeout;

  fd = connect_local(port);
  rcvbuf = 32768;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf,
      sizeof(rcvbuf)) == 0);
  sndbuf = 1024 * 1024;
  assert(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
      sizeof(timeout)) == 0);
  ctx = SSL_CTX_new(TLS_client_method());
  assert(ctx != NULL);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  ssl = SSL_new(ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
  assert(SSL_connect(ssl) == 1);
  assert(SSL_write(ssl, request, (int)(sizeof(request) - 1)) ==
      (int)(sizeof(request) - 1));
  header_used = 0;
  response_header[0] = '\0';
  while (strstr(response_header, "\r\n\r\n") == NULL) {
    assert(header_used < sizeof(response_header) - 1);
    assert(SSL_read(ssl, response_header + header_used, 1) == 1);
    response_header[++header_used] = '\0';
  }
  assert(strstr(response_header, " 200 ") != NULL);
  assert(strstr(response_header, "content-length: 131072") != NULL);
  prior_read = 0;
  while (prior_read < PRIOR_PAYLOAD_SIZE) {
    amount = SSL_read(ssl, recvbuf, sizeof(recvbuf));
    assert(amount > 0);
    assert((size_t)amount <= PRIOR_PAYLOAD_SIZE - prior_read);
    assert(memcmp(recvbuf, prior_payload + prior_read,
        (size_t)amount) == 0);
    prior_read += (size_t)amount;
  }
  header_used = 0;
  response_header[0] = '\0';
  while (strstr(response_header, "\r\n\r\n") == NULL) {
    assert(header_used < sizeof(response_header) - 1);
    assert(SSL_read(ssl, response_header + header_used, 1) == 1);
    response_header[++header_used] = '\0';
  }
  assert(strstr(response_header, " 101 ") != NULL);
  sent = 0;
  while (sent < TLS_PAYLOAD_SIZE) {
    chunk = TLS_PAYLOAD_SIZE - sent;
    if (chunk > 16384)
      chunk = 16384;
    amount = SSL_write(ssl, payload + sent, (int)chunk);
    assert(amount > 0);
    sent += (size_t)amount;
  }
  assert(SSL_shutdown(ssl) >= 0);
  usleep(100000u);
  received = 0;
  while (received < TLS_PAYLOAD_SIZE) {
    amount = SSL_read(ssl, recvbuf, sizeof(recvbuf));
    assert(amount > 0);
    assert((size_t)amount <= TLS_PAYLOAD_SIZE - received);
    assert(memcmp(recvbuf, payload + received, (size_t)amount) == 0);
    received += (size_t)amount;
  }
  amount = SSL_read(ssl, recvbuf, sizeof(recvbuf));
  assert(amount == 0 && SSL_get_error(ssl, amount) == SSL_ERROR_ZERO_RETURN);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  assert(close(fd) == 0);
}

static void
tls_read_exact(SSL *ssl, void *buffer, size_t length)
{
  size_t used;
  int got;

  used = 0;
  while (used < length) {
    got = SSL_read(ssl, (char *)buffer + used, (int)(length - used));
    assert(got > 0);
    used += (size_t)got;
  }
}

static void
tls_read_line(SSL *ssl, char *out, size_t capacity)
{
  size_t used;

  used = 0;
  for (;;) {
    assert(used < capacity - 1);
    tls_read_exact(ssl, out + used, 1);
    used++;
    out[used] = '\0';
    if (used >= 2 && out[used - 2] == '\r' && out[used - 1] == '\n')
      return;
  }
}

static void
tls_check_restore_response(SSL *ssl, size_t body_size)
{
  char line[256];
  unsigned char buffer[DIRECT_BUFFER_SIZE];
  size_t received;
  size_t chunk;

  tls_read_line(ssl, line, sizeof(line));
  assert(strstr(line, " 200 ") != NULL);
  for (;;) {
    tls_read_line(ssl, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
  }
  received = 0;
  while (received < body_size) {
    chunk = body_size - received;
    if (chunk > sizeof(buffer))
      chunk = sizeof(buffer);
    tls_read_exact(ssl, buffer, chunk);
    if (body_size == 4)
      assert(memcmp(buffer, "pong" + received, chunk) == 0);
    else
      assert(memcmp(buffer, prior_payload + received, chunk) == 0);
    received += chunk;
  }
}

static void
check_tls_http_restore(unsigned short port)
{
  static const char pipelined[] =
      "GET /direct-http HTTP/1.1\r\nHost: localhost\r\n\r\n"
      "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
  static const char later[] =
      "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct timeval timeout;
  SSL_CTX *ctx;
  SSL *ssl;
  int fd;

  fd = connect_local(port);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  ctx = SSL_CTX_new(TLS_client_method());
  assert(ctx != NULL);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  ssl = SSL_new(ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
  assert(SSL_connect(ssl) == 1);
  assert(SSL_write(ssl, pipelined, (int)(sizeof(pipelined) - 1)) ==
      (int)(sizeof(pipelined) - 1));
  tls_check_restore_response(ssl, 4);
  tls_check_restore_response(ssl, PRIOR_PAYLOAD_SIZE);
  assert(SSL_write(ssl, later, (int)(sizeof(later) - 1)) ==
      (int)(sizeof(later) - 1));
  tls_check_restore_response(ssl, PRIOR_PAYLOAD_SIZE);
  assert(SSL_shutdown(ssl) >= 0);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  assert(close(fd) == 0);
}

static void
check_tls_stream_then_queued_http(unsigned short port)
{
  static const char request[] =
      "GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n"
      "GET /direct-http HTTP/1.1\r\nHost: localhost\r\n\r\n"
      "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct timeval timeout;
  unsigned char buffer[DIRECT_BUFFER_SIZE];
  char line[256];
  SSL_CTX *ctx;
  SSL *ssl;
  size_t received;
  size_t chunk;
  int fd;

  fd = connect_local(port);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
      sizeof(timeout)) == 0);
  ctx = SSL_CTX_new(TLS_client_method());
  assert(ctx != NULL);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  ssl = SSL_new(ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
  assert(SSL_connect(ssl) == 1);
  assert(SSL_write(ssl, request, (int)(sizeof(request) - 1)) ==
      (int)(sizeof(request) - 1));
  tls_read_line(ssl, line, sizeof(line));
  assert(strstr(line, " 200 ") != NULL);
  for (;;) {
    tls_read_line(ssl, line, sizeof(line));
    if (strcmp(line, "\r\n") == 0)
      break;
  }
  received = 0;
  for (;;) {
    tls_read_line(ssl, line, sizeof(line));
    chunk = (size_t)strtoul(line, NULL, 16);
    if (chunk == 0)
      break;
    assert(chunk <= sizeof(buffer));
    assert(chunk <= sizeof(prior_payload) - received);
    tls_read_exact(ssl, buffer, chunk);
    assert(memcmp(buffer, prior_payload + received, chunk) == 0);
    received += chunk;
    tls_read_exact(ssl, line, 2);
    assert(memcmp(line, "\r\n", 2) == 0);
  }
  assert(received == sizeof(prior_payload));
  tls_read_line(ssl, line, sizeof(line));
  assert(strcmp(line, "\r\n") == 0);
  tls_check_restore_response(ssl, 4);
  tls_check_restore_response(ssl, PRIOR_PAYLOAD_SIZE);
  assert(SSL_shutdown(ssl) >= 0);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  assert(close(fd) == 0);
}

int
main(void)
{
  static const char request[] =
      "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"
      "GET /direct HTTP/1.1\r\nHost: localhost\r\n"
      "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";
  static const char direct_request[] =
      "GET /direct HTTP/1.1\r\nHost: localhost\r\n"
      "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";
  vectis_app_config config;
  vectis_cert_bundle_config certs;
  vectis_route_config route;
  vectis_error error;
  vectis_app *app;
  struct sender_args sender;
  struct pollfd watch;
  pthread_t sender_thread;
  unsigned char recvbuf[DIRECT_BUFFER_SIZE];
  char response_header[256];
  unsigned char *payload;
  unsigned short port;
  size_t received;
  size_t i;
  ssize_t got;
  int fd;
  size_t header_used;
  size_t prior_read;
  char ch;
  char cert_path[128];
  char key_path[128];
  int rcvbuf;
  int idle_probe;
  int idle_errno;

  metrics = mmap(NULL, sizeof(*metrics), PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(metrics != MAP_FAILED);
  memset(metrics, 0, sizeof(*metrics));
  payload = malloc(DIRECT_PAYLOAD_SIZE);
  assert(payload != NULL);
  memset(prior_payload, 'p', sizeof(prior_payload));
  for (i = 0; i < DIRECT_PAYLOAD_SIZE; i++)
    payload[i] = (unsigned char)(i % 251);
  port = available_port();
  vectis_kore_set_prebody_probe(direct_prebody);
  vectis_kore_set_worker_teardown_probe(direct_worker_teardown);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.server.worker_count = 1u;
  config.server.request_header_timeout_ms = 1000L;
  config.tls.bind = "127.0.0.1";
  config.tls.port = port;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/health", health, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/stream", stream_response, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  assert(app->start(app, &error) == VECTIS_OK);

  fd = connect_local(port);
  rcvbuf = 32768;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf,
      sizeof(rcvbuf)) == 0);
  assert(send(fd, request, sizeof(request) - 1, 0) ==
      (ssize_t)(sizeof(request) - 1));
  watch.fd = fd;
  watch.events = POLLIN;
  received = 0;
  header_used = 0;
  response_header[0] = '\0';
  while (strstr(response_header, "\r\n\r\n") == NULL) {
    assert(header_used < sizeof(response_header) - 1);
    assert(poll(&watch, 1, 5000) > 0);
    assert(recv(fd, &ch, 1, 0) == 1);
    response_header[header_used++] = ch;
    response_header[header_used] = '\0';
  }
  assert(strstr(response_header, " 200 ") != NULL);
  assert(strstr(response_header, "content-length: 131072") != NULL);
  prior_read = 0;
  while (prior_read < PRIOR_PAYLOAD_SIZE) {
    size_t remaining;

    remaining = PRIOR_PAYLOAD_SIZE - prior_read;
    if (remaining > sizeof(recvbuf))
      remaining = sizeof(recvbuf);
    got = recv(fd, recvbuf, remaining, 0);
    assert(got > 0);
    assert((size_t)got <= PRIOR_PAYLOAD_SIZE - prior_read);
    assert(memcmp(recvbuf, prior_payload + prior_read, (size_t)got) == 0);
    prior_read += (size_t)got;
  }
  header_used = 0;
  response_header[0] = '\0';
  while (strstr(response_header, "\r\n\r\n") == NULL) {
    assert(header_used < sizeof(response_header) - 1);
    assert(poll(&watch, 1, 5000) > 0);
    assert(recv(fd, &ch, 1, 0) == 1);
    response_header[header_used++] = ch;
    response_header[header_used] = '\0';
  }
  assert(strstr(response_header, " 101 ") != NULL);
  sender.fd = fd;
  sender.bytes = payload;
  sender.size = DIRECT_PAYLOAD_SIZE;
  assert(pthread_create(&sender_thread, NULL, send_main, &sender) == 0);
  usleep(100000u);
  while (received < DIRECT_PAYLOAD_SIZE) {
    assert(poll(&watch, 1, 5000) > 0);
    got = recv(fd, recvbuf, sizeof(recvbuf), 0);
    assert(got > 0);
    assert((size_t)got <= DIRECT_PAYLOAD_SIZE - received);
    assert(memcmp(recvbuf, payload + received, (size_t)got) == 0);
    received += (size_t)got;
  }
  assert(pthread_join(sender_thread, NULL) == 0);
  assert(poll(&watch, 1, 5000) > 0);
  assert(recv(fd, recvbuf, sizeof(recvbuf), 0) == 0);
  assert(close(fd) == 0);
  metrics->stream_probe_active = 1;
  check_stream_then_direct(port);
  metrics->stream_probe_active = 0;
  check_http_restore(port);
  metrics->stream_probe_active = 1;
  check_stream_then_queued_http(port);
  metrics->stream_probe_active = 0;

  fd = connect_local(port);
  assert(send(fd, direct_request, sizeof(direct_request) - 1, 0) ==
      (ssize_t)(sizeof(direct_request) - 1));
  watch.fd = fd;
  header_used = 0;
  response_header[0] = '\0';
  while (strstr(response_header, "\r\n\r\n") == NULL) {
    assert(header_used < sizeof(response_header) - 1);
    assert(poll(&watch, 1, 5000) > 0);
    assert(recv(fd, &ch, 1, 0) == 1);
    response_header[header_used++] = ch;
    response_header[header_used] = '\0';
  }
  assert(strstr(response_header, " 101 ") != NULL);
  usleep(1600000u);
  idle_probe = (int)recv(fd, &ch, 1, MSG_PEEK | MSG_DONTWAIT);
  idle_errno = errno;
  assert(vectis_stop(app, &error) == VECTIS_OK);
  assert(close(fd) == 0);
  app->close(app);
  assert(idle_probe < 0 &&
      (idle_errno == EAGAIN || idle_errno == EWOULDBLOCK));
  fprintf(stderr,
      "direct io: events=%u continuations=%u pauses=%u eof=%u "
      "disconnects=%u max_queue=%zu prior_seen=%u prior_drained=%u\n",
      metrics->events, metrics->continuations, metrics->write_pauses,
      metrics->read_eofs, metrics->disconnects, metrics->max_queued,
      metrics->prior_queue_seen, metrics->prior_queue_drained);
  assert(metrics->events < 10000);
  assert(metrics->write_pauses > 0);
  assert(metrics->read_eofs == 2);
  assert(metrics->disconnects == 3);
  assert(metrics->worker_teardown_calls == 1);
  assert(metrics->worker_teardown_active == 1);
  assert(metrics->max_queued <= DIRECT_BUFFER_SIZE);
  assert(metrics->prior_queue_seen == 3);
  assert(metrics->prior_queue_drained >= 1);
  assert(metrics->stream_takeover_seen == 1);
  assert(metrics->queued_stream_takeover_seen == 1);
  assert(metrics->http_restored == 2);
  assert(metrics->http_queue_chunks == 2);

  memset(metrics, 0, sizeof(*metrics));
  assert(snprintf(cert_path, sizeof(cert_path),
      "vectis-kore-direct-%ld-cert.pem", (long)getpid()) > 0);
  assert(snprintf(key_path, sizeof(key_path),
      "vectis-kore-direct-%ld-key.pem", (long)getpid()) > 0);
  vectis_cert_bundle_config_init(&certs);
  certs.subject.common_name = "localhost";
  certs.dns_names = "localhost";
  certs.output_cert_path = cert_path;
  certs.output_key_path = key_path;
  certs.key_bits = 2048u;
  certs.valid_days = 1L;
  assert(vectis_cert_generate_bundle(&certs, &error) == VECTIS_OK);
  port = available_port();
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_MANUAL;
  config.server.worker_count = 1u;
  config.tls.bind = "127.0.0.1";
  config.tls.port = port;
  config.tls.domain = "localhost";
  config.tls.certificate_path = cert_path;
  config.tls.private_key_path = key_path;
  config.tls.ca_bundle_path = cert_path;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/health", health, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/stream", stream_response, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  assert(app->start(app, &error) == VECTIS_OK);
  check_tls_relay(port, payload);
  check_tls_http_restore(port);
  metrics->stream_probe_active = 1;
  check_tls_stream_then_queued_http(port);
  metrics->stream_probe_active = 0;
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  vectis_kore_set_prebody_probe(NULL);
  vectis_kore_set_worker_teardown_probe(NULL);
  assert(remove(cert_path) == 0);
  assert(remove(key_path) == 0);
  fprintf(stderr,
      "direct TLS io: events=%u continuations=%u pauses=%u eof=%u "
      "disconnects=%u max_queue=%zu read_want_read=%u "
      "write_want_write=%u cross_read=%u cross_write=%u "
      "prior_seen=%u prior_drained=%u\n",
      metrics->events, metrics->continuations, metrics->write_pauses,
      metrics->read_eofs, metrics->disconnects, metrics->max_queued,
      metrics->tls_read_want_read, metrics->tls_write_want_write,
      metrics->tls_read_want_write, metrics->tls_write_want_read,
      metrics->prior_queue_seen, metrics->prior_queue_drained);
  assert(metrics->write_pauses > 0);
  assert(metrics->tls_read_want_read > 0);
  assert(metrics->tls_write_want_write > 0);
  assert(metrics->read_eofs == 1);
  assert(metrics->disconnects == 1);
  assert(metrics->max_queued <= DIRECT_BUFFER_SIZE);
  assert(metrics->prior_queue_seen == 2);
  assert(metrics->prior_queue_drained >= 1);
  assert(metrics->queued_stream_takeover_seen == 1);
  assert(metrics->http_restored == 2);
  assert(metrics->http_queue_chunks == 2);
  assert(munmap(metrics, sizeof(*metrics)) == 0);
  free(payload);
  return 0;
}
