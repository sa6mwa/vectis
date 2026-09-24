#include <arpa/inet.h>
#include <assert.h>
#include <curl/curl.h>
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
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char request_target[] =
    "/rewritten/%2F?x=1&x=2&raw=%2F";

struct h2_server {
  int listener;
  unsigned short port;
  pthread_t thread;
  SSL_CTX *ctx;
  int negotiated_h2;
  int saw_request;
  int saw_first_upload;
  int saw_early_response;
  int upload_complete;
  int saw_requested_method;
  int saw_rewritten_path;
  int saw_rewritten_authority;
  int saw_content_length;
  int saw_transfer_encoding;
  char upload[8];
  size_t upload_size;
};

struct h2_connection {
  struct h2_server *server;
  int fd;
  SSL *ssl;
  int32_t stream_id;
  size_t response_size;
  int response_deferred;
  int upload_complete;
};

struct client_state {
  size_t upload_size;
  int upload_paused;
  int resume_upload;
  char response[8];
  size_t response_size;
};

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
send_data(nghttp2_session *session, const uint8_t *data, size_t len,
    int flags, void *arg)
{
  struct h2_connection *connection;
  ssize_t sent;
  int error;

  (void)session;
  (void)flags;
  connection = (struct h2_connection *)arg;
  sent = SSL_write(connection->ssl, data, (int)len);
  if (sent > 0)
    return sent;
  error = SSL_get_error(connection->ssl, (int)sent);
  if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
    return NGHTTP2_ERR_WOULDBLOCK;
  return NGHTTP2_ERR_CALLBACK_FAILURE;
}

static ssize_t
produce_response(nghttp2_session *session, int32_t stream_id, uint8_t *data,
    size_t len, uint32_t *flags, nghttp2_data_source *source, void *arg)
{
  struct h2_connection *connection;
  const char *piece;

  (void)session;
  (void)stream_id;
  (void)source;
  connection = (struct h2_connection *)arg;
  assert(len >= 4);
  if (connection->response_size == 0) {
    if (connection->server->upload_size < 4) {
      connection->response_deferred = 1;
      return NGHTTP2_ERR_DEFERRED;
    }
    assert(memcmp(connection->server->upload, "ping", 4) == 0);
    connection->response_deferred = 0;
    piece = "pong";
    memcpy(data, piece, 4);
    connection->response_size = 4;
    __sync_lock_test_and_set(&connection->server->saw_early_response, 1);
    return 4;
  }
  if (!connection->upload_complete) {
    connection->response_deferred = 1;
    return NGHTTP2_ERR_DEFERRED;
  }
  assert(connection->response_size == 4);
  connection->response_deferred = 0;
  piece = "done";
  memcpy(data, piece, 4);
  connection->response_size = 8;
  *flags |= NGHTTP2_DATA_FLAG_EOF;
  return 4;
}

static int
on_data(nghttp2_session *session, uint8_t flags, int32_t stream_id,
    const uint8_t *data, size_t len, void *arg)
{
  struct h2_connection *connection;
  struct h2_server *server;

  (void)session;
  (void)flags;
  connection = (struct h2_connection *)arg;
  server = connection->server;
  assert(stream_id == connection->stream_id);
  assert(len <= sizeof(server->upload) - server->upload_size);
  memcpy(server->upload + server->upload_size, data, len);
  server->upload_size += len;
  if (server->upload_size >= 4 && !server->saw_first_upload) {
    __sync_lock_test_and_set(&server->saw_first_upload, 1);
    if (connection->response_deferred) {
      assert(nghttp2_session_resume_data(session, stream_id) == 0);
      connection->response_deferred = 0;
    }
  }
  return 0;
}

static int
on_header(nghttp2_session *session, const nghttp2_frame *frame,
    const uint8_t *name, size_t namelen, const uint8_t *value,
    size_t valuelen, uint8_t flags, void *arg)
{
  struct h2_connection *connection;
  struct h2_server *server;

  (void)session;
  (void)flags;
  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST)
    return 0;
  connection = (struct h2_connection *)arg;
  server = connection->server;
  if (namelen == 7 && memcmp(name, ":method", 7) == 0) {
#ifdef VECTIS_HTTP2_FRAMED_GET
    server->saw_requested_method = valuelen == 3 &&
        memcmp(value, "GET", 3) == 0;
#else
    server->saw_requested_method = valuelen == 4 &&
        memcmp(value, "POST", 4) == 0;
#endif
  }
  if (namelen == 5 && memcmp(name, ":path", 5) == 0)
    server->saw_rewritten_path = valuelen == sizeof(request_target) - 1 &&
        memcmp(value, request_target, sizeof(request_target) - 1) == 0;
  if (namelen == 10 && memcmp(name, ":authority", 10) == 0)
    server->saw_rewritten_authority = valuelen == 14 &&
        memcmp(value, "public.example", 14) == 0;
  if (namelen == 14 && memcmp(name, "content-length", 14) == 0)
    server->saw_content_length = valuelen == 1 && value[0] == '8';
  if (namelen == 17 && memcmp(name, "transfer-encoding", 17) == 0)
    server->saw_transfer_encoding = 1;
  return 0;
}

