#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <nghttp2/nghttp2.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
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
#define TEST_MIXED_CONNECTIONS (TEST_CONNECTIONS / 2)
#define TEST_BODY_SIZE (64u * 1024u * 1024u)
#define TEST_WS_FRAME_SIZE (4u * 1024u * 1024u)
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_ASAN_ENABLED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(TEST_ASAN_ENABLED)
/* ASan keeps redzones and quarantined allocations resident after pause. */
#define TEST_MAX_RSS_DELTA_KB (TEST_CONNECTIONS * 16384u)
#else
#define TEST_MAX_RSS_DELTA_KB (TEST_CONNECTIONS * 4096u)
#endif

typedef struct test_probe {
  volatile pid_t worker_pid;
  volatile unsigned long baseline_rss_kb;
  volatile unsigned baseline_fds;
  volatile unsigned accepted;
  volatile unsigned negotiated_h2;
  volatile unsigned requests;
  volatile size_t generated;
} test_probe;

typedef struct test_origin {
  SSL_CTX *ctx;
  int listener;
  unsigned short port;
  pthread_t thread;
  test_probe *probe;
  int connections;
} test_origin;

typedef struct test_ws_origin {
  int listener;
  int clients[TEST_MIXED_CONNECTIONS];
  unsigned short port;
  pthread_t thread;
  volatile unsigned accepted;
  volatile size_t produced;
  volatile int producer_done;
  volatile int release;
} test_ws_origin;

typedef struct test_connection {
  test_origin *origin;
  int fd;
  SSL *ssl;
  size_t generated;
} test_connection;

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

static EVP_PKEY *make_key(void) {
  EVP_PKEY_CTX *context;
  EVP_PKEY *key;

  context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
  assert(context != NULL);
  assert(EVP_PKEY_keygen_init(context) == 1);
  assert(EVP_PKEY_CTX_set_rsa_keygen_bits(context, 2048) == 1);
  key = NULL;
  assert(EVP_PKEY_keygen(context, &key) == 1);
  EVP_PKEY_CTX_free(context);
  return key;
}

static X509 *make_cert(EVP_PKEY *key) {
  X509V3_CTX extension_context;
  X509_EXTENSION *extension;
  X509_NAME *name;
  X509 *cert;

  cert = X509_new();
  assert(cert != NULL);
  assert(X509_set_version(cert, 2) == 1);
  assert(ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) == 1);
  assert(X509_gmtime_adj(X509_getm_notBefore(cert), -60) != NULL);
  assert(X509_gmtime_adj(X509_getm_notAfter(cert), 3600) != NULL);
  assert(X509_set_pubkey(cert, key) == 1);
  name = X509_get_subject_name(cert);
  assert(name != NULL);
  assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                    (const unsigned char *)"localhost", -1, -1,
                                    0) == 1);
  assert(X509_set_issuer_name(cert, name) == 1);
  X509V3_set_ctx(&extension_context, cert, cert, NULL, NULL, 0);
  extension = X509V3_EXT_conf_nid(NULL, &extension_context,
                                  NID_subject_alt_name, "DNS:localhost");
  assert(extension != NULL);
  assert(X509_add_ext(cert, extension, -1) == 1);
  X509_EXTENSION_free(extension);
  assert(X509_sign(cert, key, EVP_sha256()) > 0);
  return cert;
}

static int select_h2(SSL *ssl, const unsigned char **selected,
                     unsigned char *selected_length, const unsigned char *in,
                     unsigned int in_length, void *userdata) {
  test_origin *origin;
  unsigned int offset;
  unsigned int length;

  (void)ssl;
  origin = (test_origin *)userdata;
  offset = 0u;
  while (offset < in_length) {
    length = in[offset++];
    if (length > in_length - offset)
      break;
    if (length == 2u && memcmp(in + offset, "h2", 2u) == 0) {
      *selected = in + offset;
      *selected_length = 2u;
      (void)__sync_add_and_fetch(&origin->probe->negotiated_h2, 1u);
      return SSL_TLSEXT_ERR_OK;
    }
    offset += length;
  }
  return SSL_TLSEXT_ERR_NOACK;
}

