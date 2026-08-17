#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static void send_all(int fd, const unsigned char *data, size_t size) {
  size_t offset;
  ssize_t written;

  offset = 0u;
  while (offset < size) {
    written = send(fd, data + offset, size - offset, 0);
    assert(written > 0);
    offset += (size_t)written;
  }
}

static int connect_loopback(unsigned short port) {
  struct sockaddr_in addr;
  int fd;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  assert(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
  assert(connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0);
  return fd;
}

static ssize_t recv_some(int fd, char *buffer, size_t size) {
  struct timeval tv;
  fd_set rfds;
  int rc;

  FD_ZERO(&rfds);
  FD_SET(fd, &rfds);
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  rc = select(fd + 1, &rfds, NULL, NULL, &tv);
  assert(rc > 0);
  return recv(fd, buffer, size, 0);
}

static void recv_exact(int fd, unsigned char *buffer, size_t size) {
  size_t offset;
  ssize_t nread;

  offset = 0u;
  while (offset < size) {
    nread = recv(fd, buffer + offset, size - offset, 0);
    assert(nread > 0);
    offset += (size_t)nread;
  }
}

static void send_masked_text(int fd, const char *text) {
  unsigned char frame[256];
  static const unsigned char mask[4] = {0x31u, 0x32u, 0x33u, 0x34u};
  size_t len;
  size_t i;

  len = strlen(text);
  assert(len < 126u);
  frame[0] = 0x81u;
  frame[1] = (unsigned char)(0x80u | len);
  memcpy(frame + 2u, mask, sizeof(mask));
  for (i = 0u; i < len; ++i) {
    frame[6u + i] = ((const unsigned char *)text)[i] ^ mask[i % 4u];
  }
  send_all(fd, frame, 6u + len);
}

static void recv_text(int fd, char *out, size_t out_size) {
  unsigned char header[2];
  size_t len;

  recv_exact(fd, header, sizeof(header));
  assert((header[0] & 0x80u) == 0x80u);
  assert((header[0] & 0x0fu) == 0x01u);
  assert((header[1] & 0x80u) == 0u);
  len = (size_t)(header[1] & 0x7fu);
  assert(len < 126u);
  assert(len + 1u <= out_size);
  recv_exact(fd, (unsigned char *)out, len);
  out[len] = '\0';
}

int main(int argc, char **argv) {
  const char *path;
  const char *message;
  const char *expected;
  unsigned short port;
  char request[512];
  char response[2048];
  char reply[256];
  ssize_t nread;
  int fd;
  int written;

  if (argc != 5) {
    fprintf(stderr, "usage: vectis_ws_client PORT PATH MESSAGE EXPECTED\n");
    return 2;
  }
  port = (unsigned short)strtoul(argv[1], NULL, 10);
  path = argv[2];
  message = argv[3];
  expected = argv[4];
  fd = connect_loopback(port);
  written = snprintf(request, sizeof(request),
                     "GET %s HTTP/1.1\r\n"
                     "Host: localhost\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "\r\n",
                     path);
  assert(written > 0 && (size_t)written < sizeof(request));
  send_all(fd, (const unsigned char *)request, (size_t)written);
  memset(response, 0, sizeof(response));
  nread = recv_some(fd, response, sizeof(response) - 1u);
  assert(nread > 0);
  response[(size_t)nread] = '\0';
  assert(strstr(response, " 101 ") != NULL);
  send_masked_text(fd, message);
  recv_text(fd, reply, sizeof(reply));
  assert(strcmp(reply, expected) == 0);
  (void)shutdown(fd, SHUT_RDWR);
  (void)close(fd);
  return 0;
}
