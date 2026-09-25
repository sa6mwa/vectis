#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <vectis/proxy.h>
#include <vectis/vectis.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <kore/http.h>
#include <kore/kore.h>
#pragma GCC diagnostic pop

typedef struct test_probe {
  volatile unsigned held;
} test_probe;

typedef struct origin_server {
  int listener;
  unsigned short port;
  pthread_t thread;
  test_probe *probe;
} origin_server;

static test_probe *active_probe;

int __real_net_send_flush(struct connection *connection);
int __wrap_net_send_flush(struct connection *connection);

int __wrap_net_send_flush(struct connection *connection) {
  struct netbuf *buffer;

  buffer = TAILQ_FIRST(&connection->send_queue);
  if (active_probe != NULL && buffer != NULL && buffer->b_len >= 13u &&
      memcmp(buffer->buf, "HTTP/1.1 200 ", 13u) == 0) {
    (void)__sync_add_and_fetch(&active_probe->held, 1u);
    usleep(1000u);
    return KORE_RESULT_OK;
  }
  return __real_net_send_flush(connection);
}

static void send_all(int fd, const char *data, size_t length) {
  ssize_t amount;

  while (length != 0u) {
    amount = send(fd, data, length, 0);
    assert(amount > 0);
    data += (size_t)amount;
    length -= (size_t)amount;
  }
}

static unsigned short listen_port(int *listener) {
  struct sockaddr_in address;
  socklen_t length;

  *listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(*listener >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(*listener, (struct sockaddr *)&address, sizeof(address)) == 0);
  assert(listen(*listener, 1) == 0);
  length = sizeof(address);
  assert(getsockname(*listener, (struct sockaddr *)&address, &length) == 0);
  return ntohs(address.sin_port);
}

static unsigned short unused_port(void) {
  unsigned short port;
  int listener;

  port = listen_port(&listener);
  assert(close(listener) == 0);
  return port;
}

static void *origin_main(void *userdata) {
  static const char partial[] = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n"
                                "Connection: close\r\n\r\nx";
  origin_server *server;
  char request[1024];
  size_t used;
  ssize_t amount;
  int fd;
  int attempt;

  server = (origin_server *)userdata;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  used = 0u;
  do {
    amount = recv(fd, request + used, sizeof(request) - used - 1u, 0);
    assert(amount > 0);
    used += (size_t)amount;
    request[used] = '\0';
  } while (strstr(request, "\r\n\r\n") == NULL && used < sizeof(request) - 1u);
  assert(strstr(request, "GET /proxy/held HTTP/1.1\r\n") != NULL);
  send_all(fd, partial, sizeof(partial) - 1u);
  for (attempt = 0;
       attempt < 3000 && __sync_fetch_and_add(&server->probe->held, 0u) == 0u;
       ++attempt)
    usleep(1000u);
  assert(__sync_fetch_and_add(&server->probe->held, 0u) != 0u);
  assert(close(fd) == 0);
  return NULL;
}

static size_t request_app(unsigned short port, char *response,
                          size_t capacity) {
  static const char request[] =
      "GET /proxy/held HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct sockaddr_in address;
  struct timeval timeout;
  size_t used;
  ssize_t amount;
  int attempt;
  int fd;

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  for (attempt = 0; attempt < 100; ++attempt) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0)
      break;
    assert(close(fd) == 0);
    usleep(10000u);
  }
  assert(attempt < 100);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0);
  send_all(fd, request, sizeof(request) - 1u);
  used = 0u;
  while (used < capacity) {
    amount = recv(fd, response + used, capacity - used, 0);
    assert(amount >= 0);
    if (amount == 0)
      break;
    used += (size_t)amount;
  }
  assert(used < capacity);
  response[used] = '\0';
  assert(close(fd) == 0);
  return used;
}

int main(void) {
  origin_server origin;
  vectis_proxy_route_config proxy;
  vectis_metrics_config metrics;
  vectis_app_config config;
  vectis_mutable_bytes snapshot;
  vectis_app *app;
  vectis_error error;
  unsigned short app_port;
  char target[128];
  char response[2048];
  const char *body;
  size_t length;

  active_probe = mmap(NULL, sizeof(*active_probe), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(active_probe != MAP_FAILED);
  memset(active_probe, 0, sizeof(*active_probe));
  origin.probe = active_probe;
  origin.port = listen_port(&origin.listener);
  app_port = unused_port();
  assert(snprintf(target, sizeof(target), "http://127.0.0.1:%u",
                  (unsigned)origin.port) > 0);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = app_port;
  config.server.worker_count = 1u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  vectis_proxy_route_config_init(&proxy);
  proxy.path = "/proxy/held";
  proxy.methods = VECTIS_HTTP_METHODS_GET;
  proxy.target = target;
  assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
  vectis_metrics_config_init(&metrics);
  assert(app->metrics(app, &metrics, &error) == VECTIS_OK);
  assert(app->start(app, &error) == VECTIS_OK);
  assert(pthread_create(&origin.thread, NULL, origin_main, &origin) == 0);

  length = request_app(app_port, response, sizeof(response) - 1u);
  assert(__sync_fetch_and_add(&active_probe->held, 0u) != 0u);
  assert(length != 0u &&
         strstr(response, "HTTP/1.1 502 Bad Gateway\r\n") == response);
  assert(strstr(response, "HTTP/1.1 200 ") == NULL);
  assert(strstr(response, "Content-Length: 11\r\n") != NULL);
  body = strstr(response, "\r\n\r\n");
  assert(body != NULL);
  body += 4;
  assert(length - (size_t)(body - response) == 11u);
  assert(memcmp(body, "bad gateway", 11u) == 0);
  memset(&snapshot, 0, sizeof(snapshot));
  assert(vectis_metrics_snapshot_json(app, &snapshot, &error) == VECTIS_OK);
  assert(strstr((const char *)snapshot.data, "\"requests_total\":1") != NULL);
  assert(strstr((const char *)snapshot.data, "\"2xx\":0") != NULL);
  assert(strstr((const char *)snapshot.data, "\"5xx\":1") != NULL);
  vectis_mutable_bytes_cleanup(&snapshot);

  assert(pthread_join(origin.thread, NULL) == 0);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  assert(close(origin.listener) == 0);
  assert(munmap(active_probe, sizeof(*active_probe)) == 0);
  return 0;
}
