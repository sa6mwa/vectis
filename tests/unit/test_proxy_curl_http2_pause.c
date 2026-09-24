#include <arpa/inet.h>
#include <assert.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <nghttp2/nghttp2.h>
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
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RESPONSE_SIZE (64u * 1024u * 1024u)
#define MAX_RSS_DELTA_KB (32u * 1024u)

struct h2_server {
  int listener;
  unsigned short port;
  pthread_t thread;
  size_t generated;
  int saw_request;
  SSL_CTX *ctx;
  int negotiated_h2;
};

struct h2_connection {
  struct h2_server *server;
  int fd;
  SSL *ssl;
};

struct client_state {
  int paused;
};

static ssize_t
send_data(nghttp2_session *session, const uint8_t *data, size_t len,
    int flags, void *arg)
{
  struct h2_connection *connection;
  ssize_t sent;

  (void)session;
  (void)flags;
  connection = (struct h2_connection *)arg;
  sent = SSL_write(connection->ssl, data, (int)len);
  if (sent <= 0) {
    int error;

    error = SSL_get_error(connection->ssl, (int)sent);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
      return NGHTTP2_ERR_WOULDBLOCK;
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }
  return sent;
}

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

static int
select_h2(SSL *ssl, const unsigned char **out, unsigned char *outlen,
    const unsigned char *in, unsigned int inlen, void *arg)
{
  struct h2_server *server;
  unsigned int offset;
  unsigned int length;

  (void)ssl;
  server = (struct h2_server *)arg;
  offset = 0;
  while (offset < inlen) {
    length = in[offset++];
    if (length > inlen - offset)
      break;
    if (length == 2 && memcmp(in + offset, "h2", 2) == 0) {
      *out = in + offset;
      *outlen = 2;
      server->negotiated_h2 = 1;
      return SSL_TLSEXT_ERR_OK;
    }
    offset += length;
  }
  return SSL_TLSEXT_ERR_NOACK;
}

static ssize_t
produce_body(nghttp2_session *session, int32_t stream_id, uint8_t *data,
    size_t len, uint32_t *flags, nghttp2_data_source *source, void *arg)
{
  struct h2_connection *connection;
  size_t remaining;

  (void)session;
  (void)stream_id;
  (void)source;
  connection = (struct h2_connection *)arg;
  remaining = RESPONSE_SIZE - connection->server->generated;
  if (len > remaining)
    len = remaining;
  memset(data, 'x', len);
  connection->server->generated += len;
  if (connection->server->generated == RESPONSE_SIZE)
    *flags |= NGHTTP2_DATA_FLAG_EOF;
  return (ssize_t)len;
}

static int
request_received(nghttp2_session *session, const nghttp2_frame *frame,
    void *arg)
{
  static uint8_t status_name[] = ":status";
  static uint8_t status_value[] = "200";
  static uint8_t type_name[] = "content-type";
  static uint8_t type_value[] = "application/octet-stream";
  static nghttp2_nv header[] = {
    {status_name, status_value, 7, 3, NGHTTP2_NV_FLAG_NONE},
    {type_name, type_value, 12, 24, NGHTTP2_NV_FLAG_NONE}
  };
  struct h2_connection *connection;
  nghttp2_data_provider provider;

  connection = (struct h2_connection *)arg;
  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST)
    return 0;
  connection->server->saw_request = 1;
  memset(&provider, 0, sizeof(provider));
  provider.read_callback = produce_body;
  return nghttp2_submit_response(session, frame->hd.stream_id,
      header, sizeof(header) / sizeof(header[0]), &provider);
}

static void *
server_main(void *arg)
{
  struct h2_server *server;
  struct h2_connection connection;
  nghttp2_session_callbacks *callbacks;
  nghttp2_session *session;
  struct pollfd item;
  unsigned char input[16384];
  ssize_t got;
  int flags;
  int ready;
  int result;

  server = (struct h2_server *)arg;
  connection.server = server;
  connection.fd = accept(server->listener, NULL, NULL);
  assert(connection.fd >= 0);
  connection.ssl = SSL_new(server->ctx);
  assert(connection.ssl != NULL);
  assert(SSL_set_fd(connection.ssl, connection.fd) == 1);
  assert(SSL_accept(connection.ssl) == 1);
  flags = fcntl(connection.fd, F_GETFL, 0);
  assert(flags >= 0);
  assert(fcntl(connection.fd, F_SETFL, flags | O_NONBLOCK) == 0);
  SSL_set_mode(connection.ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  assert(nghttp2_session_callbacks_new(&callbacks) == 0);
  nghttp2_session_callbacks_set_send_callback(callbacks, send_data);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
      request_received);
  assert(nghttp2_session_server_new(&session, callbacks, &connection) == 0);
  nghttp2_session_callbacks_del(callbacks);
  assert(nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE,
      NULL, 0) == 0);
  for (;;) {
    item.fd = connection.fd;
    item.events = POLLIN | (nghttp2_session_want_write(session) ? POLLOUT : 0);
    ready = poll(&item, 1, 1000);
    assert(ready >= 0);
    if (ready == 0)
      continue;
    if (item.revents & POLLIN) {
      got = SSL_read(connection.ssl, input, sizeof(input));
      if (got <= 0) {
        int ssl_error;

        ssl_error = SSL_get_error(connection.ssl, (int)got);
        if (ssl_error == SSL_ERROR_WANT_READ ||
            ssl_error == SSL_ERROR_WANT_WRITE)
          continue;
        break;
      }
      assert(nghttp2_session_mem_recv(session, input, (size_t)got) == got);
    }
    if (item.revents & (POLLERR | POLLHUP))
      break;
    result = nghttp2_session_send(session);
    if (result != 0 && result != NGHTTP2_ERR_WOULDBLOCK)
      break;
  }
  nghttp2_session_del(session);
  SSL_free(connection.ssl);
  assert(close(connection.fd) == 0);
  return NULL;
}

