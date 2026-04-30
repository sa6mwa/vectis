#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lc/lc.h>
#include <lonejson.h>
#include <vectis/vectis.h>

typedef struct downstream_doc {
  char id[64];
  lonejson_int64 count;
} downstream_doc;

typedef struct stream_capture {
  char data[128];
  size_t size;
} stream_capture;

static const lonejson_field downstream_doc_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(downstream_doc, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64(downstream_doc, count, "count")};

LONEJSON_MAP_DEFINE(downstream_doc_map, downstream_doc, downstream_doc_fields);

static const char *env_or_default(const char *name, const char *fallback) {
  const char *value;

  value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return fallback;
  }
  return value;
}

static unsigned short env_port_or_default(const char *name, unsigned short fallback) {
  const char *value;
  long port;

  value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return fallback;
  }
  port = strtol(value, NULL, 10);
  if (port <= 0L || port > 65535L) {
    return fallback;
  }
  return (unsigned short)port;
}

static void serve_forever(void) {
  for (;;) {
    (void)sleep(3600u);
  }
}

static int print_error(const char *operation, const vectis_error *error) {
  fprintf(stderr, "%s failed", operation);
  if (error != NULL && error->message[0] != '\0') {
    fprintf(stderr, ": %s", error->message);
  }
  if (error != NULL && error->detail[0] != '\0') {
    fprintf(stderr, " (%s)", error->detail);
  }
  fprintf(stderr, "\n");
  return 1;
}

static int require_status(vectis_status status,
                          vectis_status expected,
                          const char *operation,
                          const vectis_error *error) {
  if (status == expected) {
    return 0;
  }
  fprintf(stderr, "%s returned %s, expected %s\n",
          operation,
          vectis_status_string(status),
          vectis_status_string(expected));
  if (error != NULL && error->message[0] != '\0') {
    fprintf(stderr, "%s\n", error->message);
  }
  return 1;
}

static int require_http_status(const vectis_http_response *response,
                               long expected,
                               const char *operation) {
  if (response != NULL && response->status_code == expected) {
    return 0;
  }
  fprintf(stderr, "%s returned HTTP %ld, expected %ld\n",
          operation,
          response != NULL ? response->status_code : 0L,
          expected);
  return 1;
}

static vectis_status health_handler(vectis_app *app,
                                    vectis_request *request,
                                    vectis_response *response,
                                    void *userdata,
                                    vectis_error *error) {
  (void)app;
  (void)userdata;
  if (vectis_request_method(request) == VECTIS_HTTP_HEAD) {
    return vectis_response_status(response, 204, error);
  }
  return vectis_response_text(response, 200, "text/plain", "ok\n", error);
}

