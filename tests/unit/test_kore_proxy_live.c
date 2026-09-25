#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
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

struct origin_server {
  int listener;
  unsigned short port;
  pthread_t thread;
  volatile int allow_second;
  volatile int first_sent;
  volatile int third_sent;
  volatile int finish_third;
  volatile int upload_first_seen;
};

typedef struct proxy_log_capture {
  size_t length;
  char data[16384];
} proxy_log_capture;

static int proxy_log_write(void *userdata, const char *data, size_t length,
                           size_t *written) {
  static const char marker[] = "\"msg\":\"vectis.proxy.exchange\"";
  proxy_log_capture *capture;
  size_t i;

  capture = (proxy_log_capture *)userdata;
  for (i = 0u; i + sizeof(marker) - 1u <= length; ++i) {
    if (memcmp(data + i, marker, sizeof(marker) - 1u) == 0) {
      assert(length < sizeof(capture->data) - capture->length);
      memcpy(capture->data + capture->length, data, length);
      capture->length += length;
      capture->data[capture->length] = '\0';
      break;
    }
  }
  if (written != NULL)
    *written = length;
  return 0;
}

static void send_all(int fd, const char *data, size_t length) {
  ssize_t sent;

  while (length != 0u) {
    sent = send(fd, data, length, 0);
    assert(sent > 0);
    data += (size_t)sent;
    length -= (size_t)sent;
  }
}

