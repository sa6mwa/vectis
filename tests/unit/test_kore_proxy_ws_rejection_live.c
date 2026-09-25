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

#define LARGE_BODY (1024u * 1024u)

typedef struct origin_server {
  int listener;
  int resume_pipe[2];
  unsigned short port;
  pthread_t thread;
} origin_server;

static const char client_head[] =
    "GET /ws HTTP/1.1\r\nHost: localhost\r\n"
    "Connection: Upgrade\r\nUpgrade: websocket\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n\r\n";

static void send_all(int fd, const void *data, size_t length) {
  const char *bytes;
  ssize_t sent;

  bytes = (const char *)data;
  while (length != 0u) {
    sent = send(fd, bytes, length, 0);
    assert(sent > 0);
    bytes += (size_t)sent;
    length -= (size_t)sent;
  }
}

static void read_request(int fd) {
  char request[2048];
  size_t used;
  ssize_t got;

  used = 0u;
  for (;;) {
    got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
    if (strstr(request, "\r\n\r\n") != NULL)
      break;
    assert(used < sizeof(request) - 1u);
  }
  assert(strstr(request, "GET /backend/ws HTTP/1.1\r\n") != NULL);
}

static void *origin_main(void *userdata) {
  origin_server *origin;
  struct timeval timeout;
  char chunk[4096];
  char resume;
  size_t sent;
  int fd;
  int sequence;

  origin = (origin_server *)userdata;
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  for (sequence = 0; sequence < 3; ++sequence) {
    fd = accept(origin->listener, NULL, NULL);
    assert(fd >= 0);
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
           0);
    read_request(fd);
    if (sequence == 0) {
      send_all(fd,
               "HTTP/1.1 403 Forbidden\r\nContent-Length: 1048576\r\n"
               "Connection: close\r\nX-Reason: denied\r\n\r\nA",
               strlen("HTTP/1.1 403 Forbidden\r\nContent-Length: 1048576\r\n"
                      "Connection: close\r\nX-Reason: denied\r\n\r\nA"));
      assert(read(origin->resume_pipe[0], &resume, 1u) == 1);
      memset(chunk, 'A', sizeof(chunk));
      for (sent = 1u; sent < LARGE_BODY; sent += sizeof(chunk)) {
        size_t length;

        length = LARGE_BODY - sent;
        if (length > sizeof(chunk))
          length = sizeof(chunk);
        send_all(fd, chunk, length);
      }
    } else if (sequence == 1) {
      send_all(fd,
               "HTTP/1.1 429 Too Many Requests\r\n"
               "Transfer-Encoding: chunked\r\nTrailer: Digest\r\n"
               "Connection: close\r\n\r\n5\r\nhello\r\n"
               "0\r\nDigest: sha-256=abc\r\n\r\n",
               strlen("HTTP/1.1 429 Too Many Requests\r\n"
                      "Transfer-Encoding: chunked\r\nTrailer: Digest\r\n"
                      "Connection: close\r\n\r\n5\r\nhello\r\n"
                      "0\r\nDigest: sha-256=abc\r\n\r\n"));
    } else {
      send_all(fd,
               "HTTP/1.1 103 Early Hints\r\nLink: </next>; rel=preload\r\n"
               "\r\nHTTP/1.1 503 Service Unavailable\r\n"
               "Connection: close\r\n\r\nslow",
               strlen("HTTP/1.1 103 Early Hints\r\n"
                      "Link: </next>; rel=preload\r\n\r\n"
                      "HTTP/1.1 503 Service Unavailable\r\n"
                      "Connection: close\r\n\r\nslow"));
    }
    assert(close(fd) == 0);
  }
  return NULL;
}

static unsigned short listen_local(int *listener) {
  struct sockaddr_in address;
  socklen_t length;
  unsigned short port;

  *listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(*listener >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(*listener, (struct sockaddr *)&address, sizeof(address)) == 0);
  assert(listen(*listener, 3) == 0);
  length = sizeof(address);
  assert(getsockname(*listener, (struct sockaddr *)&address, &length) == 0);
  port = ntohs(address.sin_port);
  return port;
}