static vectis_status event_handler(vectis_app *app,
                                   vectis_request *request,
                                   vectis_response *response,
                                   void *userdata,
                                   vectis_error *error) {
  downstream_doc input;
  downstream_doc output;
  const char *id;
  vectis_http_method method;

  (void)app;
  (void)userdata;
  memset(&input, 0, sizeof(input));
  memset(&output, 0, sizeof(output));
  method = vectis_request_method(request);
  if (method == VECTIS_HTTP_OPTIONS) {
    if (vectis_response_header(response, "allow", "OPTIONS, POST, PUT, PATCH, DELETE", error) !=
        VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
    return vectis_response_status(response, 204, error);
  }
  if (method == VECTIS_HTTP_DELETE) {
    return vectis_response_status(response, 204, error);
  }
  if (vectis_request_json_into(request, &downstream_doc_map, &input, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  id = vectis_request_path_param(request, "id");
  if (id == NULL) {
    id = input.id;
  }
  if (snprintf(output.id, sizeof(output.id), "%s", id) < 0) {
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to format response id");
    return VECTIS_ERR_STATE;
  }
  output.count = input.count + 1;
  return vectis_response_json(response, 200, &downstream_doc_map, &output, error);
}

static vectis_status stream_handler(vectis_app *app,
                                    vectis_request *request,
                                    vectis_response *response,
                                    void *userdata,
                                    vectis_error *error) {
  char payload[96];
  const char *id;
  lc_source *source;
  vectis_status status;

  (void)app;
  (void)userdata;
  id = vectis_request_path_param(request, "id");
  if (id == NULL) {
    id = "missing";
  }
  if (snprintf(payload, sizeof(payload), "stream:%s", id) < 0) {
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to format stream payload");
    return VECTIS_ERR_STATE;
  }
  source = NULL;
  if (lc_source_from_memory(payload, strlen(payload), &source, NULL) != LC_OK) {
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to create stream source");
    return VECTIS_ERR_STATE;
  }
  status = vectis_response_source(response, 200, "text/plain", source, error);
  lc_source_close(source);
  return status;
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

static vectis_status upload_handler(vectis_app *app,
                                    vectis_request *request,
                                    vectis_response *response,
                                    void *userdata,
                                    vectis_error *error) {
  vectis_body_materialize_config materialize;
  vectis_body_materialized body;
  char buffer[64];
  char text[96];
  const char *id;
  size_t size;
  int ok;
  vectis_status status;

  (void)app;
  (void)userdata;
  vectis_body_materialize_config_init(&materialize);
  materialize.buffer = buffer;
  materialize.buffer_size = sizeof(buffer);
  materialize.memory_limit_bytes = sizeof(buffer);
  materialize.prefix = "vectis-downstream-upload";
  status = vectis_request_body_materialize(request, &materialize, &body, error);
  if (status != VECTIS_OK) {
    return status;
  }
  size = 0u;
  ok = 0;
  if (body.kind == VECTIS_BODY_MATERIALIZED_MEMORY && body.memory.data != NULL) {
    size = body.memory.size;
    ok = size == strlen("upload-body") &&
         memcmp(body.memory.data, "upload-body", strlen("upload-body")) == 0;
  }
  vectis_body_materialized_cleanup(&body);
  if (!ok) {
    return vectis_response_text(response, 422, "text/plain", "bad upload", error);
  }
  id = vectis_request_path_param(request, "id");
  if (id == NULL) {
    id = "missing";
  }
  if (snprintf(text, sizeof(text), "uploaded:%s:%lu", id, (unsigned long)size) < 0) {
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to format upload response");
    return VECTIS_ERR_STATE;
  }
  return vectis_response_text(response, 200, "text/plain", text, error);
}

static int write_file(const char *path, const char *body) {
  FILE *fp;
  size_t length;

  length = strlen(body);
  fp = fopen(path, "wb");
  if (fp == NULL) {
    return 1;
  }
  if (fwrite(body, 1u, length, fp) != length) {
    (void)fclose(fp);
    return 1;
  }
  return fclose(fp) == 0 ? 0 : 1;
}

static int bytes_contains(const void *data,
                          size_t size,
                          const char *needle) {
  const unsigned char *bytes;
  size_t needle_size;
  size_t i;

  if (data == NULL || needle == NULL) {
    return 0;
  }
  bytes = (const unsigned char *)data;
  needle_size = strlen(needle);
  if (needle_size == 0u || needle_size > size) {
    return 0;
  }
  for (i = 0u; i + needle_size <= size; ++i) {
    if (memcmp(bytes + i, needle, needle_size) == 0) {
      return 1;
    }
  }
  return 0;
}

static int run_server(void) {
  vectis_app_config config;
  vectis_route_config route;
  vectis_error error;
  vectis_app *app;
  const char *download_path;

  download_path = env_or_default("VECTIS_DOWNSTREAM_DOWNLOAD_PATH",
                                 "/tmp/vectis-downstream-download.txt");
  if (write_file(download_path, "download-body\n") != 0) {
    fprintf(stderr, "failed to create downstream download file\n");
    return 1;
  }
  vectis_app_config_init(&config);
  config.app_name = "curl-downstream-e2e";
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = env_or_default("VECTIS_KORE_BIND", "127.0.0.1");
  config.tls.port = env_port_or_default("VECTIS_KORE_PORT", 28083u);
  config.server.max_request_body_bytes = VECTIS_BODY_DEFAULT_UPLOAD_MAX_BYTES;

  app = vectis_app_new(&config, &error);
  if (app == NULL) {
    return print_error("vectis_app_new", &error);
  }
  route = vectis_route_methods(VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD,
                               "/health",
                               health_handler,
                               NULL);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    (void)print_error("register /health", &error);
    app->close(app);
    return 1;
  }
  route = vectis_route(VECTIS_HTTP_OPTIONS, "/events", event_handler, NULL);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    (void)print_error("register OPTIONS /events", &error);
    app->close(app);
    return 1;
  }
  route = vectis_json_body_route(VECTIS_HTTP_POST, "/events", event_handler, NULL);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    (void)print_error("register POST /events", &error);
    app->close(app);
    return 1;
  }
  route = vectis_json_body_route(VECTIS_HTTP_POST, "/limited", event_handler, NULL);
  route.body.max_bytes = 4u;
  route.body.memory_buffer_limit_bytes = 4u;
  if (app->route(app, &route, &error) != VECTIS_OK) {
    (void)print_error("register POST /limited", &error);
    app->close(app);
    return 1;
  }
  route = vectis_json_body_route_methods(VECTIS_HTTP_METHODS_PUT | VECTIS_HTTP_METHODS_PATCH,
                                         "/events/:id",
                                         event_handler,
                                         NULL);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    (void)print_error("register PUT/PATCH /events/:id", &error);
    app->close(app);
    return 1;
  }
  route = vectis_route(VECTIS_HTTP_DELETE, "/events/:id", event_handler, NULL);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    (void)print_error("register DELETE /events/:id", &error);
    app->close(app);
    return 1;
  }
  route = vectis_route(VECTIS_HTTP_GET, "/stream/:id", stream_handler, NULL);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    (void)print_error("register /stream/:id", &error);
    app->close(app);
    return 1;
  }
  route = vectis_route(VECTIS_HTTP_GET, "/download", file_handler, (void *)download_path);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    (void)print_error("register /download", &error);
    app->close(app);
    return 1;
  }
  route = vectis_upload_route(VECTIS_HTTP_PUT, "/upload/:id", upload_handler, NULL);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    (void)print_error("register /upload/:id", &error);
    app->close(app);
    return 1;
  }
  if (app->start(app, &error) != VECTIS_OK) {
    (void)print_error("app->start", &error);
    app->close(app);
    return 1;
  }
  serve_forever();
  app->close(app);
  return 0;
}

