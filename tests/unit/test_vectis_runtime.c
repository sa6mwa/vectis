#include <assert.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "vectis_internal.h"
#include <vectis/vectis.h>

static vectis_status sample_handler(vectis_app *app,
                                    vectis_request *request,
                                    vectis_response *response,
                                    void *userdata,
                                    vectis_error *error) {
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 200, "text/plain", "ok", error);
}

static vectis_status metadata_handler(vectis_app *app,
                                      vectis_request *request,
                                      vectis_response *response,
                                      void *userdata,
                                      vectis_error *error) {
  const char *expand;
  const char *trace;

  (void)app;
  (void)userdata;
  expand = vectis_request_query(request, "expand");
  trace = vectis_request_header(request, "x-vectis-trace");
  if (expand == NULL || strcmp(expand, "items and logs") != 0) {
    return vectis_response_text(response, 422, "text/plain", "bad query", error);
  }
  if (trace == NULL || strcmp(trace, "runtime-smoke") != 0) {
    return vectis_response_text(response, 422, "text/plain", "bad header", error);
  }
  return vectis_response_text(response, 200, "text/plain", "metadata", error);
}

static vectis_status upload_handler(vectis_app *app,
                                    vectis_request *request,
                                    vectis_response *response,
                                    void *userdata,
                                    vectis_error *error) {
  vectis_body_materialize_config config;
  vectis_body_materialized body;
  FILE *fp;
  int ch;
  vectis_status status;

  (void)app;
  (void)userdata;
  vectis_body_materialize_config_init(&config);
  config.memory_limit_bytes = 1024u;
  config.prefix = "vectis-runtime-upload";
  status = vectis_request_body_materialize(request, &config, &body, error);
  if (status != VECTIS_OK) {
    return status;
  }
  if (body.kind != VECTIS_BODY_MATERIALIZED_FILE || body.path == NULL) {
    vectis_body_materialized_cleanup(&body);
    return vectis_response_text(response, 422, "text/plain", "not spooled", error);
  }
  fp = fopen(body.path, "rb");
  if (fp == NULL) {
    vectis_body_materialized_cleanup(&body);
    return vectis_response_text(response, 422, "text/plain", "missing spool", error);
  }
  ch = fgetc(fp);
  (void)fclose(fp);
  if (ch != 'x') {
    vectis_body_materialized_cleanup(&body);
    return vectis_response_text(response, 422, "text/plain", "bad spool", error);
  }
  (void)remove(body.path);
  vectis_body_materialized_cleanup(&body);
  return vectis_response_text(response, 200, "text/plain", "spooled", error);
}

static vectis_status file_handler(vectis_app *app,
                                  vectis_request *request,
                                  vectis_response *response,
                                  void *userdata,
                                  vectis_error *error) {
  (void)app;
  (void)request;
  return vectis_response_file(response, 200, "text/plain", (const char *)userdata, error);
}

static int connect_local(unsigned short port) {
  struct sockaddr_in addr;
  int fd;
  int rc;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  rc = inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  assert(rc == 1);
  rc = connect(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr));
  assert(rc == 0);
  return fd;
}

static void socket_send_all(int fd, const char *data, size_t size) {
  size_t offset;
  ssize_t written;

  offset = 0u;
  while (offset < size) {
    written = send(fd, data + offset, size - offset, 0);
    assert(written > 0);
    offset += (size_t)written;
  }
}

static ssize_t socket_recv_some(int fd, char *buffer, size_t size, long timeout_ms) {
  struct timeval tv;
  fd_set rfds;
  int rc;

  FD_ZERO(&rfds);
  FD_SET(fd, &rfds);
  tv.tv_sec = timeout_ms / 1000L;
  tv.tv_usec = (timeout_ms % 1000L) * 1000L;
  rc = select(fd + 1, &rfds, NULL, NULL, &tv);
  assert(rc >= 0);
  if (rc == 0) {
    return 0;
  }
  return recv(fd, buffer, size, 0);
}

static int count_token(const char *haystack, const char *needle) {
  const char *p;
  int count;

  count = 0;
  p = haystack;
  while ((p = strstr(p, needle)) != NULL) {
    ++count;
    p += strlen(needle);
  }
  return count;
}

