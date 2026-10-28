#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vectis/vectis.h>
/* Tiny, bounded observable fixtures. CTest owns timeout/cancellation; no PID
 * scanning, daemon supervisor, retry sleeps or Python test orchestration. */
static pid_t server = -1;
static int output_fd = -1, memory_mode;
static char base_url[128], events[131072];
static size_t event_size;
static void cleanup(void) {
  int status;
  if (server > 0) {
    (void)kill(server, SIGTERM);
    while (waitpid(server, &status, 0) < 0 && errno == EINTR) {
    }
    server = -1;
  }
  if (output_fd >= 0) {
    close(output_fd);
    output_fd = -1;
  }
}
static void fail(const char *file, int line, const char *condition) {
  fprintf(stderr, "%s:%d: %s\nObserved fixture events:\n%s\n", file, line,
          condition, events);
  exit(1);
}
#define CHECK(value)                                                           \
  do {                                                                         \
    if (!(value))                                                              \
      fail(__FILE__, __LINE__, #value);                                        \
  } while (0)
static long long milliseconds(void) {
  struct timespec now;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (long long)now.tv_sec * 1000 + (long long)now.tv_nsec / 1000000;
}
static void until(const char *marker) {
  struct pollfd event;
  long long deadline = milliseconds() + (memory_mode ? 45000 : 10000);
  long long remaining;
  ssize_t got;
  while (!strstr(events, marker)) {
    remaining = deadline - milliseconds();
    CHECK(remaining > 0);
    event.fd = output_fd;
    event.events = POLLIN;
    event.revents = 0;
    CHECK(poll(&event, 1, (int)remaining) > 0);
    CHECK(event_size < sizeof(events) - 1u);
    got =
        read(output_fd, events + event_size, sizeof(events) - event_size - 1u);
    CHECK(got > 0);
    event_size += (size_t)got;
    events[event_size] = 0;
  }
}
static unsigned short available_port(void) {
  struct sockaddr_in address;
  socklen_t size = sizeof(address);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(fd >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
  CHECK(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
  close(fd);
  return ntohs(address.sin_port);
}
static void start(const char *binary, const char *script, const char *root,
                  const char *mode) {
  char port[16], error_file[4096], state[4096], config[4096], memory_log[4096];
  int channel[2], errors;
  unsigned short selected = available_port();
  snprintf(port, sizeof(port), "%u", (unsigned)selected);
  snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%s", port);
  CHECK(snprintf(error_file, sizeof(error_file), "%s/stderr.log", root) > 0);
  snprintf(state, sizeof(state), "%s/state", root);
  snprintf(config, sizeof(config), "%s/config", root);
  snprintf(memory_log, sizeof(memory_log), "--log-file=%s/memory.%%p.log",
           root);
  errors = open(error_file, O_CREAT | O_WRONLY | O_TRUNC, 0600);
  CHECK(errors >= 0);
  CHECK(pipe(channel) == 0);
  server = fork();
  CHECK(server >= 0);
  if (!server) {
    close(channel[0]);
    if (dup2(channel[1], STDOUT_FILENO) < 0 ||
        dup2(errors, STDERR_FILENO) < 0 || chdir(root) != 0)
      _exit(126);
    close(channel[1]);
    close(errors);
    if (setenv("TMPDIR", root, 1) != 0 ||
        setenv("XDG_STATE_HOME", state, 1) != 0 ||
        setenv("XDG_CONFIG_HOME", config, 1) != 0 ||
        setenv("VECTIS_SOURCE_PORT", port, 1) != 0 ||
        setenv("VECTIS_WS_LOGGING",
               !strcmp(mode, "websocket-disabled") ? "false" : "true",
               1) != 0 ||
        setenv("VECTIS_WORKFLOW_ROUTE_PORT", port, 1) != 0 ||
        setenv("VECTIS_WORKFLOW_ROUTE_ROOT", root, 1) != 0 ||
        setenv("VECTIS_WORKFLOW_ROUTE_XA",
               !strcmp(mode, "workflow-xa") ? "1" : "0", 1) != 0 ||
        setenv("VECTIS_MCP_TOPOLOGY",
               !strcmp(mode, "mcp-direct") ? "direct" : "supervised", 1) != 0 ||
        setenv("VECTIS_WORKFLOW_ROUTE_PROBE_MS",
               strcmp(mode, "workflow-concurrent") != 0 ? "1000000" : "10",
               1) != 0)
      _exit(126);
    if (memory_mode) {
      execlp("valgrind", "valgrind", "--track-origins=yes", "--leak-check=full",
             "--show-leak-kinds=definite,indirect,possible",
             "--errors-for-leak-kinds=definite,indirect,possible",
             "--error-exitcode=99", memory_log, binary, script, (char *)NULL);
    } else if (!strcmp(script, "-"))
      execl(binary, binary, (char *)NULL);
    else
      execl(binary, binary, script, (char *)NULL);
    _exit(127);
  }
  close(errors);
  close(channel[1]);
  output_fd = channel[0];
  until("ready\n");
  if (!strncmp(mode, "mcp-", 4))
    until("serving http on");
}
static vectis_http_response request(const char *path, const char *body) {
  vectis_http_client_config config;
  vectis_http_client *client = NULL;
  vectis_http_request input;
  vectis_http_response response;
  vectis_error error;
  static const char *headers[] = {"Connection: close"};
  vectis_http_client_config_init(&config);
  config.base_url = base_url;
  config.timeout_ms = memory_mode ? 45000 : 5000;
  memset(&response, 0, sizeof(response));
  CHECK(vectis_http_client_new(&config, &client, &error) == VECTIS_OK);
  vectis_http_request_init(&input);
  input.url = path;
  input.method = body ? VECTIS_HTTP_POST : VECTIS_HTTP_GET;
  input.body = body;
  input.body_size = body ? strlen(body) : 0u;
  input.headers = headers;
  input.header_count = 1u;
  if (client->execute(client, &input, &response, &error) != VECTIS_OK) {
    fprintf(stderr, "HTTP %s failed: %s %s\n", path, error.message,
            error.detail);
    CHECK(0);
  }
  client->close(client);
  return response;
}
static void expect(const char *path, const char *body, long code,
                   const char *expected) {
  vectis_http_response response = request(path, body);
  if (response.status_code != code)
    fprintf(stderr, "HTTP %s returned %ld, expected %ld: %.*s\n", path,
            response.status_code, code, (int)response.body_size,
            (const char *)response.body);
  CHECK(response.status_code == code);
  if (expected)
    CHECK(response.body_size == strlen(expected) &&
          !memcmp(response.body, expected, response.body_size));
  vectis_http_response_cleanup(&response);
}
static void source_behavior(void) {
  static const char *kinds[] = {"spooled_source", "stream_source"};
  static const char *contexts[] = {"route", "before", "after", "dsv", "upload"};
  static const char *phases[] = {"read",         "reset",   "close",
                                 "content_type", "headers", "body"};
  static const char *modes[] = {"throw", "guard"};
  static const char *shapes[] = {"number", "table"};
  static const char *lifetime[] = {"upload open",     "upload chunk",
                                   "upload complete", "upload cleanup",
                                   "dsv row",         "dsv complete"};
  char path[256], marker[256];
  size_t i, j, k, n;
  const char *body;
  vectis_http_response response;
  for (i = 0; i < 2u; ++i) {
    snprintf(path, sizeof(path), "/%s", kinds[i]);
    response = request(path, NULL);
    CHECK(response.status_code == 200 && response.body_size == 9000u);
    for (n = 0; n < response.body_size; ++n)
      CHECK(((const char *)response.body)[n] == 'x');
    vectis_http_response_cleanup(&response);
    for (j = 0; j < 3u; ++j) {
      snprintf(marker, sizeof(marker), "%s %s rejected\n", kinds[i], phases[j]);
      until(marker);
    }
  }
  for (j = 0; j < 2u; ++j) {
    snprintf(path, sizeof(path), "/fail-%s", phases[j]);
    expect(path, NULL, 500, NULL);
    snprintf(marker, sizeof(marker), "failed %s close rejected\n", phases[j]);
    until(marker);
    expect("/healthy", NULL, 200, "healthy");
  }
  expect("/upload", "abc", 200, "owned");
  until("upload_source close rejected\n");
  until("upload_source read rejected\n");
  until("upload_source reset rejected\n");
  expect("/dsv", "id\none\n", 200, "owned");
  until("dsv_source close rejected\n");
  until("dsv_source read rejected\n");
  until("dsv_source reset rejected\n");
  for (i = 0; i < 6u; ++i) {
    snprintf(marker, sizeof(marker), "%s rejected\n", lifetime[i]);
    until(marker);
  }
  for (i = 0; i < 5u; ++i)
    for (j = 0; j < 6u; ++j)
      for (k = 0; k < 2u; ++k) {
        body = i == 3u ? "id\none\n" : i == 4u ? "abc" : NULL;
        snprintf(path, sizeof(path), "/convert-%s-%s-%s", contexts[i],
                 phases[j], modes[k]);
        expect(path, body, k ? 200 : 500, k ? "resolved" : NULL);
        expect("/healthy", NULL, 200, "healthy");
      }
  for (i = 0; i < 5u; ++i) {
    if (i == 1u || i == 2u)
      continue;
    body = i == 3u ? "id\none\n" : i == 4u ? "abc" : NULL;
    for (j = 0; j < 2u; ++j)
      for (k = 0; k < 2u; ++k) {
        snprintf(path, sizeof(path), "/source-error-%s-%s-%s", contexts[i],
                 phases[j], shapes[k]);
        expect(path, body, 500, NULL);
        snprintf(marker, sizeof(marker), "%s close rejected\n", path);
        until(marker);
        expect("/healthy", NULL, 200, "healthy");
      }
  }
  expect("/source-error-cleanup", NULL, 200, "clean");
  for (k = 0; k < 2u; ++k) {
    snprintf(path, sizeof(path), "/convert-auth-%s", modes[k]);
    expect(path, NULL, k ? 200 : 500, NULL);
    expect("/healthy", NULL, 200, "healthy");
  }
  expect("/conversion-ownership", NULL, 200, "12");
  expect("/graceful", NULL, 200, "stopping");
  until("graceful route stop accepted\n");
  CHECK(kill(server, SIGTERM) == 0);
  until("closed\n");
  until("fork exit kore_worker\n");
  until("fork exit kore_parent\n");
}
static void *effect(void *context) {
  const char *identity = (const char *)context;
  vectis_http_response response = request("/effect", identity);
  char text[128];
  CHECK(response.status_code == 200 && response.body_size < sizeof(text));
  memcpy(text, response.body, response.body_size);
  text[response.body_size] = 0;
  CHECK(strstr(text, "\"receipts\":1") &&
        strstr(text, "\"effects_in_producer\":0"));
  vectis_http_response_cleanup(&response);
  return NULL;
}
static void workflow_behavior(int concurrent, int xa) {
  pthread_t threads[8];
  char identities[8][32], marker[64];
  int i, count = 0;
  const char *at;
  until("sus drained\n");
  for (i = 0; i < 8; ++i) {
    snprintf(identities[i], sizeof(identities[i]), "%s-%d",
             i % 2 ? "two" : "one", i);
    if (concurrent)
      CHECK(pthread_create(&threads[i], NULL, effect, identities[i]) == 0);
    else {
      effect(identities[i]);
      snprintf(marker, sizeof(marker), "effect %s\n", identities[i]);
      until(marker);
    }
  }
  if (concurrent)
    for (i = 0; i < 8; ++i)
      CHECK(pthread_join(threads[i], NULL) == 0);
  for (i = 0; i < 8; ++i) {
    snprintf(marker, sizeof(marker), "effect %s\n", identities[i]);
    until(marker);
  }
  for (at = events; (at = strstr(at, "effect ")) != NULL; at += 7)
    if (at == events || at[-1] == '\n')
      ++count;
  CHECK(count == 8);
  /* Separate managed XA calls have distinct global transaction identities;
   * cross-transaction business deduplication is not this wake-path fixture's
   * contract. Ordinary outbox duplicate behavior remains covered here. */
  if (!xa)
    expect("/effect", identities[0], 200, NULL);
  CHECK(kill(server, SIGTERM) == 0);
  until("stopped 8\n");
}
static void mcp_behavior(void) {
  struct sockaddr_in address;
  struct timeval timeout;
  char body[2049], request_bytes[2400], response[8192], protocol[80];
  const char *at, *end, *client = getenv("VECTIS_MCP_CLIENT"),
                        *runner = getenv("VECTIS_MCP_RUNNER");
  size_t used = 0, sent = 0, total;
  ssize_t got;
  int fd, status;
  pid_t child;
  CHECK(client && runner);
  /* An oversized unfinished body must be rejected before its remaining bytes
   * arrive. This checks real socket closure, independently of libcurl. */
  fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(fd >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons((unsigned short)atoi(strrchr(base_url, ':') + 1));
  timeout.tv_sec = 3;
  timeout.tv_usec = 0;
  CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
        0);
  CHECK(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ==
        0);
  CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
  memset(body, 'x', sizeof(body));
  total =
      (size_t)snprintf(request_bytes, sizeof(request_bytes),
                       "POST /mcp HTTP/1.1\r\nHost: localhost\r\nContent-Type: "
                       "application/json\r\nContent-Length: 4096\r\n\r\n");
  memcpy(request_bytes + total, body, sizeof(body));
  total += sizeof(body);
  while (sent < total) {
    got = send(fd, request_bytes + sent, total - sent, 0);
    CHECK(got > 0);
    sent += (size_t)got;
  }
  while (1) {
    CHECK(used < sizeof(response) - 1u);
    got = recv(fd, response + used, sizeof(response) - 1u - used, 0);
    CHECK(got >= 0);
    if (!got)
      break;
    used += (size_t)got;
  }
  close(fd);
  response[used] = 0;
  for (sent = 0; sent < used; ++sent)
    response[sent] = (char)tolower((unsigned char)response[sent]);
  CHECK(strstr(response, " 413 ") && strstr(response, "connection: close"));
  at = strstr(events, "PROTOCOL ");
  CHECK(at);
  at += 9;
  end = strchr(at, '\n');
  CHECK(end && (size_t)(end - at) < sizeof(protocol));
  memcpy(protocol, at, (size_t)(end - at));
  protocol[end - at] = 0;
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    execl(runner, runner, client, base_url, protocol, (char *)NULL);
    _exit(127);
  }
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);
  CHECK(kill(server, SIGTERM) == 0);
  until("mcp failure cleanup ok\n");
}
static void websocket_behavior(void) {
  const char *client = getenv("VECTIS_WS_CLIENT");
  const char *port = strrchr(base_url, ':');
  pid_t child;
  int status;
  CHECK(client && port);
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    execl(client, client, port + 1, "/ws", "ping", "lua:ping", (char *)NULL);
    _exit(127);
  }
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);
  CHECK(kill(server, SIGTERM) == 0);
  until("websocket behavior ok\n");
  CHECK(!strstr(events, "fixture/") && !strstr(events, "callback_failed"));
}
int main(int argc, char **argv) {
  int status;
  CHECK(argc == 5 || argc == 6);
  CHECK(atexit(cleanup) == 0);
  memory_mode = argc == 6;
  start(argv[1], argv[2], argv[4], argv[3]);
  if (!strcmp(argv[3], "source"))
    source_behavior();
  else if (!strcmp(argv[3], "fork-failure"))
    until("fork failure behavior ok\n");
  else if (!strncmp(argv[3], "mcp-", 4))
    mcp_behavior();
  else if (!strncmp(argv[3], "websocket-", 10))
    websocket_behavior();
  else
    workflow_behavior(strcmp(argv[3], "workflow-direct") != 0,
                      !strcmp(argv[3], "workflow-xa"));
  CHECK(waitpid(server, &status, 0) == server);
  server = -1;
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  puts("Lua HTTP behavior ok");
  return 0;
}