static unsigned short available_port(void) {
  int listener;
  unsigned short port;

  port = listen_local(&listener);
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
  send_all(fd, client_head, sizeof(client_head) - 1u);
  return fd;
}

static size_t read_head(int fd, char *buffer, size_t capacity) {
  size_t used;
  ssize_t got;

  used = 0u;
  do {
    got = recv(fd, buffer + used, capacity - used - 1u, 0);
    assert(got > 0);
    used += (size_t)got;
    buffer[used] = '\0';
    assert(used < capacity - 1u);
  } while (strstr(buffer, "\r\n\r\n") == NULL);
  return used;
}

static void test_fixed(unsigned short port, int resume_pipe) {
  char head[2048];
  char body[4096];
  char *boundary;
  size_t used;
  size_t received;
  size_t i;
  ssize_t got;
  int fd;

  fd = connect_app(port);
  used = read_head(fd, head, sizeof(head));
  boundary = strstr(head, "\r\n\r\n");
  assert(boundary != NULL);
  assert(strstr(head, "HTTP/1.1 403 ") == head);
  assert(strstr(head, "Content-Length: 1048576\r\n") != NULL);
  assert(strstr(head, "X-Reason: denied\r\n") != NULL);
  received = used - (size_t)(boundary + 4u - head);
  if (received == 0u) {
    got = recv(fd, body, 1u, 0);
    assert(got == 1);
    received = 1u;
  } else {
    assert(received == 1u && boundary[4] == 'A');
  }
  assert(write(resume_pipe, "x", 1u) == 1);
  while (received < LARGE_BODY) {
    got = recv(fd, body, sizeof(body), 0);
    assert(got > 0);
    for (i = 0u; i < (size_t)got; ++i)
      assert(body[i] == 'A');
    received += (size_t)got;
  }
  assert(received == LARGE_BODY);
  assert(recv(fd, body, sizeof(body), 0) == 0);
  assert(close(fd) == 0);
}

static void test_small(unsigned short port, int sequence) {
  char response[4096];
  size_t used;
  ssize_t got;
  int fd;

  fd = connect_app(port);
  used = 0u;
  for (;;) {
    got = recv(fd, response + used, sizeof(response) - used - 1u, 0);
    assert(got >= 0);
    if (got == 0)
      break;
    used += (size_t)got;
    assert(used < sizeof(response) - 1u);
  }
  response[used] = '\0';
  if (sequence == 1) {
    assert(strstr(response, "HTTP/1.1 429 ") == response);
    assert(strstr(response, "Trailer: Digest\r\n") != NULL);
    assert(strstr(response, "5\r\nhello\r\n") != NULL);
    assert(strstr(response, "0\r\nDigest: sha-256=abc\r\n\r\n") != NULL);
  } else {
    assert(strstr(response, "HTTP/1.1 103 ") == response);
    assert(strstr(response, "Link: </next>; rel=preload\r\n") != NULL);
    assert(strstr(response, "HTTP/1.1 503 ") != NULL);
    assert(strstr(response, "4\r\nslow\r\n0\r\n\r\n") != NULL);
  }
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

  origin.port = listen_local(&origin.listener);
  assert(pipe(origin.resume_pipe) == 0);
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
  assert(app->start(app, &error) == VECTIS_OK);
  assert(pthread_create(&origin.thread, NULL, origin_main, &origin) == 0);
  test_fixed(app_port, origin.resume_pipe[1]);
  test_small(app_port, 1);
  test_small(app_port, 2);
  assert(pthread_join(origin.thread, NULL) == 0);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  assert(close(origin.resume_pipe[0]) == 0);
  assert(close(origin.resume_pipe[1]) == 0);
  assert(close(origin.listener) == 0);
  return 0;
}
