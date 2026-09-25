#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <vectis/proxy.h>
#include <vectis/vectis.h>

#define WSS_PAYLOAD_BYTES (128u * 1024u)

typedef struct tls_origin {
  SSL_CTX *ctx;
  int listener;
  pthread_t thread;
  unsigned short port;
  int hello_count;
  int ws_offered_alpn;
  int http_offered_alpn;
} tls_origin;

static const char client_head[] =
    "GET /ws HTTP/1.1\r\nHost: localhost\r\n"
    "Connection: Upgrade\r\nUpgrade: websocket\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n\r\n";
static const char upstream_head[] =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Connection: Upgrade\r\nUpgrade: websocket\r\n"
    "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
static const unsigned char client_early[] = {
    0x81u, 0x82u, 0x11u, 0x22u, 0x33u, 0x44u, 'h' ^ 0x11, 'i' ^ 0x22};
static const unsigned char server_early[] = {0x81u, 0x02u, 'o', 'k'};
static const unsigned char client_big_head[] = {
    0x82u, 0xffu, 0u, 0u, 0u, 0u, 0u, 2u, 0u, 0u, 0x12u, 0x34u, 0x56u, 0x78u};
static const unsigned char server_big_head[] = {0x82u, 0x7fu, 0u, 0u, 0u,
                                                0u,    0u,    2u, 0u, 0u};
static const unsigned char mask[] = {0x12u, 0x34u, 0x56u, 0x78u};

static EVP_PKEY *make_key(void) {
  EVP_PKEY_CTX *ctx;
  EVP_PKEY *key;

  ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
  assert(ctx != NULL);
  assert(EVP_PKEY_keygen_init(ctx) == 1);
  assert(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) == 1);
  key = NULL;
  assert(EVP_PKEY_keygen(ctx, &key) == 1);
  EVP_PKEY_CTX_free(ctx);
  return key;
}

static X509 *make_cert(EVP_PKEY *key) {
  X509V3_CTX extension_ctx;
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
  X509V3_set_ctx(&extension_ctx, cert, cert, NULL, NULL, 0);
  extension = X509V3_EXT_conf_nid(NULL, &extension_ctx, NID_subject_alt_name,
                                  "DNS:localhost");
  assert(extension != NULL);
  assert(X509_add_ext(cert, extension, -1) == 1);
  X509_EXTENSION_free(extension);
  assert(X509_sign(cert, key, EVP_sha256()) > 0);
  return cert;
}

static int check_client_hello(SSL *ssl, int *alert, void *userdata) {
  tls_origin *origin;
  const unsigned char *extension;
  size_t length;

  (void)alert;
  origin = (tls_origin *)userdata;
  if (SSL_client_hello_get0_ext(
          ssl, TLSEXT_TYPE_application_layer_protocol_negotiation, &extension,
          &length)) {
    if (origin->hello_count < 2)
      origin->ws_offered_alpn = 1;
    else
      origin->http_offered_alpn = 1;
  }
  origin->hello_count++;
  return SSL_CLIENT_HELLO_SUCCESS;
}

static void ssl_write_all(SSL *ssl, const void *data, size_t length) {
  const unsigned char *bytes;
  int sent;

  bytes = (const unsigned char *)data;
  while (length != 0u) {
    sent = SSL_write(ssl, bytes, (int)length);
    assert(sent > 0);
    bytes += (size_t)sent;
    length -= (size_t)sent;
  }
}

static void ssl_read_exact(SSL *ssl, void *data, size_t length) {
  unsigned char *bytes;
  int got;

  bytes = (unsigned char *)data;
  while (length != 0u) {
    got = SSL_read(ssl, bytes, (int)length);
    assert(got > 0);
    bytes += (size_t)got;
    length -= (size_t)got;
  }
}