static void
start_server(struct h2_server *server, X509 *cert, EVP_PKEY *key)
{
  struct sockaddr_in addr;
  socklen_t size;

  memset(server, 0, sizeof(*server));
  server->ctx = SSL_CTX_new(TLS_server_method());
  assert(server->ctx != NULL);
  assert(SSL_CTX_use_certificate(server->ctx, cert) == 1);
  assert(SSL_CTX_use_PrivateKey(server->ctx, key) == 1);
  SSL_CTX_set_alpn_select_cb(server->ctx, select_h2, server);
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
pause_download(char *data, size_t size, size_t count, void *arg)
{
  struct client_state *state;

  (void)data;
  (void)size;
  (void)count;
  state = (struct client_state *)arg;
  state->paused = 1;
  return CURL_WRITEFUNC_PAUSE;
}

static unsigned long
rss_kb(void)
{
  FILE *file;
  char line[256];
  unsigned long amount;

  file = fopen("/proc/self/status", "r");
  assert(file != NULL);
  amount = 0;
  while (fgets(line, sizeof(line), file) != NULL) {
    if (sscanf(line, "VmRSS: %lu kB", &amount) == 1)
      break;
  }
  assert(fclose(file) == 0);
  assert(amount > 0);
  return amount;
}

int
main(void)
{
  struct h2_server server;
  struct client_state state;
  struct curl_blob ca;
  CURLM *multi;
  CURL *easy;
  EVP_PKEY *key;
  X509 *cert;
  BIO *pem;
  BUF_MEM *pem_data;
  char url[128];
  long version;
  int running;
  int numfds;
  int attempt;
  unsigned long baseline;
  unsigned long after_pause;
  unsigned long after_wait;

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
  start_server(&server, cert, key);
  memset(&state, 0, sizeof(state));
  assert(curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
  multi = curl_multi_init();
  easy = curl_easy_init();
  assert(multi != NULL && easy != NULL);
  assert(curl_multi_setopt(multi, CURLMOPT_PIPELINING,
      CURLPIPE_NOTHING) == CURLM_OK);
  assert(snprintf(url, sizeof(url), "https://localhost:%u/",
      (unsigned)server.port) > 0);
  assert(curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_CAINFO_BLOB, &ca) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_NOPROXY, "*") == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,
      CURL_HTTP_VERSION_2TLS) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,
      pause_download) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &state) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK);
  baseline = rss_kb();
  assert(curl_multi_add_handle(multi, easy) == CURLM_OK);
  assert(curl_multi_perform(multi, &running) == CURLM_OK);
  for (attempt = 0; attempt < 100 && running && !state.paused; attempt++) {
    assert(curl_multi_poll(multi, NULL, 0, 50, &numfds) == CURLM_OK);
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
  }
  assert(state.paused);
  assert(curl_easy_getinfo(easy, CURLINFO_HTTP_VERSION, &version) == CURLE_OK);
  assert(version == CURL_HTTP_VERSION_2_0);
  after_pause = rss_kb();
  for (attempt = 0; attempt < 20 && running; attempt++) {
    assert(curl_multi_poll(multi, NULL, 0, 100, &numfds) == CURLM_OK);
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
  }
  after_wait = rss_kb();
  assert(curl_multi_remove_handle(multi, easy) == CURLM_OK);
  curl_easy_cleanup(easy);
  assert(curl_multi_cleanup(multi) == CURLM_OK);
  curl_global_cleanup();
  assert(pthread_join(server.thread, NULL) == 0);
  assert(close(server.listener) == 0);
  SSL_CTX_free(server.ctx);
  BIO_free(pem);
  X509_free(cert);
  EVP_PKEY_free(key);
  fprintf(stderr, "curl h2 pause: baseline=%luKB paused=%luKB later=%luKB "
      "server_generated=%lu\n", baseline, after_pause, after_wait,
      (unsigned long)server.generated);
  assert(server.saw_request);
  assert(server.negotiated_h2);
  assert(server.generated > 65536u);
  assert(after_wait >= baseline);
  assert(after_wait - baseline <= MAX_RSS_DELTA_KB);
  assert(after_wait <= after_pause + 4096u);
  return 0;
}
