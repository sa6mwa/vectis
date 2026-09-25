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

static size_t request_app(unsigned short port, const char *request,
                          char *response, size_t capacity) {
  size_t used;
  ssize_t amount;
  int fd;

  fd = connect_app(port);
  send_all(fd, request, strlen(request));
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

static unsigned long metric_bucket(vectis_app *app, const char *name) {
  vectis_mutable_bytes snapshot;
  vectis_error error;
  char key[32];
  const char *value;
  char *end;
  unsigned long count;

  memset(&snapshot, 0, sizeof(snapshot));
  assert(vectis_metrics_snapshot_json(app, &snapshot, &error) == VECTIS_OK);
  assert(snprintf(key, sizeof(key), "\"%s\":", name) > 0);
  value = strstr((const char *)snapshot.data, key);
  assert(value != NULL);
  value += strlen(key);
  count = strtoul(value, &end, 10);
  assert(end != value);
  vectis_mutable_bytes_cleanup(&snapshot);
  return count;
}

static void *origin_main(void *userdata) {
  origin_server *server;
  char request[1024];
  size_t used;
  ssize_t amount;
  int fd;

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
  assert(strstr(request, "GET /base/proxy/preflight/allow HTTP/1.1\r\n") !=
         NULL);
  send_all(fd,
           "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
           "Connection: close\r\n\r\nok",
           strlen("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                  "Connection: close\r\n\r\nok"));
  assert(close(fd) == 0);
  return NULL;
}

static vectis_status preflight(const vectis_proxy_inbound *in,
                               vectis_proxy_local_response *response,
                               void *userdata, vectis_error *error) {
  const char *id;
  const char body[] = {'d', 'e', 'n', 'y'};

  (void)userdata;
  assert(strcmp(vectis_proxy_inbound_host(in), "localhost") == 0);
  id = vectis_proxy_inbound_path_param(in, "id");
  assert(id != NULL);
  if (strcmp(id, "allow") == 0)
    return VECTIS_OK;
  if (strcmp(id, "bad") == 0) {
    assert(vectis_proxy_local_respond(response, 200, "ok", 2u, error) ==
           VECTIS_OK);
    assert(vectis_proxy_local_add_header(response, "Content-Length", "2",
                                         error) == VECTIS_ERR_INVALID);
    return VECTIS_OK;
  }
  assert(strcmp(id, "deny") == 0);
  assert(vectis_proxy_local_respond(response, 401, body, sizeof(body), error) ==
         VECTIS_OK);
  assert(vectis_proxy_local_add_header(response, "Set-Cookie", "a=1", error) ==
         VECTIS_OK);
  assert(vectis_proxy_local_add_header(response, "Set-Cookie", "b=2", error) ==
         VECTIS_OK);
  return vectis_proxy_local_add_header(response, "X-Local", "preflight", error);
}

static vectis_status on_error(const vectis_error *cause, int default_status,
                              vectis_proxy_local_response *response,
                              void *userdata, vectis_error *error) {
  (void)userdata;
  assert(cause != NULL && cause->message[0] != '\0');
  assert(default_status == 502);
  assert(vectis_proxy_local_respond(response, 503, "upstream down", 13u,
                                    error) == VECTIS_OK);
  return vectis_proxy_local_add_header(response, "X-Local", "gateway", error);
}

int main(void) {
  origin_server origin;
  vectis_proxy_route_config proxy;
  vectis_metrics_config metrics;
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  unsigned short app_port;
  unsigned short dead_port;
  char target[128];
  char dead_target[128];
  char response[2048];
  const char *boundary;
  size_t length;
  unsigned long prior_2xx;
  unsigned long prior_5xx;

  origin.port = listen_port(&origin.listener);
  app_port = unused_port();
  do {
    dead_port = unused_port();
  } while (dead_port == app_port);
  assert(snprintf(target, sizeof(target), "http://127.0.0.1:%u/base",
                  (unsigned)origin.port) > 0);
  assert(snprintf(dead_target, sizeof(dead_target), "http://127.0.0.1:%u",
                  (unsigned)dead_port) > 0);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = app_port;
  config.server.worker_count = 1u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  vectis_proxy_route_config_init(&proxy);
  proxy.path = "/proxy/preflight/:id";
  proxy.methods = VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD |
                  VECTIS_HTTP_METHODS_POST;
  proxy.path_kind = VECTIS_ROUTE_PATH_PARAMS;
  proxy.target = target;
  proxy.preflight = preflight;
  assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
  proxy.path = "/proxy/error";
  proxy.methods = VECTIS_HTTP_METHODS_GET;
  proxy.path_kind = VECTIS_ROUTE_PATH_LITERAL;
  proxy.target = dead_target;
  proxy.preflight = NULL;
  proxy.on_error = on_error;
  assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
  proxy.path = "/proxy/ws";
  assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
  vectis_metrics_config_init(&metrics);
  assert(app->metrics(app, &metrics, &error) == VECTIS_OK);
  assert(app->start(app, &error) == VECTIS_OK);
  assert(pthread_create(&origin.thread, NULL, origin_main, &origin) == 0);

  length = request_app(app_port,
                       "POST /proxy/preflight/deny HTTP/1.1\r\n"
                       "Host: localhost\r\nContent-Length: 100\r\n\r\nx",
                       response, sizeof(response) - 1u);
  boundary = strstr(response, "\r\n\r\n");
  assert(boundary != NULL);
  assert(strstr(response, "401 Unauthorized") != NULL ||
         strstr(response, "HTTP/1.1 401 ") != NULL);
  assert(strstr(response, "Set-Cookie: a=1\r\n") != NULL);
  assert(strstr(response, "Set-Cookie: b=2\r\n") != NULL);
  assert(strstr(response, "X-Local: preflight\r\n") != NULL);
  assert(strstr(response, "Content-Length: 4\r\n") != NULL);
  assert(length == (size_t)(boundary + 4u - response) + 4u);
  assert(memcmp(boundary + 4u, "deny", 4u) == 0);

  length = request_app(app_port,
                       "POST /proxy/preflight/deny HTTP/1.1\r\n"
                       "Host: localhost\r\nExpect: 100-continue\r\n"
                       "Content-Length: 100\r\n\r\n",
                       response, sizeof(response) - 1u);
  assert(length != 0u && strstr(response, "HTTP/1.1 401 ") == response);
  assert(strstr(response, "100 Continue") == NULL);

  length = request_app(app_port,
                       "GET /proxy/preflight/bad HTTP/1.1\r\n"
                       "Host: localhost\r\n\r\n",
                       response, sizeof(response) - 1u);
  assert(length != 0u && strstr(response, "HTTP/1.1 500 ") == response);

  length = request_app(app_port,
                       "HEAD /proxy/preflight/deny HTTP/1.1\r\n"
                       "Host: localhost\r\n\r\n",
                       response, sizeof(response) - 1u);
  boundary = strstr(response, "\r\n\r\n");
  assert(boundary != NULL && strstr(response, "Content-Length: 4\r\n") != NULL);
  assert(length == (size_t)(boundary + 4u - response));

  prior_2xx = metric_bucket(app, "2xx");
  length = request_app(app_port,
                       "GET /proxy/preflight/allow HTTP/1.1\r\n"
                       "Host: localhost\r\n\r\n",
                       response, sizeof(response) - 1u);
  assert(length != 0u && strstr(response, "200 ") != NULL);
  assert(strstr(response, "Transfer-Encoding: chunked\r\n") != NULL);
  assert(strstr(response, "\r\n\r\n2\r\nok\r\n0\r\n\r\n") != NULL);
  assert(metric_bucket(app, "2xx") == prior_2xx + 1u);

  prior_5xx = metric_bucket(app, "5xx");
  length = request_app(app_port,
                       "GET /proxy/error HTTP/1.1\r\n"
                       "Host: localhost\r\n\r\n",
                       response, sizeof(response) - 1u);
  assert(length != 0u && strstr(response, "503 ") != NULL);
  assert(strstr(response, "X-Local: gateway\r\n") != NULL);
  assert(strstr(response, "\r\n\r\nupstream down") != NULL);
  assert(metric_bucket(app, "5xx") == prior_5xx + 1u);

  length = request_app(app_port,
                       "GET /proxy/ws HTTP/1.1\r\nHost: localhost\r\n"
                       "Connection: Upgrade\r\nUpgrade: websocket\r\n"
                       "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                       "Sec-WebSocket-Version: 13\r\n\r\n",
                       response, sizeof(response) - 1u);
  assert(length != 0u && strstr(response, "503 ") != NULL);
  assert(strstr(response, "X-Local: gateway\r\n") != NULL);
  assert(strstr(response, "\r\n\r\nupstream down") != NULL);
  assert(metric_bucket(app, "5xx") == prior_5xx + 2u);

  assert(pthread_join(origin.thread, NULL) == 0);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  assert(close(origin.listener) == 0);
  return 0;
}
