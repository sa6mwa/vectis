#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <vectis/proxy.h>
#include <vectis/vectis.h>

#define TEST_CONNECTIONS 16
#define TEST_FRAME_SIZE (4u * 1024u * 1024u)
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_ASAN_ENABLED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(TEST_ASAN_ENABLED)
#define TEST_MAX_RSS_DELTA_KB (TEST_CONNECTIONS * 16384u)
#else
#define TEST_MAX_RSS_DELTA_KB (TEST_CONNECTIONS * 4096u)
#endif

static const char ws_request[] =
    "GET /proxy/ws HTTP/1.1\r\nHost: localhost\r\n"
    "Connection: Upgrade\r\nUpgrade: websocket\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
static const char http_request[] =
    "GET /proxy/ws HTTP/1.1\r\nHost: localhost\r\n\r\n";
static const char ws_response[] =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Connection: Upgrade\r\nUpgrade: websocket\r\n"
    "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
static const unsigned char frame_header[] = {0x82u, 0x7fu, 0u,    0u, 0u,
                                             0u,    0u,    0x40u, 0u, 0u};

typedef struct test_probe {
  volatile pid_t worker_pid;
  volatile unsigned long baseline_rss_kb;
  volatile unsigned baseline_fds;
} test_probe;

typedef struct test_origin {
  int listener;
  int clients[TEST_CONNECTIONS + 1];
  unsigned short port;
  pthread_t thread;
  volatile unsigned accepted;
  volatile int release;
  volatile int producer_done;
  volatile size_t produced;
  int filled;
} test_origin;

static unsigned long process_rss_kb(pid_t pid) {
  char path[64];
  char line[256];
  unsigned long amount;
  FILE *file;

  assert(snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid) > 0);
  file = fopen(path, "r");
  assert(file != NULL);
  amount = 0u;
  while (fgets(line, sizeof(line), file) != NULL) {
    if (sscanf(line, "VmRSS: %lu kB", &amount) == 1)
      break;
  }
  assert(fclose(file) == 0);
  assert(amount != 0u);
  return amount;
}

static unsigned process_fd_count(pid_t pid) {
  char path[64];
  struct dirent *entry;
  DIR *directory;
  unsigned count;

  assert(snprintf(path, sizeof(path), "/proc/%ld/fd", (long)pid) > 0);
  directory = opendir(path);
  assert(directory != NULL);
  count = 0u;
  while ((entry = readdir(directory)) != NULL) {
    if (entry->d_name[0] != '.')
      ++count;
  }
  assert(closedir(directory) == 0);
  return count;
}

static vectis_status capture_worker(const vectis_proxy_inbound *inbound,
                                    vectis_proxy_local_response *response,
                                    void *userdata, vectis_error *error) {
  test_probe *probe;

  (void)inbound;
  (void)response;
  (void)error;
  probe = (test_probe *)userdata;
  if (probe->worker_pid == 0) {
    probe->baseline_rss_kb = process_rss_kb(getpid());
    probe->baseline_fds = process_fd_count(getpid());
    __sync_synchronize();
    probe->worker_pid = getpid();
  }
  return VECTIS_OK;
}

static void send_all(int fd, const char *data, size_t length) {
  ssize_t amount;

  while (length != 0u) {
    amount = send(fd, data, length, 0);
    assert(amount > 0);
    data += (size_t)amount;
    length -= (size_t)amount;
  }
}

static unsigned short listen_port(int *listener) {
  struct sockaddr_in address;
  socklen_t length;

  *listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(*listener >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(*listener, (struct sockaddr *)&address, sizeof(address)) == 0);
  assert(listen(*listener, TEST_CONNECTIONS + 1) == 0);
  length = sizeof(address);
  assert(getsockname(*listener, (struct sockaddr *)&address, &length) == 0);
  return ntohs(address.sin_port);
}