static int
on_frame(nghttp2_session *session, const nghttp2_frame *frame, void *arg)
{
  static uint8_t status_name[] = ":status";
  static uint8_t status_value[] = "200";
  static nghttp2_nv headers[] = {
    {status_name, status_value, 7, 3, NGHTTP2_NV_FLAG_NONE}
  };
  struct h2_connection *connection;
  nghttp2_data_provider provider;

  connection = (struct h2_connection *)arg;
  if (frame->hd.type == NGHTTP2_HEADERS &&
      frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
    assert(connection->stream_id == 0);
    connection->stream_id = frame->hd.stream_id;
    connection->server->saw_request = 1;
    memset(&provider, 0, sizeof(provider));
    provider.read_callback = produce_response;
    assert(nghttp2_submit_response(session, connection->stream_id,
        headers, sizeof(headers) / sizeof(headers[0]), &provider) == 0);
  }
  if (frame->hd.type == NGHTTP2_DATA &&
      (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
    assert(frame->hd.stream_id == connection->stream_id);
    connection->upload_complete = 1;
    __sync_lock_test_and_set(&connection->server->upload_complete, 1);
    if (connection->response_deferred) {
      assert(nghttp2_session_resume_data(session, connection->stream_id) == 0);
      connection->response_deferred = 0;
    }
  }
  return 0;
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
  int ssl_error;

  server = (struct h2_server *)arg;
  memset(&connection, 0, sizeof(connection));
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
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame);
  nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
      on_data);
  assert(nghttp2_session_server_new(&session, callbacks, &connection) == 0);
  nghttp2_session_callbacks_del(callbacks);
  assert(nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, NULL, 0) == 0);
  for (;;) {
    item.fd = connection.fd;
    item.events = POLLIN | (nghttp2_session_want_write(session) ? POLLOUT : 0);
    ready = poll(&item, 1, 1000);
    assert(ready >= 0);
    if (ready == 0)
      continue;
    if (item.revents & POLLIN) {
      for (;;) {
        got = SSL_read(connection.ssl, input, sizeof(input));
        if (got > 0) {
          assert(nghttp2_session_mem_recv(session, input, (size_t)got) == got);
          continue;
        }
        ssl_error = SSL_get_error(connection.ssl, (int)got);
        if (ssl_error == SSL_ERROR_WANT_READ ||
            ssl_error == SSL_ERROR_WANT_WRITE)
          break;
        goto server_done;
      }
    }
    if (item.revents & (POLLERR | POLLHUP))
      break;
    result = nghttp2_session_send(session);
    if (result != 0 && result != NGHTTP2_ERR_WOULDBLOCK)
      break;
  }
server_done:
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
read_upload(char *data, size_t size, size_t count, void *arg)
{
  struct client_state *state;

  state = (struct client_state *)arg;
  assert(size * count >= 4);
  if (state->upload_size == 8)
    return 0;
  if (state->upload_size == 0) {
    memcpy(data, "ping", 4);
    state->upload_size = 4;
    return 4;
  }
  if (!state->resume_upload) {
    state->upload_paused = 1;
    return CURL_READFUNC_PAUSE;
  }
  assert(state->upload_size == 4);
  memcpy(data, "rest", 4);
  state->upload_size = 8;
  return 4;
}

static size_t
write_response(char *data, size_t size, size_t count, void *arg)
{
  struct client_state *state;
  size_t amount;

  state = (struct client_state *)arg;
  amount = size * count;
  assert(amount <= sizeof(state->response) - state->response_size);
  memcpy(state->response + state->response_size, data, amount);
  state->response_size += amount;
  return amount;
}

int
main(void)
{
  struct h2_server server;
  struct client_state state;
  struct curl_blob ca;
  struct curl_slist *headers;
  CURLM *multi;
  CURL *easy;
  CURLMsg *message;
  EVP_PKEY *key;
  X509 *cert;
  BIO *pem;
  BUF_MEM *pem_data;
  char url[128];
  long version;
  long code;
  int running;
  int numfds;
  int messages;
  int attempt;

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
  assert(multi != NULL);
  easy = curl_easy_init();
  assert(easy != NULL);
  assert(snprintf(url, sizeof(url), "https://localhost:%u/ignored?wrong=1",
      (unsigned)server.port) > 0);
  headers = NULL;
  headers = curl_slist_append(headers, "Expect:");
  assert(headers != NULL);
  headers = curl_slist_append(headers, "Host: public.example");
  assert(headers != NULL);
  assert(curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_REQUEST_TARGET,
      request_target) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_CAINFO_BLOB, &ca) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_NOPROXY, "*") == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,
      CURL_HTTP_VERSION_2TLS) == CURLE_OK);