static void assert_large_header_rejected(unsigned short port) {
  const char *prefix;
  const char *suffix;
  char request[3072];
  char response[1024];
  ssize_t nread;
  size_t prefix_size;
  size_t suffix_size;
  int fd;

  prefix = "GET /health HTTP/1.1\r\nHost: localhost\r\nX-Big: ";
  suffix = "\r\n\r\n";
  prefix_size = strlen(prefix);
  suffix_size = strlen(suffix);
  fd = connect_local(port);
  memset(request, 'a', sizeof(request));
  memcpy(request, prefix, prefix_size);
  memcpy(request + sizeof(request) - suffix_size, suffix, suffix_size);
  socket_send_all(fd, request, sizeof(request));
  memset(response, 0, sizeof(response));
  nread = socket_recv_some(fd, response, sizeof(response) - 1u, 2000L);
  if (nread > 0) {
    response[(size_t)nread] = '\0';
    assert(strstr(response, " 200 ") == NULL);
  }
  (void)shutdown(fd, SHUT_RDWR);
  (void)close(fd);
}

static void assert_keepalive_limit(unsigned short port) {
  const char *request;
  char response[2048];
  ssize_t nread;
  int fd;

  request = "GET /health HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "GET /health HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
  fd = connect_local(port);
  socket_send_all(fd, request, strlen(request));
  memset(response, 0, sizeof(response));
  nread = socket_recv_some(fd, response, sizeof(response) - 1u, 2000L);
  assert(nread > 0);
  response[(size_t)nread] = '\0';
  assert(count_token(response, " 200 ") == 1);
  (void)shutdown(fd, SHUT_RDWR);
  (void)close(fd);
}

