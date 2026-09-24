#include <arpa/inet.h>
#include <assert.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct server {
  int listener;
  unsigned short port;
  pthread_t thread;
};

struct result {
  char headers[1024];
  size_t header_used;
  size_t body_used;
};

static const char key_header[] = "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==";
static const char extension_header[] =
    "Sec-WebSocket-Extensions: permessage-deflate";
static const unsigned char server_frame[] = {0xc1, 0x04, 'p', 'o', 'n', 'g'};
static const unsigned char client_frame[] = {
    0xc1, 0x84,       0x11,       0x22,       0x33,
    0x44, 'p' ^ 0x11, 'i' ^ 0x22, 'n' ^ 0x33, 'g' ^ 0x44};

static void send_all(int fd, const void *data, size_t length) {
  const unsigned char *bytes;
  size_t offset;
  ssize_t amount;

  bytes = data;
  offset = 0;
  while (offset < length) {
    amount = send(fd, bytes + offset, length - offset, 0);
    assert(amount > 0);
    offset += (size_t)amount;
  }
}

static void read_all(int fd, void *data, size_t length) {
  unsigned char *bytes;
  size_t offset;
  ssize_t amount;

  bytes = data;
  offset = 0;
  while (offset < length) {
    amount = recv(fd, bytes + offset, length - offset, 0);
    assert(amount > 0);
    offset += (size_t)amount;
  }
}

static void read_headers(int fd, char *buffer, size_t capacity) {
  size_t used;
  ssize_t amount;

  used = 0;
  do {
    assert(used + 1 < capacity);
    amount = recv(fd, buffer + used, 1, 0);
    assert(amount == 1);
    used++;
    buffer[used] = '\0';
  } while (strstr(buffer, "\r\n\r\n") == NULL);
}

static void *server_main(void *arg) {
  struct server *server;
  static const char accepted[] =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
      "Sec-WebSocket-Extensions: permessage-deflate\r\n"
      "Sec-WebSocket-Protocol: chat\r\n\r\n";
  static const char invalid_accept[] = "HTTP/1.1 101 Switching Protocols\r\n"
                                       "Upgrade: websocket\r\n"
                                       "Connection: Upgrade\r\n"
                                       "Sec-WebSocket-Accept: invalid\r\n\r\n";
  static const char rejected[] = "HTTP/1.1 403 Forbidden\r\n"
                                 "Content-Length: 4\r\n"
                                 "Connection: close\r\n\r\n"
                                 "deny";
  unsigned char frame[sizeof(client_frame)];
  struct timeval timeout;
  char request[4096];
  int fd;
  int i;

  server = arg;
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  for (i = 0; i < 3; i++) {
    fd = accept(server->listener, NULL, NULL);
    assert(fd >= 0);
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
           0);
    assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ==
           0);
    read_headers(fd, request, sizeof(request));
    assert(strstr(request, "GET /chat HTTP/1.1\r\n") == request);
    assert(strstr(request, key_header) != NULL);
    assert(strstr(request, extension_header) != NULL);
    assert(strstr(request, "Sec-WebSocket-Protocol: chat") != NULL);
    if (i == 0) {
      send_all(fd, accepted, sizeof(accepted) - 1);
      send_all(fd, server_frame, sizeof(server_frame));
      read_all(fd, frame, sizeof(frame));
      assert(memcmp(frame, client_frame, sizeof(frame)) == 0);
    } else if (i == 1) {
      send_all(fd, invalid_accept, sizeof(invalid_accept) - 1);
    } else {
      send_all(fd, rejected, sizeof(rejected) - 1);
    }
    assert(close(fd) == 0);
  }
  return NULL;
}

