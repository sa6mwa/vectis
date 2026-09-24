#include "vectis_proxy_curl.h"

#include <arpa/inet.h>
#include <assert.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <kore/http.h>
#include <kore/kore.h>
#include <vectis/vectis.h>

struct pool_observation {
  volatile unsigned submitted;
  volatile unsigned completed;
  volatile unsigned failed;
  volatile unsigned bytes;
  volatile unsigned max_active;
  volatile unsigned rejected;
  volatile unsigned cancelled;
};

static struct pool_observation *observation;
static unsigned short server_port;
static unsigned short stalled_port;
static char received[2][16];
static size_t received_size[2];
static vectis_proxy_curl_transfer *stalled[16];

extern void vectis_kore_set_prebody_probe(int (*probe)(struct http_request *,
                                                       const void *, size_t));

static size_t collect_body(char *bytes, size_t size, size_t count, void *arg) {
  size_t index;
  size_t length;

  index = (size_t)(uintptr_t)arg;
  length = size * count;
  if (index >= 2u || length > sizeof(received[index]) - received_size[index]) {
    return 0u;
  }
  memcpy(received[index] + received_size[index], bytes, length);
  received_size[index] += length;
  return length;
}

static void transfer_done(CURL *easy, CURLcode result, void *userdata) {
  size_t index;
  long version;

  index = (size_t)(uintptr_t)userdata;
  version = 0L;
  if (result != CURLE_OK || index >= 2u || received_size[index] != 4u ||
      memcmp(received[index], "pong", 4u) != 0 ||
      curl_easy_getinfo(easy, CURLINFO_HTTP_VERSION, &version) != CURLE_OK ||
      version != CURL_HTTP_VERSION_1_1) {
    observation->failed++;
  } else {
    observation->bytes += (unsigned)received_size[index];
  }
  observation->completed++;
}

static int trigger(struct http_request *request, const void *data, size_t len) {
  vectis_proxy_curl_transfer *transfer;
  vectis_error error;
  char url[128];
  CURL *easy;
  size_t i;

  (void)data;
  (void)len;
  if (strcmp(request->path, "/cancel") == 0) {
    for (i = 0u; i < 16u; ++i) {
      assert(stalled[i] != NULL);
      vectis_proxy_curl_cancel(stalled[i]);
      stalled[i] = NULL;
      observation->cancelled++;
    }
    assert(vectis_proxy_curl_active_count() == 0u);
    return KORE_RESULT_OK;
  }
  if (strcmp(request->path, "/saturate") == 0) {
    assert(snprintf(url, sizeof(url), "http://127.0.0.1:%u/hold",
                    (unsigned)stalled_port) > 0);
    for (i = 0u; i < 16u; ++i) {
      easy = curl_easy_init();
      assert(easy != NULL);
      assert(curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK);
      assert(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK);
      assert(vectis_proxy_curl_submit(easy, 0, transfer_done, NULL, &stalled[i],
                                      &error) == VECTIS_OK);
      assert(stalled[i] != NULL);
    }
    assert(vectis_proxy_curl_active_count() == 16u);
    observation->max_active = 16u;
    easy = curl_easy_init();
    assert(easy != NULL);
    assert(curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK);
    transfer = NULL;
    assert(vectis_proxy_curl_submit(easy, 0, transfer_done, NULL, &transfer,
                                    &error) == VECTIS_ERR_STATE);
    assert(transfer == NULL);
    curl_easy_cleanup(easy);
    observation->rejected++;
    return KORE_RESULT_OK;
  }
  if (strcmp(request->path, "/trigger") != 0) {
    return KORE_RESULT_OK;
  }
  received_size[0] = 0u;
  received_size[1] = 0u;
  assert(snprintf(url, sizeof(url), "http://127.0.0.1:%u/origin",
                  (unsigned)server_port) > 0);
  for (i = 0u; i < 2u; ++i) {
    easy = curl_easy_init();
    assert(easy != NULL);
    assert(curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK);
    assert(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, collect_body) ==
           CURLE_OK);
    assert(curl_easy_setopt(easy, CURLOPT_WRITEDATA, (void *)(uintptr_t)i) ==
           CURLE_OK);
    assert(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK);
    assert(vectis_proxy_curl_submit(easy, (int)i, transfer_done,
                                    (void *)(uintptr_t)i, &transfer,
                                    &error) == VECTIS_OK);
    assert(transfer != NULL);
    observation->submitted++;
  }
  if (vectis_proxy_curl_active_count() > observation->max_active) {
    observation->max_active = (unsigned)vectis_proxy_curl_active_count();
  }
  return KORE_RESULT_OK;
}