static void *origin_main(void *userdata) {
  static const char first[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
      "Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n"
      "Transfer-Encoding: chunked\r\nTrailer: X-End\r\n"
      "Connection: close\r\n\r\n5\r\nhello\r\n";
  static const char second[] = "5\r\nworld\r\n0\r\nX-End: done\r\n\r\n";
  struct origin_server *server;
  struct timeval timeout;
  char request[2048];
  char big_chunk[4096];
  char *body_start;
  size_t body_seen;
  size_t used;
  ssize_t got;
  int fd;
  int spins;

  server = (struct origin_server *)userdata;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0);
  used = 0u;
  do {
    got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
  } while (strstr(request, "\r\n\r\n") == NULL && used < sizeof(request) - 1u);
  assert(strstr(request, "GET /alternate/changed/a%2Fb?x=1&x=2 HTTP/1.1\r\n") !=
         NULL);
  assert(strstr(request, "Host: public.example\r\n") != NULL ||
         strstr(request, "host: public.example\r\n") != NULL);
  assert(strstr(request, "X-Trace: rewritten\r\n") != NULL ||
         strstr(request, "x-trace: rewritten\r\n") != NULL);
  assert(strstr(request, "X-Director: yes\r\n") != NULL);
  assert(strstr(request, "Connection:") == NULL);
  send_all(fd, first, sizeof(first) - 1u);
  server->first_sent = 1;
  for (spins = 0; spins < 500 && !server->allow_second; ++spins)
    usleep(10000u);
  assert(server->allow_second);
  send_all(fd, second, sizeof(second) - 1u);
  assert(close(fd) == 0);
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  assert(close(fd) == 0);
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  used = 0u;
  do {
    got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
  } while (strstr(request, "\r\n\r\n") == NULL && used < sizeof(request) - 1u);
  send_all(fd, first, sizeof(first) - 1u);
  server->third_sent = 1;
  for (spins = 0; spins < 500 && !server->finish_third; ++spins)
    usleep(10000u);
  assert(server->finish_third);
  assert(close(fd) == 0);
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0);
  used = 0u;
  do {
    got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
  } while (strstr(request, "\r\n\r\nABCD") == NULL &&
           used < sizeof(request) - 1u);
  assert(strstr(request, "PUT /api/proxy/upload HTTP/1.1\r\n") != NULL);
  assert(strstr(request, "Content-Length: 8\r\n") != NULL ||
         strstr(request, "content-length: 8\r\n") != NULL);
  server->upload_first_seen = 1;
  send_all(fd,
           "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
           "Connection: close\r\n\r\n4\r\npong\r\n",
           strlen("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                  "Connection: close\r\n\r\n4\r\npong\r\n"));
  used = 0u;
  do {
    got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
  } while (strstr(request, "EFGH") == NULL && used < sizeof(request) - 1u);
  send_all(fd, "4\r\ndone\r\n0\r\n\r\n", strlen("4\r\ndone\r\n0\r\n\r\n"));
  assert(close(fd) == 0);
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0);
  used = 0u;
  do {
    got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
  } while (strstr(request, "X-Trace: yes\r\n\r\n") == NULL &&
           used < sizeof(request) - 1u);
  assert(strstr(request, "POST /api/proxy/chunked HTTP/1.1\r\n") != NULL);
  assert(strstr(request, "Transfer-Encoding: chunked\r\n") != NULL ||
         strstr(request, "transfer-encoding: chunked\r\n") != NULL);
  assert(strstr(request, "3\r\nabc\r\n0\r\nX-Trace: yes\r\n\r\n") != NULL);
  send_all(fd,
           "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n"
           "Connection: close\r\n\r\nok",
           strlen("HTTP/1.1 201 Created\r\nContent-Length: 2\r\n"
                  "Connection: close\r\n\r\nok"));
  assert(close(fd) == 0);
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0);
  used = 0u;
  do {
    got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
  } while (strstr(request, "\r\n\r\n") == NULL && used < sizeof(request) - 1u);
  assert(strstr(request, "POST /api/proxy/big HTTP/1.1\r\n") != NULL);
  assert(strstr(request, "Content-Length: 32768\r\n") != NULL ||
         strstr(request, "content-length: 32768\r\n") != NULL);
  body_start = strstr(request, "\r\n\r\n") + 4;
  body_seen = used - (size_t)(body_start - request);
  for (used = 0u; used < body_seen; ++used)
    assert(body_start[used] == 'A');
  while (body_seen < 32768u) {
    got = recv(fd, big_chunk, sizeof(big_chunk), 0);
    assert(got > 0);
    for (used = 0u; used < (size_t)got; ++used)
      assert(big_chunk[used] == 'A');
    body_seen += (size_t)got;
  }
  assert(body_seen == 32768u);
  send_all(fd,
           "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
           "Connection: close\r\n\r\nok",
           strlen("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                  "Connection: close\r\n\r\nok"));
  assert(close(fd) == 0);
  return NULL;
}

static void start_origin(struct origin_server *server) {
  struct sockaddr_in address;
  socklen_t length;

  memset(server, 0, sizeof(*server));
  server->listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(server->listener >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(server->listener, (struct sockaddr *)&address, sizeof(address)) ==
         0);
  assert(listen(server->listener, 1) == 0);
  length = sizeof(address);
  assert(getsockname(server->listener, (struct sockaddr *)&address, &length) ==
         0);
  server->port = ntohs(address.sin_port);
}

static unsigned short available_port(void) {
  struct sockaddr_in address;
  socklen_t length;
  unsigned short port;
  int fd;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
  length = sizeof(address);
  assert(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
  port = ntohs(address.sin_port);
  assert(close(fd) == 0);
  return port;
}

static int connect_app(unsigned short port) {
  struct sockaddr_in address;
  struct timeval timeout;
  int fd;
  int attempt;

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

static vectis_status plain(vectis_app *app, vectis_request *request,
                           vectis_response *response, void *userdata,
                           vectis_error *error) {
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 200, "text/plain", "plain", error);
}

static vectis_status rewrite_first(const vectis_proxy_inbound *in,
                                   vectis_proxy_outbound *out, void *userdata,
                                   vectis_error *error) {
  const char *name;
  const char *value;

  (void)userdata;
  if (strcmp(vectis_proxy_inbound_path(in), "/proxy/deny") == 0) {
    assert(vectis_proxy_outbound_set_path(out, "/../secret", error) ==
           VECTIS_ERR_INVALID);
    return VECTIS_OK;
  }
  if (strcmp(vectis_proxy_inbound_path(in), "/proxy/upload") == 0)
    return vectis_proxy_outbound_set_method(out, VECTIS_HTTP_PUT, error);
  if (strcmp(vectis_proxy_inbound_path(in), "/proxy/a%2Fb") != 0)
    return VECTIS_OK;
  assert(vectis_proxy_inbound_method(in) == VECTIS_HTTP_GET);
  assert(strcmp(vectis_proxy_inbound_query(in), "q=1&q=2") == 0);
  assert(strcmp(vectis_proxy_inbound_host(in), "localhost") == 0);
  assert(strcmp(vectis_proxy_inbound_path_param(in, "id"), "a%2Fb") == 0);
  assert(!vectis_proxy_inbound_websocket(in));
  assert(vectis_proxy_inbound_header_count(in) >= 2u);
  assert(vectis_proxy_inbound_header_at(in, 0u, &name, &value) == VECTIS_OK);
  assert(strcmp(name, "Host") == 0 && strcmp(value, "localhost") == 0);
  assert(vectis_proxy_outbound_select_target(out, 1u, error) == VECTIS_OK);
  assert(vectis_proxy_outbound_set_path(out, "/changed/a%2Fb", error) ==
         VECTIS_OK);
  assert(vectis_proxy_outbound_set_query(out, "x=1&x=2", error) == VECTIS_OK);
  assert(vectis_proxy_outbound_set_host(out, "public.example", error) ==
         VECTIS_OK);
  assert(vectis_proxy_outbound_set_header(out, "X-Trace", "rewritten", error) ==
         VECTIS_OK);
  return vectis_proxy_outbound_add_header(out, "X-Director", "yes", error);
}

static vectis_status modify_stream_response(vectis_proxy_response *response,
                                            void *userdata,
                                            vectis_error *error) {
  const char *name;
  const char *value;
  size_t i;

  (void)userdata;
  for (i = 0u; i < vectis_proxy_response_header_count(response); ++i) {
    assert(vectis_proxy_response_header_at(response, i, &name, &value) ==
           VECTIS_OK);
    if (strcmp(name, "Content-Type") == 0 &&
        strcmp(value, "text/event-stream") == 0) {
      assert(vectis_proxy_response_status(response) == 200);
      if (vectis_proxy_response_set_status(response, 202, error) != VECTIS_OK)
        return error->code;
      return vectis_proxy_response_add_header(response, "X-Modified", "yes",
                                              error);
    }
  }
  return VECTIS_OK;
}

int main(void) {
  struct origin_server origin;
  vectis_proxy_route_config proxy;
  vectis_app_config config;
  pslog_config log_config;
  pslog_logger *logger;
  proxy_log_capture *logs;
  vectis_route_config route;
  vectis_error error;
  vectis_app *app;
  unsigned short app_port;
  char target[128];
  char alternate[128];
  const char *alternates[1];
  char response[8192];
  char big_body[32768];
  size_t used;
  ssize_t got;
  int fd;

  start_origin(&origin);
  app_port = available_port();
  assert(snprintf(target, sizeof(target), "http://127.0.0.1:%u/api",
                  (unsigned)origin.port) > 0);
  assert(snprintf(alternate, sizeof(alternate), "http://127.0.0.1:%u/alternate",
                  (unsigned)origin.port) > 0);
  alternates[0] = alternate;
  logs = (proxy_log_capture *)mmap(NULL, sizeof(*logs), PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(logs != MAP_FAILED);
  pslog_default_config(&log_config);
  log_config.mode = PSLOG_MODE_JSON;
  log_config.output.write = proxy_log_write;
  log_config.output.close = NULL;
  log_config.output.isatty = NULL;
  log_config.output.userdata = logs;
  log_config.output.owned = 0;
  logger = pslog_new(&log_config);
  assert(logger != NULL);
  vectis_app_config_init(&config);
  config.logger = logger;
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = app_port;
  config.server.worker_count = 1u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  vectis_proxy_route_config_init(&proxy);
  proxy.path = "/proxy/:id";
  proxy.path_kind = VECTIS_ROUTE_PATH_PARAMS;
  proxy.methods = VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_POST;
  proxy.target = target;
  proxy.alternate_targets = alternates;
  proxy.alternate_target_count = 1u;
  proxy.rewrite = rewrite_first;
  proxy.modify_response = modify_stream_response;
  assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/plain", plain, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  if (app->start(app, &error) != VECTIS_OK) {
    fprintf(stderr, "proxy live startup: %s\n", error.message);
    assert(0);
  }
  assert(pthread_create(&origin.thread, NULL, origin_main, &origin) == 0);
  fd = connect_app(app_port);
  send_all(fd,
           "GET /plain HTTP/1.1\r\nHost: localhost\r\n"
           "Connection: close\r\n\r\n",
           strlen("GET /plain HTTP/1.1\r\nHost: localhost\r\n"
                  "Connection: close\r\n\r\n"));
  got = recv(fd, response, sizeof(response) - 1u, 0);
  assert(got > 0);
  response[got] = '\0';
  assert(strstr(response, "200 OK") != NULL);
  assert(close(fd) == 0);

  fd = connect_app(app_port);
  send_all(fd,
           "GET /proxy/ws HTTP/1.1\r\nHost: localhost\r\n"
           "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n",
           strlen("GET /proxy/ws HTTP/1.1\r\nHost: localhost\r\n"
                  "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n"));
  used = 0u;
  while (used < sizeof(response) - 1u) {
    got = recv(fd, response + used, sizeof(response) - used - 1u, 0);
    assert(got >= 0);
    if (got == 0)
      break;
    used += (size_t)got;
    response[used] = '\0';
  }
  assert(strstr(response, "400 Bad Request") != NULL);
  assert(close(fd) == 0);

  fd = connect_app(app_port);
  send_all(fd,
           "GET /proxy/deny HTTP/1.1\r\nHost: localhost\r\n"
           "Connection: close\r\n\r\n",
           strlen("GET /proxy/deny HTTP/1.1\r\nHost: localhost\r\n"
                  "Connection: close\r\n\r\n"));
  used = 0u;
  while (used < sizeof(response) - 1u) {
    got = recv(fd, response + used, sizeof(response) - used - 1u, 0);
    assert(got >= 0);
    if (got == 0)
      break;
    used += (size_t)got;
    response[used] = '\0';
  }
  assert(strstr(response, "400 Bad Request") != NULL);
  assert(close(fd) == 0);

  fd = connect_app(app_port);
  send_all(fd,
           "GET /proxy/a%2Fb?q=1&q=2 HTTP/1.1\r\nHost: localhost\r\n"
           "X-Trace: stream-test\r\nConnection: close\r\n\r\n",
           strlen("GET /proxy/a%2Fb?q=1&q=2 HTTP/1.1\r\n"
                  "Host: localhost\r\nX-Trace: stream-test\r\n"
                  "Connection: close\r\n\r\n"));
  used = 0u;
  response[0] = '\0';
  do {
    got = recv(fd, response + used, sizeof(response) - used - 1u, 0);
    if (got <= 0)
      fprintf(stderr, "proxy live recv=%ld errno=%d used=%lu head=%.512s\n",
              (long)got, errno, (unsigned long)used, response);
    assert(got > 0);
    used += (size_t)got;
    response[used] = '\0';
  } while (strstr(response, "hello\r\n") == NULL &&
           used < sizeof(response) - 1u);
  assert(origin.first_sent);
  assert(strstr(response, "HTTP/1.1 202 ") != NULL);
  assert(strstr(response, "X-Modified: yes\r\n") != NULL);
  assert(strstr(response, "Transfer-Encoding: chunked\r\n") != NULL);
  assert(strstr(response, "Set-Cookie: a=1\r\n") != NULL);
  assert(strstr(response, "Set-Cookie: b=2\r\n") != NULL);
  assert(strstr(response, "world") == NULL);
  origin.allow_second = 1;
  while (used < sizeof(response) - 1u) {
    got = recv(fd, response + used, sizeof(response) - used - 1u, 0);
    assert(got >= 0);
    if (got == 0)
      break;
    used += (size_t)got;
    response[used] = '\0';
  }
  assert(strstr(response, "hello\r\n5\r\nworld\r\n") != NULL);
  assert(strstr(response, "0\r\nX-End: done\r\n\r\n") != NULL);
  assert(close(fd) == 0);
  fd = connect_app(app_port);
  send_all(fd,
           "GET /proxy/unavailable HTTP/1.1\r\nHost: localhost\r\n"
           "Connection: close\r\n\r\n",
           strlen("GET /proxy/unavailable HTTP/1.1\r\nHost: localhost\r\n"
                  "Connection: close\r\n\r\n"));
  used = 0u;
  while (used < sizeof(response) - 1u) {
    got = recv(fd, response + used, sizeof(response) - used - 1u, 0);
    assert(got >= 0);
    if (got == 0)
      break;
    used += (size_t)got;
    response[used] = '\0';
  }
  assert(strstr(response, "HTTP/1.1 502 Bad Gateway\r\n") != NULL);
  assert(strstr(response, "bad gateway") != NULL);
  assert(close(fd) == 0);
  fd = connect_app(app_port);
  send_all(fd,
           "GET /proxy/abort HTTP/1.1\r\nHost: localhost\r\n"
           "Connection: close\r\n\r\n",
           strlen("GET /proxy/abort HTTP/1.1\r\nHost: localhost\r\n"
                  "Connection: close\r\n\r\n"));
  used = 0u;
  do {
    got = recv(fd, response + used, sizeof(response) - used - 1u, 0);
    assert(got > 0);
    used += (size_t)got;
    response[used] = '\0';
  } while (strstr(response, "hello\r\n") == NULL &&
           used < sizeof(response) - 1u);
  assert(origin.third_sent);
  send_all(fd, "unexpected", sizeof("unexpected") - 1u);
  got = recv(fd, response, sizeof(response), 0);
  assert(got == 0 || (got < 0 && errno == ECONNRESET));
  assert(close(fd) == 0);
  origin.finish_third = 1;
  fd = connect_app(app_port);
  send_all(fd,
           "POST /proxy/upload HTTP/1.1\r\nHost: localhost\r\n"
           "Content-Length: 8\r\nConnection: close\r\n\r\nABCD",
           strlen("POST /proxy/upload HTTP/1.1\r\nHost: localhost\r\n"
                  "Content-Length: 8\r\nConnection: close\r\n\r\nABCD"));
  used = 0u;
  do {
    got = recv(fd, response + used, sizeof(response) - used - 1u, 0);
    assert(got > 0);
    used += (size_t)got;
    response[used] = '\0';
  } while (strstr(response, "pong\r\n") == NULL &&
           used < sizeof(response) - 1u);
  assert(origin.upload_first_seen);
  assert(strstr(response, "HTTP/1.1 200 ") != NULL);
  send_all(fd, "EFGH", 4u);
  while (used < sizeof(response) - 1u) {
    got = recv(fd, response + used, sizeof(response) - used - 1u, 0);
    assert(got >= 0);
    if (got == 0)
      break;
    used += (size_t)got;
    response[used] = '\0';
  }
  assert(strstr(response, "pong\r\n4\r\ndone\r\n0\r\n\r\n") != NULL);
  assert(close(fd) == 0);
  fd = connect_app(app_port);
  send_all(fd,
           "POST /proxy/chunked HTTP/1.1\r\nHost: localhost\r\n"
           "Transfer-Encoding: chunked\r\nTrailer: X-Trace\r\n"
           "Connection: close\r\n\r\n3\r\nabc\r\n",
           strlen("POST /proxy/chunked HTTP/1.1\r\nHost: localhost\r\n"
                  "Transfer-Encoding: chunked\r\nTrailer: X-Trace\r\n"
                  "Connection: close\r\n\r\n3\r\nabc\r\n"));
  send_all(fd, "0\r\nX-Trace: yes\r\n\r\n",
           strlen("0\r\nX-Trace: yes\r\n\r\n"));
  used = 0u;
  while (used < sizeof(response) - 1u) {
    got = recv(fd, response + used, sizeof(response) - used - 1u, 0);
    assert(got >= 0);
    if (got == 0)
      break;
    used += (size_t)got;
    response[used] = '\0';
  }
  assert(strstr(response, "HTTP/1.1 201 ") != NULL);
  assert(strstr(response, "ok") != NULL);
  assert(close(fd) == 0);
  fd = connect_app(app_port);
  send_all(fd,
           "POST /proxy/big HTTP/1.1\r\nHost: localhost\r\n"
           "Content-Length: 32768\r\nConnection: close\r\n\r\n",
           strlen("POST /proxy/big HTTP/1.1\r\nHost: localhost\r\n"
                  "Content-Length: 32768\r\nConnection: close\r\n\r\n"));
  memset(big_body, 'A', sizeof(big_body));
  send_all(fd, big_body, sizeof(big_body));
  used = 0u;
  while (used < sizeof(response) - 1u) {
    got = recv(fd, response + used, sizeof(response) - used - 1u, 0);
    assert(got >= 0);
    if (got == 0)
      break;
    used += (size_t)got;
    response[used] = '\0';
  }
  assert(strstr(response, "HTTP/1.1 200 ") != NULL);
  assert(strstr(response, "ok") != NULL);
  assert(close(fd) == 0);
  assert(pthread_join(origin.thread, NULL) == 0);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  assert(strstr(logs->data, "\"msg\":\"vectis.proxy.exchange\"") != NULL);
  assert(strstr(logs->data, "\"route\":\"/proxy/:id\"") != NULL);
  assert(strstr(logs->data, "\"status\":202") != NULL);
  assert(strstr(logs->data, "\"downstream_bytes\":10") != NULL);
  assert(strstr(logs->data, "\"upstream_bytes\":32768,") != NULL);
  assert(strstr(logs->data, "\"upstream_bytes\":3,") != NULL);
  assert(strstr(logs->data, "q=1") == NULL);
  assert(strstr(logs->data, "hello") == NULL);
  logger->destroy(logger);
  assert(munmap(logs, sizeof(*logs)) == 0);
  assert(close(origin.listener) == 0);
  return 0;
}
