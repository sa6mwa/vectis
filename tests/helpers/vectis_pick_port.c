#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int main(void) {
  struct sockaddr_in addr;
  socklen_t addr_len;
  int fd;
  int opt;
  unsigned short port;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return 1;
  }

  opt = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    perror("bind");
    (void)close(fd);
    return 1;
  }

  addr_len = (socklen_t)sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) != 0) {
    perror("getsockname");
    (void)close(fd);
    return 1;
  }

  port = ntohs(addr.sin_port);
  (void)close(fd);
  if (port == 0u) {
    fputs("failed to allocate TCP port\n", stderr);
    return 1;
  }

  printf("%u\n", (unsigned)port);
  return 0;
}
