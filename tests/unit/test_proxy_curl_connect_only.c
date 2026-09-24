#include <arpa/inet.h>
#include <assert.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct echo_server {
  int listener;
  unsigned short port;
  pthread_t thread;
};

static void *
echo_main(void *arg)
{
  struct echo_server *server;
  char bytes[4];
  int fd;
  ssize_t got;

  server = (struct echo_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  got = recv(fd, bytes, sizeof(bytes), MSG_WAITALL);
  assert(got == (ssize_t)sizeof(bytes));
  assert(memcmp(bytes, "ping", sizeof(bytes)) == 0);
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

static void
wait_socket(curl_socket_t fd, short events)
{
  struct pollfd item;

  item.fd = (int)fd;
  item.events = events;
  assert(poll(&item, 1, 1000) > 0);
}

int
main(void)
{
  struct echo_server server;
  CURLM *multi;
  CURL *easy;
  CURLMsg *msg;
  curl_socket_t fd;
  CURLcode code;
  char url[128];
  char reply[4];
  int running;
  int pending;
  int numfds;
  int attempt;
  size_t sent;
  size_t received;

  start_echo(&server);
  assert(curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
  multi = curl_multi_init();
  easy = curl_easy_init();
  assert(multi != NULL && easy != NULL);
  assert(snprintf(url, sizeof(url), "http://127.0.0.1:%u/",
      (unsigned)server.port) > 0);
  assert(curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 1L) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,
      CURL_HTTP_VERSION_1_1) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK);
  assert(curl_multi_add_handle(multi, easy) == CURLM_OK);
  assert(curl_multi_perform(multi, &running) == CURLM_OK);
  for (attempt = 0; running > 0 && attempt < 20; attempt++) {
    assert(curl_multi_poll(multi, NULL, 0, 500, &numfds) == CURLM_OK);
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
  }
  assert(running == 0);
  msg = curl_multi_info_read(multi, &pending);
  assert(msg != NULL && msg->msg == CURLMSG_DONE);
  assert(msg->data.result == CURLE_OK);
  assert(curl_easy_getinfo(easy, CURLINFO_ACTIVESOCKET, &fd) == CURLE_OK);
  assert(fd != CURL_SOCKET_BAD);

  do {
    sent = 0;
    code = curl_easy_send(easy, "ping", 4, &sent);
    if (code == CURLE_AGAIN)
      wait_socket(fd, POLLOUT);
  } while (code == CURLE_AGAIN);
  assert(code == CURLE_OK && sent == 4);
  do {
    received = 0;
    code = curl_easy_recv(easy, reply, sizeof(reply), &received);
    if (code == CURLE_AGAIN)
      wait_socket(fd, POLLIN);
  } while (code == CURLE_AGAIN);
  assert(code == CURLE_OK && received == 4);
  assert(memcmp(reply, "pong", 4) == 0);

  assert(curl_multi_remove_handle(multi, easy) == CURLM_OK);
  sent = 0;
  assert(curl_easy_send(easy, "ping", 4, &sent) != CURLE_OK);
  curl_easy_cleanup(easy);
  assert(curl_multi_cleanup(multi) == CURLM_OK);
  curl_global_cleanup();
  assert(pthread_join(server.thread, NULL) == 0);
  assert(close(server.listener) == 0);
  return 0;
}
