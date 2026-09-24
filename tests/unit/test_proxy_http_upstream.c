#include "vectis_proxy_http_upstream.h"

#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct stream_server {
  int listener;
  unsigned short port;
  pthread_t thread;
  volatile int send_second;
};

struct stream_observation {
  unsigned informational;
  unsigned final;
  unsigned body;
  unsigned trailers;
};

static void ready(vectis_proxy_http_upstream *upstream,
                  vectis_proxy_http_event event, void *userdata);

static void send_all(int fd, const void *data, size_t length) {
  const unsigned char *cursor;
  ssize_t sent;

  cursor = (const unsigned char *)data;
  while (length != 0u) {
    sent = send(fd, cursor, length, 0);
    assert(sent > 0);
    cursor += (size_t)sent;
    length -= (size_t)sent;
  }
}

static void *serve(void *userdata) {
  static const char start[] =
      "HTTP/1.1 103 Early Hints\r\nLink: </a.css>; rel=preload\r\n\r\n"
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
      "Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n"
      "Transfer-Encoding: chunked\r\nTrailer: X-Final\r\n"
      "Connection: close\r\n\r\n2000\r\n";
  static const char end[] = "\r\n0\r\nX-Final: yes\r\n\r\n";
  struct stream_server *server;
  struct timeval timeout;
  char request[2048];
  char body[8192];
  size_t used;
  ssize_t got;
  int fd;
  int spins;

  server = (struct stream_server *)userdata;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0);
  used = 0u;
  do {
    got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
  } while (strstr(request, "\r\n\r\n") == NULL && used < sizeof(request) - 1u);
  assert(strstr(request, "GET /rewritten?q=1&q=2 HTTP/1.1\r\n") != NULL);
  send_all(fd, start, sizeof(start) - 1u);
  memset(body, 'A', sizeof(body));
  send_all(fd, body, sizeof(body));
  send_all(fd, "\r\n", 2u);
  for (spins = 0; spins < 500 && !server->send_second; ++spins)
    usleep(10000u);
  assert(server->send_second);
  send_all(fd, "2000\r\n", 6u);
  memset(body, 'B', sizeof(body));
  send_all(fd, body, sizeof(body));
  send_all(fd, end, sizeof(end) - 1u);
  assert(close(fd) == 0);
  return NULL;
}

static void start_server(struct stream_server *server,
                         void *(*handler)(void *)) {
  struct sockaddr_in address;
  socklen_t length;

  memset(server, 0, sizeof(*server));
  server->listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(server->listener >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(server->listener, (struct sockaddr *)&address, sizeof(address)) ==
         0);
  assert(listen(server->listener, 1) == 0);
  length = sizeof(address);
  assert(getsockname(server->listener, (struct sockaddr *)&address, &length) ==
         0);
  server->port = ntohs(address.sin_port);
  assert(pthread_create(&server->thread, NULL, handler, server) == 0);
}

static void *serve_head(void *userdata) {
  static const char response[] =
      "HTTP/1.1 304 Not Modified\r\nContent-Length: 123\r\n"
      "Connection: close\r\n\r\n";
  struct stream_server *server;
  char request[1024];
  size_t used;
  ssize_t got;
  int fd;

  server = (struct stream_server *)userdata;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  used = 0u;
  do {
    got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
  } while (strstr(request, "\r\n\r\n") == NULL && used < sizeof(request) - 1u);
  assert(strstr(request, "HEAD /head HTTP/1.1\r\n") != NULL);
  send_all(fd, response, sizeof(response) - 1u);
  assert(close(fd) == 0);
  return NULL;
}

static void test_head(void) {
  struct stream_server server;
  struct stream_observation observation;
  vectis_proxy_http_upstream upstream;
  vectis_error error;
  const char *reason;
  char url[128];
  CURL *easy;
  CURLcode result;
  size_t length;

  start_server(&server, serve_head);
  memset(&observation, 0, sizeof(observation));
  assert(snprintf(url, sizeof(url), "http://127.0.0.1:%u/original",
                  (unsigned)server.port) > 0);
  easy = curl_easy_init();
  assert(easy != NULL);
  assert(vectis_proxy_http_upstream_init(&upstream, easy, url, "/head", "HEAD",
                                         NULL, 8192u, 5000L, 10000L, ready,
                                         &observation, &error) == VECTIS_OK);
  result = curl_easy_perform(easy);
  assert(result == CURLE_OK);
  assert(observation.final == 1u && observation.body == 0u);
  assert(upstream.response.status == 304 && !upstream.response.body_allowed);
  assert(upstream.response.content_length == 123u);
  assert(vectis_proxy_http_upstream_body(&upstream, &length) == NULL &&
         length == 0u);
  reason = NULL;
  assert(vectis_proxy_http_upstream_finish(&upstream, result, &reason) ==
         VECTIS_PROXY_HEADER_OK);
  vectis_proxy_http_upstream_cleanup(&upstream);
  curl_easy_cleanup(easy);
  assert(pthread_join(server.thread, NULL) == 0);
  assert(close(server.listener) == 0);
}

