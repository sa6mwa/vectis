#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <vectis/proxy.h>
#include <vectis/vectis.h>

#define TEST_EXCHANGES 16

typedef struct test_origin {
  int listener;
  int clients[TEST_EXCHANGES];
  unsigned short port;
  pthread_t thread;
  volatile int accepted;
  volatile int release;
} test_origin;

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
  assert(listen(*listener, TEST_EXCHANGES + 1) == 0);
  length = sizeof(address);
  assert(getsockname(*listener, (struct sockaddr *)&address, &length) == 0);
  return ntohs(address.sin_port);
}

static unsigned short unused_port(void) {
  int listener;
  unsigned short port;

  port = listen_port(&listener);
  assert(close(listener) == 0);
  return port;
}

static void *origin_main(void *userdata) {
  static const char reply[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
      "Transfer-Encoding: chunked\r\n\r\n4\r\ntest\r\n";
  test_origin *origin;
  char request[512];
  size_t used;
  ssize_t amount;
  int i;

  origin = (test_origin *)userdata;
  for (i = 0; i < TEST_EXCHANGES; ++i) {
    origin->clients[i] = accept(origin->listener, NULL, NULL);
    assert(origin->clients[i] >= 0);
    used = 0u;
    do {
      amount = recv(origin->clients[i], request + used,
                    sizeof(request) - used - 1u, 0);
      assert(amount > 0);
      used += (size_t)amount;
      request[used] = '\0';
    } while (strstr(request, "\r\n\r\n") == NULL &&
             used < sizeof(request) - 1u);
    assert(strstr(request, "GET /proxy/stream HTTP/1.1\r\n") == request);
    send_all(origin->clients[i], reply, sizeof(reply) - 1u);
    (void)__sync_add_and_fetch(&origin->accepted, 1);
  }
  while (__sync_fetch_and_add(&origin->release, 0) == 0)
    usleep(1000u);
  for (i = 0; i < TEST_EXCHANGES; ++i) {
    send_all(origin->clients[i], "0\r\n\r\n", 5u);
    assert(close(origin->clients[i]) == 0);
  }
  return NULL;
}

static int connect_app_raw(unsigned short port) {
  struct sockaddr_in address;
  struct timeval timeout;
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
  return fd;
}

static int connect_app(unsigned short port, int websocket) {
  static const char http_request[] =
      "GET /proxy/stream HTTP/1.1\r\nHost: localhost\r\n\r\n";
  static const char ws_request[] =
      "GET /proxy/stream HTTP/1.1\r\nHost: localhost\r\n"
      "Connection: Upgrade\r\nUpgrade: websocket\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
  int fd;

  fd = connect_app_raw(port);
  if (websocket)
    send_all(fd, ws_request, sizeof(ws_request) - 1u);
  else
    send_all(fd, http_request, sizeof(http_request) - 1u);
  return fd;
}

static void read_head(int fd, char *response, size_t capacity) {
  size_t used;
  ssize_t amount;

  used = 0u;
  while (strstr(response, "\r\n\r\n") == NULL) {
    assert(used < capacity - 1u);
    amount = recv(fd, response + used, capacity - used - 1u, 0);
    assert(amount > 0);
    used += (size_t)amount;
    response[used] = '\0';
  }
}

static void read_all(int fd, char *response, size_t capacity) {
  size_t used;
  ssize_t amount;

  used = 0u;
  do {
    assert(used < capacity - 1u);
    amount = recv(fd, response + used, capacity - used - 1u, 0);
    assert(amount >= 0);
    used += (size_t)amount;
  } while (amount != 0);
  response[used] = '\0';
}

int main(void) {
  test_origin origin;
  vectis_proxy_route_config proxy;
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  struct pollfd pending;
  unsigned short app_port;
  char target[128];
  char response[2048];
  int clients[TEST_EXCHANGES];
  int overflow;
  int slow;
  int i;

  memset(&origin, 0, sizeof(origin));
  origin.port = listen_port(&origin.listener);
  app_port = unused_port();
  assert(snprintf(target, sizeof(target), "http://127.0.0.1:%u",
                  (unsigned)origin.port) > 0);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = app_port;
  config.server.worker_count = 1u;
  config.server.request_header_timeout_ms = 1000L;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  vectis_proxy_route_config_init(&proxy);
  proxy.path = "/proxy/stream";
  proxy.methods = VECTIS_HTTP_METHODS_GET;
  proxy.target = target;
  assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
  assert(app->start(app, &error) == VECTIS_OK);
  assert(pthread_create(&origin.thread, NULL, origin_main, &origin) == 0);

  slow = connect_app_raw(app_port);
  send_all(slow, "GET /proxy/stream HTTP/1.1\r\nHost: localhost",
           sizeof("GET /proxy/stream HTTP/1.1\r\nHost: localhost") - 1u);
  read_all(slow, response, sizeof(response));
  assert(close(slow) == 0);
  assert(__sync_fetch_and_add(&origin.accepted, 0) == 0);
  pending.fd = origin.listener;
  pending.events = POLLIN;
  pending.revents = 0;
  assert(poll(&pending, 1u, 100) == 0);

  for (i = 0; i < TEST_EXCHANGES; ++i) {
    clients[i] = connect_app(app_port, 0);
    response[0] = '\0';
    read_head(clients[i], response, sizeof(response));
    assert(strstr(response, "HTTP/1.1 200 ") == response);
  }
  assert(__sync_fetch_and_add(&origin.accepted, 0) == TEST_EXCHANGES);
  overflow = connect_app(app_port, 0);
  response[0] = '\0';
  read_all(overflow, response, sizeof(response));
  assert(strstr(response, "HTTP/1.1 503 Service Unavailable\r\n") == response);
  assert(strstr(response, "proxy worker exchange limit reached\n") != NULL);
  assert(close(overflow) == 0);
  overflow = connect_app(app_port, 1);
  read_all(overflow, response, sizeof(response));
  assert(strstr(response, "HTTP/1.1 503 Service Unavailable\r\n") == response);
  assert(strstr(response, "proxy worker exchange limit reached\n") != NULL);
  assert(close(overflow) == 0);
  pending.fd = origin.listener;
  pending.events = POLLIN;
  pending.revents = 0;
  assert(poll(&pending, 1u, 100) == 0);

  (void)__sync_lock_test_and_set(&origin.release, 1);
  assert(pthread_join(origin.thread, NULL) == 0);
  for (i = 0; i < TEST_EXCHANGES; ++i)
    assert(close(clients[i]) == 0);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  assert(close(origin.listener) == 0);
  return 0;
}
