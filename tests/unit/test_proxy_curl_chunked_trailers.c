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

struct trailer_server {
  int listener;
  unsigned short port;
  pthread_t thread;
  int first_chunk_seen;
  int final_chunk_seen;
};

struct trailer_state {
  int first_sent;
  int allow_second;
  int second_sent;
  int trailer_called;
  int saw_early_response;
  int saw_response_trailer;
  char response[8];
  size_t response_length;
};

static void *server_main(void *arg) {
  static const char first_reply[] =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
      "Trailer: X-Final\r\nConnection: close\r\n\r\n4\r\npong\r\n";
  static const char last_reply[] = "4\r\ndone\r\n0\r\nX-Final: yes\r\n\r\n";
  struct trailer_server *server;
  struct timeval timeout;
  char input[2048];
  char *body;
  size_t used;
  ssize_t got;
  int fd;

  server = (struct trailer_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0);
  used = 0;
  body = NULL;
  while (body == NULL && used < sizeof(input) - 1) {
    got = recv(fd, input + used, sizeof(input) - 1 - used, 0);
    assert(got > 0);
    used += (size_t)got;
    input[used] = '\0';
    body = strstr(input, "\r\n\r\n");
  }
  assert(body != NULL);
  assert(strstr(input, "POST /trailers HTTP/1.1\r\n") != NULL);
  assert(strstr(input, "Transfer-Encoding: chunked\r\n") != NULL);
  assert(strstr(input, "Trailer: X-Trace\r\n") != NULL);
  assert(strstr(input, "Expect: 100-continue\r\n") == NULL);
  body += 4;
  while (used - (size_t)(body - input) < 9) {
    got = recv(fd, input + used, sizeof(input) - 1 - used, 0);
    assert(got > 0);
    used += (size_t)got;
    input[used] = '\0';
  }
  assert(memcmp(body, "4\r\nABCD\r\n", 9) == 0);
  server->first_chunk_seen = 1;
  assert(send(fd, first_reply, sizeof(first_reply) - 1, MSG_NOSIGNAL) ==
         (ssize_t)(sizeof(first_reply) - 1));
  while (strstr(body, "0\r\nX-Trace: done\r\n\r\n") == NULL &&
         used < sizeof(input) - 1) {
    got = recv(fd, input + used, sizeof(input) - 1 - used, 0);
    assert(got > 0);
    used += (size_t)got;
    input[used] = '\0';
  }
  assert(strcmp(body, "4\r\nABCD\r\n4\r\nEFGH\r\n0\r\n"
                      "X-Trace: done\r\n\r\n") == 0);
  server->final_chunk_seen = 1;
  assert(send(fd, last_reply, sizeof(last_reply) - 1, MSG_NOSIGNAL) ==
         (ssize_t)(sizeof(last_reply) - 1));
  assert(close(fd) == 0);
  return NULL;
}

static void start_server(struct trailer_server *server) {
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

static size_t upload(char *buffer, size_t size, size_t items, void *arg) {
  struct trailer_state *state;

  state = (struct trailer_state *)arg;
  assert(size * items >= 4);
  if (!state->first_sent) {
    memcpy(buffer, "ABCD", 4);
    state->first_sent = 1;
    return 4;
  }
  if (!state->allow_second)
    return CURL_READFUNC_PAUSE;
  if (state->second_sent)
    return 0;
  memcpy(buffer, "EFGH", 4);
  state->second_sent = 1;
  return 4;
}

static int trailer(struct curl_slist **list, void *arg) {
  struct trailer_state *state;

  state = (struct trailer_state *)arg;
  assert(state->second_sent);
  state->trailer_called++;
  *list = curl_slist_append(NULL, "X-Trace: done");
  assert(*list != NULL);
  return CURL_TRAILERFUNC_OK;
}

static size_t header(char *buffer, size_t size, size_t items, void *arg) {
  struct trailer_state *state;
  size_t amount;

  state = (struct trailer_state *)arg;
  amount = size * items;
  if (amount == sizeof("X-Final: yes\r\n") - 1 &&
      memcmp(buffer, "X-Final: yes\r\n", amount) == 0)
    state->saw_response_trailer = 1;
  return amount;
}

static size_t download(char *buffer, size_t size, size_t items, void *arg) {
  struct trailer_state *state;
  size_t amount;

  state = (struct trailer_state *)arg;
  amount = size * items;
  assert(amount <= sizeof(state->response) - state->response_length);
  memcpy(state->response + state->response_length, buffer, amount);
  state->response_length += amount;
  if (state->response_length >= 4 && !state->allow_second)
    state->saw_early_response = 1;
  return amount;
}

int main(void) {
  struct trailer_server server;
  struct trailer_state state;
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
  headers = curl_slist_append(headers, "Trailer: X-Trace");
  assert(headers != NULL);
  assert(snprintf(url, sizeof(url), "http://127.0.0.1:%u/trailers",
                  (unsigned)server.port) > 0);
  assert(curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "POST") == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_INFILESIZE_LARGE, (curl_off_t)-1) ==
         CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_READFUNCTION, upload) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_READDATA, &state) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_TRAILERFUNCTION, trailer) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_TRAILERDATA, &state) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, header) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_HEADERDATA, &state) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, download) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &state) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1) ==
         CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK);
  assert(curl_multi_add_handle(multi, easy) == CURLM_OK);
  assert(curl_multi_perform(multi, &running) == CURLM_OK);
  for (attempt = 0; attempt < 50 && running > 0 && state.response_length < 4;
       attempt++) {
    assert(curl_multi_poll(multi, NULL, 0, 100, &numfds) == CURLM_OK);
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
  }
  assert(state.saw_early_response);
  state.allow_second = 1;
  assert(curl_easy_pause(easy, CURLPAUSE_CONT) == CURLE_OK);
  for (attempt = 0; attempt < 70 && running > 0; attempt++) {
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
          "curl trailers: early=%d upload=%d/%d trailer=%d "
          "response=%lu response_trailer=%d running=%d result=%d\n",
          state.saw_early_response, server.first_chunk_seen,
          server.final_chunk_seen, state.trailer_called,
          (unsigned long)state.response_length, state.saw_response_trailer,
          running, result);
  return state.saw_early_response && server.first_chunk_seen &&
                 server.final_chunk_seen && state.trailer_called == 1 &&
                 state.response_length == sizeof(state.response) &&
                 memcmp(state.response, "pongdone", sizeof(state.response)) ==
                     0 &&
                 state.saw_response_trailer && running == 0 &&
                 result == CURLE_OK
             ? 0
             : 1;
}
