#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <vectis/proxy.h>
#include <vectis/vectis.h>

typedef struct origin_server {
  int listener;
  unsigned short port;
  pthread_t thread;
} origin_server;

static unsigned short listen_port(int *listener) {
  struct sockaddr_in address;
  socklen_t length;

  *listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(*listener >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(*listener, (struct sockaddr *)&address, sizeof(address)) == 0);
  assert(listen(*listener, 2) == 0);
  length = sizeof(address);
  assert(getsockname(*listener, (struct sockaddr *)&address, &length) == 0);
  return ntohs(address.sin_port);
}

static int connect_app(unsigned short port) {
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

static void *origin_main(void *userdata) {
  static const char reply[] =
      "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n"
      "Connection: close\r\n\r\n";
  origin_server *server;
  char request[1024];
  size_t used;
  ssize_t got;
  int fd;
  int i;

  server = (origin_server *)userdata;
  for (i = 0; i < 2; ++i) {
    fd = accept(server->listener, NULL, NULL);
    assert(fd >= 0);
    used = 0u;
    do {
      got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
      assert(got > 0);
      used += (size_t)got;
      request[used] = '\0';
    } while (strstr(request, "\r\n\r\n") == NULL &&
             used < sizeof(request) - 1u);
    assert(strstr(request, "GET /base/proxy/upstream HTTP/1.1\r\n") != NULL);
    assert(send(fd, reply, sizeof(reply) - 1u, 0) ==
           (ssize_t)(sizeof(reply) - 1u));
    assert(close(fd) == 0);
  }
  return NULL;
}

static vectis_status preflight(const vectis_proxy_inbound *in,
                               vectis_proxy_local_response *response,
                               void *userdata, vectis_error *error) {
  (void)userdata;
  if (strcmp(vectis_proxy_inbound_path(in), "/proxy/local") == 0)
    return vectis_proxy_local_respond(response, 401, NULL, 0u, error);
  return VECTIS_OK;
}

static void expect_status(unsigned short port, const char *path) {
  char request[128];
  char response[1024];
  size_t used;
  ssize_t got;
  int fd;
  int count;

  fd = connect_app(port);
  count = snprintf(request, sizeof(request),
                   "GET %s HTTP/1.1\r\nHost: localhost\r\n"
                   "Connection: close\r\n\r\n",
                   path);
  assert(count > 0 && (size_t)count < sizeof(request));
  assert(send(fd, request, (size_t)count, 0) == count);
  used = 0u;
  while (used < sizeof(response) - 1u) {
    got = recv(fd, response + used, sizeof(response) - used - 1u, 0);
    assert(got >= 0);
    if (got == 0)
      break;
    used += (size_t)got;
  }
  response[used] = '\0';
  assert(strstr(response, "HTTP/1.1 401 ") != NULL);
  assert(close(fd) == 0);
}

int main(void) {
  origin_server origin;
  vectis_proxy_route_config proxy;
  vectis_app_config config;
  vectis_error error;
  vectis_app *app;
  unsigned short app_port;
  char target[128];
  char response[16];
  int listener;
  int fd;

  origin.port = listen_port(&origin.listener);
  app_port = listen_port(&listener);
  assert(close(listener) == 0);
  assert(snprintf(target, sizeof(target), "http://127.0.0.1:%u/base",
                  (unsigned)origin.port) > 0);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = app_port;
  config.server.worker_count = 1u;
  config.server.autoblock.enabled = 1;
  config.server.autoblock.status_rule_count = 1u;
  config.server.autoblock.status_rules[0].status = 401u;
  config.server.autoblock.status_rules[0].threshold = 3u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  vectis_proxy_route_config_init(&proxy);
  proxy.path = "/proxy/:id";
  proxy.path_kind = VECTIS_ROUTE_PATH_PARAMS;
  proxy.methods = VECTIS_HTTP_METHODS_GET;
  proxy.target = target;
  proxy.preflight = preflight;
  assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
  assert(app->start(app, &error) == VECTIS_OK);
  assert(pthread_create(&origin.thread, NULL, origin_main, &origin) == 0);
  expect_status(app_port, "/proxy/upstream");
  expect_status(app_port, "/proxy/local");
  expect_status(app_port, "/proxy/upstream");
  fd = connect_app(app_port);
  assert(recv(fd, response, sizeof(response), 0) == 0);
  assert(close(fd) == 0);
  assert(pthread_join(origin.thread, NULL) == 0);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  assert(close(origin.listener) == 0);
  return 0;
}
