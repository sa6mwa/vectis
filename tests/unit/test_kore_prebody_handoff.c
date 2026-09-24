#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <kore/http.h>
#include <kore/kore.h>
#include <vectis/vectis.h>

struct probe_state {
  struct http_request *req;
  char body[4];
  size_t have;
  char pipeline[256];
  size_t pipeline_len;
};

extern void vectis_kore_set_prebody_probe(
    int (*probe)(struct http_request *, const void *, size_t));

static int
probe_connection_handle(struct connection *c)
{
  if ((c->evt.flags & KORE_EVENT_READ) && !net_recv_flush(c))
    return KORE_RESULT_ERROR;
  if ((c->evt.flags & KORE_EVENT_WRITE) && !net_send_flush(c))
    return KORE_RESULT_ERROR;
  return KORE_RESULT_OK;
}

static void
probe_finish(struct connection *c)
{
  struct probe_state *state;

  state = (struct probe_state *)c->hdlr_extra;
  assert(state != NULL && state->have == sizeof(state->body));
  http_response(state->req, 200, state->body, sizeof(state->body));
  state->req->flags |= HTTP_REQUEST_DELETE;
  http_request_wakeup(state->req);
  c->handle = kore_connection_handle;
  c->flags &= ~CONN_IS_BUSY;
  if (state->pipeline_len > 0) {
    assert(c->http_pipeline == NULL);
    c->http_pipeline = kore_malloc(state->pipeline_len);
    memcpy(c->http_pipeline, state->pipeline, state->pipeline_len);
    c->http_pipeline_len = state->pipeline_len;
  }
  c->hdlr_extra = NULL;
  kore_free(state);
  http_start_recv(c);
}

static int
probe_recv(struct netbuf *nb)
{
  struct connection *c;
  struct probe_state *state;

  c = nb->owner;
  state = (struct probe_state *)c->hdlr_extra;
  assert(state != NULL);
  assert(nb->s_off <= sizeof(state->body) - state->have);
  memcpy(state->body + state->have, nb->buf, nb->s_off);
  state->have += nb->s_off;
  if (state->have == sizeof(state->body))
    probe_finish(c);
  return KORE_RESULT_OK;
}

static int
raw_recv(struct netbuf *nb)
{
  struct connection *c;

  c = nb->owner;
  net_send_queue(c, nb->buf, nb->s_off);
  net_recv_reset(c, 4096, raw_recv);
  return KORE_RESULT_OK;
}

static int
probe_prebody(struct http_request *req, const void *data, size_t len)
{
  struct connection *c;
  struct probe_state *state;
  u_int64_t declared;
  size_t first;

  if (strcmp(req->path, "/reject") == 0)
    return KORE_RESULT_ERROR;
  if (strcmp(req->path, "/forbidden") == 0) {
    req->owner->flags |= CONN_CLOSE_EMPTY;
    http_response(req, 403, "forbidden", 9);
    return KORE_RESULT_ERROR;
  }
  if (strcmp(req->path, "/raw") == 0) {
    static const char upgrade[] =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    c = req->owner;
    c->flags |= CONN_IS_BUSY;
    c->handle = probe_connection_handle;
    http_request_sleep(req);
    net_send_queue(c, upgrade, sizeof(upgrade) - 1);
    if (len > 0)
      net_send_queue(c, data, len);
    net_recv_reset(c, 4096, raw_recv);
    return KORE_RESULT_RETRY;
  }
  if (strcmp(req->path, "/probe") != 0)
    return KORE_RESULT_OK;
  assert(req->method == HTTP_METHOD_POST);
  assert(http_request_header_uint64(req, "content-length", &declared));
  assert(declared == 4);
  c = req->owner;
  assert(c->hdlr_extra == NULL);
  state = kore_calloc(1, sizeof(*state));
  state->req = req;
  first = len < sizeof(state->body) ? len : sizeof(state->body);
  memcpy(state->body, data, first);
  state->have = first;
  assert(len - first <= sizeof(state->pipeline));
  if (len > first) {
    state->pipeline_len = len - first;
    memcpy(state->pipeline, (const char *)data + first, state->pipeline_len);
  }
  c->hdlr_extra = state;
  c->flags |= CONN_IS_BUSY;
  c->handle = probe_connection_handle;
  http_request_sleep(req);
  if (state->have == sizeof(state->body)) {
    probe_finish(c);
  } else {
    net_recv_reset(c, sizeof(state->body) - state->have, probe_recv);
  }
  return KORE_RESULT_RETRY;
}

static vectis_status
reply(vectis_app *app, vectis_request *request, vectis_response *response,
    void *userdata, vectis_error *error)
{
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 200, "text/plain", "ok", error);
}

static unsigned short
available_port(void)
{
  struct sockaddr_in addr;
  socklen_t size;
  int fd;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  size = sizeof(addr);
  assert(getsockname(fd, (struct sockaddr *)&addr, &size) == 0);
  assert(close(fd) == 0);
  return ntohs(addr.sin_port);
}