#ifdef VECTIS_HTTP2_FRAMED_GET
  assert(curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L) == CURLE_OK);
#ifdef VECTIS_HTTP2_UNKNOWN_LENGTH
  assert(curl_easy_setopt(easy, CURLOPT_INFILESIZE_LARGE,
      (curl_off_t)-1) == CURLE_OK);
#else
  assert(curl_easy_setopt(easy, CURLOPT_INFILESIZE_LARGE,
      (curl_off_t)8) == CURLE_OK);
#endif
  assert(curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "GET") == CURLE_OK);
#else
  assert(curl_easy_setopt(easy, CURLOPT_POST, 1L) == CURLE_OK);
#ifdef VECTIS_HTTP2_UNKNOWN_LENGTH
  assert(curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE,
      (curl_off_t)-1) == CURLE_OK);
#else
  assert(curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE,
      (curl_off_t)8) == CURLE_OK);
#endif
#endif
  assert(curl_easy_setopt(easy, CURLOPT_READFUNCTION,
      read_upload) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_READDATA, &state) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,
      write_response) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &state) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK);
  assert(curl_multi_add_handle(multi, easy) == CURLM_OK);
  assert(curl_multi_perform(multi, &running) == CURLM_OK);
  for (attempt = 0; attempt < 200 && running &&
      (!state.upload_paused || state.response_size < 4); attempt++) {
    assert(curl_multi_poll(multi, NULL, 0, 50, &numfds) == CURLM_OK);
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
  }
  fprintf(stderr, "h2 early response: paused=%d uploaded=%lu received=%lu "
      "attempts=%d running=%d\n", state.upload_paused,
      (unsigned long)state.upload_size, (unsigned long)state.response_size,
      attempt, running);
  assert(running == 1);
  assert(state.upload_paused == 1);
  assert(state.upload_size == 4);
  assert(state.response_size == 4);
  assert(memcmp(state.response, "pong", 4) == 0);
  assert(__sync_fetch_and_add(&server.saw_first_upload, 0) == 1);
  assert(__sync_fetch_and_add(&server.saw_early_response, 0) == 1);
  assert(__sync_fetch_and_add(&server.upload_complete, 0) == 0);
  assert(curl_easy_getinfo(easy, CURLINFO_HTTP_VERSION, &version) == CURLE_OK);
  assert(version == CURL_HTTP_VERSION_2_0);
  state.resume_upload = 1;
  assert(curl_easy_pause(easy, CURLPAUSE_CONT) == CURLE_OK);
  for (attempt = 0; attempt < 200 && running; attempt++) {
    assert(curl_multi_poll(multi, NULL, 0, 50, &numfds) == CURLM_OK);
    assert(curl_multi_perform(multi, &running) == CURLM_OK);
  }
  assert(running == 0);
  message = curl_multi_info_read(multi, &messages);
  assert(message != NULL && message->msg == CURLMSG_DONE);
  assert(message->data.result == CURLE_OK);
  assert(state.upload_size == 8);
  assert(state.response_size == 8);
  assert(memcmp(state.response, "pongdone", 8) == 0);
  assert(curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK);
  assert(code == 200);
  assert(curl_multi_remove_handle(multi, easy) == CURLM_OK);
  curl_easy_cleanup(easy);
  assert(curl_multi_cleanup(multi) == CURLM_OK);
  curl_global_cleanup();
  assert(pthread_join(server.thread, NULL) == 0);
  assert(server.negotiated_h2 == 1);
  assert(server.saw_request == 1);
  assert(server.saw_requested_method == 1);
  assert(server.saw_rewritten_path == 1);
  assert(server.saw_rewritten_authority == 1);
#ifdef VECTIS_HTTP2_UNKNOWN_LENGTH
  assert(server.saw_content_length == 0);
#else
  assert(server.saw_content_length == 1);
#endif
  assert(server.saw_transfer_encoding == 0);
  assert(server.upload_complete == 1);
  assert(server.upload_size == 8);
  assert(memcmp(server.upload, "pingrest", 8) == 0);
  assert(close(server.listener) == 0);
  SSL_CTX_free(server.ctx);
  curl_slist_free_all(headers);
  BIO_free(pem);
  X509_free(cert);
  EVP_PKEY_free(key);
  return 0;
}
