#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t publisher = -1;
static void cleanup(void) {
  if (publisher > 0) {
    (void)kill(publisher, SIGTERM);
    while (waitpid(publisher, NULL, 0) < 0 && errno == EINTR) {
    }
  }
}
static void check(int value, int line) {
  if (!value) {
    fprintf(stderr, "MQTT wire failure at line %d: %s\n", line,
            strerror(errno));
    exit(1);
  }
}
#define CHECK(value) check(!!(value), __LINE__)
static void exact(int fd, void *buffer, size_t size) {
  size_t used = 0;
  ssize_t got;
  while (used < size) {
    got = read(fd, (char *)buffer + used, size - used);
    if (got < 0 && errno == EINTR)
      continue;
    CHECK(got > 0);
    used += (size_t)got;
  }
}
static size_t packet(int fd, unsigned char kind, unsigned char *body,
                     size_t cap) {
  unsigned char byte;
  size_t size = 0, factor = 1;
  unsigned i;
  exact(fd, &byte, 1);
  CHECK(byte == kind);
  for (i = 0; i < 4; ++i) {
    exact(fd, &byte, 1);
    size += (size_t)(byte & 127u) * factor;
    if (!(byte & 128u))
      break;
    factor *= 128u;
  }
  CHECK(i < 4 && size <= cap);
  exact(fd, body, size);
  return size;
}
int main(int argc, char **argv) {
  static const char *short_payloads[] = {"body", "payload", "message",
                                         "binary\0tail", ""};
  static const size_t sizes[] = {4, 7, 7, 11, 0, 16384};
  struct sockaddr_in address;
  struct timeval timeout;
  struct pollfd ready;
  socklen_t address_size = sizeof(address);
  unsigned char body[18432];
  char port[16];
  int listener, connection, status;
  size_t length, topic, i, j;
  CHECK(argc == 3);
  CHECK(atexit(cleanup) == 0);
  (void)signal(SIGPIPE, SIG_IGN);
  listener = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(listener >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
  CHECK(getsockname(listener, (struct sockaddr *)&address, &address_size) == 0);
  CHECK(listen(listener, 1) == 0);
  snprintf(port, sizeof(port), "%u", (unsigned)ntohs(address.sin_port));
  publisher = fork();
  CHECK(publisher >= 0);
  if (!publisher) {
    close(listener);
    execl(argv[1], argv[1], argv[2], port, (char *)NULL);
    _exit(127);
  }
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  for (i = 0; i < 6; ++i) {
    ready.fd = listener;
    ready.events = POLLIN;
    ready.revents = 0;
    CHECK(poll(&ready, 1, 5000) > 0);
    connection = accept(listener, NULL, NULL);
    CHECK(connection >= 0);
    CHECK(setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout)) == 0);
    CHECK(setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout)) == 0);
    length = packet(connection, 0x10u, body, sizeof(body));
    CHECK(length >= 7 && !memcmp(body, "\0\4MQTT\4", 7));
    CHECK(write(connection, "\x20\x02\0\0", 4) == 4);
    length = packet(connection, 0x30u, body, sizeof(body));
    CHECK(length >= 2);
    topic = (size_t)body[0] * 256u + body[1];
    CHECK(topic == 11 && length == 2 + topic + sizes[i]);
    CHECK(!memcmp(body + 2, "vectis/wire", topic));
    if (i < 5)
      CHECK(!memcmp(body + 2 + topic, short_payloads[i], sizes[i]));
    else
      for (j = 0; j < sizes[i]; ++j)
        CHECK(body[2 + topic + j] == 'x');
    CHECK(packet(connection, 0xe0u, body, sizeof(body)) == 0);
    close(connection);
  }
  close(listener);
  CHECK(waitpid(publisher, &status, 0) == publisher);
  publisher = -1;
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  puts("MQTT wire regression passed: six payloads, PUBLISH and DISCONNECT");
  return 0;
}