static vectis_status reply(vectis_app *app, vectis_request *request,
                           vectis_response *response, void *userdata,
                           vectis_error *error) {
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 200, "text/plain", "pong", error);
}

static unsigned short available_port(void) {
  struct sockaddr_in address;
  socklen_t length;
  unsigned short port;
  int fd;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
  length = sizeof(address);
  assert(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
  port = ntohs(address.sin_port);
  assert(close(fd) == 0);
  return port;
}

static void send_trigger(const char *path) {
  struct sockaddr_in address;
  struct timeval timeout;
  char reply_bytes[512];
  char message[256];
  ssize_t got;
  int fd;
  int attempt;

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(server_port);
  for (attempt = 0; attempt < 100; ++attempt) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
      break;
    }
    assert(close(fd) == 0);
    usleep(10000u);
  }
  assert(attempt < 100);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0);
  assert(snprintf(
             message, sizeof(message),
             "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
             path) > 0);
  assert(send(fd, message, strlen(message), 0) == (ssize_t)strlen(message));
  got = recv(fd, reply_bytes, sizeof(reply_bytes) - 1u, 0);
  assert(got > 0);
  reply_bytes[got] = '\0';
  assert(strstr(reply_bytes, "200 OK") != NULL);
  assert(close(fd) == 0);
}

int main(void) {
  vectis_app_config config;
  vectis_route_config route;
  vectis_error error;
  vectis_app *app;
  struct sockaddr_in stalled_address;
  socklen_t stalled_length;
  int stalled_fd;
  int i;

  observation = mmap(NULL, sizeof(*observation), PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(observation != MAP_FAILED);
  memset(observation, 0, sizeof(*observation));
  assert(curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
  server_port = available_port();
  stalled_fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(stalled_fd >= 0);
  memset(&stalled_address, 0, sizeof(stalled_address));
  stalled_address.sin_family = AF_INET;
  stalled_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(stalled_fd, (struct sockaddr *)&stalled_address,
              sizeof(stalled_address)) == 0);
  assert(listen(stalled_fd, 32) == 0);
  stalled_length = sizeof(stalled_address);
  assert(getsockname(stalled_fd, (struct sockaddr *)&stalled_address,
                     &stalled_length) == 0);
  stalled_port = ntohs(stalled_address.sin_port);
  vectis_kore_set_prebody_probe(trigger);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = server_port;
  config.server.worker_count = 1u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/trigger", reply, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/origin", reply, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/saturate", reply, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/cancel", reply, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  if (app->start(app, &error) != VECTIS_OK) {
    fprintf(stderr, "proxy curl pool startup: %s\n", error.message);
    assert(0);
  }
  send_trigger("/trigger");
  for (i = 0; i < 500 && observation->completed < 2u; ++i) {
    usleep(10000u);
  }
  assert(observation->submitted == 2u);
  assert(observation->completed == 2u);
  assert(observation->failed == 0u);
  assert(observation->bytes == 8u);
  assert(observation->max_active == 2u);
  send_trigger("/saturate");
  assert(observation->rejected == 1u);
  assert(observation->max_active == 16u);
  send_trigger("/cancel");
  assert(observation->cancelled == 16u);
  send_trigger("/trigger");
  for (i = 0; i < 500 && observation->completed < 4u; ++i) {
    usleep(10000u);
  }
  assert(observation->completed == 4u);
  assert(observation->failed == 0u);
  assert(observation->bytes == 16u);
  send_trigger("/saturate");
  assert(observation->rejected == 2u);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  vectis_kore_set_prebody_probe(NULL);
  curl_global_cleanup();
  assert(close(stalled_fd) == 0);
  assert(munmap(observation, sizeof(*observation)) == 0);
  return 0;
}