static void start_server(struct server *server) {
  struct sockaddr_in addr;
  socklen_t length;

  memset(server, 0, sizeof(*server));
  server->listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(server->listener >= 0);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(server->listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  assert(listen(server->listener, 2) == 0);
  length = sizeof(addr);
  assert(getsockname(server->listener, (struct sockaddr *)&addr, &length) == 0);
  server->port = ntohs(addr.sin_port);
  assert(pthread_create(&server->thread, NULL, server_main, server) == 0);
}

static size_t on_header(char *data, size_t size, size_t count, void *arg) {
  struct result *result;
  size_t length;

  result = arg;
  length = size * count;
  assert(length <= sizeof(result->headers) - result->header_used - 1);
  memcpy(result->headers + result->header_used, data, length);
  result->header_used += length;
  result->headers[result->header_used] = '\0';
  return length;
}

static size_t on_body(char *data, size_t size, size_t count, void *arg) {
  struct result *result;
  size_t length;
  size_t i;

  result = arg;
  length = size * count;
  assert(length <= 4 - result->body_used);
  for (i = 0; i < length; i++)
    assert(data[i] == "deny"[result->body_used + i]);
  result->body_used += length;
  return length;
}

static CURLcode perform_handshake(CURLM *multi, CURL *easy) {
  CURLMsg *message;
  CURLcode result;
  int pending;
  int running;
  int numfds;
  int attempt;

  assert(curl_multi_add_handle(multi, easy) == CURLM_OK);
  assert(curl_multi_perform(multi, &running) == CURLM_OK);
  for (attempt = 0; running && attempt < 100; attempt++) {
    assert(curl_multi_poll(multi, NULL, 0, 100, &numfds) == CURLM_OK);
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
  }
  assert(running == 0);
  message = curl_multi_info_read(multi, &pending);
  assert(message != NULL && message->msg == CURLMSG_DONE);
  assert(message->easy_handle == easy);
  result = message->data.result;
  assert(curl_multi_info_read(multi, &pending) == NULL);
  return result;
}

static void wait_socket(curl_socket_t fd, short events) {
  struct pollfd item;

  item.fd = (int)fd;
  item.events = events;
  assert(poll(&item, 1, 1000) > 0);
}

int main(void) {
  struct server server;
  struct curl_slist *headers;
  struct result result;
  CURLM *multi;
  CURL *easy;
  curl_socket_t fd;
  CURLcode code;
  char url[128];
  unsigned char frame[sizeof(server_frame)];
  size_t amount;
  size_t offset;
  int i;

  assert(curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
  start_server(&server);
  multi = curl_multi_init();
  assert(multi != NULL);
  assert(snprintf(url, sizeof(url), "ws://localhost:%u/chat",
                  (unsigned)server.port) > 0);
  for (i = 0; i < 3; i++) {
    easy = curl_easy_init();
    assert(easy != NULL);
    headers = NULL;
    headers = curl_slist_append(headers, key_header);
    headers = curl_slist_append(headers, extension_header);
    headers = curl_slist_append(headers, "Sec-WebSocket-Protocol: chat");
    assert(headers != NULL);
    memset(&result, 0, sizeof(result));
    assert(curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK);
    assert(curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers) == CURLE_OK);
    assert(curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 2L) == CURLE_OK);
    assert(curl_easy_setopt(easy, CURLOPT_WS_OPTIONS, CURLWS_RAW_MODE) ==
           CURLE_OK);
    assert(curl_easy_setopt(easy, CURLOPT_NOPROXY, "*") == CURLE_OK);
    assert(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK);
    assert(curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, on_header) ==
           CURLE_OK);
    assert(curl_easy_setopt(easy, CURLOPT_HEADERDATA, &result) == CURLE_OK);
    assert(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_body) == CURLE_OK);
    assert(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &result) == CURLE_OK);
    code = perform_handshake(multi, easy);
    if (i == 0) {
      assert(code == CURLE_OK);
      assert(strstr(result.headers, "101 Switching Protocols") != NULL);
      assert(strstr(result.headers, extension_header) != NULL);
      assert(curl_easy_getinfo(easy, CURLINFO_ACTIVESOCKET, &fd) == CURLE_OK);
      assert(fd != CURL_SOCKET_BAD);
      offset = 0;
      while (offset < sizeof(frame)) {
        amount = 0;
        code = curl_easy_recv(easy, frame + offset, sizeof(frame) - offset,
                              &amount);
        if (code == CURLE_AGAIN) {
          wait_socket(fd, POLLIN);
          continue;
        }
        assert(code == CURLE_OK && amount > 0);
        offset += amount;
      }
      assert(memcmp(frame, server_frame, sizeof(frame)) == 0);
      offset = 0;
      while (offset < sizeof(client_frame)) {
        amount = 0;
        code = curl_easy_send(easy, client_frame + offset,
                              sizeof(client_frame) - offset, &amount);
        if (code == CURLE_AGAIN) {
          wait_socket(fd, POLLOUT);
          continue;
        }
        assert(code == CURLE_OK && amount > 0);
        offset += amount;
      }
    } else if (i == 1) {
      /* This libcurl version accepts a 101 without checking the key hash. */
      assert(code == CURLE_OK);
      assert(strstr(result.headers, "Sec-WebSocket-Accept: invalid") != NULL);
      assert(result.body_used == 0);
    } else {
      /* The WS API fails the upgrade before delivering the response body. */
      assert(code == CURLE_HTTP_RETURNED_ERROR);
      assert(strstr(result.headers, "403 Forbidden") != NULL);
      assert(result.body_used == 0);
    }
    assert(curl_multi_remove_handle(multi, easy) == CURLM_OK);
    curl_easy_cleanup(easy);
    curl_slist_free_all(headers);
  }
  assert(curl_multi_cleanup(multi) == CURLM_OK);
  assert(pthread_join(server.thread, NULL) == 0);
  assert(close(server.listener) == 0);
  curl_global_cleanup();
  return 0;
}