static void assert_kore_smoke(void) {
  vectis_app_config config;
  vectis_http_client_config http;
  vectis_http_request request;
  vectis_http_request oversized;
  vectis_http_response response;
  vectis_http_response metadata_response;
  vectis_http_response oversized_response;
  vectis_http_response upload_response;
  vectis_route_config route;
  vectis_route_config limited_route;
  vectis_route_config upload_route;
  vectis_route_config file_route;
  vectis_app_config second_config;
  vectis_route_config second_route;
  const char *headers[] = {"x-vectis-trace: runtime-smoke"};
  const char upload_path[] = "/tmp/vectis-runtime-upload.bin";
  const char response_file_path[] = "/tmp/vectis-runtime-response.txt";
  const char response_file_body[] = "file-response";
  vectis_error error;
  vectis_error second_error;
  vectis_status status;
  vectis_status second_status;
  vectis_app *app;
  vectis_app *second_app;
  FILE *fp;
  int attempt;
  int i;

  memset(&response, 0, sizeof(response));
  memset(&metadata_response, 0, sizeof(metadata_response));
  memset(&oversized_response, 0, sizeof(oversized_response));
  memset(&upload_response, 0, sizeof(upload_response));
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = 28080u;
  config.server.max_request_header_bytes = 1024u;
  config.server.max_request_body_bytes = 1024u;
  config.server.request_header_timeout_ms = 1000L;
  config.server.request_body_idle_timeout_ms = 1000L;
  config.server.request_body_min_rate_bytes_per_sec = 1024u;
  config.server.request_body_min_rate_grace_ms = 500L;
  config.server.keepalive_max_requests = 1u;
  fp = fopen(response_file_path, "wb");
  assert(fp != NULL);
  assert(fwrite(response_file_body, 1u, sizeof(response_file_body) - 1u, fp) ==
         sizeof(response_file_body) - 1u);
  assert(fclose(fp) == 0);
  app = vectis_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/health", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/metadata", metadata_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  limited_route = vectis_route(VECTIS_HTTP_POST, "/limited", sample_handler, NULL);
  limited_route.body = vectis_body_buffered_max(4u);
  status = vectis_register_route(app, &limited_route, &error);
  assert(status == VECTIS_OK);
  upload_route = vectis_upload_route_max(VECTIS_HTTP_POST, "/upload", 4096u, upload_handler, NULL);
  upload_route.body.memory_buffer_limit_bytes = 1024u;
  status = vectis_register_route(app, &upload_route, &error);
  assert(status == VECTIS_OK);
  file_route = vectis_route(VECTIS_HTTP_GET, "/file", file_handler, (void *)response_file_path);
  status = vectis_register_route(app, &file_route, &error);
  assert(status == VECTIS_OK);
  status = vectis_start(app, &error);
  assert(status == VECTIS_OK);

  vectis_app_config_init(&second_config);
  second_config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  second_config.tls.bind = "127.0.0.1";
  second_config.tls.port = 28081u;
  second_app = vectis_new(&second_config, &second_error);
  assert(second_app != NULL);
  second_route = vectis_route(VECTIS_HTTP_GET, "/health", sample_handler, NULL);
  second_status = vectis_register_route(second_app, &second_route, &second_error);
  assert(second_status == VECTIS_OK);
  second_status = vectis_start(second_app, &second_error);
  assert(second_status == VECTIS_ERR_STATE);
  assert(strstr(second_error.message, "Kore runtime is already running") != NULL);
  vectis_destroy(second_app);

  vectis_http_client_config_init(&http);
  http.timeout_ms = 1000L;
  http.connect_timeout_ms = 200L;
  status = VECTIS_ERR_STATE;
  for (attempt = 0; attempt < 100 && status != VECTIS_OK; ++attempt) {
    vectis_http_response_cleanup(&response);
    status = vectis_http_get(&http, "http://127.0.0.1:28080/health", &response, &error);
    if (status != VECTIS_OK) {
      usleep(100000u);
    }
  }
  assert(status == VECTIS_OK);
  assert(response.status_code == 200L);
  assert(response.body_size == 2u);
  assert(memcmp(response.body, "ok", 2u) == 0);

  assert_large_header_rejected(28080u);
  assert_keepalive_limit(28080u);

  vectis_http_response_cleanup(&response);
  status = vectis_http_get(&http, "http://127.0.0.1:28080/file", &response, &error);
  assert(status == VECTIS_OK);
  assert(response.status_code == 200L);
  assert(response.body_size == sizeof(response_file_body) - 1u);
  assert(memcmp(response.body, response_file_body, sizeof(response_file_body) - 1u) == 0);
  vectis_http_response_cleanup(&response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = "http://127.0.0.1:28080/metadata?expand=items+and+logs";
  request.headers = headers;
  request.header_count = 1u;
  status = vectis_http_execute(&http, &request, &metadata_response, &error);
  assert(status == VECTIS_OK);
  assert(metadata_response.status_code == 200L);
  assert(metadata_response.body_size == 8u);
  assert(memcmp(metadata_response.body, "metadata", 8u) == 0);
  vectis_http_response_cleanup(&metadata_response);

  vectis_http_request_init(&oversized);
  oversized.method = VECTIS_HTTP_POST;
  oversized.url = "http://127.0.0.1:28080/limited";
  oversized.body = "12345";
  oversized.body_size = 5u;
  status = vectis_http_execute(&http, &oversized, &oversized_response, &error);
  assert(status == VECTIS_OK);
  assert(oversized_response.status_code == 413L);
  vectis_http_response_cleanup(&oversized_response);

  fp = fopen(upload_path, "wb");
  assert(fp != NULL);
  for (i = 0; i < 2048; ++i) {
    assert(fputc('x', fp) == 'x');
  }
  assert(fclose(fp) == 0);
  status = vectis_http_upload_file(&http,
                                   VECTIS_HTTP_POST,
                                   "http://127.0.0.1:28080/upload",
                                   upload_path,
                                   "application/octet-stream",
                                   &upload_response,
                                   &error);
  assert(status == VECTIS_OK);
  assert(upload_response.status_code == 200L);
  assert(upload_response.body_size == 7u);
  assert(memcmp(upload_response.body, "spooled", 7u) == 0);
  vectis_http_response_cleanup(&upload_response);
  remove(upload_path);
  remove(response_file_path);

  status = vectis_stop(app, &error);
  assert(status == VECTIS_OK);
  vectis_destroy(app);
}

int main(void) {
  vectis_app_config config;
  vectis_error error;
  vectis_app *app;
  vectis_status status;
  vectis_route_config bad_route;
  vectis_route_config route;

  vectis_app_config_init(&config);

  app = vectis_new(&config, &error);
  assert(app != NULL);
  assert(vectis_internal_lockd_client(app) == NULL);

  status = vectis_start(app, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "manual TLS requires") != NULL);
  vectis_destroy(app);

  config.tls.cert_key_bundle = vectis_source_from_path("/tmp/server.pem");
  app = vectis_new(&config, &error);
  assert(app != NULL);
  assert(vectis_internal_lockd_client(app) == NULL);

  status = vectis_start(app, &error);
  assert(status == VECTIS_OK);
  status = vectis_stop(app, &error);
  assert(status == VECTIS_OK);

  vectis_destroy(app);
  config.server.request_header_timeout_ms = 0L;
  app = vectis_new(&config, &error);
  assert(app == NULL);
  assert(strstr(error.message, "request_header_timeout_ms") != NULL);
  vectis_app_config_init(&config);
  config.tls.cert_key_bundle = vectis_source_from_path("/tmp/server.pem");
  config.lockd.unix_socket_path = "/tmp/lockd.sock";
  config.server.keepalive_max_requests = 0u;
  app = vectis_new(&config, &error);
  assert(app == NULL);
  assert(strstr(error.message, "keepalive_max_requests") != NULL);
  vectis_app_config_init(&config);
  config.tls.cert_key_bundle = vectis_source_from_path("/tmp/server.pem");
  config.lockd.unix_socket_path = "/tmp/lockd.sock";
  config.server.keepalive_disabled = 1;
  config.server.keepalive_timeout_ms = 0L;
  config.server.keepalive_max_requests = 0u;
  app = vectis_new(&config, &error);
  assert(app != NULL);

  vectis_route_config_init(&bad_route);
  bad_route.method = VECTIS_HTTP_GET;
  bad_route.path = "missing-slash";
  bad_route.handler = sample_handler;
  status = vectis_register_route(app, &bad_route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "route path must start with '/'") != NULL);

  vectis_route_config_init(&route);
  route.method = VECTIS_HTTP_GET;
  route.path = "/orders/:id";
  route.handler = sample_handler;
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "path_kind") != NULL);

  route = vectis_route_methods(VECTIS_HTTP_METHODS_NONE, "/bad-method", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "HTTP method") != NULL);

  route = vectis_route_methods(VECTIS_HTTP_METHODS_OPTIONS | VECTIS_HTTP_METHODS_HEAD,
                               "/multi",
                               sample_handler,
                               NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_OPTIONS, "/multi", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_CONFLICT);

  route = vectis_upload_route(VECTIS_HTTP_POST, "/upload", sample_handler, NULL);
  assert(route.body.mode == VECTIS_BODY_STREAMING_UPLOAD);
  assert(route.body.max_bytes == VECTIS_BODY_DEFAULT_UPLOAD_MAX_BYTES);
  assert(route.body.disk_spool_disabled == 0);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);

  route = vectis_upload_route_max(VECTIS_HTTP_POST,
                                  "/bad-upload",
                                  (size_t)1024u,
                                  sample_handler,
                                  NULL);
  route.body.disk_spool_disabled = 1;
  route.body.memory_buffer_limit_bytes = 512u;
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "spooling") != NULL);

  vectis_route_config_init(&route);
  route.method = VECTIS_HTTP_GET;
  route.path = "/orders/:bad-name";
  route.path_kind = VECTIS_ROUTE_PATH_PARAMS;
  route.handler = sample_handler;
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "parameter name") != NULL);

  vectis_route_config_init(&route);
  route.method = VECTIS_HTTP_GET;
  route.path = "/orders/../secrets";
  route.path_kind = VECTIS_ROUTE_PATH_PARAMS;
  route.handler = sample_handler;
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "dot segments") != NULL);

  vectis_route_config_init(&route);
  route.method = VECTIS_HTTP_GET;
  route.path = "/orders/:id??";
  route.path_kind = VECTIS_ROUTE_PATH_PARAMS;
  route.handler = sample_handler;
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "parameter name") != NULL);

  vectis_route_config_init(&route);
  route.method = VECTIS_HTTP_GET;
  route.path = "/orders?status=open";
  route.path_kind = VECTIS_ROUTE_PATH_PARAMS;
  route.handler = sample_handler;
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "only allowed") != NULL);

  vectis_route_config_init(&route);
  route.method = VECTIS_HTTP_GET;
  route.path = "^/reports/[0-9]+$";
  route.path_kind = VECTIS_ROUTE_PATH_REGEX;
  route.handler = sample_handler;
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);

  vectis_route_config_init(&route);
  route.method = VECTIS_HTTP_GET;
  route.path = "^/reports/([0-9]+$";
  route.path_kind = VECTIS_ROUTE_PATH_REGEX;
  route.handler = sample_handler;
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "regex") != NULL);

  status = vectis_json_validate_cstr("{\"ok\":true}", &error);
  assert(status == VECTIS_OK);

  status = vectis_json_validate_cstr("{\"ok\":", &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(error.source == VECTIS_ERROR_SOURCE_LONEJSON);
  assert(strstr(error.message, "invalid json") != NULL);

  status = vectis_stop(app, &error);
  assert(status == VECTIS_ERR_STATE);

  vectis_destroy(app);
  assert_kore_smoke();
  return 0;
}