static vectis_status stream_capture_body(const void *data,
                                         size_t size,
                                         void *userdata,
                                         vectis_error *error) {
  stream_capture *capture;

  (void)error;
  capture = (stream_capture *)userdata;
  if (capture == NULL || data == NULL ||
      size > sizeof(capture->data) - capture->size - 1u) {
    return VECTIS_ERR_STATE;
  }
  memcpy(capture->data + capture->size, data, size);
  capture->size += size;
  capture->data[capture->size] = '\0';
  return VECTIS_OK;
}

static int run_json_call(vectis_http_client *client,
                         vectis_http_method method,
                         const char *path,
                         const char *expected_id,
                         lonejson_int64 count) {
  vectis_http_request request;
  vectis_http_response response;
  vectis_error error;
  downstream_doc input;
  downstream_doc output;
  vectis_status status;

  memset(&response, 0, sizeof(response));
  memset(&input, 0, sizeof(input));
  memset(&output, 0, sizeof(output));
  if (snprintf(input.id, sizeof(input.id), "%s", expected_id) < 0) {
    return 1;
  }
  input.count = count;
  vectis_http_request_init(&request);
  request.method = method;
  request.url = path;
  request.json_map = &downstream_doc_map;
  request.json_value = &input;
  status = client->execute(client, &request, &response, &error);
  if (require_status(status, VECTIS_OK, "json HTTP call", &error) != 0 ||
      require_http_status(&response, 200L, "json HTTP call") != 0) {
    vectis_http_response_cleanup(&response);
    return 1;
  }
  status = vectis_http_response_json_into(&response, &downstream_doc_map, &output, &error);
  if (require_status(status, VECTIS_OK, "parse JSON response", &error) != 0) {
    vectis_http_response_cleanup(&response);
    return 1;
  }
  if (strcmp(output.id, expected_id) != 0 || output.count != count + 1) {
    fprintf(stderr, "unexpected JSON response id=%s count=%ld\n",
            output.id,
            (long)output.count);
    vectis_http_response_cleanup(&response);
    return 1;
  }
  vectis_http_response_cleanup(&response);
  return 0;
}