static void ready(vectis_proxy_http_upstream *upstream,
                  vectis_proxy_http_event event, void *userdata) {
  struct stream_observation *observation;

  (void)upstream;
  observation = (struct stream_observation *)userdata;
  if (event == VECTIS_PROXY_HTTP_INFORMATIONAL)
    observation->informational++;
  else if (event == VECTIS_PROXY_HTTP_FINAL)
    observation->final++;
  else if (event == VECTIS_PROXY_HTTP_TRAILERS)
    observation->trailers++;
  else
    observation->body++;
}

int main(void) {
  struct stream_server server;
  struct stream_observation observation;
  vectis_proxy_http_upstream upstream;
  vectis_error error;
  const unsigned char *body;
  const char *reason;
  char url[128];
  CURLM *multi;
  CURL *easy;
  CURLMsg *message;
  size_t length;
  size_t total_received;
  size_t first_length;
  size_t partial;
  size_t i;
  int running;
  int messages;
  int spins;

  start_server(&server, serve);
  memset(&observation, 0, sizeof(observation));
  assert(snprintf(url, sizeof(url), "http://127.0.0.1:%u/original",
                  (unsigned)server.port) > 0);
  easy = curl_easy_init();
  multi = curl_multi_init();
  assert(easy != NULL && multi != NULL);
  assert(vectis_proxy_http_upstream_init(
             &upstream, easy, url, "/rewritten?q=1&q=2", "GET", NULL, 8192u,
             5000L, 10000L, ready, &observation, &error) == VECTIS_OK);
  assert(curl_multi_add_handle(multi, easy) == CURLM_OK);
  running = 1;
  for (spins = 0; spins < 500 && observation.body == 0u; ++spins) {
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
    assert(curl_multi_poll(multi, NULL, 0u, 10, NULL) == CURLM_OK);
  }
  assert(observation.informational == 1u);
  assert(observation.final == 1u && observation.body == 1u);
  assert(running == 1);
  body = vectis_proxy_http_upstream_body(&upstream, &length);
  assert(body != NULL && length != 0u && length <= 8192u);
  for (i = 0u; i < length; ++i)
    assert(body[i] == 'A');
  total_received = length;
  first_length = length;
  server.send_second = 1;
  for (spins = 0; spins < 500 && !upstream.paused; ++spins) {
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
    assert(curl_multi_poll(multi, NULL, 0u, 10, NULL) == CURLM_OK);
  }
  assert(upstream.paused && running == 1);
  assert(upstream.body_capacity <= 16384u);
  partial = first_length / 2u;
  vectis_proxy_http_upstream_consume(&upstream, partial);
  body = vectis_proxy_http_upstream_body(&upstream, &length);
  assert(length == first_length - partial && body[0] == 'A');
  assert(vectis_proxy_http_upstream_resume(&upstream, &error) ==
         VECTIS_ERR_STATE);
  vectis_proxy_http_upstream_consume(&upstream, length);
  assert(vectis_proxy_http_upstream_resume(&upstream, &error) == VECTIS_OK);
  for (spins = 0; spins < 500 && (running || total_received < 16384u);
       ++spins) {
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
    body = vectis_proxy_http_upstream_body(&upstream, &length);
    if (body != NULL) {
      assert(total_received + length <= 16384u);
      for (i = 0u; i < length; ++i)
        assert(body[i] == (total_received + i < 8192u ? 'A' : 'B'));
      total_received += length;
      vectis_proxy_http_upstream_consume(&upstream, length);
      if (upstream.paused)
        assert(vectis_proxy_http_upstream_resume(&upstream, &error) ==
               VECTIS_OK);
    }
    assert(curl_multi_poll(multi, NULL, 0u, 10, NULL) == CURLM_OK);
  }
  assert(total_received == 16384u && observation.body >= 2u);
  for (spins = 0; spins < 500 && running; ++spins) {
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
    assert(curl_multi_poll(multi, NULL, 0u, 10, NULL) == CURLM_OK);
  }
  assert(running == 0);
  message = curl_multi_info_read(multi, &messages);
  assert(message != NULL && message->msg == CURLMSG_DONE &&
         message->data.result == CURLE_OK);
  assert(upstream.response.body_bytes == 16384u);
  assert(upstream.response.trailers.count == 1u);
  assert(upstream.outbound_headers.count == 3u);
  reason = NULL;
  assert(vectis_proxy_http_upstream_finish(&upstream, message->data.result,
                                           &reason) == VECTIS_PROXY_HEADER_OK);
  assert(observation.trailers == 1u);
  assert(curl_multi_remove_handle(multi, easy) == CURLM_OK);
  vectis_proxy_http_upstream_cleanup(&upstream);
  curl_easy_cleanup(easy);
  curl_multi_cleanup(multi);
  assert(pthread_join(server.thread, NULL) == 0);
  assert(close(server.listener) == 0);
  test_head();
  return 0;
}
