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
#define CONCURRENT_TRANSFERS 4
#define MAX_RSS_DELTA_KB (16u * 1024u)

struct h2_server {
  int listener;
  unsigned short port;
  pthread_t thread;
  size_t generated;
  unsigned accepted;
  unsigned saw_request;
  SSL_CTX *ctx;
  unsigned negotiated_h2;
};

struct h2_connection {
  struct h2_server *server;
  int fd;
  SSL *ssl;
  size_t generated;
};

struct client_state {
  int paused;
  int resume;
  size_t received;
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
      __sync_fetch_and_add(&server->negotiated_h2, 1);
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
  remaining = RESPONSE_SIZE - connection->generated;
  if (len > remaining)
    len = remaining;
  memset(data, 'x', len);
  connection->generated += len;
  __sync_fetch_and_add(&connection->server->generated, len);
  if (connection->generated == RESPONSE_SIZE)
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
  __sync_fetch_and_add(&connection->server->saw_request, 1);
  memset(&provider, 0, sizeof(provider));
  provider.read_callback = produce_body;
  return nghttp2_submit_response(session, frame->hd.stream_id,
      header, sizeof(header) / sizeof(header[0]), &provider);
}

static void *
connection_main(void *arg)
{
  struct h2_connection *connection;
  nghttp2_session_callbacks *callbacks;
  nghttp2_session *session;
  struct pollfd item;
  unsigned char input[16384];
  ssize_t got;
  int flags;
  int ready;
  int result;

  connection = (struct h2_connection *)arg;
  connection->ssl = SSL_new(connection->server->ctx);
  assert(connection->ssl != NULL);
  assert(SSL_set_fd(connection->ssl, connection->fd) == 1);
  assert(SSL_accept(connection->ssl) == 1);
  flags = fcntl(connection->fd, F_GETFL, 0);
  assert(flags >= 0);
  assert(fcntl(connection->fd, F_SETFL, flags | O_NONBLOCK) == 0);
  SSL_set_mode(connection->ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  assert(nghttp2_session_callbacks_new(&callbacks) == 0);
  nghttp2_session_callbacks_set_send_callback(callbacks, send_data);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
      request_received);
  assert(nghttp2_session_server_new(&session, callbacks, connection) == 0);
  nghttp2_session_callbacks_del(callbacks);
  assert(nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE,
      NULL, 0) == 0);
  for (;;) {
    item.fd = connection->fd;
    item.events = POLLIN | (nghttp2_session_want_write(session) ? POLLOUT : 0);
    ready = poll(&item, 1, 1000);
    assert(ready >= 0);
    if (ready == 0)
      continue;
    if (item.revents & POLLIN) {
      for (;;) {
        int ssl_error;

        got = SSL_read(connection->ssl, input, sizeof(input));
        if (got > 0) {
          assert(nghttp2_session_mem_recv(session, input,
              (size_t)got) == got);
          continue;
        }
        ssl_error = SSL_get_error(connection->ssl, (int)got);
        if (ssl_error == SSL_ERROR_WANT_READ ||
            ssl_error == SSL_ERROR_WANT_WRITE)
          break;
        goto connection_done;
      }
    }
    if (item.revents & (POLLERR | POLLHUP))
      break;
    result = nghttp2_session_send(session);
    if (result != 0 && result != NGHTTP2_ERR_WOULDBLOCK)
      break;
  }
connection_done:
  nghttp2_session_del(session);
  SSL_free(connection->ssl);
  assert(close(connection->fd) == 0);
  free(connection);
  return NULL;
}

static void *
server_main(void *arg)
{
  struct h2_server *server;
  struct h2_connection *connection;
  pthread_t threads[CONCURRENT_TRANSFERS];
  unsigned i;

  server = (struct h2_server *)arg;
  for (i = 0; i < CONCURRENT_TRANSFERS; i++) {
    connection = calloc(1, sizeof(*connection));
    assert(connection != NULL);
    connection->server = server;
    connection->fd = accept(server->listener, NULL, NULL);
    assert(connection->fd >= 0);
    server->accepted++;
    assert(pthread_create(&threads[i], NULL, connection_main,
        connection) == 0);
  }
  for (i = 0; i < CONCURRENT_TRANSFERS; i++)
    assert(pthread_join(threads[i], NULL) == 0);
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
  assert(listen(server->listener, CONCURRENT_TRANSFERS) == 0);
  size = sizeof(addr);
  assert(getsockname(server->listener, (struct sockaddr *)&addr, &size) == 0);
  server->port = ntohs(addr.sin_port);
  assert(pthread_create(&server->thread, NULL, server_main, server) == 0);
}