static int
connect_local(unsigned short port)
{
  struct sockaddr_in addr;
  int attempt;
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  for (attempt = 0; attempt < 100; attempt++) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
      return fd;
    assert(close(fd) == 0);
    usleep(10000u);
  }
  assert(0 && "Vectis listener did not start");
  return -1;
}

static unsigned
response_count(const char *data)
{
  unsigned count;
  const char *p;

  count = 0;
  p = data;
  while ((p = strstr(p, "HTTP/1.1 200")) != NULL) {
    count++;
    p++;
  }
  return count;
}

static int
check_local_rejection(unsigned short port, const char *path, int status)
{
  struct pollfd watch;
  char wire[256];
  char output[1024];
  char marker[16];
  ssize_t got;
  int fd;

  fd = connect_local(port);
  assert(snprintf(wire, sizeof(wire),
      "POST %s HTTP/1.1\r\nHost: localhost\r\n"
      "Content-Length: 4\r\n\r\n", path) > 0);
  assert(send(fd, wire, strlen(wire), 0) == (ssize_t)strlen(wire));
  watch.fd = fd;
  watch.events = POLLIN;
  got = 0;
  if (poll(&watch, 1, 1000) > 0)
    got = recv(fd, output, sizeof(output) - 1, 0);
  if (got > 0)
    output[got] = '\0';
  else
    output[0] = '\0';
  assert(snprintf(marker, sizeof(marker), " %d ", status) > 0);
  assert(close(fd) == 0);
  return strstr(output, marker) != NULL;
}

static int
check_raw_handoff(unsigned short port, int tls)
{
  static const char frame[] = "\x81\x85\x01\x02\x03\x04igohn";
  static const char wire[] =
      "GET /raw HTTP/1.1\r\nHost: localhost\r\n"
      "Connection: Upgrade\r\nUpgrade: websocket\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"
      "\x81\x85\x01\x02\x03\x04igohn";
  SSL_CTX *ctx;
  SSL *ssl;
  struct pollfd watch;
  char output[4096];
  size_t used;
  ssize_t got;
  int fd;
  int ok;

  fd = connect_local(port);
  ctx = NULL;
  ssl = NULL;
  if (tls) {
    ctx = SSL_CTX_new(TLS_client_method());
    assert(ctx != NULL);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    ssl = SSL_new(ctx);
    assert(ssl != NULL);
    assert(SSL_set_fd(ssl, fd) == 1);
    assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
    assert(SSL_connect(ssl) == 1);
    assert(SSL_write(ssl, wire, (int)(sizeof(wire) - 1)) ==
        (int)(sizeof(wire) - 1));
  } else {
    assert(send(fd, wire, sizeof(wire) - 1, 0) ==
        (ssize_t)(sizeof(wire) - 1));
  }
  watch.fd = fd;
  watch.events = POLLIN;
  used = 0;
  output[0] = '\0';
  while (used < sizeof(output) - 1 && strstr(output, frame) == NULL) {
    if ((!tls || SSL_pending(ssl) == 0) && poll(&watch, 1, 1000) <= 0)
      break;
    if (tls)
      got = SSL_read(ssl, output + used, (int)(sizeof(output) - 1 - used));
    else
      got = recv(fd, output + used, sizeof(output) - 1 - used, 0);
    if (got <= 0)
      break;
    used += (size_t)got;
    output[used] = '\0';
  }
  ok = strstr(output, " 101 ") != NULL && strstr(output, frame) != NULL;
  fprintf(stderr, "%s early WebSocket frame: %s\n",
      tls ? "TLS" : "cleartext", ok ? "preserved" : "missing");
  if (ssl != NULL)
    SSL_free(ssl);
  if (ctx != NULL)
    SSL_CTX_free(ctx);
  assert(close(fd) == 0);
  return ok;
}

