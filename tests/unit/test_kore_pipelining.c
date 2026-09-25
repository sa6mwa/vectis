#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <vectis/vectis.h>

static vectis_status reply(vectis_app *app, vectis_request *request,
                           vectis_response *response, void *userdata,
                           vectis_error *error) {
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 200, "text/plain", "ok", error);
}

static unsigned short available_port(void) {
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

static int connect_local(unsigned short port) {
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

static unsigned response_count(const char *data) {
  unsigned count;
  const char *p;

  count = 0;
  p = data;
  while ((p = strstr(p, "HTTP/1.1 200")) != NULL) {
    count++;
    p++;
  }
  return count;
}

static int check_pipeline(unsigned short port, const char *wire,
                          const char *label) {
  struct pollfd watch;
  char output[4096];
  size_t used;
  ssize_t got;
  int fd;
  unsigned count;

  fd = connect_local(port);
  assert(send(fd, wire, strlen(wire), 0) == (ssize_t)strlen(wire));
  watch.fd = fd;
  watch.events = POLLIN;
  used = 0;
  output[0] = '\0';
  while (used < sizeof(output) - 1 && response_count(output) < 2) {
    if (poll(&watch, 1, 1000) <= 0)
      break;
    got = recv(fd, output + used, sizeof(output) - 1 - used, 0);
    if (got <= 0)
      break;
    used += (size_t)got;
    output[used] = '\0';
  }
  count = response_count(output);
  fprintf(stderr, "%s responses: %u\n", label, count);
  assert(close(fd) == 0);
  return count == 2;
}

int main(void) {
  static const char two_gets[] = "GET /one HTTP/1.1\r\nHost: localhost\r\n\r\n"
                                 "GET /two HTTP/1.1\r\nHost: localhost\r\n\r\n";
  static const char post_then_get[] =
      "POST /post HTTP/1.1\r\nHost: localhost\r\n"
      "Content-Length: 4\r\n\r\ndata"
      "GET /two HTTP/1.1\r\nHost: localhost\r\n\r\n";
  static const char zero_then_get[] =
      "POST /post HTTP/1.1\r\nHost: localhost\r\n"
      "Content-Length: 0\r\n\r\n"
      "GET /two HTTP/1.1\r\nHost: localhost\r\n\r\n";
  vectis_app_config config;
  vectis_route_config route;
  vectis_error error;
  vectis_app *app;
  unsigned short port;
  int passed;

  port = available_port();
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = port;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/one", reply, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/two", reply, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_POST, "/post", reply, NULL);
  route.body = vectis_body_buffered_max(16u);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  assert(app->start(app, &error) == VECTIS_OK);

  passed = check_pipeline(port, two_gets, "GET + GET");
  passed &= check_pipeline(port, post_then_get, "POST body + GET");
  passed &= check_pipeline(port, zero_then_get, "POST zero + GET");
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  return passed ? 0 : 1;
}