static void *origin_main(void *userdata) {
  tls_origin *origin;
  struct timeval timeout;
  unsigned char request[2048];
  unsigned char response[sizeof(upstream_head) - 1u + sizeof(server_early)];
  unsigned char early[sizeof(client_early)];
  unsigned char header[sizeof(client_big_head)];
  unsigned char chunk[4096];
  SSL *ssl;
  size_t used;
  size_t offset;
  size_t i;
  int fd;
  int got;

  origin = (tls_origin *)userdata;
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  fd = accept(origin->listener, NULL, NULL);
  assert(fd >= 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0);
  ssl = SSL_new(origin->ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_accept(ssl) == 1);
  used = 0u;
  do {
    got = SSL_read(ssl, request + used, (int)(sizeof(request) - used - 1u));
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
    assert(used < sizeof(request) - 1u);
  } while (strstr((const char *)request, "\r\n\r\n") == NULL);
  assert(strstr((const char *)request, "GET /backend/ws HTTP/1.1\r\n") != NULL);
  assert(strstr((const char *)request, "Host: localhost:") != NULL ||
         strstr((const char *)request, "host: localhost:") != NULL);
  memcpy(response, upstream_head, sizeof(upstream_head) - 1u);
  memcpy(response + sizeof(upstream_head) - 1u, server_early,
         sizeof(server_early));
  ssl_write_all(ssl, response, sizeof(response));
  ssl_read_exact(ssl, early, sizeof(early));
  assert(memcmp(early, client_early, sizeof(early)) == 0);
  ssl_read_exact(ssl, header, sizeof(header));
  assert(memcmp(header, client_big_head, sizeof(header)) == 0);
  for (offset = 0u; offset < WSS_PAYLOAD_BYTES; offset += sizeof(chunk)) {
    ssl_read_exact(ssl, chunk, sizeof(chunk));
    for (i = 0u; i < sizeof(chunk); ++i)
      assert(chunk[i] == (unsigned char)('A' ^ mask[(offset + i) % 4u]));
  }
  ssl_write_all(ssl, server_big_head, sizeof(server_big_head));
  memset(chunk, 'B', sizeof(chunk));
  for (offset = 0u; offset < WSS_PAYLOAD_BYTES; offset += sizeof(chunk))
    ssl_write_all(ssl, chunk, sizeof(chunk));
  SSL_free(ssl);
  assert(close(fd) == 0);

  /* The same certificate must fail without the route's trusted CA bundle. */
  fd = accept(origin->listener, NULL, NULL);
  assert(fd >= 0);
  ssl = SSL_new(origin->ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  (void)SSL_accept(ssl);
  SSL_free(ssl);
  assert(close(fd) == 0);

  fd = accept(origin->listener, NULL, NULL);
  assert(fd >= 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0);
  ssl = SSL_new(origin->ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_accept(ssl) == 1);
  used = 0u;
  do {
    got = SSL_read(ssl, request + used, (int)(sizeof(request) - used - 1u));
    assert(got > 0);
    used += (size_t)got;
    request[used] = '\0';
    assert(used < sizeof(request) - 1u);
  } while (strstr((const char *)request, "\r\n\r\n") == NULL);
  assert(strstr((const char *)request, "GET /backend/http HTTP/1.1\r\n") !=
         NULL);
  ssl_write_all(ssl,
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                "Connection: close\r\n\r\nok",
                strlen("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                       "Connection: close\r\n\r\nok"));
  SSL_free(ssl);
  assert(close(fd) == 0);
  return NULL;
}

static void send_all(int fd, const void *data, size_t length) {
  const unsigned char *bytes;
  ssize_t sent;

  bytes = (const unsigned char *)data;
  while (length != 0u) {
    sent = send(fd, bytes, length, 0);
    assert(sent > 0);
    bytes += (size_t)sent;
    length -= (size_t)sent;
  }
}

static void read_exact(int fd, void *data, size_t length) {
  unsigned char *bytes;
  ssize_t got;

  bytes = (unsigned char *)data;
  while (length != 0u) {
    got = recv(fd, bytes, length, 0);
    assert(got > 0);
    bytes += (size_t)got;
    length -= (size_t)got;
  }
}

static int connect_app(unsigned short port) {
  struct sockaddr_in address;
  struct timeval timeout;
  int attempt;
  int fd;

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  for (attempt = 0; attempt < 100; ++attempt) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
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
  return fd;
}

static unsigned short listen_origin(tls_origin *origin) {
  struct sockaddr_in address;
  socklen_t length;

  origin->listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(origin->listener >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(origin->listener, (struct sockaddr *)&address, sizeof(address)) ==
         0);
  assert(listen(origin->listener, 2) == 0);
  length = sizeof(address);
  assert(getsockname(origin->listener, (struct sockaddr *)&address, &length) ==
         0);
  return ntohs(address.sin_port);
}

static unsigned short available_port(void) {
  tls_origin temporary;
  unsigned short port;

  port = listen_origin(&temporary);
  assert(close(temporary.listener) == 0);
  return port;
}

static void run_trusted(unsigned short app_port) {
  unsigned char response[2048];
  unsigned char chunk[4096];
  unsigned char header[sizeof(server_big_head)];
  const unsigned char *frame;
  const char *boundary;
  size_t used;
  size_t offset;
  size_t i;
  ssize_t got;
  int fd;

  fd = connect_app(app_port);
  send_all(fd, client_head, sizeof(client_head) - 1u);
  send_all(fd, client_early, sizeof(client_early));
  used = 0u;
  do {
    got = recv(fd, response + used, sizeof(response) - used - 1u, 0);
    assert(got > 0);
    used += (size_t)got;
    response[used] = '\0';
    boundary = strstr((const char *)response, "\r\n\r\n");
  } while ((boundary == NULL ||
            used < (size_t)(boundary + 4u - (const char *)response) +
                       sizeof(server_early)) &&
           used < sizeof(response));
  assert(boundary != NULL);
  assert(strstr((const char *)response, "HTTP/1.1 101 ") != NULL);
  frame = (const unsigned char *)boundary + 4u;
  assert(memcmp(frame, server_early, sizeof(server_early)) == 0);
  send_all(fd, client_big_head, sizeof(client_big_head));
  for (offset = 0u; offset < WSS_PAYLOAD_BYTES; offset += sizeof(chunk)) {
    for (i = 0u; i < sizeof(chunk); ++i)
      chunk[i] = (unsigned char)('A' ^ mask[(offset + i) % 4u]);
    send_all(fd, chunk, sizeof(chunk));
  }
  read_exact(fd, header, sizeof(header));
  assert(memcmp(header, server_big_head, sizeof(header)) == 0);
  for (offset = 0u; offset < WSS_PAYLOAD_BYTES; offset += sizeof(chunk)) {
    read_exact(fd, chunk, sizeof(chunk));
    for (i = 0u; i < sizeof(chunk); ++i)
      assert(chunk[i] == 'B');
  }
  assert(close(fd) == 0);
}

static void run_untrusted(unsigned short app_port) {
  char response[1024];
  ssize_t got;
  int fd;

  fd = connect_app(app_port);
  send_all(fd,
           "GET /ws-untrusted HTTP/1.1\r\nHost: localhost\r\n"
           "Connection: Upgrade\r\nUpgrade: websocket\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Sec-WebSocket-Version: 13\r\n\r\n",
           strlen("GET /ws-untrusted HTTP/1.1\r\nHost: localhost\r\n"
                  "Connection: Upgrade\r\nUpgrade: websocket\r\n"
                  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                  "Sec-WebSocket-Version: 13\r\n\r\n"));
  got = recv(fd, response, sizeof(response) - 1u, 0);
  assert(got > 0);
  response[got] = '\0';
  assert(strstr(response, "502 Bad Gateway") != NULL);
  assert(close(fd) == 0);
}

static void run_http(unsigned short app_port) {
  char response[2048];
  size_t used;
  ssize_t got;
  int fd;

  fd = connect_app(app_port);
  send_all(fd, "GET /http HTTP/1.1\r\nHost: localhost\r\n\r\n",
           strlen("GET /http HTTP/1.1\r\nHost: localhost\r\n\r\n"));
  used = 0u;
  for (;;) {
    got = recv(fd, response + used, sizeof(response) - used - 1u, 0);
    assert(got >= 0);
    if (got == 0)
      break;
    used += (size_t)got;
    assert(used < sizeof(response) - 1u);
  }
  response[used] = '\0';
  assert(strstr(response, "HTTP/1.1 200 ") == response);
  assert(strstr(response, "2\r\nok\r\n0\r\n\r\n") != NULL);
  assert(close(fd) == 0);
}

int main(void) {
  tls_origin origin;
  vectis_app_config app_config;
  vectis_proxy_route_config proxy;
  vectis_error error;
  vectis_app *app;
  EVP_PKEY *key;
  X509 *cert;
  BIO *pem;
  BUF_MEM *pem_data;
  char *ca_pem;
  char target[128];
  unsigned short app_port;

  assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
  memset(&origin, 0, sizeof(origin));
  key = make_key();
  cert = make_cert(key);
  origin.ctx = SSL_CTX_new(TLS_server_method());
  assert(origin.ctx != NULL);
  assert(SSL_CTX_use_certificate(origin.ctx, cert) == 1);
  assert(SSL_CTX_use_PrivateKey(origin.ctx, key) == 1);
  SSL_CTX_set_client_hello_cb(origin.ctx, check_client_hello, &origin);
  origin.port = listen_origin(&origin);
  pem = BIO_new(BIO_s_mem());
  assert(pem != NULL);
  assert(PEM_write_bio_X509(pem, cert) == 1);
  BIO_get_mem_ptr(pem, &pem_data);
  assert(pem_data != NULL);
  ca_pem = (char *)malloc(pem_data->length + 1u);
  assert(ca_pem != NULL);
  memcpy(ca_pem, pem_data->data, pem_data->length);
  ca_pem[pem_data->length] = '\0';
  assert(snprintf(target, sizeof(target), "https://localhost:%u/backend",
                  (unsigned)origin.port) > 0);
  app_port = available_port();
  vectis_app_config_init(&app_config);
  app_config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  app_config.tls.bind = "127.0.0.1";
  app_config.tls.port = app_port;
  app_config.server.worker_count = 1u;
  app = vectis_app_new(&app_config, &error);
  assert(app != NULL);
  vectis_proxy_route_config_init(&proxy);
  proxy.path = "/ws";
  proxy.methods = VECTIS_HTTP_METHODS_GET;
  proxy.target = target;
  proxy.tls_ca_pem = ca_pem;
  assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
  proxy.path = "/http";
  assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
  free(ca_pem);
  proxy.path = "/ws-untrusted";
  proxy.tls_ca_pem = NULL;
  assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
  assert(app->start(app, &error) == VECTIS_OK);
  assert(pthread_create(&origin.thread, NULL, origin_main, &origin) == 0);
  run_trusted(app_port);
  run_untrusted(app_port);
  run_http(app_port);
  assert(pthread_join(origin.thread, NULL) == 0);
  assert(origin.hello_count == 3);
  assert(!origin.ws_offered_alpn);
  assert(origin.http_offered_alpn);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  assert(close(origin.listener) == 0);
  SSL_CTX_free(origin.ctx);
  BIO_free(pem);
  X509_free(cert);
  EVP_PKEY_free(key);
  return 0;
}
