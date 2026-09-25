#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
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

static const unsigned char early_client_frame[] = {
    0x81u, 0x82u, 0x11u, 0x22u, 0x33u, 0x44u, 'h' ^ 0x11, 'i' ^ 0x22};
static const unsigned char later_client_frame[] = {
    0x81u, 0x82u, 0x55u, 0x66u, 0x77u, 0x88u, 'b' ^ 0x55, 'y' ^ 0x66};
static const unsigned char early_server_frame[] = {0x81u, 0x02u, 'o', 'k'};
static const unsigned char later_server_frame[] = {0x81u, 0x03u, 'b', 'y', 'e'};
#define BIG_PAYLOAD_SIZE (1024u * 1024u)
static const unsigned char big_client_header[] = {0x82u, 0xffu, 0u,    0u,   0u,
                                                  0u,    0u,    0x10u, 0u,   0u,
                                                  0x12u, 0x34u, 0x56u, 0x78u};
static const unsigned char big_server_header[] = {0x82u, 0x7fu, 0u,    0u, 0u,
                                                  0u,    0u,    0x10u, 0u, 0u};
static const unsigned char big_mask[] = {0x12u, 0x34u, 0x56u, 0x78u};
static const char client_head[] =
    "GET /ws?q=1&q=2 HTTP/1.1\r\nHost: localhost\r\n"
    "Connection: Upgrade\r\nUpgrade: websocket\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: chat\r\n\r\n";
static const char upstream_head[] =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Connection: Upgrade\r\nUpgrade: websocket\r\n"
    "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
    "Sec-WebSocket-Protocol: chat\r\n\r\n";

typedef struct origin_server {
  int listener;
  unsigned short port;
  pthread_t thread;
} origin_server;

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

static void *origin_main(void *userdata) {
  origin_server *origin;
  struct timeval timeout;
  unsigned char request[2048];
  unsigned char frame[sizeof(early_client_frame)];
  unsigned char big_header[sizeof(big_client_header)];
  unsigned char big_chunk[4096];
  unsigned char
      response[sizeof(upstream_head) - 1u + sizeof(early_server_frame)];
  size_t used;
  size_t offset;
  size_t i;
  ssize_t got;
  int fd;

  origin = (origin_server *)userdata;
  fd = accept(origin->listener, NULL, NULL);
  assert(fd >= 0);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0);
  used = 0u;
  for (;;) {
    got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
    if (strstr((const char *)request, "\r\n\r\n") != NULL)
      break;
    assert(used < sizeof(request) - 1u);
  }
  assert(strstr((const char *)request,
                "GET /backend/ws?q=1&q=2 HTTP/1.1\r\n") != NULL);
  assert(strstr((const char *)request, "Sec-WebSocket-Key: ") != NULL ||
         strstr((const char *)request, "sec-websocket-key: ") != NULL);
  assert(used == strlen((const char *)request));
  memcpy(response, upstream_head, sizeof(upstream_head) - 1u);
  memcpy(response + sizeof(upstream_head) - 1u, early_server_frame,
         sizeof(early_server_frame));
  send_all(fd, response, sizeof(response));
  read_exact(fd, frame, sizeof(frame));
  assert(memcmp(frame, early_client_frame, sizeof(frame)) == 0);
  read_exact(fd, frame, sizeof(frame));
  assert(memcmp(frame, later_client_frame, sizeof(frame)) == 0);
  send_all(fd, later_server_frame, sizeof(later_server_frame));
  read_exact(fd, big_header, sizeof(big_header));
  assert(memcmp(big_header, big_client_header, sizeof(big_header)) == 0);
  for (offset = 0u; offset < BIG_PAYLOAD_SIZE; offset += sizeof(big_chunk)) {
    read_exact(fd, big_chunk, sizeof(big_chunk));
    for (i = 0u; i < sizeof(big_chunk); ++i)
      assert(big_chunk[i] ==
             (unsigned char)('A' ^ big_mask[(offset + i) % 4u]));
  }
  send_all(fd, big_server_header, sizeof(big_server_header));
  memset(big_chunk, 'B', sizeof(big_chunk));
  for (offset = 0u; offset < BIG_PAYLOAD_SIZE; offset += sizeof(big_chunk))
    send_all(fd, big_chunk, sizeof(big_chunk));
  assert(close(fd) == 0);
  return NULL;
}

