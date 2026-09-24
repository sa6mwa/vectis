#include <arpa/inet.h>
#include <assert.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct tls_server {
  int listener;
  unsigned short port;
  pthread_t thread;
  SSL_CTX *ctx;
  int no_alpn;
  int hello_count;
};

struct http_result {
  char body[3];
  size_t used;
};

static const char ws_request[] =
    "GET /chat HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Protocol: chat\r\n\r\n";
static const char ws_response[] =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
    "Sec-WebSocket-Protocol: chat\r\n\r\n";
static const unsigned char client_frame[] = {
    0x81, 0x84, 0x11, 0x22, 0x33, 0x44, 'p' ^ 0x11, 'i' ^ 0x22,
    'n' ^ 0x33, 'g' ^ 0x44};
static const unsigned char server_frame[] = {0x81, 0x04, 'p', 'o', 'n', 'g'};

static EVP_PKEY *
make_key(void)
{
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

static X509 *
make_cert(EVP_PKEY *key)
{
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
      (const unsigned char *)"localhost", -1, -1, 0) == 1);
  assert(X509_set_issuer_name(cert, name) == 1);
  X509V3_set_ctx(&extension_ctx, cert, cert, NULL, NULL, 0);
  extension = X509V3_EXT_conf_nid(NULL, &extension_ctx,
      NID_subject_alt_name, "DNS:localhost");
  assert(extension != NULL);
  assert(X509_add_ext(cert, extension, -1) == 1);
  X509_EXTENSION_free(extension);
  assert(X509_sign(cert, key, EVP_sha256()) > 0);
  return cert;
}

