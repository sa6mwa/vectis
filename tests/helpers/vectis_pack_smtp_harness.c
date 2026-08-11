#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct smtp_server {
  int listen_fd;
  unsigned short port;
  const char *mailbox_path;
  pthread_t thread;
  int started;
  volatile int stop;
  int failed;
  char data[8192];
  size_t data_size;
} smtp_server;

static int send_all(int fd, const char *text) {
  size_t len;
  size_t sent;
  ssize_t n;

  len = strlen(text);
  sent = 0u;
  while (sent < len) {
    n = send(fd, text + sent, len - sent, 0);
    if (n <= 0) {
      return 0;
    }
    sent += (size_t)n;
  }
  return 1;
}

static int read_line(int fd, char *line, size_t line_size) {
  size_t used;
  char ch;
  ssize_t n;

  used = 0u;
  while (used + 1u < line_size) {
    n = recv(fd, &ch, 1u, 0);
    if (n <= 0) {
      return 0;
    }
    line[used++] = ch;
    if (ch == '\n') {
      break;
    }
  }
  line[used] = '\0';
  return used > 0u;
}

static void capture_line(smtp_server *server, const char *line) {
  size_t len;
  size_t copy;

  len = strlen(line);
  if (server->data_size >= sizeof(server->data) - 1u) {
    return;
  }
  copy = len;
  if (copy > sizeof(server->data) - 1u - server->data_size) {
    copy = sizeof(server->data) - 1u - server->data_size;
  }
  memcpy(server->data + server->data_size, line, copy);
  server->data_size += copy;
  server->data[server->data_size] = '\0';
}

static int write_mailbox(smtp_server *server) {
  FILE *file;

  file = fopen(server->mailbox_path, "wb");
  if (file == NULL) {
    return 0;
  }
  if (server->data_size > 0u &&
      fwrite(server->data, 1u, server->data_size, file) != server->data_size) {
    (void)fclose(file);
    return 0;
  }
  return fclose(file) == 0;
}

static void smtp_handle_client(smtp_server *server, int client_fd) {
  char line[1024];
  int in_data;

  in_data = 0;
  if (!send_all(client_fd, "220 vectis smtp mock\r\n")) {
    server->failed = 1;
    (void)close(client_fd);
    return;
  }
  while (read_line(client_fd, line, sizeof(line))) {
    if (in_data) {
      if (strcmp(line, ".\r\n") == 0 || strcmp(line, ".\n") == 0) {
        in_data = 0;
        if (!write_mailbox(server) || !send_all(client_fd, "250 queued\r\n")) {
          server->failed = 1;
          break;
        }
      } else {
        capture_line(server, line);
      }
    } else if (strncmp(line, "EHLO", 4u) == 0 ||
               strncmp(line, "HELO", 4u) == 0) {
      if (!send_all(client_fd, "250-localhost\r\n250 OK\r\n")) {
        server->failed = 1;
        break;
      }
    } else if (strncmp(line, "MAIL FROM:", 10u) == 0 ||
               strncmp(line, "RCPT TO:", 8u) == 0) {
      if (!send_all(client_fd, "250 OK\r\n")) {
        server->failed = 1;
        break;
      }
    } else if (strncmp(line, "DATA", 4u) == 0) {
      in_data = 1;
      if (!send_all(client_fd, "354 end with dot\r\n")) {
        server->failed = 1;
        break;
      }
    } else if (strncmp(line, "QUIT", 4u) == 0) {
      (void)send_all(client_fd, "221 bye\r\n");
      break;
    } else if (!send_all(client_fd, "250 OK\r\n")) {
      server->failed = 1;
      break;
    }
  }
  (void)close(client_fd);
}

static void *smtp_thread(void *userdata) {
  smtp_server *server;
  struct sockaddr_in peer;
  socklen_t peer_len;
  fd_set read_set;
  struct timeval timeout;
  int client_fd;
  int select_result;

  server = (smtp_server *)userdata;
  while (!server->stop && !server->failed) {
    FD_ZERO(&read_set);
    FD_SET(server->listen_fd, &read_set);
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    select_result =
        select(server->listen_fd + 1, &read_set, NULL, NULL, &timeout);
    if (select_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      server->failed = 1;
      break;
    }
    if (select_result == 0) {
      continue;
    }

    peer_len = (socklen_t)sizeof(peer);
    client_fd = accept(server->listen_fd, (struct sockaddr *)&peer, &peer_len);
    if (client_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      server->failed = 1;
      break;
    }
    smtp_handle_client(server, client_fd);
  }
  return NULL;
}

static int smtp_start(smtp_server *server, const char *mailbox_path) {
  struct sockaddr_in addr;
  socklen_t addr_len;
  int enabled;
  int flags;

  memset(server, 0, sizeof(*server));
  server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server->listen_fd < 0) {
    perror("socket");
    return 0;
  }
  flags = fcntl(server->listen_fd, F_GETFD, 0);
  if (flags >= 0) {
    (void)fcntl(server->listen_fd, F_SETFD, flags | FD_CLOEXEC);
  }
  enabled = 1;
  (void)setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                   (socklen_t)sizeof(enabled));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(server->listen_fd, 1) != 0) {
    perror("bind/listen");
    (void)close(server->listen_fd);
    server->listen_fd = -1;
    return 0;
  }
  addr_len = (socklen_t)sizeof(addr);
  if (getsockname(server->listen_fd, (struct sockaddr *)&addr, &addr_len) !=
      0) {
    perror("getsockname");
    (void)close(server->listen_fd);
    server->listen_fd = -1;
    return 0;
  }
  server->port = ntohs(addr.sin_port);
  server->mailbox_path = mailbox_path;
  if (pthread_create(&server->thread, NULL, smtp_thread, server) != 0) {
    perror("pthread_create");
    (void)close(server->listen_fd);
    server->listen_fd = -1;
    return 0;
  }
  server->started = 1;
  return 1;
}

static void smtp_stop(smtp_server *server) {
  if (server->started) {
    server->stop = 1;
    (void)pthread_join(server->thread, NULL);
    server->started = 0;
  }
  if (server->listen_fd >= 0) {
    (void)close(server->listen_fd);
    server->listen_fd = -1;
  }
}

int main(int argc, char **argv) {
  smtp_server server;
  char smtp_url[128];
  pid_t pid;
  int status;

  if (argc != 3) {
    fprintf(stderr, "usage: %s <packed-executable> <mailbox-path>\n", argv[0]);
    return 2;
  }
  if (!smtp_start(&server, argv[2])) {
    return 1;
  }
  if (snprintf(smtp_url, sizeof(smtp_url), "smtp://127.0.0.1:%u",
               (unsigned)server.port) <= 0) {
    smtp_stop(&server);
    return 1;
  }
  pid = fork();
  if (pid < 0) {
    perror("fork");
    smtp_stop(&server);
    return 1;
  }
  if (pid == 0) {
    if (setenv("VECTIS_PACK_SMTP_URL", smtp_url, 1) != 0 ||
        setenv("VECTIS_PACK_SMTP_MAILBOX", argv[2], 1) != 0) {
      perror("setenv");
      _exit(127);
    }
    execl(argv[1], argv[1], (char *)NULL);
    perror("execl");
    _exit(127);
  }
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid");
    smtp_stop(&server);
    return 1;
  }
  smtp_stop(&server);
  if (server.failed) {
    fprintf(stderr, "SMTP mock server failed\n");
    return 1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    fprintf(stderr, "packed executable terminated by signal %d\n",
            WTERMSIG(status));
  }
  return 1;
}