static ssize_t send_h2(nghttp2_session *session, const uint8_t *data,
                       size_t length, int flags, void *userdata) {
  test_connection *connection;
  int sent;
  int error;

  (void)session;
  (void)flags;
  connection = (test_connection *)userdata;
  sent = SSL_write(connection->ssl, data, (int)length);
  if (sent > 0)
    return (ssize_t)sent;
  error = SSL_get_error(connection->ssl, sent);
  if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
    return NGHTTP2_ERR_WOULDBLOCK;
  return NGHTTP2_ERR_CALLBACK_FAILURE;
}

static ssize_t produce_body(nghttp2_session *session, int32_t stream_id,
                            uint8_t *data, size_t length, uint32_t *flags,
                            nghttp2_data_source *source, void *userdata) {
  test_connection *connection;
  size_t remaining;

  (void)session;
  (void)stream_id;
  (void)source;
  connection = (test_connection *)userdata;
  remaining = TEST_BODY_SIZE - connection->generated;
  if (length > remaining)
    length = remaining;
  memset(data, 'x', length);
  connection->generated += length;
  (void)__sync_fetch_and_add(&connection->origin->probe->generated, length);
  if (connection->generated == TEST_BODY_SIZE)
    *flags |= NGHTTP2_DATA_FLAG_EOF;
  return (ssize_t)length;
}

static int request_received(nghttp2_session *session,
                            const nghttp2_frame *frame, void *userdata) {
  static uint8_t status_name[] = ":status";
  static uint8_t status_value[] = "200";
  static uint8_t type_name[] = "content-type";
  static uint8_t type_value[] = "text/event-stream";
  static nghttp2_nv headers[] = {
      {status_name, status_value, 7u, 3u, NGHTTP2_NV_FLAG_NONE},
      {type_name, type_value, 12u, 17u, NGHTTP2_NV_FLAG_NONE}};
  nghttp2_data_provider provider;
  test_connection *connection;

  connection = (test_connection *)userdata;
  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST)
    return 0;
  (void)__sync_add_and_fetch(&connection->origin->probe->requests, 1u);
  memset(&provider, 0, sizeof(provider));
  provider.read_callback = produce_body;
  return nghttp2_submit_response(session, frame->hd.stream_id, headers,
                                 sizeof(headers) / sizeof(headers[0]),
                                 &provider);
}

static void *connection_main(void *userdata) {
  test_connection *connection;
  nghttp2_session_callbacks *callbacks;
  nghttp2_session *session;
  struct pollfd watch;
  unsigned char input[16384];
  ssize_t got;
  int flags;
  int ready;
  int result;
  int ssl_error;

  connection = (test_connection *)userdata;
  connection->ssl = SSL_new(connection->origin->ctx);
  assert(connection->ssl != NULL);
  assert(SSL_set_fd(connection->ssl, connection->fd) == 1);
  assert(SSL_accept(connection->ssl) == 1);
  flags = fcntl(connection->fd, F_GETFL, 0);
  assert(flags >= 0);
  assert(fcntl(connection->fd, F_SETFL, flags | O_NONBLOCK) == 0);
  SSL_set_mode(connection->ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  assert(nghttp2_session_callbacks_new(&callbacks) == 0);
  nghttp2_session_callbacks_set_send_callback(callbacks, send_h2);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
                                                       request_received);
  assert(nghttp2_session_server_new(&session, callbacks, connection) == 0);
  nghttp2_session_callbacks_del(callbacks);
  assert(nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, NULL, 0) == 0);
  for (;;) {
    watch.fd = connection->fd;
    watch.events = POLLIN | (nghttp2_session_want_write(session) ? POLLOUT : 0);
    watch.revents = 0;
    ready = poll(&watch, 1u, 1000);
    assert(ready >= 0);
    if (ready == 0)
      continue;
    if ((watch.revents & POLLIN) != 0) {
      for (;;) {
        got = SSL_read(connection->ssl, input, sizeof(input));
        if (got > 0) {
          assert(nghttp2_session_mem_recv(session, input, (size_t)got) == got);
          continue;
        }
        ssl_error = SSL_get_error(connection->ssl, (int)got);
        if (ssl_error == SSL_ERROR_WANT_READ ||
            ssl_error == SSL_ERROR_WANT_WRITE)
          break;
        goto done;
      }
    }
    if ((watch.revents & (POLLERR | POLLHUP)) != 0)
      break;
    result = nghttp2_session_send(session);
    if (result != 0 && result != NGHTTP2_ERR_WOULDBLOCK)
      break;
  }