static int
check_tls_handoff(unsigned short port, const char *wire)
{
  SSL_CTX *ctx;
  SSL *ssl;
  struct pollfd watch;
  char output[4096];
  size_t used;
  int fd;
  int got;
  unsigned count;

  fd = connect_local(port);
  ctx = SSL_CTX_new(TLS_client_method());
  assert(ctx != NULL);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  ssl = SSL_new(ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
  assert(SSL_connect(ssl) == 1);
  assert(SSL_write(ssl, wire, (int)strlen(wire)) == (int)strlen(wire));
  watch.fd = fd;
  watch.events = POLLIN;
  used = 0;
  output[0] = '\0';
  while (used < sizeof(output) - 1 && response_count(output) < 3) {
    if (SSL_pending(ssl) == 0 && poll(&watch, 1, 1000) <= 0)
      break;
    got = SSL_read(ssl, output + used, (int)(sizeof(output) - 1 - used));
    if (got <= 0)
      break;
    used += (size_t)got;
    output[used] = '\0';
  }
  count = response_count(output);
  fprintf(stderr, "TLS ordinary + takeover + ordinary responses: %u\n",
      count);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  assert(close(fd) == 0);
  return count == 3 && strstr(output, "data") != NULL;
}

int
main(void)
{
  static const char wire[] =
      "GET /one HTTP/1.1\r\nHost: localhost\r\n\r\n"
      "POST /probe HTTP/1.1\r\nHost: localhost\r\n"
      "Content-Length: 4\r\n\r\ndata"
      "GET /two HTTP/1.1\r\nHost: localhost\r\n\r\n";
  static const char split_start[] =
      "POST /probe HTTP/1.1\r\nHost: localhost\r\n"
      "Content-Length: 4\r\n\r\nda";
  static const char split_end[] =
      "taGET /two HTTP/1.1\r\nHost: localhost\r\n\r\n";
  vectis_app_config config;
  vectis_cert_bundle_config certs;
  vectis_route_config route;
  vectis_error error;
  vectis_app *app;
  struct pollfd watch;
  char output[4096];
  unsigned short port;
  size_t used;
  ssize_t got;
  int fd;
  unsigned count;
  unsigned split_count;
  int tls_passed;
  int raw_passed;
  int tls_raw_passed;
  int reject_passed;
  char cert_path[128];
  char key_path[128];

  port = available_port();
  vectis_kore_set_prebody_probe(probe_prebody);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = port;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/one", reply, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/two", reply, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  assert(app->start(app, &error) == VECTIS_OK);

  fd = connect_local(port);
  assert(send(fd, wire, sizeof(wire) - 1, 0) == (ssize_t)(sizeof(wire) - 1));
  watch.fd = fd;
  watch.events = POLLIN;
  used = 0;
  output[0] = '\0';
  while (used < sizeof(output) - 1 && response_count(output) < 3) {
    if (poll(&watch, 1, 1000) <= 0)
      break;
    got = recv(fd, output + used, sizeof(output) - 1 - used, 0);
    if (got <= 0)
      break;
    used += (size_t)got;
    output[used] = '\0';
  }
  count = response_count(output);
  fprintf(stderr, "ordinary + takeover + ordinary responses: %u\n", count);
  assert(close(fd) == 0);

  fd = connect_local(port);
  assert(send(fd, split_start, sizeof(split_start) - 1, 0) ==
      (ssize_t)(sizeof(split_start) - 1));
  usleep(20000u);
  assert(send(fd, split_end, sizeof(split_end) - 1, 0) ==
      (ssize_t)(sizeof(split_end) - 1));
  watch.fd = fd;
  used = 0;
  output[0] = '\0';
  while (used < sizeof(output) - 1 && response_count(output) < 2) {
    if (poll(&watch, 1, 1000) <= 0)
      break;
    got = recv(fd, output + used, sizeof(output) - 1 - used, 0);
    if (got <= 0)
      break;
    used += (size_t)got;
    output[used] = '\0';
  }
  split_count = response_count(output);
  fprintf(stderr, "split takeover + ordinary responses: %u\n", split_count);
  assert(close(fd) == 0);
  raw_passed = check_raw_handoff(port, 0);
  reject_passed = check_local_rejection(port, "/reject", 400);
  reject_passed &= check_local_rejection(port, "/forbidden", 403);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);

  assert(snprintf(cert_path, sizeof(cert_path),
      "/tmp/vectis-kore-prebody-%ld-cert.pem", (long)getpid()) > 0);
  assert(snprintf(key_path, sizeof(key_path),
      "/tmp/vectis-kore-prebody-%ld-key.pem", (long)getpid()) > 0);
  vectis_cert_bundle_config_init(&certs);
  certs.subject.common_name = "localhost";
  certs.dns_names = "localhost";
  certs.output_cert_path = cert_path;
  certs.output_key_path = key_path;
  certs.key_bits = 2048u;
  certs.valid_days = 1L;
  assert(vectis_cert_generate_bundle(&certs, &error) == VECTIS_OK);
  port = available_port();
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_MANUAL;
  config.tls.bind = "127.0.0.1";
  config.tls.port = port;
  config.tls.domain = "localhost";
  config.tls.certificate_path = cert_path;
  config.tls.private_key_path = key_path;
  config.tls.ca_bundle_path = cert_path;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/one", reply, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/two", reply, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  assert(app->start(app, &error) == VECTIS_OK);
  tls_passed = check_tls_handoff(port, wire);
  tls_raw_passed = check_raw_handoff(port, 1);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  assert(remove(cert_path) == 0);
  assert(remove(key_path) == 0);
  vectis_kore_set_prebody_probe(NULL);
  return count == 3 && split_count == 2 && tls_passed &&
      raw_passed && tls_raw_passed && reject_passed &&
      strstr(output, "data") != NULL ? 0 : 1;
}