static int run_client(void) {
  vectis_http_client_config config;
  vectis_http_client *client;
  vectis_http_request request;
  vectis_http_response response;
  vectis_error error;
  vectis_status status;
  stream_capture capture;
  const char *base_url;
  const char *upload_path;
  const char *bad_upload_path;
  const char *download_path;
  FILE *fp;
  char downloaded[64];

  client = NULL;
  memset(&response, 0, sizeof(response));
  base_url = env_or_default("VECTIS_DOWNSTREAM_BASE_URL", "http://127.0.0.1:28083");
  upload_path = env_or_default("VECTIS_DOWNSTREAM_UPLOAD_PATH",
                               "/tmp/vectis-downstream-upload.txt");
  bad_upload_path = env_or_default("VECTIS_DOWNSTREAM_BAD_UPLOAD_PATH",
                                   "/tmp/vectis-downstream-bad-upload.txt");
  download_path = env_or_default("VECTIS_DOWNSTREAM_CLIENT_DOWNLOAD_PATH",
                                 "/tmp/vectis-downstream-client-download.txt");
  if (write_file(upload_path, "upload-body") != 0) {
    fprintf(stderr, "failed to write upload fixture\n");
    return 1;
  }
  if (write_file(bad_upload_path, "wrong-body") != 0) {
    fprintf(stderr, "failed to write bad upload fixture\n");
    return 1;
  }
  vectis_http_client_config_init(&config);
  config.base_url = base_url;
  config.timeout_ms = 5000L;
  config.connect_timeout_ms = 500L;
  if (vectis_http_client_new(&config, &client, &error) != VECTIS_OK) {
    return print_error("vectis_http_client_new", &error);
  }

  status = client->get(client, "/health", &response, &error);
  if (require_status(status, VECTIS_OK, "GET /health", &error) != 0 ||
      require_http_status(&response, 200L, "GET /health") != 0 ||
      response.body_size != 3u ||
      memcmp(response.body, "ok\n", 3u) != 0) {
    vectis_http_response_cleanup(&response);
    client->close(client);
    return 1;
  }
  vectis_http_response_cleanup(&response);

  status = client->get(client, "/missing", &response, &error);
  if (require_status(status, VECTIS_OK, "GET /missing", &error) != 0 ||
      require_http_status(&response, 404L, "GET /missing") != 0) {
    vectis_http_response_cleanup(&response);
    client->close(client);
    return 1;
  }
  vectis_http_response_cleanup(&response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_POST;
  request.url = "/events";
  request.body = "{bad-json";
  request.body_size = strlen("{bad-json");
  request.content_type = "application/json";
  status = client->execute(client, &request, &response, &error);
  if (require_status(status, VECTIS_OK, "POST /events malformed JSON", &error) != 0 ||
      require_http_status(&response, 400L, "POST /events malformed JSON") != 0) {
    vectis_http_response_cleanup(&response);
    client->close(client);
    return 1;
  }
  vectis_http_response_cleanup(&response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_POST;
  request.url = "/events";
  request.body = "{}";
  request.body_size = strlen("{}");
  request.content_type = "application/json";
  status = client->execute(client, &request, &response, &error);
  if (require_status(status, VECTIS_OK, "POST /events missing required field", &error) != 0 ||
      require_http_status(&response, 400L, "POST /events missing required field") != 0) {
    vectis_http_response_cleanup(&response);
    client->close(client);
    return 1;
  }
  vectis_http_response_cleanup(&response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_POST;
  request.url = "/limited";
  request.body = "12345";
  request.body_size = strlen("12345");
  request.content_type = "application/json";
  status = client->execute(client, &request, &response, &error);
  if (require_status(status, VECTIS_OK, "POST /limited oversized body", &error) != 0 ||
      require_http_status(&response, 413L, "POST /limited oversized body") != 0) {
    vectis_http_response_cleanup(&response);
    client->close(client);
    return 1;
  }
  vectis_http_response_cleanup(&response);

  status = client->head(client, "/health", &response, &error);
  if (require_status(status, VECTIS_OK, "HEAD /health", &error) != 0 ||
      require_http_status(&response, 204L, "HEAD /health") != 0 ||
      response.body_size != 0u) {
    vectis_http_response_cleanup(&response);
    client->close(client);
    return 1;
  }
  vectis_http_response_cleanup(&response);

  status = client->options(client, "/events", &response, &error);
  if (require_status(status, VECTIS_OK, "OPTIONS /events", &error) != 0 ||
      require_http_status(&response, 204L, "OPTIONS /events") != 0) {
    vectis_http_response_cleanup(&response);
    client->close(client);
    return 1;
  }
  vectis_http_response_cleanup(&response);

  if (run_json_call(client, VECTIS_HTTP_POST, "/events", "order-1001", 1) != 0 ||
      run_json_call(client, VECTIS_HTTP_PUT, "/events/order-1001", "order-1001", 2) != 0 ||
      run_json_call(client, VECTIS_HTTP_PATCH, "/events/order-1001", "order-1001", 3) != 0) {
    client->close(client);
    return 1;
  }

  status = client->delete_(client, "/events/order-1001", &response, &error);
  if (require_status(status, VECTIS_OK, "DELETE /events/order-1001", &error) != 0 ||
      require_http_status(&response, 204L, "DELETE /events/order-1001") != 0) {
    vectis_http_response_cleanup(&response);
    client->close(client);
    return 1;
  }
  vectis_http_response_cleanup(&response);

  memset(&capture, 0, sizeof(capture));
  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = "/stream/order-1001";
  request.response_body = stream_capture_body;
  request.response_body_userdata = &capture;
  status = client->execute(client, &request, &response, &error);
  if (require_status(status, VECTIS_OK, "GET /stream/order-1001", &error) != 0 ||
      require_http_status(&response, 200L, "GET /stream/order-1001") != 0 ||
      strcmp(capture.data, "stream:order-1001") != 0) {
    vectis_http_response_cleanup(&response);
    client->close(client);
    return 1;
  }
  vectis_http_response_cleanup(&response);

  status = client->download_file(client, "/download", download_path, &response, &error);
  if (require_status(status, VECTIS_OK, "GET /download", &error) != 0 ||
      require_http_status(&response, 200L, "GET /download") != 0) {
    vectis_http_response_cleanup(&response);
    client->close(client);
    return 1;
  }
  vectis_http_response_cleanup(&response);
  fp = fopen(download_path, "rb");
  if (fp == NULL) {
    client->close(client);
    return 1;
  }
  memset(downloaded, 0, sizeof(downloaded));
  if (fread(downloaded, 1u, strlen("download-body\n"), fp) != strlen("download-body\n")) {
    (void)fclose(fp);
    client->close(client);
    return 1;
  }
  (void)fclose(fp);
  if (strcmp(downloaded, "download-body\n") != 0) {
    fprintf(stderr, "unexpected downloaded content: %s\n", downloaded);
    client->close(client);
    return 1;
  }

  status = client->upload_file(client,
                                          VECTIS_HTTP_PUT,
                                          "/upload/order-1001",
                                          upload_path,
                                          "text/plain",
                                          &response,
                                          &error);
  if (require_status(status, VECTIS_OK, "PUT /upload/order-1001", &error) != 0 ||
      require_http_status(&response, 200L, "PUT /upload/order-1001") != 0 ||
      response.body_size == 0u ||
      !bytes_contains(response.body, response.body_size, "uploaded:order-1001")) {
    vectis_http_response_cleanup(&response);
    client->close(client);
    return 1;
  }
  vectis_http_response_cleanup(&response);

  status = client->upload_file(client,
                                          VECTIS_HTTP_PUT,
                                          "/upload/order-1001",
                                          bad_upload_path,
                                          "text/plain",
                                          &response,
                                          &error);
  if (require_status(status, VECTIS_OK, "PUT /upload/order-1001 bad body", &error) != 0 ||
      require_http_status(&response, 422L, "PUT /upload/order-1001 bad body") != 0) {
    vectis_http_response_cleanup(&response);
    client->close(client);
    return 1;
  }
  vectis_http_response_cleanup(&response);
  client->close(client);
  (void)remove(upload_path);
  (void)remove(bad_upload_path);
  (void)remove(download_path);
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "server") == 0) {
    return run_server();
  }
  if (argc == 2 && strcmp(argv[1], "client") == 0) {
    return run_client();
  }
  fprintf(stderr, "usage: %s server|client\n", argc > 0 ? argv[0] : "curl_downstream_e2e");
  return 2;
}
