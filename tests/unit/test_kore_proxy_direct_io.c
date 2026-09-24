#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <kore/http.h>
#include <kore/kore.h>
#include <vectis/vectis.h>

#define DIRECT_BUFFER_SIZE 8192
#define DIRECT_PAYLOAD_SIZE (1024 * 1024)

struct direct_metrics {
  unsigned events;
  unsigned continuations;
  unsigned disconnects;
  unsigned write_pauses;
  unsigned read_eofs;
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
};

struct sender_args {
  int fd;
  unsigned char *bytes;
  size_t size;
};

static struct direct_metrics *metrics;

extern void vectis_kore_set_prebody_probe(
    int (*probe)(struct http_request *, const void *, size_t));

static void direct_pump(struct connection *c);

static void
direct_interest(struct connection *c, struct direct_state *state)
{
  int events;

  events = 0;
  if (state->length > state->offset) {
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
  int step;

  state = (struct direct_state *)c->hdlr_extra;
  assert(state != NULL);
  for (step = 0; step < 64; step++) {
    if (state->length > state->offset) {
      amount = send(c->fd, state->output + state->offset,
          state->length - state->offset, MSG_NOSIGNAL);
      if (amount > 0) {
        state->offset += (size_t)amount;
        state->write_wait = 0;
        if (state->offset == state->length) {
          state->offset = 0;
          state->length = 0;
        }
        continue;
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
    amount = recv(c->fd, state->output, sizeof(state->output), 0);
    if (amount > 0) {
      state->length = (size_t)amount;
      state->read_wait = 0;
      if (state->length > metrics->max_queued)
        metrics->max_queued = state->length;
      continue;
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
  c->evt.flags = 0;
  (void)error;
  direct_pump(c);
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

  if (strcmp(req->path, "/direct") != 0)
    return KORE_RESULT_OK;
  assert(len <= DIRECT_BUFFER_SIZE - (sizeof(header) - 1));
  c = req->owner;
  assert(TAILQ_EMPTY(&c->send_queue));
  sndbuf = 4096;
  assert(setsockopt(c->fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0);
  state = kore_calloc(1, sizeof(*state));
  state->req = req;
  memcpy(state->output, header, sizeof(header) - 1);
  state->length = sizeof(header) - 1;
  if (len > 0) {
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
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 200, "text/plain", "ok", error);
}

int
main(void)
{
  static const char request[] =
      "GET /direct HTTP/1.1\r\nHost: localhost\r\n"
      "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";
  vectis_app_config config;
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
  char ch;

  metrics = mmap(NULL, sizeof(*metrics), PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(metrics != MAP_FAILED);
  memset(metrics, 0, sizeof(*metrics));
  payload = malloc(DIRECT_PAYLOAD_SIZE);
  assert(payload != NULL);
  for (i = 0; i < DIRECT_PAYLOAD_SIZE; i++)
    payload[i] = (unsigned char)(i % 251);
  port = available_port();
  vectis_kore_set_prebody_probe(direct_prebody);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = port;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/health", health, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  assert(app->start(app, &error) == VECTIS_OK);

  fd = connect_local(port);
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
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  vectis_kore_set_prebody_probe(NULL);
  fprintf(stderr,
      "direct io: events=%u continuations=%u pauses=%u eof=%u "
      "disconnects=%u max_queue=%zu\n",
      metrics->events, metrics->continuations, metrics->write_pauses,
      metrics->read_eofs, metrics->disconnects, metrics->max_queued);
  assert(metrics->events < 10000);
  assert(metrics->write_pauses > 0);
  assert(metrics->read_eofs == 1);
  assert(metrics->disconnects == 1);
  assert(metrics->max_queued <= DIRECT_BUFFER_SIZE);
  assert(munmap(metrics, sizeof(*metrics)) == 0);
  free(payload);
  return 0;
}