static void produce_frames(test_origin *origin) {
  unsigned char payload[4096];
  size_t offsets[TEST_CONNECTIONS];
  const unsigned char *data;
  size_t length;
  ssize_t amount;
  unsigned idle;
  int i;
  int active;
  int progress;

  memset(payload, 'B', sizeof(payload));
  memset(offsets, 0, sizeof(offsets));
  idle = 0u;
  while (idle < 200u) {
    active = 0;
    progress = 0;
    for (i = 0; i < TEST_CONNECTIONS; ++i) {
      if (offsets[i] == sizeof(frame_header) + TEST_FRAME_SIZE)
        continue;
      active = 1;
      if (offsets[i] < sizeof(frame_header)) {
        data = frame_header + offsets[i];
        length = sizeof(frame_header) - offsets[i];
      } else {
        data = payload;
        length = sizeof(payload);
        if (length > sizeof(frame_header) + TEST_FRAME_SIZE - offsets[i])
          length = sizeof(frame_header) + TEST_FRAME_SIZE - offsets[i];
      }
      amount = send(origin->clients[i], data, length, MSG_DONTWAIT);
      if (amount > 0) {
        if (offsets[i] >= sizeof(frame_header))
          (void)__sync_add_and_fetch(&origin->produced, (size_t)amount);
        offsets[i] += (size_t)amount;
        progress = 1;
      } else {
        assert(amount == -1);
        assert(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
      }
    }
    if (!active)
      break;
    if (progress)
      idle = 0u;
    else {
      ++idle;
      usleep(1000u);
    }
  }
  (void)__sync_lock_test_and_set(&origin->producer_done, 1);
}

static void *origin_main(void *userdata) {
  test_origin *origin;
  char request[2048];
  size_t used;
  ssize_t amount;
  int i;

  origin = (test_origin *)userdata;
  for (i = 0; i < TEST_CONNECTIONS + 1; ++i) {
    origin->clients[i] = accept(origin->listener, NULL, NULL);
    assert(origin->clients[i] >= 0);
    used = 0u;
    request[0] = '\0';
    while (strstr(request, "\r\n\r\n") == NULL) {
      assert(used < sizeof(request) - 1u);
      amount = recv(origin->clients[i], request + used,
                    sizeof(request) - used - 1u, 0);
      assert(amount > 0);
      used += (size_t)amount;
      request[used] = '\0';
    }
    assert(strstr(request, "GET /proxy/ws HTTP/1.1\r\n") == request);
    assert(strstr(request, "Upgrade: websocket\r\n") != NULL ||
           strstr(request, "upgrade: websocket\r\n") != NULL);
    send_all(origin->clients[i], ws_response, sizeof(ws_response) - 1u);
    (void)__sync_add_and_fetch(&origin->accepted, 1u);
    if (i == TEST_CONNECTIONS - 1 && origin->filled)
      produce_frames(origin);
  }
  while (__sync_fetch_and_add(&origin->release, 0) == 0)
    usleep(1000u);
  for (i = 0; i < TEST_CONNECTIONS + 1; ++i)
    assert(close(origin->clients[i]) == 0);
  return NULL;
}

static int connect_app(unsigned short port, int websocket, int filled) {
  struct sockaddr_in address;
  struct timeval timeout;
  int receive_buffer;
  int attempt;
  int fd;

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  for (attempt = 0; attempt < 100; ++attempt) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    if (websocket && filled) {
      receive_buffer = 4096;
      assert(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                        sizeof(receive_buffer)) == 0);
    }
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0)
      break;
    assert(close(fd) == 0);
    usleep(10000u);
  }
  assert(attempt < 100);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0);
  if (websocket)
    send_all(fd, ws_request, sizeof(ws_request) - 1u);
  else
    send_all(fd, http_request, sizeof(http_request) - 1u);
  return fd;
}

static void read_head(int fd, char *response, size_t capacity) {
  size_t used;
  ssize_t amount;

  used = 0u;
  response[0] = '\0';
  while (strstr(response, "\r\n\r\n") == NULL) {
    assert(used < capacity - 1u);
    amount = recv(fd, response + used, 1u, 0);
    assert(amount == 1);
    ++used;
    response[used] = '\0';
  }
}