static size_t
pause_download(char *data, size_t size, size_t count, void *arg)
{
  struct client_state *state;
  size_t amount;
  size_t i;

  state = (struct client_state *)arg;
  amount = size * count;
  if (!state->resume) {
    state->paused = 1;
    return CURL_WRITEFUNC_PAUSE;
  }
  for (i = 0; i < amount; i++)
    assert(data[i] == 'x');
  state->received += amount;
  return amount;
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
  struct client_state states[CONCURRENT_TRANSFERS];
  struct curl_blob ca;
  curl_version_info_data *curl_info;
  CURLM *multi;
  CURL *easy[CONCURRENT_TRANSFERS];
  EVP_PKEY *key;
  X509 *cert;
  BIO *pem;
  BUF_MEM *pem_data;
  char url[128];
  long version;
  int running;
  int numfds;
  int attempt;
  int paused;
  int i;
  unsigned long baseline;
  unsigned long after_pause;
  unsigned long after_wait;
  unsigned long after_resume;
  size_t generated_at_pause;
  time_t resume_start;

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
  memset(states, 0, sizeof(states));
  assert(curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
  curl_info = curl_version_info(CURLVERSION_NOW);
  assert(curl_info != NULL);
  assert(curl_info->features & CURL_VERSION_ASYNCHDNS);
  assert(curl_info->features & CURL_VERSION_HTTP2);
  assert(curl_info->features & CURL_VERSION_SSL);
  multi = curl_multi_init();
  assert(multi != NULL);
  assert(curl_multi_setopt(multi, CURLMOPT_PIPELINING,
      CURLPIPE_NOTHING) == CURLM_OK);
  assert(curl_multi_setopt(multi, CURLMOPT_MAX_CONCURRENT_STREAMS,
      1L) == CURLM_OK);
  assert(snprintf(url, sizeof(url), "https://localhost:%u/",
      (unsigned)server.port) > 0);
  for (i = 0; i < CONCURRENT_TRANSFERS; i++) {
    easy[i] = curl_easy_init();
    assert(easy[i] != NULL);
    assert(curl_easy_setopt(easy[i], CURLOPT_URL, url) == CURLE_OK);
    assert(curl_easy_setopt(easy[i], CURLOPT_CAINFO_BLOB, &ca) == CURLE_OK);
    assert(curl_easy_setopt(easy[i], CURLOPT_NOPROXY, "*") == CURLE_OK);
    assert(curl_easy_setopt(easy[i], CURLOPT_HTTP_VERSION,
        CURL_HTTP_VERSION_2TLS) == CURLE_OK);
    assert(curl_easy_setopt(easy[i], CURLOPT_WRITEFUNCTION,
        pause_download) == CURLE_OK);
    assert(curl_easy_setopt(easy[i], CURLOPT_WRITEDATA,
        &states[i]) == CURLE_OK);
    assert(curl_easy_setopt(easy[i], CURLOPT_NOSIGNAL, 1L) == CURLE_OK);
  }
  baseline = rss_kb();
  for (i = 0; i < CONCURRENT_TRANSFERS; i++)
    assert(curl_multi_add_handle(multi, easy[i]) == CURLM_OK);
  assert(curl_multi_perform(multi, &running) == CURLM_OK);
  paused = 0;
  for (attempt = 0; attempt < 200 && running &&
      paused < CONCURRENT_TRANSFERS; attempt++) {
    assert(curl_multi_poll(multi, NULL, 0, 50, &numfds) == CURLM_OK);
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
    paused = 0;
    for (i = 0; i < CONCURRENT_TRANSFERS; i++)
      paused += states[i].paused;
  }
  assert(paused == CONCURRENT_TRANSFERS);
  for (i = 0; i < CONCURRENT_TRANSFERS; i++) {
    assert(curl_easy_getinfo(easy[i], CURLINFO_HTTP_VERSION,
        &version) == CURLE_OK);
    assert(version == CURL_HTTP_VERSION_2_0);
  }
  after_pause = rss_kb();
  for (attempt = 0; attempt < 20 && running; attempt++) {
    assert(curl_multi_poll(multi, NULL, 0, 100, &numfds) == CURLM_OK);
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
  }
  after_wait = rss_kb();
  generated_at_pause = __sync_fetch_and_add(&server.generated, 0);
  for (i = 0; i < CONCURRENT_TRANSFERS; i++) {
    states[i].resume = 1;
    assert(curl_easy_pause(easy[i], CURLPAUSE_CONT) == CURLE_OK);
  }
  resume_start = time(NULL);
  for (attempt = 0; running && time(NULL) - resume_start < 15;
      attempt++) {
    assert(curl_multi_poll(multi, NULL, 0, 50, &numfds) == CURLM_OK);
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
  }
  fprintf(stderr, "h2 resume: running=%d attempts=%d received=%lu,%lu,%lu,%lu\n",
      running, attempt, (unsigned long)states[0].received,
      (unsigned long)states[1].received,
      (unsigned long)states[2].received,
      (unsigned long)states[3].received);
  assert(running == 0);
  for (i = 0; i < CONCURRENT_TRANSFERS; i++)
    assert(states[i].received == RESPONSE_SIZE);
  after_resume = rss_kb();
  for (i = 0; i < CONCURRENT_TRANSFERS; i++) {
    assert(curl_multi_remove_handle(multi, easy[i]) == CURLM_OK);
    curl_easy_cleanup(easy[i]);
  }
  assert(curl_multi_cleanup(multi) == CURLM_OK);
  curl_global_cleanup();
  assert(pthread_join(server.thread, NULL) == 0);
  assert(close(server.listener) == 0);
  SSL_CTX_free(server.ctx);
  BIO_free(pem);
  X509_free(cert);
  EVP_PKEY_free(key);
  fprintf(stderr, "curl h2 pause: baseline=%luKB paused=%luKB later=%luKB "
      "resumed=%luKB generated_at_pause=%lu server_generated=%lu\n",
      baseline, after_pause, after_wait, after_resume,
      (unsigned long)generated_at_pause, (unsigned long)server.generated);
  assert(server.accepted == CONCURRENT_TRANSFERS);
  assert(server.saw_request == CONCURRENT_TRANSFERS);
  assert(server.negotiated_h2 == CONCURRENT_TRANSFERS);
  assert(generated_at_pause > CONCURRENT_TRANSFERS * 65536u);
  assert(generated_at_pause < CONCURRENT_TRANSFERS * RESPONSE_SIZE);
  assert(server.generated == CONCURRENT_TRANSFERS * RESPONSE_SIZE);
  assert(after_wait >= baseline);
  assert(after_wait - baseline <= MAX_RSS_DELTA_KB);
  assert(after_wait <= after_pause + 4096u);
  assert(after_resume >= baseline);
  assert(after_resume - baseline <= MAX_RSS_DELTA_KB);
  return 0;
}
