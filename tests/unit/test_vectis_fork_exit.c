#include <arpa/inet.h>
#include <assert.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vectis/vectis.h>

typedef struct observed {
  pid_t original;
  unsigned worker, parent, original_called;
  int write_fd, fail_parent;
} observed;
static vectis_status exiting(vectis_app *app, const char *role, void *context,
                             vectis_error *error) {
  observed *state = context;
  char marker = 'W';
  int worker_role = !strcmp(role, "kore_worker");
  if (getpid() == state->original)
    __sync_add_and_fetch(&state->original_called, 1u);
  if (worker_role) {
    __sync_add_and_fetch(&state->worker, 1u);
  } else {
    assert(!strcmp(role, "kore_parent"));
    __sync_add_and_fetch(&state->parent, 1u);
  }
  /* The public contract permits reclaiming the app during this callback. */
  app->close(app);
  if (worker_role)
    assert(write(state->write_fd, &marker, 1u) == 1);
  else if (state->fail_parent) {
    memset(error, 0, sizeof(*error));
    error->code = VECTIS_ERR_STATE;
    strcpy(error->message, "injected fork exit failure");
    return VECTIS_ERR_STATE;
  }
  return VECTIS_OK;
}
static vectis_status identify(vectis_app *app, vectis_request *request,
                              vectis_response *response, void *context,
                              vectis_error *error) {
  char pid[40];
  (void)app;
  (void)request;
  (void)context;
  snprintf(pid, sizeof(pid), "%ld", (long)getpid());
  return vectis_response_text(response, 200, "text/plain", pid, error);
}
static unsigned short available_port(void) {
  struct sockaddr_in address;
  socklen_t size = sizeof(address);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
  assert(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
  close(fd);
  return ntohs(address.sin_port);
}
static pid_t get_worker(vectis_http_client *client) {
  static const char *headers[] = {"Connection: close"};
  vectis_http_request request;
  vectis_http_response response;
  vectis_error error;
  char text[40], *end;
  long pid;
  memset(&response, 0, sizeof(response));
  vectis_http_request_init(&request);
  request.url = "/pid";
  request.headers = headers;
  request.header_count = 1;
  if (client->execute(client, &request, &response, &error) != VECTIS_OK) {
    fprintf(stderr, "worker readiness request failed: %s (%s)\n", error.message,
            error.detail);
    assert(0);
  }
  assert(response.status_code == 200 && response.body_size < sizeof(text));
  memcpy(text, response.body, response.body_size);
  text[response.body_size] = 0;
  pid = strtol(text, &end, 10);
  assert(pid > 1 && *end == 0);
  vectis_http_response_cleanup(&response);
  return (pid_t)pid;
}
int main(int argc, char **argv) {
  vectis_app_config config;
  vectis_http_client_config http;
  vectis_http_client *client = NULL;
  vectis_app *app;
  vectis_route_config route;
  vectis_error error;
  observed *state;
  struct pollfd event;
  char url[128], marker;
  pid_t before, after;
  int respawn = argc == 2 && !strcmp(argv[1], "respawn");
  int pipe_fd[2], crash = respawn || (argc == 2 && !strcmp(argv[1], "crash"));
  assert(pipe(pipe_fd) == 0);
  state = mmap(NULL, sizeof(*state), PROT_READ | PROT_WRITE,
               MAP_ANONYMOUS | MAP_SHARED, -1, 0);
  assert(state != MAP_FAILED);
  memset(state, 0, sizeof(*state));
  state->original = getpid();
  state->fail_parent = argc == 2 && !strcmp(argv[1], "failure");
  state->write_fd = pipe_fd[1];
  vectis_app_config_init(&config);
  config.app_name = "fork-exit-c-test";
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = available_port();
  config.server.worker_count = respawn ? 1u : 2u;
  config.server.worker_set_affinity_disabled = 1;
  config.supervision_policy = VECTIS_SUPERVISION_SUPERVISED;
  app = vectis_app_new(&config, &error);
  assert(app);
  assert(vectis_app_set_fork_exit_handler(NULL, exiting, state, &error) ==
         VECTIS_ERR_INVALID);
  assert(vectis_app_set_fork_exit_handler(app, exiting, state, &error) ==
         VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/pid", identify, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  assert(app->start(app, &error) == VECTIS_OK);
  assert(vectis_app_set_fork_exit_handler(app, NULL, NULL, &error) ==
         VECTIS_ERR_STATE);
  snprintf(url, sizeof(url), "http://127.0.0.1:%u", (unsigned)config.tls.port);
  vectis_http_client_config_init(&http);
  http.base_url = url;
  http.timeout_ms = 3000;
  http.retry_max_attempts = 1;
  assert(vectis_http_client_new(&http, &client, &error) == VECTIS_OK);
  before = get_worker(client);
  assert(kill(before, crash ? SIGKILL : SIGTERM) == 0);
  event.fd = pipe_fd[0];
  event.events = POLLIN;
  event.revents = 0;
  if (!crash)
    assert(poll(&event, 1, 3000) > 0 && read(pipe_fd[0], &marker, 1u) == 1 &&
           marker == 'W');
  after = get_worker(client);
  assert(after != before);
  client->close(client);
  assert(app->stop(app, &error) ==
         (state->fail_parent ? VECTIS_ERR_STATE : VECTIS_OK));
  assert(state->worker >= (crash ? 1u : 2u) && state->parent == 1u &&
         !state->original_called);
  app->close(app);
  assert(!state->original_called);
  close(pipe_fd[0]);
  close(pipe_fd[1]);
  assert(munmap(state, sizeof(*state)) == 0);
  return 0;
}
