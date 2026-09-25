#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vectis/proxy.h>
#include <vectis/vectis.h>

static const unsigned char mask_bytes[4] = {0x12u, 0x34u, 0x56u, 0x78u};
static const char request_head[] =
    "GET /ws HTTP/1.1\r\nHost: localhost\r\n"
    "Origin: https://public.example\r\n"
    "Connection: Upgrade\r\nUpgrade: websocket\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: chat\r\n\r\n";

static void send_all(int fd, const void *data, size_t length) {
  const unsigned char *bytes;
  ssize_t sent;

  bytes = (const unsigned char *)data;
  while (length != 0u) {
    sent = send(fd, bytes, length, 0);
    assert(sent > 0);
    bytes += (size_t)sent;
    length -= (size_t)sent;
  }
}

static void read_exact(int fd, unsigned char *data, size_t length) {
  ssize_t got;

  while (length != 0u) {
    got = recv(fd, data, length, 0);
    assert(got > 0);
    data += (size_t)got;
    length -= (size_t)got;
  }
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

static int connect_port(unsigned short port) {
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

static vectis_status rewrite_ws(const vectis_proxy_inbound *in,
                                vectis_proxy_outbound *out, void *userdata,
                                vectis_error *error) {
  (void)userdata;
  assert(vectis_proxy_inbound_websocket(in));
  assert(strcmp(vectis_proxy_inbound_host(in), "localhost") == 0);
  assert(strcmp(vectis_proxy_inbound_path(in), "/ws") == 0);
  assert(vectis_proxy_outbound_select_target(out, 1u, error) == VECTIS_OK);
  assert(vectis_proxy_outbound_set_path(out, "/rewritten", error) == VECTIS_OK);
  assert(vectis_proxy_outbound_set_query(out, "route=ws", error) == VECTIS_OK);
  assert(vectis_proxy_outbound_set_host(out, "public.example", error) ==
         VECTIS_OK);
  return vectis_proxy_outbound_add_header(out, "X-Director", "websocket",
                                          error);
}

int main(void) {
  vectis_proxy_route_config proxy;
  vectis_app_config config;
  vectis_error error;
  vectis_app *app;
  unsigned char response[2048];
  unsigned char frame[32768];
  unsigned char header[8];
  const char *boundary;
  const char *alternates[1];
  unsigned short go_port;
  unsigned short app_port;
  char address[64];
  char target[128];
  char alternate[128];
  pid_t go_child;
  size_t used;
  size_t index;
  size_t i;
  ssize_t got;
  int status;
  int fd;

  go_port = available_port();
  do {
    app_port = available_port();
  } while (app_port == go_port);
  assert(snprintf(address, sizeof(address), "127.0.0.1:%u", (unsigned)go_port) >
         0);
  go_child = fork();
  assert(go_child >= 0);
  if (go_child == 0) {
    execl(VECTIS_TEST_GO_WS_ORIGIN, VECTIS_TEST_GO_WS_ORIGIN, address,
          (char *)NULL);
    _exit(127);
  }
  fd = connect_port(go_port);
  assert(close(fd) == 0);

  assert(snprintf(target, sizeof(target), "http://127.0.0.1:%u/backend",
                  (unsigned)go_port) > 0);
  assert(snprintf(alternate, sizeof(alternate), "http://127.0.0.1:%u/alternate",
                  (unsigned)go_port) > 0);
  alternates[0] = alternate;
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = app_port;
  config.server.worker_count = 1u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  vectis_proxy_route_config_init(&proxy);
  proxy.path = "/ws";
  proxy.methods = VECTIS_HTTP_METHODS_GET;
  proxy.target = target;
  proxy.alternate_targets = alternates;
  proxy.alternate_target_count = 1u;
  proxy.rewrite = rewrite_ws;
  assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
  if (app->start(app, &error) != VECTIS_OK) {
    fprintf(stderr, "Go WebSocket proxy startup: %s\n", error.message);
    assert(0);
  }
  fd = connect_port(app_port);
  send_all(fd, request_head, sizeof(request_head) - 1u);
  header[0] = 0x81u;
  header[1] = 0x82u;
  memcpy(header + 2u, mask_bytes, sizeof(mask_bytes));
  header[6] = (unsigned char)('h' ^ mask_bytes[0]);
  header[7] = (unsigned char)('i' ^ mask_bytes[1]);
  send_all(fd, header, sizeof(header));
  used = 0u;
  do {
    got = recv(fd, response + used, sizeof(response) - used - 1u, 0);
    assert(got > 0);
    used += (size_t)got;
    response[used] = '\0';
    boundary = strstr((const char *)response, "\r\n\r\n");
  } while ((boundary == NULL ||
            used < (size_t)(boundary + 4u - (const char *)response) + 4u) &&
           used < sizeof(response) - 1u);
  assert(boundary != NULL);
  assert(strstr((const char *)response, "101 Switching Protocols") != NULL);
  assert(strstr((const char *)response, "Sec-WebSocket-Protocol: chat\r\n") !=
         NULL);
  assert(memcmp(boundary + 4u, "\x81\x02hi", 4u) == 0);
  assert(used == (size_t)(boundary + 4u - (const char *)response) + 4u);

  for (index = 0u; index < 32u; ++index) {
    header[0] = 0x82u;
    header[1] = 0xfeu;
    header[2] = 0x80u;
    header[3] = 0u;
    memcpy(header + 4u, mask_bytes, sizeof(mask_bytes));
    send_all(fd, header, sizeof(header));
    for (i = 0u; i < sizeof(frame); ++i)
      frame[i] =
          (unsigned char)(('A' + (int)(index % 26u)) ^ mask_bytes[i % 4u]);
    send_all(fd, frame, sizeof(frame));
    read_exact(fd, header, 4u);
    assert(memcmp(header, "\x82\x7e\x80\x00", 4u) == 0);
    read_exact(fd, frame, sizeof(frame));
    for (i = 0u; i < sizeof(frame); ++i)
      assert(frame[i] == (unsigned char)('A' + (int)(index % 26u)));
  }

  header[0] = 0x88u;
  header[1] = 0x82u;
  memcpy(header + 2u, mask_bytes, sizeof(mask_bytes));
  header[6] = (unsigned char)(0x03u ^ mask_bytes[0]);
  header[7] = (unsigned char)(0xe8u ^ mask_bytes[1]);
  send_all(fd, header, sizeof(header));
  read_exact(fd, header, 4u);
  assert(memcmp(header, "\x88\x02\x03\xe8", 4u) == 0);
  assert(close(fd) == 0);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  assert(kill(go_child, SIGTERM) == 0);
  assert(waitpid(go_child, &status, 0) == go_child);
  assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
  return 0;
}