int main(void) {
  test_origin origin;
  test_probe *probe;
  vectis_proxy_route_config proxy;
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  struct pollfd pending;
  unsigned short app_port;
  char target[128];
  char response[2048];
  int clients[TEST_CONNECTIONS];
  int overflow;
  int replacement;
  int i;
  unsigned long at_headers;
  unsigned long after_warmup;
  unsigned long peak;
  unsigned long later;
  unsigned long after_close;
  unsigned at_headers_fds;
  unsigned after_close_fds;

  (void)signal(SIGPIPE, SIG_IGN);
  probe = mmap(NULL, sizeof(*probe), PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(probe != MAP_FAILED);
  memset(probe, 0, sizeof(*probe));
  memset(&origin, 0, sizeof(origin));
  origin.filled = getenv("VECTIS_PROXY_WS_FILLED_BUFFER") != NULL;
  origin.port = listen_port(&origin.listener);
  app_port = listen_port(&overflow);
  assert(close(overflow) == 0);
  assert(snprintf(target, sizeof(target), "http://127.0.0.1:%u",
                  (unsigned)origin.port) > 0);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = app_port;
  config.server.worker_count = 1u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  vectis_proxy_route_config_init(&proxy);
  proxy.path = "/proxy/ws";
  proxy.methods = VECTIS_HTTP_METHODS_GET;
  proxy.target = target;
  proxy.buffer_limit_bytes = 1048576u;
  proxy.preflight = capture_worker;
  proxy.preflight_userdata = probe;
  assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
  assert(app->start(app, &error) == VECTIS_OK);
  assert(pthread_create(&origin.thread, NULL, origin_main, &origin) == 0);

  for (i = 0; i < TEST_CONNECTIONS; ++i) {
    clients[i] = connect_app(app_port, 1, origin.filled);
    read_head(clients[i], response, sizeof(response));
    assert(strstr(response, "HTTP/1.1 101 Switching Protocols\r\n") ==
           response);
  }
  assert(probe->worker_pid > 0);
  assert(__sync_fetch_and_add(&origin.accepted, 0u) == TEST_CONNECTIONS);
  if (origin.filled) {
    for (i = 0; i < 500; ++i) {
      if (__sync_fetch_and_add(&origin.producer_done, 0) != 0)
        break;
      usleep(10000u);
    }
    assert(i < 500);
  }
  at_headers = process_rss_kb(probe->worker_pid);
  at_headers_fds = process_fd_count(probe->worker_pid);
  peak = at_headers;
  after_warmup = at_headers;
  for (i = 0; i < 200; ++i) {
    unsigned long current;

    usleep(20000u);
    current = process_rss_kb(probe->worker_pid);
    if (current > peak)
      peak = current;
    if (i == 99)
      after_warmup = current;
  }
  later = process_rss_kb(probe->worker_pid);
  fprintf(stderr,
          "production websocket worker: baseline=%luKB headers=%luKB "
          "warmup=%luKB peak=%luKB later=%luKB fds=%u,%u produced=%lu\n",
          probe->baseline_rss_kb, at_headers, after_warmup, peak, later,
          probe->baseline_fds, at_headers_fds, (unsigned long)origin.produced);
  assert(peak <= probe->baseline_rss_kb + TEST_MAX_RSS_DELTA_KB);
  assert(later <= after_warmup + 4096u);
  assert(at_headers_fds <= probe->baseline_fds + 64u);
  if (origin.filled) {
    assert(__sync_fetch_and_add(&origin.producer_done, 0) == 1);
    assert(__sync_fetch_and_add(&origin.produced, 0u) >=
           (size_t)TEST_CONNECTIONS * 1048576u);
  }

  overflow = connect_app(app_port, 0, origin.filled);
  read_head(overflow, response, sizeof(response));
  assert(strstr(response, "HTTP/1.1 503 Service Unavailable\r\n") == response);
  assert(close(overflow) == 0);
  overflow = connect_app(app_port, 1, origin.filled);
  read_head(overflow, response, sizeof(response));
  assert(strstr(response, "HTTP/1.1 503 Service Unavailable\r\n") == response);
  assert(close(overflow) == 0);
  pending.fd = origin.listener;
  pending.events = POLLIN;
  pending.revents = 0;
  assert(poll(&pending, 1u, 100) == 0);

  for (i = 0; i < TEST_CONNECTIONS; ++i)
    assert(close(clients[i]) == 0);
  for (i = 0; i < 200; ++i) {
    if (process_fd_count(probe->worker_pid) <= at_headers_fds - 16u)
      break;
    usleep(10000u);
  }
  assert(i < 200);
  replacement = connect_app(app_port, 1, origin.filled);
  read_head(replacement, response, sizeof(response));
  assert(strstr(response, "HTTP/1.1 101 Switching Protocols\r\n") == response);
  assert(__sync_fetch_and_add(&origin.accepted, 0u) == TEST_CONNECTIONS + 1u);
  assert(close(replacement) == 0);
  (void)__sync_lock_test_and_set(&origin.release, 1);
  assert(pthread_join(origin.thread, NULL) == 0);
  usleep(100000u);
  after_close = process_rss_kb(probe->worker_pid);
  after_close_fds = process_fd_count(probe->worker_pid);
  fprintf(stderr, "production websocket worker cleanup: rss=%luKB fds=%u\n",
          after_close, after_close_fds);
  assert(after_close <= probe->baseline_rss_kb + TEST_MAX_RSS_DELTA_KB);
  assert(after_close_fds <= probe->baseline_fds + 16u);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  assert(close(origin.listener) == 0);
  assert(munmap(probe, sizeof(*probe)) == 0);
  return 0;
}
