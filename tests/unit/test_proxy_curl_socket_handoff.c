#include <arpa/inet.h>
#include <assert.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

struct echo_server {
  int listener;
  unsigned short port;
  pthread_t thread;
};

struct loop_state {
  int epoll_fd;
  int watched_fd;
  int watched_events;
  int removes;
  long timeout_ms;
};

static void *
echo_main(void *arg)
{
  struct echo_server *server;
  char bytes[4];
  int fd;

  server = (struct echo_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  assert(recv(fd, bytes, sizeof(bytes), MSG_WAITALL) == 4);
  assert(memcmp(bytes, "ping", 4) == 0);
  assert(send(fd, "pong", 4, 0) == 4);
  assert(close(fd) == 0);
  return NULL;
}

static void
start_echo(struct echo_server *server)
{
  struct sockaddr_in addr;
  socklen_t size;

  server->listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(server->listener >= 0);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(server->listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  assert(listen(server->listener, 1) == 0);
  size = sizeof(addr);
  assert(getsockname(server->listener, (struct sockaddr *)&addr, &size) == 0);
  server->port = ntohs(addr.sin_port);
  assert(pthread_create(&server->thread, NULL, echo_main, server) == 0);
}

static int
socket_change(CURL *easy, curl_socket_t socket, int what,
    void *arg, void *socket_arg)
{
  struct loop_state *state;
  struct epoll_event event;
  int operation;

  (void)easy;
  (void)socket_arg;
  state = (struct loop_state *)arg;
  if (what == CURL_POLL_REMOVE) {
    if (state->watched_fd == (int)socket) {
      assert(epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL,
          (int)socket, NULL) == 0);
      state->watched_fd = -1;
      state->watched_events = 0;
    }
    state->removes++;
    return 0;
  }
  memset(&event, 0, sizeof(event));
  event.events = (what & CURL_POLL_IN ? EPOLLIN : 0) |
      (what & CURL_POLL_OUT ? EPOLLOUT : 0);
  event.data.fd = (int)socket;
  operation = state->watched_fd == (int)socket
      ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
  assert(epoll_ctl(state->epoll_fd, operation,
      (int)socket, &event) == 0);
  state->watched_fd = (int)socket;
  state->watched_events = (int)event.events;
  return 0;
}

static int
timer_change(CURLM *multi, long timeout_ms, void *arg)
{
  struct loop_state *state;

  (void)multi;
  state = (struct loop_state *)arg;
  state->timeout_ms = timeout_ms;
  return 0;
}

static void
drive_socket(CURLM *multi, struct loop_state *state, int *running)
{
  struct epoll_event event;
  int events;
  int flags;

  events = epoll_wait(state->epoll_fd, &event, 1,
      state->timeout_ms >= 0 ? (int)state->timeout_ms : 1000);
  assert(events >= 0);
  if (events == 0) {
    assert(curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT,
        0, running) == CURLM_OK);
    return;
  }
  flags = (event.events & EPOLLIN ? CURL_CSELECT_IN : 0) |
      (event.events & EPOLLOUT ? CURL_CSELECT_OUT : 0) |
      (event.events & (EPOLLERR | EPOLLHUP) ? CURL_CSELECT_ERR : 0);
  assert(curl_multi_socket_action(multi, event.data.fd,
      flags, running) == CURLM_OK);
}

int
main(void)
{
  struct echo_server server;
  struct loop_state state;
  struct epoll_event event;
  CURLM *multi;
  CURL *easy;
  CURLMsg *msg;
  curl_socket_t fd;
  CURLcode code;
  char url[128];
  char reply[4];
  int running;
  int attempt;
  int pending;
  size_t amount;

  start_echo(&server);
  assert(curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
  memset(&state, 0, sizeof(state));
  state.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  assert(state.epoll_fd >= 0);
  state.watched_fd = -1;
  state.timeout_ms = -1;
  multi = curl_multi_init();
  easy = curl_easy_init();
  assert(multi != NULL && easy != NULL);
  assert(curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION,
      socket_change) == CURLM_OK);
  assert(curl_multi_setopt(multi, CURLMOPT_SOCKETDATA, &state) == CURLM_OK);
  assert(curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION,
      timer_change) == CURLM_OK);
  assert(curl_multi_setopt(multi, CURLMOPT_TIMERDATA, &state) == CURLM_OK);
  assert(snprintf(url, sizeof(url), "http://127.0.0.1:%u/",
      (unsigned)server.port) > 0);
  assert(curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 1L) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK);
  assert(curl_multi_add_handle(multi, easy) == CURLM_OK);
  running = 1;
  assert(curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT,
      0, &running) == CURLM_OK);
  for (attempt = 0; running > 0 && attempt < 40; attempt++)
    drive_socket(multi, &state, &running);
  assert(running == 0);
  msg = curl_multi_info_read(multi, &pending);
  assert(msg != NULL && msg->msg == CURLMSG_DONE);
  assert(msg->data.result == CURLE_OK);
  assert(curl_easy_getinfo(easy, CURLINFO_ACTIVESOCKET, &fd) == CURLE_OK);
  assert(fd != CURL_SOCKET_BAD);
  assert(state.removes > 0);
  assert(state.watched_fd == -1);

  memset(&event, 0, sizeof(event));
  event.events = EPOLLIN | EPOLLOUT;
  event.data.fd = (int)fd;
  assert(epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD,
      (int)fd, &event) == 0);
  do {
    amount = 0;
    code = curl_easy_send(easy, "ping", 4, &amount);
    if (code == CURLE_AGAIN)
      assert(epoll_wait(state.epoll_fd, &event, 1, 1000) > 0);
  } while (code == CURLE_AGAIN);
  assert(code == CURLE_OK && amount == 4);
  event.events = EPOLLIN;
  assert(epoll_ctl(state.epoll_fd, EPOLL_CTL_MOD,
      (int)fd, &event) == 0);
  do {
    amount = 0;
    code = curl_easy_recv(easy, reply, sizeof(reply), &amount);
    if (code == CURLE_AGAIN)
      assert(epoll_wait(state.epoll_fd, &event, 1, 1000) > 0);
  } while (code == CURLE_AGAIN);
  assert(code == CURLE_OK && amount == 4);
  assert(memcmp(reply, "pong", 4) == 0);
  assert(epoll_ctl(state.epoll_fd, EPOLL_CTL_DEL,
      (int)fd, NULL) == 0);
  assert(curl_multi_remove_handle(multi, easy) == CURLM_OK);
  curl_easy_cleanup(easy);
  assert(curl_multi_cleanup(multi) == CURLM_OK);
  curl_global_cleanup();
  assert(close(state.epoll_fd) == 0);
  assert(pthread_join(server.thread, NULL) == 0);
  assert(close(server.listener) == 0);
  return 0;
}