static void *
tls_main(void *arg)
{
  struct tls_server *server;
  unsigned char request[sizeof(ws_request) - 1 + sizeof(client_frame)];
  unsigned char response[sizeof(ws_response) - 1 + sizeof(server_frame)];
  static const char http_response[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
  char http_request[1024];
  SSL *http_ssl;
  SSL *ssl;
  int http_fd;
  int fd;
  int got;
  size_t received;

  server = (struct tls_server *)arg;
  fd = accept(server->listener, NULL, NULL);
  assert(fd >= 0);
  ssl = SSL_new(server->ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_accept(ssl) == 1);

  /* A completed connect-only handle remains attached while a regular
   * transfer to the same origin uses a separate connection. */
  http_fd = accept(server->listener, NULL, NULL);
  assert(http_fd >= 0);
  http_ssl = SSL_new(server->ctx);
  assert(http_ssl != NULL);
  assert(SSL_set_fd(http_ssl, http_fd) == 1);
  assert(SSL_accept(http_ssl) == 1);
  received = 0;
  while (received < sizeof(http_request) - 1) {
    got = SSL_read(http_ssl, http_request + received,
        (int)(sizeof(http_request) - 1 - received));
    assert(got > 0);
    received += (size_t)got;
    http_request[received] = '\0';
    if (strstr(http_request, "\r\n\r\n") != NULL)
      break;
  }
  assert(strncmp(http_request, "GET /ordinary HTTP/1.1\r\n", 24) == 0);
  assert(strstr(http_request, "\r\n\r\n") != NULL);
  assert(SSL_write(http_ssl, http_response,
      (int)(sizeof(http_response) - 1)) ==
      (int)(sizeof(http_response) - 1));
  assert(SSL_shutdown(http_ssl) >= 0);
  SSL_free(http_ssl);
  assert(close(http_fd) == 0);

  received = 0;
  while (received < sizeof(request)) {
    got = SSL_read(ssl, request + received, (int)(sizeof(request) - received));
    assert(got > 0);
    received += (size_t)got;
  }
  assert(memcmp(request, ws_request, sizeof(ws_request) - 1) == 0);
  assert(memcmp(request + sizeof(ws_request) - 1, client_frame,
      sizeof(client_frame)) == 0);
  memcpy(response, ws_response, sizeof(ws_response) - 1);
  memcpy(response + sizeof(ws_response) - 1, server_frame,
      sizeof(server_frame));
  assert(SSL_write(ssl, response, sizeof(response)) == (int)sizeof(response));
  assert(SSL_shutdown(ssl) >= 0);
  SSL_free(ssl);
  assert(close(fd) == 0);
  return NULL;
}

static size_t
http_body(char *data, size_t size, size_t count, void *arg)
{
  struct http_result *result;
  size_t len;

  result = (struct http_result *)arg;
  len = size * count;
  assert(len <= 2 - result->used);
  memcpy(result->body + result->used, data, len);
  result->used += len;
  return len;
}

static int
check_client_hello(SSL *ssl, int *alert, void *arg)
{
  struct tls_server *server;
  const unsigned char *extension;
  size_t extension_size;

  (void)alert;
  server = (struct tls_server *)arg;
  if (server->hello_count++ == 0)
    server->no_alpn = !SSL_client_hello_get0_ext(ssl,
        TLSEXT_TYPE_application_layer_protocol_negotiation,
        &extension, &extension_size);
  return SSL_CLIENT_HELLO_SUCCESS;
}

static void
start_tls(struct tls_server *server, X509 *cert, EVP_PKEY *key)
{
  struct sockaddr_in addr;
  socklen_t size;

  memset(server, 0, sizeof(*server));
  server->ctx = SSL_CTX_new(TLS_server_method());
  assert(server->ctx != NULL);
  assert(SSL_CTX_use_certificate(server->ctx, cert) == 1);
  assert(SSL_CTX_use_PrivateKey(server->ctx, key) == 1);
  SSL_CTX_set_client_hello_cb(server->ctx, check_client_hello, server);
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
  assert(pthread_create(&server->thread, NULL, tls_main, server) == 0);
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
  struct tls_server server;
  struct curl_blob ca;
  CURLM *multi;
  CURL *easy;
  CURL *http_easy;
  CURLMsg *msg;
  EVP_PKEY *key;
  X509 *cert;
  BIO *pem;
  BUF_MEM *pem_data;
  curl_socket_t fd;
  CURLcode code;
  char url[128];
  char http_url[128];
  struct http_result http_result;
  unsigned char request[sizeof(ws_request) - 1 + sizeof(client_frame)];
  unsigned char reply[sizeof(ws_response) - 1 + sizeof(server_frame)];
  int running;
  int pending;
  int numfds;
  int attempt;
  size_t sent;
  size_t received;

  key = make_key();
  cert = make_cert(key);
  pem = BIO_new(BIO_s_mem());
  assert(pem != NULL);
  assert(PEM_write_bio_X509(pem, cert) == 1);
  BIO_get_mem_ptr(pem, &pem_data);
  assert(pem_data != NULL);
  ca.data = pem_data->data;
  ca.len = pem_data->length;
  ca.flags = CURL_BLOB_COPY;
  start_tls(&server, cert, key);

  assert(curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
  multi = curl_multi_init();
  easy = curl_easy_init();
  assert(multi != NULL && easy != NULL);
  assert(snprintf(url, sizeof(url), "https://localhost:%u/",
      (unsigned)server.port) > 0);
  assert(curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 1L) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_CAINFO_BLOB, &ca) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,
      CURL_HTTP_VERSION_1_1) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_SSL_ENABLE_ALPN, 0L) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_NOPROXY, "*") == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK);
  assert(curl_multi_add_handle(multi, easy) == CURLM_OK);
  assert(curl_multi_perform(multi, &running) == CURLM_OK);
  for (attempt = 0; running > 0 && attempt < 40; attempt++) {
    assert(curl_multi_poll(multi, NULL, 0, 100, &numfds) == CURLM_OK);
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
  }
  assert(running == 0);
  msg = curl_multi_info_read(multi, &pending);
  assert(msg != NULL && msg->msg == CURLMSG_DONE);
  assert(msg->data.result == CURLE_OK);
  assert(curl_easy_getinfo(easy, CURLINFO_ACTIVESOCKET, &fd) == CURLE_OK);
  assert(fd != CURL_SOCKET_BAD);

  http_easy = curl_easy_init();
  assert(http_easy != NULL);
  memset(&http_result, 0, sizeof(http_result));
  assert(snprintf(http_url, sizeof(http_url),
      "https://localhost:%u/ordinary", (unsigned)server.port) > 0);
  assert(curl_easy_setopt(http_easy, CURLOPT_URL, http_url) == CURLE_OK);
  assert(curl_easy_setopt(http_easy, CURLOPT_CAINFO_BLOB, &ca) == CURLE_OK);
  assert(curl_easy_setopt(http_easy, CURLOPT_HTTP_VERSION,
      CURL_HTTP_VERSION_1_1) == CURLE_OK);
  assert(curl_easy_setopt(http_easy, CURLOPT_NOPROXY, "*") == CURLE_OK);
  assert(curl_easy_setopt(http_easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK);
  assert(curl_easy_setopt(http_easy, CURLOPT_WRITEFUNCTION,
      http_body) == CURLE_OK);
  assert(curl_easy_setopt(http_easy, CURLOPT_WRITEDATA,
      &http_result) == CURLE_OK);
  assert(curl_multi_add_handle(multi, http_easy) == CURLM_OK);
  assert(curl_multi_perform(multi, &running) == CURLM_OK);
  for (attempt = 0; running > 0 && attempt < 40; attempt++) {
    assert(curl_multi_poll(multi, NULL, 0, 100, &numfds) == CURLM_OK);
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
  }
  assert(running == 0);
  msg = curl_multi_info_read(multi, &pending);
  assert(msg != NULL && msg->msg == CURLMSG_DONE);
  assert(msg->easy_handle == http_easy && msg->data.result == CURLE_OK);
  assert(http_result.used == 2);
  assert(memcmp(http_result.body, "ok", 2) == 0);
  assert(curl_multi_info_read(multi, &pending) == NULL);
  assert(curl_multi_remove_handle(multi, http_easy) == CURLM_OK);
  curl_easy_cleanup(http_easy);

  memcpy(request, ws_request, sizeof(ws_request) - 1);
  memcpy(request + sizeof(ws_request) - 1, client_frame,
      sizeof(client_frame));
  sent = 0;
  while (sent < sizeof(request)) {
    size_t amount;

    amount = 0;
    code = curl_easy_send(easy, request + sent, sizeof(request) - sent,
        &amount);
    if (code == CURLE_AGAIN) {
      wait_socket(fd, POLLOUT);
      continue;
    }
    assert(code == CURLE_OK && amount > 0);
    sent += amount;
  }
  received = 0;
  while (received < sizeof(reply)) {
    size_t amount;

    amount = 0;
    code = curl_easy_recv(easy, reply + received,
        sizeof(reply) - received, &amount);
    if (code == CURLE_AGAIN) {
      wait_socket(fd, POLLIN);
      continue;
    }
    assert(code == CURLE_OK && amount > 0);
    received += amount;
  }
  assert(memcmp(reply, ws_response, sizeof(ws_response) - 1) == 0);
  assert(memcmp(reply + sizeof(ws_response) - 1, server_frame,
      sizeof(server_frame)) == 0);
  assert(curl_multi_remove_handle(multi, easy) == CURLM_OK);
  curl_easy_cleanup(easy);
  assert(curl_multi_cleanup(multi) == CURLM_OK);
  curl_global_cleanup();
  assert(pthread_join(server.thread, NULL) == 0);
  assert(server.hello_count == 2);
  assert(server.no_alpn);
  assert(close(server.listener) == 0);
  SSL_CTX_free(server.ctx);
  BIO_free(pem);
  X509_free(cert);
  EVP_PKEY_free(key);
  return 0;
}