static void listen_origin(origin_server *origin) {
  struct sockaddr_in address;
  socklen_t length;

  memset(origin, 0, sizeof(*origin));
  origin->listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(origin->listener >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(origin->listener, (struct sockaddr *)&address, sizeof(address)) ==
         0);
  assert(listen(origin->listener, 1) == 0);
  length = sizeof(address);
  assert(getsockname(origin->listener, (struct sockaddr *)&address, &length) ==
         0);
  origin->port = ntohs(address.sin_port);
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

int main(void) {
  origin_server origin;
  vectis_proxy_route_config proxy;
  vectis_app_config config;
  vectis_error error;
  vectis_app *app;
  unsigned char response[2048];
  unsigned char big_chunk[4096];
  unsigned char big_head[sizeof(big_server_header)];
  const char *boundary;
  const unsigned char *frame;
  unsigned short app_port;
  char target[128];
  size_t used;
  size_t offset;
  size_t i;
  ssize_t got;
  int fd;

  listen_origin(&origin);
  app_port = available_port();
  assert(snprintf(target, sizeof(target), "http://127.0.0.1:%u/backend",
                  (unsigned)origin.port) > 0);
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
  assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
  if (app->start(app, &error) != VECTIS_OK) {
    fprintf(stderr, "WebSocket proxy startup: %s\n", error.message);
    assert(0);
  }
  assert(pthread_create(&origin.thread, NULL, origin_main, &origin) == 0);
  fd = connect_app(app_port);
  send_all(fd,
           "GET /ws HTTP/1.1\r\nHost: localhost\r\n"
           "Connection: Upgrade\r\nUpgrade: websocket\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Sec-WebSocket-Version: 13\r\nContent-Length: 0\r\n\r\n",
           strlen("GET /ws HTTP/1.1\r\nHost: localhost\r\n"
                  "Connection: Upgrade\r\nUpgrade: websocket\r\n"
                  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                  "Sec-WebSocket-Version: 13\r\nContent-Length: 0\r\n\r\n"));
  got = recv(fd, response, sizeof(response) - 1u, 0);
  assert(got > 0);
  response[got] = '\0';
  assert(strstr((const char *)response, "400 Bad Request") != NULL);
  assert(close(fd) == 0);
  fd = connect_app(app_port);
  send_all(fd, client_head, sizeof(client_head) - 1u);
  send_all(fd, early_client_frame, sizeof(early_client_frame));
  used = 0u;
  do {
    got = recv(fd, response + used, sizeof(response) - used - 1u, 0);
    assert(got > 0);
    used += (size_t)got;
    response[used] = '\0';
    boundary = strstr((const char *)response, "\r\n\r\n");
  } while ((boundary == NULL ||
            used < (size_t)(boundary + 4u - (const char *)response) +
                       sizeof(early_server_frame)) &&
           used < sizeof(response));
  assert(boundary != NULL);
  assert(strstr((const char *)response, "101 Switching Protocols") != NULL);
  assert(strstr((const char *)response, "Sec-WebSocket-Protocol: chat\r\n") !=
         NULL);
  frame = (const unsigned char *)boundary + 4u;
  assert(memcmp(frame, early_server_frame, sizeof(early_server_frame)) == 0);
  send_all(fd, later_client_frame, sizeof(later_client_frame));
  read_exact(fd, response, sizeof(later_server_frame));
  assert(memcmp(response, later_server_frame, sizeof(later_server_frame)) == 0);
  send_all(fd, big_client_header, sizeof(big_client_header));
  for (offset = 0u; offset < BIG_PAYLOAD_SIZE; offset += sizeof(big_chunk)) {
    for (i = 0u; i < sizeof(big_chunk); ++i)
      big_chunk[i] = (unsigned char)('A' ^ big_mask[(offset + i) % 4u]);
    send_all(fd, big_chunk, sizeof(big_chunk));
  }
  read_exact(fd, big_head, sizeof(big_head));
  assert(memcmp(big_head, big_server_header, sizeof(big_head)) == 0);
  for (offset = 0u; offset < BIG_PAYLOAD_SIZE; offset += sizeof(big_chunk)) {
    read_exact(fd, big_chunk, sizeof(big_chunk));
    for (i = 0u; i < sizeof(big_chunk); ++i)
      assert(big_chunk[i] == 'B');
  }
  assert(close(fd) == 0);
  assert(pthread_join(origin.thread, NULL) == 0);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  assert(close(origin.listener) == 0);
  return 0;
}
