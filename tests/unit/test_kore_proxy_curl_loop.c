#include <arpa/inet.h>
#include <assert.h>
#include <curl/curl.h>
#include <errno.h>
#include <netinet/in.h>
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

struct echo_server {
  int listener;
  unsigned short port;
  pthread_t thread;
};

struct loop_metrics {
  unsigned connect_done;
  unsigned curl_watch_removed;
  unsigned tunnel_done;
  unsigned downstream_done;
  unsigned disconnects;
  unsigned max_curl_watchers;
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
  CURLM *multi;
  CURL *easy;
  curl_socket_t upstream_fd;
  unsigned curl_watch_count;
  int tunnel_watched;
  int phase;
  int running;
  size_t response_sent;
};

static struct loop_metrics *metrics;
static unsigned short upstream_port;

extern void vectis_kore_set_prebody_probe(
    int (*probe)(struct http_request *, const void *, size_t));

static void drive_curl(struct proxy_state *state, curl_socket_t fd, int flags);
static void curl_event(void *arg, int error);

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
tunnel_event(void *arg, int error)
{
  struct proxy_state *state;
  CURLcode code;
  char reply[4];
  size_t amount;

  state = (struct proxy_state *)((char *)arg -
      offsetof(struct proxy_state, tunnel_event));
  state->tunnel_event.flags = 0;
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
drive_curl(struct proxy_state *state, curl_socket_t fd, int flags)
{
  CURLMsg *msg;
  int pending;

  assert(curl_multi_socket_action(state->multi, fd,
      flags, &state->running) == CURLM_OK);
  if (state->running != 0 || state->phase != 0)
    return;
  msg = curl_multi_info_read(state->multi, &pending);
  assert(msg != NULL && msg->msg == CURLMSG_DONE);
  assert(msg->data.result == CURLE_OK);
  assert(curl_easy_getinfo(state->easy, CURLINFO_ACTIVESOCKET,
      &state->upstream_fd) == CURLE_OK);
  assert(state->upstream_fd != CURL_SOCKET_BAD);
  assert(state->curl_watch_count == 0);
  metrics->connect_done++;
  state->phase = 1;
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
downstream_disconnect(struct connection *connection)
{
  struct proxy_state *state;

  state = (struct proxy_state *)connection->hdlr_extra;
  metrics->disconnects++;
  if (state == NULL)
    return;
  if (state->timer != NULL)
    kore_timer_remove(state->timer);
  if (state->tunnel_watched)
    kore_platform_disable_read((int)state->upstream_fd);
  if (state->easy != NULL) {
    assert(curl_multi_remove_handle(state->multi,
        state->easy) == CURLM_OK);
    curl_easy_cleanup(state->easy);
  }
  assert(state->curl_watch_count == 0);
  if (state->multi != NULL)
    assert(curl_multi_cleanup(state->multi) == CURLM_OK);
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
  char url[128];

  (void)data;
  if (strcmp(req->path, "/curl") != 0)
    return KORE_RESULT_OK;
  assert(len == 0);
  state = kore_calloc(1, sizeof(*state));
  state->downstream = req->owner;
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
  assert(snprintf(url, sizeof(url), "http://127.0.0.1:%u/",
      (unsigned)upstream_port) > 0);
  assert(curl_easy_setopt(state->easy, CURLOPT_URL, url) == CURLE_OK);
  assert(curl_easy_setopt(state->easy, CURLOPT_CONNECT_ONLY, 1L) == CURLE_OK);
  assert(curl_easy_setopt(state->easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK);
  assert(curl_multi_add_handle(state->multi, state->easy) == CURLM_OK);
  state->running = 1;
  drive_curl(state, CURL_SOCKET_TIMEOUT, 0);
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

int
main(void)
{
  static const char request[] =
      "GET /curl HTTP/1.1\r\nHost: localhost\r\n\r\n";
  static const char expected[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\npong";
  struct echo_server upstream;
  struct sockaddr_in addr;
  struct timeval timeout;
  vectis_app_config config;
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
  upstream_port = upstream.port;
  port = available_port();
  vectis_kore_set_prebody_probe(takeover);
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
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  vectis_kore_set_prebody_probe(NULL);
  assert(pthread_join(upstream.thread, NULL) == 0);
  assert(close(upstream.listener) == 0);
  curl_global_cleanup();
  assert(metrics->connect_done == 1);
  assert(metrics->curl_watch_removed > 0);
  assert(metrics->tunnel_done == 1);
  assert(metrics->downstream_done == 1);
  assert(metrics->disconnects == 1);
  assert(metrics->max_curl_watchers > 0);
  fprintf(stderr, "Kore curl loop: max_watchers=%u removed=%u\n",
      metrics->max_curl_watchers, metrics->curl_watch_removed);
  assert(munmap(metrics, sizeof(*metrics)) == 0);
  return 0;
}