done:
  nghttp2_session_del(session);
  SSL_free(connection->ssl);
  assert(close(connection->fd) == 0);
  free(connection);
  return NULL;
}

static void *origin_main(void *userdata) {
  test_origin *origin;
  test_connection *connection;
  pthread_t threads[TEST_CONNECTIONS];
  int i;

  origin = (test_origin *)userdata;
  for (i = 0; i < origin->connections; ++i) {
    connection = (test_connection *)calloc(1u, sizeof(*connection));
    assert(connection != NULL);
    connection->origin = origin;
    connection->fd = accept(origin->listener, NULL, NULL);
    assert(connection->fd >= 0);
    (void)__sync_add_and_fetch(&origin->probe->accepted, 1u);
    assert(pthread_create(&threads[i], NULL, connection_main, connection) == 0);
  }
  for (i = 0; i < origin->connections; ++i)
    assert(pthread_join(threads[i], NULL) == 0);
  return NULL;
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

static unsigned short unused_port(void) {
  unsigned short port;
  int listener;

  port = listen_port(&listener);
  assert(close(listener) == 0);
  return port;
}

static void send_all(int fd, const char *bytes, size_t length) {
  ssize_t amount;

  while (length != 0u) {
    amount = send(fd, bytes, length, 0);
    assert(amount > 0);
    bytes += (size_t)amount;
    length -= (size_t)amount;
  }
}

static void *ws_origin_main(void *userdata) {
  static const char response[] =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Connection: Upgrade\r\nUpgrade: websocket\r\n"
      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
  static const unsigned char frame_header[] = {0x82u, 0x7fu, 0u,    0u, 0u,
                                               0u,    0u,    0x40u, 0u, 0u};
  test_ws_origin *origin;
  char request[2048];
  char payload[4096];
  size_t offsets[TEST_MIXED_CONNECTIONS];
  const char *data;
  size_t length;
  size_t used;
  ssize_t amount;
  int progress;
  int active;
  int idle;
  int i;

  origin = (test_ws_origin *)userdata;
  memset(payload, 'w', sizeof(payload));
  memset(offsets, 0, sizeof(offsets));
  for (i = 0; i < TEST_MIXED_CONNECTIONS; ++i) {
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
    send_all(origin->clients[i], response, sizeof(response) - 1u);
    (void)__sync_add_and_fetch(&origin->accepted, 1u);
  }
  idle = 0;
  while (idle < 200) {
    active = 0;
    progress = 0;
    for (i = 0; i < TEST_MIXED_CONNECTIONS; ++i) {
      if (offsets[i] == sizeof(frame_header) + TEST_WS_FRAME_SIZE)
        continue;
      active = 1;
      if (offsets[i] < sizeof(frame_header)) {
        data = (const char *)frame_header + offsets[i];
        length = sizeof(frame_header) - offsets[i];
      } else {
        data = payload;
        length = sizeof(payload);
        if (length > sizeof(frame_header) + TEST_WS_FRAME_SIZE - offsets[i])
          length = sizeof(frame_header) + TEST_WS_FRAME_SIZE - offsets[i];
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
      idle = 0;
    else {
      ++idle;
      usleep(1000u);
    }
  }
  (void)__sync_lock_test_and_set(&origin->producer_done, 1);
  while (__sync_fetch_and_add(&origin->release, 0) == 0)
    usleep(1000u);
  for (i = 0; i < TEST_MIXED_CONNECTIONS; ++i)
    assert(close(origin->clients[i]) == 0);
  return NULL;
}

static int connect_app(unsigned short port, int websocket) {
  static const char http_request[] =
      "GET /proxy/h2 HTTP/1.1\r\nHost: localhost\r\n\r\n";
  static const char ws_request[] =
      "GET /proxy/ws HTTP/1.1\r\nHost: localhost\r\n"
      "Connection: Upgrade\r\nUpgrade: websocket\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
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
    receive_buffer = 4096;
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                      sizeof(receive_buffer)) == 0);
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

static void read_head(int fd, char *head, size_t capacity) {
  size_t used;
  ssize_t got;

  used = 0u;
  head[0] = '\0';
  while (strstr(head, "\r\n\r\n") == NULL) {
    assert(used < capacity - 1u);
    got = recv(fd, head + used, 1u, 0);
    assert(got == 1);
    ++used;
    head[used] = '\0';
  }
}

int main(void) {
  test_origin origin;
  test_ws_origin ws_origin;
  test_probe *probe;
  vectis_proxy_route_config proxy;
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  EVP_PKEY *key;
  X509 *cert;
  BIO *pem;
  BUF_MEM *pem_data;
  BIO *key_pem;
  BUF_MEM *key_pem_data;
  struct pollfd pending;
  unsigned short app_port;
  char *ca_pem;
  char *client_key_pem;
  char target[128];
  char ws_target[128];
  char head[2048];
  int clients[TEST_CONNECTIONS];
  int overflow;
  int i;
  unsigned long at_headers;
  unsigned long after_warmup;
  unsigned long later;
  unsigned long peak;
  unsigned long after_close;
  unsigned at_headers_fds;
  unsigned after_close_fds;
  size_t generated_at_headers;
  size_t generated_after_warmup;
  size_t generated_later;
  size_t buffer_limit;
  int mixed;
  int mtls;
  int h2_connections;

  (void)signal(SIGPIPE, SIG_IGN);
  mixed = getenv("VECTIS_PROXY_MIXED_MEMORY") != NULL;
  mtls = getenv("VECTIS_PROXY_H2_MTLS") != NULL;
  h2_connections = mixed ? TEST_MIXED_CONNECTIONS : TEST_CONNECTIONS;
  probe = mmap(NULL, sizeof(*probe), PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(probe != MAP_FAILED);
  memset(probe, 0, sizeof(*probe));
  key = make_key();
  cert = make_cert(key);
  pem = BIO_new(BIO_s_mem());
  assert(pem != NULL);
  assert(PEM_write_bio_X509(pem, cert) == 1);
  BIO_get_mem_ptr(pem, &pem_data);
  assert(pem_data != NULL);
  ca_pem = (char *)malloc(pem_data->length + 1u);
  assert(ca_pem != NULL);
  memcpy(ca_pem, pem_data->data, pem_data->length);
  ca_pem[pem_data->length] = '\0';
  key_pem = NULL;
  client_key_pem = NULL;
  if (mtls) {
    key_pem = BIO_new(BIO_s_mem());
    assert(key_pem != NULL);
    assert(PEM_write_bio_PrivateKey(key_pem, key, NULL, NULL, 0, NULL, NULL) ==
           1);
    BIO_get_mem_ptr(key_pem, &key_pem_data);
    assert(key_pem_data != NULL);
    client_key_pem = (char *)malloc(key_pem_data->length + 1u);
    assert(client_key_pem != NULL);
    memcpy(client_key_pem, key_pem_data->data, key_pem_data->length);
    client_key_pem[key_pem_data->length] = '\0';
  }
  memset(&origin, 0, sizeof(origin));
  origin.probe = probe;
  origin.connections = h2_connections;
  memset(&ws_origin, 0, sizeof(ws_origin));
  origin.ctx = SSL_CTX_new(TLS_server_method());
  assert(origin.ctx != NULL);
  assert(SSL_CTX_use_certificate(origin.ctx, cert) == 1);
  assert(SSL_CTX_use_PrivateKey(origin.ctx, key) == 1);
  if (mtls) {
    assert(X509_STORE_add_cert(SSL_CTX_get_cert_store(origin.ctx), cert) == 1);
    SSL_CTX_set_verify(origin.ctx,
                       SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
  }
  SSL_CTX_set_alpn_select_cb(origin.ctx, select_h2, &origin);
  origin.port = listen_port(&origin.listener);
  if (mixed)
    ws_origin.port = listen_port(&ws_origin.listener);
  app_port = unused_port();
  assert(snprintf(target, sizeof(target), "https://localhost:%u",
                  (unsigned)origin.port) > 0);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = app_port;
  config.server.worker_count = 1u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  vectis_proxy_route_config_init(&proxy);
  proxy.path = "/proxy/h2";
  proxy.methods = VECTIS_HTTP_METHODS_GET;
  proxy.target = target;
  proxy.tls_ca_pem = ca_pem;
  if (mtls) {
    proxy.tls_client_cert_pem = ca_pem;
    proxy.tls_client_key_pem = client_key_pem;
  }
  buffer_limit =
      getenv("VECTIS_PROXY_H2_LARGE_BUFFER") != NULL ? 1048576u : 8192u;
  proxy.buffer_limit_bytes = buffer_limit;
  proxy.preflight = capture_worker;
  proxy.preflight_userdata = probe;
  assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
  if (mixed) {
    assert(snprintf(ws_target, sizeof(ws_target), "http://127.0.0.1:%u",
                    (unsigned)ws_origin.port) > 0);
    vectis_proxy_route_config_init(&proxy);
    proxy.path = "/proxy/ws";
    proxy.methods = VECTIS_HTTP_METHODS_GET;
    proxy.target = ws_target;
    proxy.buffer_limit_bytes = 1048576u;
    assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
  }
  free(ca_pem);
  if (client_key_pem != NULL) {
    OPENSSL_cleanse(client_key_pem, strlen(client_key_pem));
    free(client_key_pem);
  }
  assert(app->start(app, &error) == VECTIS_OK);
  assert(pthread_create(&origin.thread, NULL, origin_main, &origin) == 0);
  if (mixed)
    assert(pthread_create(&ws_origin.thread, NULL, ws_origin_main,
                          &ws_origin) == 0);

  for (i = 0; i < h2_connections; ++i) {
    clients[i] = connect_app(app_port, 0);
    read_head(clients[i], head, sizeof(head));
    assert(strstr(head, "HTTP/1.1 200 ") == head);
    assert(strstr(head, "Transfer-Encoding: chunked\r\n") != NULL);
  }
  if (mixed) {
    for (i = h2_connections; i < TEST_CONNECTIONS; ++i) {
      clients[i] = connect_app(app_port, 1);
      read_head(clients[i], head, sizeof(head));
      assert(strstr(head, "HTTP/1.1 101 Switching Protocols\r\n") == head);
    }
    for (i = 0; i < 500; ++i) {
      if (__sync_fetch_and_add(&ws_origin.producer_done, 0) != 0)
        break;
      usleep(10000u);
    }
    assert(i < 500);
    assert(__sync_fetch_and_add(&ws_origin.accepted, 0u) ==
           TEST_MIXED_CONNECTIONS);
    assert(__sync_fetch_and_add(&ws_origin.produced, 0u) >=
           (size_t)TEST_MIXED_CONNECTIONS * 1048576u);
  }
  assert(probe->worker_pid > 0);
  assert(__sync_fetch_and_add(&probe->accepted, 0u) ==
         (unsigned)h2_connections);
  assert(__sync_fetch_and_add(&probe->negotiated_h2, 0u) ==
         (unsigned)h2_connections);
  assert(__sync_fetch_and_add(&probe->requests, 0u) ==
         (unsigned)h2_connections);
  at_headers = process_rss_kb(probe->worker_pid);
  at_headers_fds = process_fd_count(probe->worker_pid);
  generated_at_headers = __sync_fetch_and_add(&probe->generated, 0u);
  peak = at_headers;
  after_warmup = at_headers;
  generated_after_warmup = generated_at_headers;
  for (i = 0; i < 200; ++i) {
    unsigned long current;

    usleep(20000u);
    current = process_rss_kb(probe->worker_pid);
    if (current > peak)
      peak = current;
    if (i == 99) {
      after_warmup = current;
      generated_after_warmup = __sync_fetch_and_add(&probe->generated, 0u);
    }
  }
  later = process_rss_kb(probe->worker_pid);
  generated_later = __sync_fetch_and_add(&probe->generated, 0u);
  fprintf(stderr,
          "production %s worker: buffer=%lu baseline=%luKB headers=%luKB "
          "warmup=%luKB peak=%luKB later=%luKB fds=%u,%u "
          "generated=%lu,%lu,%lu ws=%lu\n",
          mixed ? "mixed h2/ws" : "h2", (unsigned long)buffer_limit,
          probe->baseline_rss_kb, at_headers, after_warmup, peak, later,
          probe->baseline_fds, at_headers_fds,
          (unsigned long)generated_at_headers,
          (unsigned long)generated_after_warmup, (unsigned long)generated_later,
          (unsigned long)(mixed ? ws_origin.produced : 0u));
  assert(at_headers <= probe->baseline_rss_kb + TEST_MAX_RSS_DELTA_KB);
  assert(peak <= probe->baseline_rss_kb + TEST_MAX_RSS_DELTA_KB);
  assert(later <= after_warmup + 4096u);
  assert(at_headers_fds <= probe->baseline_fds + 64u);
  assert(generated_later > (size_t)h2_connections * 1048576u);
  assert(generated_later < (size_t)h2_connections * TEST_BODY_SIZE / 4u);
  assert(generated_later - generated_after_warmup <=
         (size_t)h2_connections * 1048576u);

  overflow = connect_app(app_port, 0);
  read_head(overflow, head, sizeof(head));
  assert(strstr(head, "HTTP/1.1 503 Service Unavailable\r\n") == head);
  assert(close(overflow) == 0);
  if (mixed) {
    overflow = connect_app(app_port, 1);
    read_head(overflow, head, sizeof(head));
    assert(strstr(head, "HTTP/1.1 503 Service Unavailable\r\n") == head);
    assert(close(overflow) == 0);
  }
  pending.fd = origin.listener;
  pending.events = POLLIN;
  pending.revents = 0;
  assert(poll(&pending, 1u, 100) == 0);
  if (mixed) {
    pending.fd = ws_origin.listener;
    pending.revents = 0;
    assert(poll(&pending, 1u, 100) == 0);
  }

  for (i = 0; i < TEST_CONNECTIONS; ++i)
    assert(close(clients[i]) == 0);
  assert(pthread_join(origin.thread, NULL) == 0);
  if (mixed) {
    (void)__sync_lock_test_and_set(&ws_origin.release, 1);
    assert(pthread_join(ws_origin.thread, NULL) == 0);
  }
  usleep(100000u);
  after_close = process_rss_kb(probe->worker_pid);
  after_close_fds = process_fd_count(probe->worker_pid);
  fprintf(stderr, "production h2 worker cleanup: rss=%luKB fds=%u\n",
          after_close, after_close_fds);
  assert(after_close <= probe->baseline_rss_kb + TEST_MAX_RSS_DELTA_KB);
  assert(after_close_fds <= probe->baseline_fds + 16u);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  assert(close(origin.listener) == 0);
  if (mixed)
    assert(close(ws_origin.listener) == 0);
  SSL_CTX_free(origin.ctx);
  BIO_free(pem);
  BIO_free(key_pem);
  X509_free(cert);
  EVP_PKEY_free(key);
  assert(munmap(probe, sizeof(*probe)) == 0);
  return 0;
}
