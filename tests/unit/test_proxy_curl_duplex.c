#include <arpa/inet.h>
#include <assert.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct duplex_server {
  int listener;
  unsigned short port;
  pthread_t thread;
  int first_body_seen;
  int second_body_seen;
};

struct transfer_state {
  int first_sent;
  int allow_second;
  int saw_response_before_resume;
  char response[16];
  size_t response_size;
};

static void *
server_main(void *arg)
{
  static const char first_reply[] =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
      "Connection: close\r\n\r\n4\r\npong\r\n";
  static const char last_reply[] = "4\r\ndone\r\n0\r\n\r\n";
  struct duplex_server *server;
  struct timeval timeout;
  char input[1024];
  char *body;
  size_t used;
  size_t body_size;
  ssize_t got;
  int fd;

  server = (struct duplex_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
      sizeof(timeout)) == 0);
  used = 0;
  input[0] = '\0';
  body = NULL;
  while (body == NULL && used < sizeof(input) - 1) {
    got = recv(fd, input + used, sizeof(input) - 1 - used, 0);
    if (got <= 0)
      break;
    used += (size_t)got;
    input[used] = '\0';
    body = strstr(input, "\r\n\r\n");
  }
  assert(body != NULL);
  assert(strstr(input, "POST /duplex HTTP/1.1\r\n") != NULL);
  assert(strstr(input, "Content-Length: 8\r\n") != NULL ||
      strstr(input, "content-length: 8\r\n") != NULL);
  body += 4;
  body_size = used - (size_t)(body - input);
  while (body_size < 4) {
    got = recv(fd, input + used, sizeof(input) - 1 - used, 0);
    if (got <= 0)
      break;
    used += (size_t)got;
    body_size += (size_t)got;
  }
  if (body_size >= 4 && memcmp(body, "ABCD", 4) == 0) {
    server->first_body_seen = 1;
    assert(send(fd, first_reply, sizeof(first_reply) - 1, 0) ==
        (ssize_t)(sizeof(first_reply) - 1));
  }
  while (body_size < 8) {
    got = recv(fd, input + used, sizeof(input) - 1 - used, 0);
    if (got <= 0)
      break;
    used += (size_t)got;
    body_size += (size_t)got;
  }
  if (body_size >= 8 && memcmp(body + 4, "EFGH", 4) == 0) {
    server->second_body_seen = 1;
    (void)send(fd, last_reply, sizeof(last_reply) - 1, MSG_NOSIGNAL);
  }
  assert(close(fd) == 0);
  return NULL;
}

static void
start_server(struct duplex_server *server)
{
  struct sockaddr_in addr;
  socklen_t size;

  memset(server, 0, sizeof(*server));
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
  assert(pthread_create(&server->thread, NULL, server_main, server) == 0);
}

static size_t
upload(char *buffer, size_t size, size_t items, void *arg)
{
  struct transfer_state *state;

  state = (struct transfer_state *)arg;
  assert(size * items >= 4);
  if (!state->first_sent) {
    memcpy(buffer, "ABCD", 4);
    state->first_sent = 1;
    return 4;
  }
  if (!state->allow_second)
    return CURL_READFUNC_PAUSE;
  if (state->allow_second == 2)
    return 0;
  memcpy(buffer, "EFGH", 4);
  state->allow_second = 2;
  return 4;
}

static size_t
download(char *buffer, size_t size, size_t items, void *arg)
{
  struct transfer_state *state;
  size_t amount;

  state = (struct transfer_state *)arg;
  amount = size * items;
  assert(amount <= sizeof(state->response) - state->response_size);
  memcpy(state->response + state->response_size, buffer, amount);
  state->response_size += amount;
  if (state->response_size >= 4 && state->allow_second == 0)
    state->saw_response_before_resume = 1;
  return amount;
}

int
main(void)
{
  struct duplex_server server;
  struct transfer_state state;
  struct curl_slist *headers;
  CURLM *multi;
  CURL *easy;
  CURLMsg *msg;
  char url[128];
  int running;
  int attempt;
  int numfds;
  int pending;
  int result;

  start_server(&server);
  memset(&state, 0, sizeof(state));
  assert(curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
  multi = curl_multi_init();
  easy = curl_easy_init();
  assert(multi != NULL && easy != NULL);
  headers = NULL;
  headers = curl_slist_append(headers, "Expect:");
  assert(headers != NULL);
  assert(snprintf(url, sizeof(url), "http://127.0.0.1:%u/duplex",
      (unsigned)server.port) > 0);
  assert(curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "POST") == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_INFILESIZE_LARGE,
      (curl_off_t)8) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_READFUNCTION, upload) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_READDATA, &state) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, download) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &state) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,
      CURL_HTTP_VERSION_1_1) == CURLE_OK);
  assert(curl_multi_add_handle(multi, easy) == CURLM_OK);
  assert(curl_multi_perform(multi, &running) == CURLM_OK);
  for (attempt = 0; attempt < 30 && running > 0 &&
      state.response_size < 4; attempt++) {
    assert(curl_multi_poll(multi, NULL, 0, 100, &numfds) == CURLM_OK);
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
  }
  state.allow_second = 1;
  assert(curl_easy_pause(easy, CURLPAUSE_CONT) == CURLE_OK);
  for (attempt = 0; attempt < 60 && running > 0; attempt++) {
    assert(curl_multi_poll(multi, NULL, 0, 100, &numfds) == CURLM_OK);
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
  }
  msg = curl_multi_info_read(multi, &pending);
  result = msg != NULL ? (int)msg->data.result : -1;
  assert(curl_multi_remove_handle(multi, easy) == CURLM_OK);
  curl_easy_cleanup(easy);
  curl_slist_free_all(headers);
  assert(curl_multi_cleanup(multi) == CURLM_OK);
  curl_global_cleanup();
  assert(pthread_join(server.thread, NULL) == 0);
  assert(close(server.listener) == 0);
  fprintf(stderr,
      "curl duplex: early=%d response=%lu first_upload=%d second_upload=%d "
      "running=%d result=%d\n",
      state.saw_response_before_resume, (unsigned long)state.response_size,
      server.first_body_seen, server.second_body_seen, running, result);
  return state.saw_response_before_resume &&
      state.response_size == 8 &&
      memcmp(state.response, "pongdone", 8) == 0 &&
      server.first_body_seen && server.second_body_seen &&
      running == 0 && result == CURLE_OK
      ? 0 : 1;
}
