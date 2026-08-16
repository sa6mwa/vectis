#include "vectis_internal.h"
#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <lc/lc.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <vectis/auth.h>
#include <vectis/embedded_fs.h>
#include <vectis/totp_qr.h>
#include <vectis/vectis.h>
#include <vectis/webdav.h>

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#define VECTIS_TEST_ASAN 1
#else
#define VECTIS_TEST_ASAN 0
#endif

typedef struct source_json_doc {
  lonejson_source payload;
} source_json_doc;

typedef struct runtime_dsv_row {
  char id[32];
  lonejson_int64 count;
  int active;
} runtime_dsv_row;

typedef struct runtime_xml_doc {
  char body[2048];
} runtime_xml_doc;

typedef struct runtime_dsv_summary {
  size_t rows;
  lonejson_int64 total;
  size_t active;
} runtime_dsv_summary;

typedef struct stream_probe_context {
  size_t open_count;
  size_t write_count;
  size_t finish_count;
  size_t close_count;
  size_t total_size;
  int saw_body_reader;
  int saw_body_path;
} stream_probe_context;

typedef struct runtime_thread_probe {
  volatile int done;
} runtime_thread_probe;

static const lonejson_field source_json_doc_fields[] = {
    LONEJSON_FIELD_STRING_SOURCE_REQ(source_json_doc, payload, "payload")};

static const lonejson_field runtime_dsv_row_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(runtime_dsv_row, id, "id",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64_REQ(runtime_dsv_row, count, "count"),
    LONEJSON_FIELD_BOOL_REQ(runtime_dsv_row, active, "active")};

static const lonejson_field runtime_xml_doc_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(runtime_xml_doc, body, "body",
                                    LONEJSON_OVERFLOW_FAIL)};

LONEJSON_MAP_DEFINE(source_json_doc_map, source_json_doc,
                    source_json_doc_fields);
LONEJSON_MAP_DEFINE(runtime_dsv_row_map, runtime_dsv_row,
                    runtime_dsv_row_fields);
LONEJSON_MAP_DEFINE(runtime_xml_doc_map, runtime_xml_doc,
                    runtime_xml_doc_fields);

static vectis_status sample_handler(vectis_app *app, vectis_request *request,
                                    vectis_response *response, void *userdata,
                                    vectis_error *error) {
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 200, "text/plain", "ok", error);
}

static int sample_consumer_handler(void *context, lc_consumer_message *message,
                                   lc_error *error) {
  (void)context;
  (void)message;
  (void)error;
  return LC_OK;
}

static int failing_consumer_handler(void *context, lc_consumer_message *message,
                                    lc_error *error) {
  int *count;

  (void)message;
  (void)error;
  count = (int *)context;
  if (count != NULL) {
    *count += 1;
  }
  return LC_ERR_PROTOCOL;
}

static int failing_consumer_on_error(void *context,
                                     const lc_consumer_error *event,
                                     lc_error *error) {
  (void)context;
  (void)event;
  (void)error;
  return LC_ERR_PROTOCOL;
}

static void enqueue_lockd_test_message(const char *endpoint,
                                       const char *queue) {
  const char *endpoints[1];
  lc_client_config client_config;
  lc_client *client;
  lc_error lcerr;
  lc_enqueue_req enqueue_req;
  lc_enqueue_res enqueue_res;
  lc_source *source;
  const char payload[] = "{\"ok\":true}";
  int rc;

  endpoints[0] = endpoint;
  lc_error_init(&lcerr);
  lc_client_config_init(&client_config);
  client_config.endpoints = endpoints;
  client_config.endpoint_count = 1u;
  client = NULL;
  rc = lc_client_open(&client_config, &client, &lcerr);
  assert(rc == LC_OK);
  lc_enqueue_req_init(&enqueue_req);
  enqueue_req.queue = queue;
  enqueue_req.visibility_timeout_seconds = 1L;
  enqueue_req.ttl_seconds = 60L;
  source = NULL;
  rc = lc_source_from_memory(payload, sizeof(payload) - 1u, &source, &lcerr);
  assert(rc == LC_OK);
  memset(&enqueue_res, 0, sizeof(enqueue_res));
  rc = client->enqueue(client, &enqueue_req, source, &enqueue_res, &lcerr);
  assert(rc == LC_OK);
  lc_source_close(source);
  lc_enqueue_res_cleanup(&enqueue_res);
  lc_client_close(client);
  lc_error_cleanup(&lcerr);
}

static void *runtime_probe_thread(void *userdata) {
  runtime_thread_probe *probe;
  struct timespec delay;

  probe = (runtime_thread_probe *)userdata;
  delay.tv_sec = 0;
  delay.tv_nsec = 10000000L;
  while (probe != NULL && !probe->done) {
    (void)nanosleep(&delay, NULL);
  }
  return NULL;
}

static vectis_status state_error_handler(vectis_app *app,
                                         vectis_request *request,
                                         vectis_response *response,
                                         void *userdata, vectis_error *error) {
  (void)app;
  (void)request;
  (void)response;
  (void)userdata;
  vectis_set_error(error, VECTIS_ERR_STATE, "handler dependency failed");
  return VECTIS_ERR_STATE;
}

static vectis_status metadata_handler(vectis_app *app, vectis_request *request,
                                      vectis_response *response, void *userdata,
                                      vectis_error *error) {
  const char *expand;
  const char *trace;

  (void)app;
  (void)userdata;
  expand = vectis_request_query(request, "expand");
  trace = vectis_request_header(request, "x-vectis-trace");
  if (expand == NULL || strcmp(expand, "items and logs") != 0) {
    return vectis_response_text(response, 422, "text/plain", "bad query",
                                error);
  }
  if (trace == NULL || strcmp(trace, "runtime-smoke") != 0) {
    return vectis_response_text(response, 422, "text/plain", "bad header",
                                error);
  }
  return vectis_response_text(response, 200, "text/plain", "metadata", error);
}

static vectis_status json_source_handler(vectis_app *app,
                                         vectis_request *request,
                                         vectis_response *response,
                                         void *userdata, vectis_error *error) {
  const char *content_length;
  const char *transfer_encoding;

  (void)app;
  (void)userdata;
  content_length = vectis_request_header(request, "content-length");
  transfer_encoding = vectis_request_header(request, "transfer-encoding");
  if (content_length == NULL || strcmp(content_length, "17") != 0) {
    return vectis_response_text(response, 422, "text/plain",
                                "bad content-length", error);
  }
  if (transfer_encoding != NULL) {
    return vectis_response_text(response, 422, "text/plain",
                                "unexpected transfer-encoding", error);
  }
  return vectis_response_text(response, 200, "text/plain", "json-source",
                              error);
}

static vectis_status upload_handler(vectis_app *app, vectis_request *request,
                                    vectis_response *response, void *userdata,
                                    vectis_error *error) {
  vectis_body_materialize_config config;
  vectis_body_materialized body;
  vectis_status status;

  (void)app;
  (void)userdata;
  vectis_body_materialize_config_init(&config);
  config.memory_limit_bytes = 0u;
  config.prefix = "vectis-runtime-upload";
  status = vectis_request_body_materialize(request, &config, &body, error);
  if (status != VECTIS_OK) {
    return status;
  }
  if (body.kind != VECTIS_BODY_MATERIALIZED_MEMORY ||
      body.memory.data == NULL || body.memory.size == 0u) {
    vectis_body_materialized_cleanup(&body);
    return vectis_response_text(response, 422, "text/plain", "not buffered",
                                error);
  }
  if (((const char *)body.memory.data)[0] != 'x') {
    vectis_body_materialized_cleanup(&body);
    return vectis_response_text(response, 422, "text/plain", "bad buffer",
                                error);
  }
  vectis_body_materialized_cleanup(&body);
  return vectis_response_text(response, 200, "text/plain", "buffered", error);
}

static vectis_status spooled_upload_handler(vectis_app *app,
                                            vectis_request *request,
                                            vectis_response *response,
                                            void *userdata,
                                            vectis_error *error) {
  vectis_body_materialized body;
  vectis_body_materialize_config config;
  vectis_status status;

  (void)app;
  (void)userdata;
  if (!vectis_request_body_is_spooled(request) ||
      vectis_request_body_path(request) == NULL) {
    return vectis_response_text(response, 422, "text/plain", "not spooled",
                                error);
  }
  vectis_body_materialize_config_init(&config);
  config.memory_limit_bytes = 4u;
  status = vectis_request_body_materialize(request, &config, &body, error);
  if (status != VECTIS_OK) {
    return status;
  }
  if (body.kind != VECTIS_BODY_MATERIALIZED_FILE || body.size != 32u) {
    vectis_body_materialized_cleanup(&body);
    return vectis_response_text(response, 422, "text/plain", "bad spooled body",
                                error);
  }
  vectis_body_materialized_cleanup(&body);
  return vectis_response_text(response, 200, "text/plain", "spooled", error);
}

static vectis_status default_spooled_upload_handler(vectis_app *app,
                                                    vectis_request *request,
                                                    vectis_response *response,
                                                    void *userdata,
                                                    vectis_error *error) {
  vectis_body_materialized body;
  vectis_body_materialize_config config;
  vectis_status status;

  (void)app;
  (void)userdata;
  if (!vectis_request_body_is_spooled(request) ||
      vectis_request_body_path(request) == NULL) {
    return vectis_response_text(response, 422, "text/plain",
                                "not default spooled", error);
  }
  vectis_body_materialize_config_init(&config);
  status = vectis_request_body_materialize(request, &config, &body, error);
  if (status != VECTIS_OK) {
    return status;
  }
  if (body.kind != VECTIS_BODY_MATERIALIZED_FILE ||
      body.size != VECTIS_BODY_DEFAULT_MEMORY_BUFFER_LIMIT_BYTES + 1u) {
    vectis_body_materialized_cleanup(&body);
    return vectis_response_text(response, 422, "text/plain",
                                "bad default spooled body", error);
  }
  vectis_body_materialized_cleanup(&body);
  return vectis_response_text(response, 200, "text/plain", "default-spooled",
                              error);
}

static vectis_status stream_probe_open(vectis_app *app, vectis_request *request,
                                       void *userdata, void **state,
                                       vectis_error *error) {
  stream_probe_context *context;

  (void)app;
  (void)request;
  context = (stream_probe_context *)userdata;
  if (context == NULL || state == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "stream context is required");
    return VECTIS_ERR_INVALID;
  }
  context->open_count++;
  *state = context;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status stream_probe_write(vectis_app *app,
                                        vectis_request *request,
                                        const void *data, size_t size,
                                        void *state, void *userdata,
                                        vectis_error *error) {
  stream_probe_context *context;
  vectis_bytes body;
  vectis_status status;

  (void)app;
  (void)userdata;
  context = (stream_probe_context *)state;
  if (context == NULL || data == NULL || size == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "stream chunk is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_request_body_reader(request) != NULL) {
    context->saw_body_reader = 1;
  }
  if (vectis_request_body_path(request) != NULL ||
      vectis_request_body_is_spooled(request)) {
    context->saw_body_path = 1;
  }
  status = vectis_request_body_bytes(request, &body, error);
  if (status != VECTIS_ERR_INVALID) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "streaming upload exposed materialized body bytes");
    return VECTIS_ERR_STATE;
  }
  context->write_count++;
  context->total_size += size;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status stream_probe_finish(vectis_app *app,
                                         vectis_request *request,
                                         vectis_response *response, void *state,
                                         void *userdata, vectis_error *error) {
  stream_probe_context *context;
  char text[64];

  (void)app;
  (void)request;
  (void)userdata;
  context = (stream_probe_context *)state;
  if (context == NULL || context->saw_body_reader || context->saw_body_path) {
    return vectis_response_text(response, 500, "text/plain", "materialized",
                                error);
  }
  context->finish_count++;
  (void)snprintf(text, sizeof(text), "%lu", (unsigned long)context->total_size);
  return vectis_response_text(response, 200, "text/plain", text, error);
}

static void stream_probe_close(vectis_app *app, vectis_request *request,
                               void *state, void *userdata) {
  stream_probe_context *context;

  (void)app;
  (void)request;
  (void)userdata;
  context = (stream_probe_context *)state;
  if (context != NULL) {
    context->close_count++;
  }
}

static vectis_status
stream_reader_handler(vectis_app *app, vectis_request *request,
                      struct lc_source *reader, vectis_response *response,
                      void *userdata, vectis_error *error) {
  lc_error lcerr;
  vectis_bytes body;
  vectis_status status;
  unsigned char chunk[8192];
  size_t total;
  size_t nread;
  char text[64];

  (void)app;
  (void)userdata;
  if (reader == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "upload reader is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_request_body_reader(request) != NULL ||
      vectis_request_body_path(request) != NULL ||
      vectis_request_body_is_spooled(request)) {
    return vectis_response_text(response, 500, "text/plain", "materialized",
                                error);
  }
  status = vectis_request_body_bytes(request, &body, error);
  if (status != VECTIS_ERR_INVALID) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "streaming reader exposed materialized body bytes");
    return VECTIS_ERR_STATE;
  }
  total = 0u;
  lc_error_init(&lcerr);
  for (;;) {
    nread = reader->read(reader, chunk, sizeof(chunk), &lcerr);
    if (nread == 0u) {
      if (lcerr.code != 0) {
        vectis_set_error(error, VECTIS_ERR_STATE,
                         "failed to read streaming upload");
        lc_error_cleanup(&lcerr);
        return VECTIS_ERR_STATE;
      }
      break;
    }
    total += nread;
  }
  lc_error_cleanup(&lcerr);
  (void)snprintf(text, sizeof(text), "%lu", (unsigned long)total);
  return vectis_response_text(response, 200, "text/plain", text, error);
}

static vectis_status xml_route_handler(vectis_app *app, vectis_request *request,
                                       void *input, vectis_response *response,
                                       void *userdata, vectis_error *error) {
  runtime_xml_doc *doc;
  char text[64];

  (void)app;
  (void)userdata;
  doc = (runtime_xml_doc *)input;
  if (doc == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML input is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_request_body_reader(request) != NULL ||
      vectis_request_body_path(request) != NULL ||
      vectis_request_body_is_spooled(request)) {
    return vectis_response_text(response, 500, "text/plain", "materialized",
                                error);
  }
  (void)snprintf(text, sizeof(text), "%lu", (unsigned long)strlen(doc->body));
  return vectis_response_text(response, 200, "text/plain", text, error);
}

static vectis_status dsv_route_handler(vectis_app *app, vectis_request *request,
                                       vectis_dsv_rows *rows,
                                       vectis_response *response,
                                       void *userdata, vectis_error *error) {
  runtime_dsv_summary *summary;
  const runtime_dsv_row *row;
  const void *row_ptr;
  size_t row_number;
  int has_row;
  char text[128];
  vectis_status status;

  (void)app;
  summary = (runtime_dsv_summary *)userdata;
  if (summary == NULL || rows == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV summary is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_request_body_reader(request) != NULL ||
      vectis_request_body_path(request) != NULL ||
      vectis_request_body_is_spooled(request)) {
    return vectis_response_text(response, 500, "text/plain", "materialized",
                                error);
  }
  memset(summary, 0, sizeof(*summary));
  for (;;) {
    status = vectis_dsv_rows_next(rows, &has_row, &row_number, &row_ptr, error);
    if (status != VECTIS_OK) {
      return status;
    }
    if (!has_row) {
      break;
    }
    row = (const runtime_dsv_row *)row_ptr;
    assert(row_number == summary->rows + 1u);
    summary->rows++;
    summary->total += row->count;
    if (row->active) {
      summary->active++;
    }
  }
  (void)snprintf(text, sizeof(text), "%lu:%lld:%lu",
                 (unsigned long)summary->rows, (long long)summary->total,
                 (unsigned long)summary->active);
  return vectis_response_text(response, 200, "text/plain", text, error);
}

static vectis_status file_handler(vectis_app *app, vectis_request *request,
                                  vectis_response *response, void *userdata,
                                  vectis_error *error) {
  (void)app;
  (void)request;
  return vectis_response_file(response, 200, "text/plain",
                              (const char *)userdata, error);
}

static vectis_status param_handler(vectis_app *app, vectis_request *request,
                                   vectis_response *response, void *userdata,
                                   vectis_error *error) {
  const char *id;
  const char *item_id;

  (void)app;
  (void)userdata;
  id = vectis_request_path_param(request, "id");
  item_id = vectis_request_path_param(request, "item_id");
  if (item_id != NULL) {
    return vectis_response_text(response, 200, "text/plain", item_id, error);
  }
  if (id != NULL) {
    return vectis_response_text(response, 200, "text/plain", id, error);
  }
  return vectis_response_status(response, 422, error);
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

static int reserve_loopback_port(unsigned short *out) {
  struct sockaddr_in addr;
  socklen_t addr_len;
  int fd;
  int opt;
  int rc;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  opt = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, (socklen_t)sizeof(opt));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(0u);
  rc = inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  assert(rc == 1);
  rc = bind(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr));
  assert(rc == 0);
  rc = listen(fd, 1);
  assert(rc == 0);
  addr_len = (socklen_t)sizeof(addr);
  rc = getsockname(fd, (struct sockaddr *)&addr, &addr_len);
  assert(rc == 0);
  *out = ntohs(addr.sin_port);
  return fd;
}

static vectis_status
metrics_required_provider(const vectis_auth_provider_request *request,
                          vectis_auth_provider_response *response,
                          void *userdata, vectis_error *error) {
  (void)request;
  (void)userdata;
  (void)error;
  response->action = VECTIS_AUTH_REQUIRED;
  response->status_code = 401;
  response->content_type = "text/plain; charset=utf-8";
  response->body = "metrics auth required\n";
  response->body_size = strlen((const char *)response->body);
  (void)snprintf(response->www_authenticate, sizeof(response->www_authenticate),
                 "%s", "Bearer realm=\"metrics\"");
  return VECTIS_OK;
}

static void assert_metrics_surface(void) {
  vectis_app *app;
  vectis_metrics_config metrics;
  vectis_mutable_bytes snapshot;
  vectis_request *request;
  vectis_response *response;
  vectis_bytes body;
  vectis_error error;
  vectis_status status;
  vectis_auth_provider provider;

  vectis_error_clear(&error);
  app = vectis_app_new(NULL, &error);
  assert(app != NULL);
  memset(&snapshot, 0, sizeof(snapshot));
  status = vectis_metrics_snapshot_json(app, &snapshot, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(snapshot.data == NULL);

  vectis_metrics_config_init(&metrics);
  metrics.path = "/metrics";
  metrics.json_path = "/metrics.json";
  metrics.title = "runtime metrics";
  status = app->metrics(app, &metrics, &error);
  assert(status == VECTIS_OK);
  assert(app->route_count(app) == 2u);
  status = vectis_metrics_snapshot_json(app, &snapshot, &error);
  assert(status == VECTIS_OK);
  assert(snapshot.data != NULL);
  assert(strstr((const char *)snapshot.data,
                "\"service\":\"runtime metrics\"") != NULL);
  assert(strstr((const char *)snapshot.data, "\"requests_total\":0") != NULL);
  vectis_mutable_bytes_cleanup(&snapshot);

  request = vectis_internal_request_new(&error);
  response = vectis_internal_response_new(&error);
  assert(request != NULL && response != NULL);
  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_GET, "/metrics.json",
                                          request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 200);
  assert(strcmp(vectis_internal_response_content_type(response),
                "application/json") == 0);
  body = vectis_internal_response_body(response);
  assert(body.data != NULL &&
         strstr((const char *)body.data, "\"route_count\":2") != NULL);
  vectis_internal_response_free(response);
  vectis_internal_request_free(request);

  request = vectis_internal_request_new(&error);
  response = vectis_internal_response_new(&error);
  assert(request != NULL && response != NULL);
  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_GET, "/metrics",
                                          request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 200);
  assert(strcmp(vectis_internal_response_content_type(response),
                "text/html; charset=utf-8") == 0);
  body = vectis_internal_response_body(response);
  assert(body.data != NULL &&
         strstr((const char *)body.data, "runtime metrics") != NULL);
  vectis_internal_response_free(response);
  vectis_internal_request_free(request);
  app->close(app);

  app = vectis_app_new(NULL, &error);
  assert(app != NULL);
  vectis_auth_provider_init(&provider);
  status = vectis_auth_provider_from_callback(
      &provider, metrics_required_provider, NULL, &error);
  assert(status == VECTIS_OK);
  vectis_metrics_config_init(&metrics);
  metrics.path = "/secure-metrics";
  metrics.json_path = "/secure-metrics.json";
  metrics.auth_provider = &provider;
  metrics.auth_purpose = "metrics";
  status = app->metrics(app, &metrics, &error);
  assert(status == VECTIS_OK);
  request = vectis_internal_request_new(&error);
  response = vectis_internal_response_new(&error);
  assert(request != NULL && response != NULL);
  status = vectis_internal_dispatch_route(
      app, VECTIS_HTTP_GET, "/secure-metrics.json", request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 401);
  body = vectis_internal_response_body(response);
  assert(body.data != NULL &&
         strstr((const char *)body.data, "metrics auth required") != NULL);
  vectis_internal_response_free(response);
  vectis_internal_request_free(request);
  status = vectis_metrics_snapshot_json(app, &snapshot, &error);
  assert(status == VECTIS_OK);
  assert(strstr((const char *)snapshot.data, "\"required\":1") != NULL);
  vectis_mutable_bytes_cleanup(&snapshot);
  app->close(app);
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

static ssize_t socket_recv_some(int fd, char *buffer, size_t size,
                                long timeout_ms) {
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

static int bytes_contain(const void *haystack, size_t haystack_size,
                         const char *needle) {
  const unsigned char *bytes;
  size_t needle_size;
  size_t i;

  if (haystack == NULL || needle == NULL) {
    return 0;
  }
  needle_size = strlen(needle);
  if (needle_size == 0u || needle_size > haystack_size) {
    return 0;
  }
  bytes = (const unsigned char *)haystack;
  for (i = 0u; i + needle_size <= haystack_size; ++i) {
    if (memcmp(bytes + i, needle, needle_size) == 0) {
      return 1;
    }
  }
  return 0;
}

static char runtime_base64_digit(unsigned value) {
  static const char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  return table[value & 63u];
}

static int runtime_base64_encode(const char *input, char *out,
                                 size_t out_size) {
  size_t input_len;
  size_t offset;
  size_t written;
  unsigned a;
  unsigned b;
  unsigned c;

  input_len = strlen(input);
  written = 0u;
  for (offset = 0u; offset < input_len; offset += 3u) {
    if (written + 4u >= out_size) {
      return 0;
    }
    a = (unsigned)(unsigned char)input[offset];
    b = offset + 1u < input_len ? (unsigned)(unsigned char)input[offset + 1u]
                                : 0u;
    c = offset + 2u < input_len ? (unsigned)(unsigned char)input[offset + 2u]
                                : 0u;
    out[written++] = runtime_base64_digit(a >> 2u);
    out[written++] = runtime_base64_digit(((a & 3u) << 4u) | (b >> 4u));
    out[written++] = offset + 1u < input_len
                         ? runtime_base64_digit(((b & 15u) << 2u) | (c >> 6u))
                         : '=';
    out[written++] = offset + 2u < input_len ? runtime_base64_digit(c) : '=';
  }
  out[written] = '\0';
  return 1;
}

static int runtime_response_line_value(const void *body, size_t body_size,
                                       const char *key, char *out,
                                       size_t out_size) {
  const unsigned char *bytes;
  size_t key_size;
  size_t start;
  size_t end;
  size_t value_size;

  if (body == NULL || key == NULL || out == NULL || out_size == 0u) {
    return 0;
  }
  bytes = (const unsigned char *)body;
  key_size = strlen(key);
  for (start = 0u; start + key_size < body_size; ++start) {
    if ((start == 0u || bytes[start - 1u] == '\n') &&
        memcmp(bytes + start, key, key_size) == 0 &&
        bytes[start + key_size] == '=') {
      start += key_size + 1u;
      end = start;
      while (end < body_size && bytes[end] != '\n') {
        end++;
      }
      value_size = end - start;
      if (value_size >= out_size) {
        return 0;
      }
      memcpy(out, bytes + start, value_size);
      out[value_size] = '\0';
      return 1;
    }
  }
  return 0;
}

static vectis_status
runtime_webdav_auth(const vectis_webdav_auth_request *request,
                    vectis_webdav_auth_response *response, void *userdata,
                    vectis_error *error) {
  const char *token;

  (void)userdata;
  vectis_error_clear(error);
  vectis_webdav_auth_response_init(response);
  token = vectis_request_header(request->request, "x-vectis-webdav-auth");
  if (token != NULL && strcmp(token, "ok") == 0) {
    response->action = VECTIS_WEBDAV_AUTH_ALLOW;
    (void)snprintf(response->principal, sizeof(response->principal),
                   "runtime-user");
    return VECTIS_OK;
  }
  if (token != NULL && strcmp(token, "required") == 0) {
    static const char body[] = "runtime auth required";

    response->action = VECTIS_WEBDAV_AUTH_REQUIRED;
    response->status_code = 401;
    response->www_authenticate = "Basic realm=\"runtime\"";
    response->content_type = "text/plain";
    response->body = body;
    response->body_size = sizeof(body) - 1u;
    return VECTIS_OK;
  }
  if (token != NULL && strcmp(token, "deny") == 0) {
    response->action = VECTIS_WEBDAV_AUTH_DENY;
    return VECTIS_OK;
  }
  response->action = VECTIS_WEBDAV_AUTH_REDIRECT;
  response->status_code = 302;
  response->location = "/auth/webdav";
  return VECTIS_OK;
}

static void remove_tree(const char *path) {
  DIR *directory;
  struct dirent *item;
  struct stat st;
  char child[4096];
  int written;

  directory = opendir(path);
  if (directory == NULL) {
    return;
  }
  while ((item = readdir(directory)) != NULL) {
    if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
      continue;
    }
    written = snprintf(child, sizeof(child), "%s/%s", path, item->d_name);
    if (written < 0 || (size_t)written >= sizeof(child) ||
        lstat(child, &st) == -1) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      remove_tree(child);
    } else {
      (void)unlink(child);
    }
  }
  (void)closedir(directory);
  (void)rmdir(path);
}

static int count_fd_dir(const char *path) {
  DIR *dir;
  struct dirent *entry;
  int count;

  dir = opendir(path);
  if (dir == NULL) {
    return 0;
  }
  count = 0;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
      ++count;
    }
  }
  (void)closedir(dir);
  return count;
}

static long proc_ppid(const char *pid_name) {
  char path[512];
  char buffer[512];
  char *end_comm;
  FILE *fp;
  long ppid;
  char state;

  sprintf(path, "/proc/%s/stat", pid_name);
  fp = fopen(path, "r");
  if (fp == NULL) {
    return -1L;
  }
  if (fgets(buffer, sizeof(buffer), fp) == NULL) {
    (void)fclose(fp);
    return -1L;
  }
  (void)fclose(fp);
  end_comm = strrchr(buffer, ')');
  if (end_comm == NULL) {
    return -1L;
  }
  if (sscanf(end_comm + 2, "%c %ld", &state, &ppid) != 2) {
    return -1L;
  }
  (void)state;
  return ppid;
}

static int current_process_tree_fd_count(void) {
  DIR *proc;
  struct dirent *entry;
  char fd_path[512];
  long self;
  long ppid;
  int count;

  self = (long)getpid();
  count = count_fd_dir("/proc/self/fd");
  proc = opendir("/proc");
  assert(proc != NULL);
  while ((entry = readdir(proc)) != NULL) {
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
      continue;
    }
    ppid = proc_ppid(entry->d_name);
    if (ppid == self) {
      sprintf(fd_path, "/proc/%s/fd", entry->d_name);
      count += count_fd_dir(fd_path);
    }
  }
  (void)closedir(proc);
  return count;
}

static void assert_repeated_file_responses_do_not_leak_fds(
    const vectis_http_client_config *http, const char *url,
    const char *expected, size_t expected_size, vectis_error *error) {
  vectis_http_response response;
  vectis_status status;
  int before;
  int after;
  int i;

  memset(&response, 0, sizeof(response));
  for (i = 0; i < 48; ++i) {
    status = vectis_http_get(http, url, &response, error);
    assert(status == VECTIS_OK);
    assert(response.status_code == 200L);
    assert(response.body_size == expected_size);
    assert(memcmp(response.body, expected, expected_size) == 0);
    vectis_http_response_cleanup(&response);
  }
  before = current_process_tree_fd_count();
  for (i = 0; i < 48; ++i) {
    status = vectis_http_get(http, url, &response, error);
    assert(status == VECTIS_OK);
    assert(response.status_code == 200L);
    assert(response.body_size == expected_size);
    assert(memcmp(response.body, expected, expected_size) == 0);
    vectis_http_response_cleanup(&response);
  }
  after = current_process_tree_fd_count();
  assert(after <= before + 2);
}

static void assert_invalid_server_config(vectis_app_config *config,
                                         const char *expected_message) {
  vectis_error error;
  vectis_app *app;

  app = vectis_app_new(config, &error);
  assert(app == NULL);
  assert(strstr(error.message, expected_message) != NULL);
}

static void assert_valid_server_config(vectis_app_config *config) {
  vectis_error error;
  vectis_app *app;

  app = vectis_app_new(config, &error);
  assert(app != NULL);
  app->close(app);
}

static void assert_server_config_validation(void) {
  vectis_app_config config;
  vectis_error error;
  vectis_app *app;

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;

  config.server.max_connections = 0u;
  config.server.max_request_header_bytes = 0u;
  config.server.max_request_body_bytes = 0u;
  config.server.request_header_timeout_ms = 0L;
  config.server.request_body_idle_timeout_ms = 0L;
  config.server.response_write_idle_timeout_ms = 0L;
  config.server.request_body_min_rate_bytes_per_sec = 0u;
  config.server.request_body_min_rate_grace_ms = 0L;
  config.server.idle_timeout_ms = 0L;
  config.server.keepalive_timeout_ms = 0L;
  config.server.keepalive_max_requests = 0u;
  assert_valid_server_config(&config);

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.server.max_request_header_bytes = 1023u;
  assert_invalid_server_config(&config, "max_request_header_bytes");

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.server.request_header_timeout_ms = -1L;
  assert_invalid_server_config(&config, "request_header_timeout_ms");

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.server.request_body_idle_timeout_ms = -1L;
  assert_invalid_server_config(&config, "request_body_idle_timeout_ms");

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.server.response_write_idle_timeout_ms = -1L;
  assert_invalid_server_config(&config, "response_write_idle_timeout_ms");

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.server.request_body_min_rate_grace_ms = -1L;
  assert_invalid_server_config(&config, "request_body_min_rate_grace_ms");

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.server.idle_timeout_ms = -1L;
  assert_invalid_server_config(&config, "idle_timeout_ms");

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.server.keepalive_timeout_ms = -1L;
  assert_invalid_server_config(&config, "keepalive_timeout_ms");

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.server.keepalive_disabled = 1;
  config.server.keepalive_timeout_ms = 0L;
  config.server.keepalive_max_requests = 0u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  app->close(app);
}

static void assert_route_body_policy_validation(void) {
  vectis_app_config config;
  vectis_route_config route;
  vectis_error error;
  vectis_status status;
  vectis_app *app;

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.server.max_request_body_bytes = 64u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  assert(vectis_internal_max_request_body_bytes(app) == 64u);

  route =
      vectis_route(VECTIS_HTTP_POST, "/json-too-large", sample_handler, NULL);
  route.body = vectis_body_json_default();
  route.body.max_bytes = 65u;
  route.body.memory_buffer_limit_bytes = 65u;
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "server max_request_body_bytes") != NULL);

  route = vectis_route(VECTIS_HTTP_POST, "/buffer-equals-server",
                       sample_handler, NULL);
  route.body = vectis_body_buffered_max(64u);
  assert(route.body.memory_buffer_limit_bytes == 64u);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);

  route = vectis_upload_route_max(VECTIS_HTTP_POST, "/upload-too-large", 65u,
                                  sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "server max_request_body_bytes") != NULL);

  route = vectis_upload_route_max(VECTIS_HTTP_POST, "/upload-equals-server",
                                  64u, sample_handler, NULL);
  assert(route.body.memory_buffer_limit_bytes ==
         VECTIS_BODY_DEFAULT_MEMORY_BUFFER_LIMIT_BYTES);
  route.body.memory_buffer_limit_bytes = 8u;
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);

  vectis_route_config_init(&route);
  route.method = VECTIS_HTTP_POST;
  route.path = "/bad-body-mode";
  route.handler = sample_handler;
  route.body.mode = (vectis_body_mode)99;
  route.body.max_bytes = 1u;
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "mode") != NULL);

  route = vectis_route(VECTIS_HTTP_POST, "/zero-buffer-defaults",
                       sample_handler, NULL);
  route.body = vectis_body_buffered_max(64u);
  route.body.max_bytes = 0u;
  route.body.memory_buffer_limit_bytes = 0u;
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);

  route = vectis_upload_route_max(VECTIS_HTTP_POST, "/zero-upload-defaults",
                                  64u, sample_handler, NULL);
  route.body.max_bytes = 0u;
  route.body.memory_buffer_limit_bytes = 0u;
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "server max_request_body_bytes") != NULL);

  route =
      vectis_route(VECTIS_HTTP_POST, "/buffer-over-max", sample_handler, NULL);
  route.body = vectis_body_buffered_max(8u);
  route.body.memory_buffer_limit_bytes = 9u;
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "memory_buffer_limit_bytes") != NULL);

  app->close(app);
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

static void assert_default_header_limit_accepts_64k(void) {
  vectis_app_config config;
  vectis_http_client_config http;
  vectis_http_response response;
  vectis_route_config route;
  vectis_error error;
  vectis_app *app;
  vectis_status status;
  const char *prefix;
  const char *suffix;
  char reply[1024];
  char *request;
  ssize_t nread;
  size_t request_size;
  size_t prefix_size;
  size_t suffix_size;
  size_t value_size;
  int fd;
  int attempt;

  memset(&response, 0, sizeof(response));
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = 28082u;
  config.server.keepalive_disabled = 1;
  config.server.keepalive_timeout_ms = 0L;
  config.server.keepalive_max_requests = 0u;
  assert(config.server.max_request_header_bytes ==
         VECTIS_SERVER_DEFAULT_MAX_REQUEST_HEADER_BYTES);

  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/health", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_OK);

  vectis_http_client_config_init(&http);
  http.timeout_ms = 1000L;
  http.connect_timeout_ms = 200L;
  status = VECTIS_ERR_STATE;
  for (attempt = 0; attempt < 100 && status != VECTIS_OK; ++attempt) {
    vectis_http_response_cleanup(&response);
    status = vectis_http_get(&http, "http://127.0.0.1:28082/health", &response,
                             &error);
    if (status != VECTIS_OK) {
      usleep(100000u);
    }
  }
  assert(status == VECTIS_OK);
  vectis_http_response_cleanup(&response);

  prefix = "GET /health HTTP/1.1\r\n"
           "Host: localhost\r\n"
           "Connection: close\r\n"
           "X-Boundary: ";
  suffix = "\r\n\r\n";
  prefix_size = strlen(prefix);
  suffix_size = strlen(suffix);
  request_size = VECTIS_SERVER_DEFAULT_MAX_REQUEST_HEADER_BYTES;
  assert(request_size > prefix_size + suffix_size);
  value_size = request_size - prefix_size - suffix_size;
  request = (char *)malloc(request_size);
  assert(request != NULL);
  memcpy(request, prefix, prefix_size);
  memset(request + prefix_size, 'a', value_size);
  memcpy(request + prefix_size + value_size, suffix, suffix_size);

  fd = connect_local(28082u);
  socket_send_all(fd, request, request_size);
  free(request);
  memset(reply, 0, sizeof(reply));
  nread = socket_recv_some(fd, reply, sizeof(reply) - 1u, 2000L);
  assert(nread > 0);
  reply[(size_t)nread] = '\0';
  assert(strstr(reply, " 200 ") != NULL);
  (void)shutdown(fd, SHUT_RDWR);
  (void)close(fd);

  status = vectis_stop(app, &error);
  assert(status == VECTIS_OK);
  app->close(app);
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
  vectis_http_request json_source_request;
  vectis_http_request no_body_request;
  vectis_http_request oversized;
  vectis_http_response response;
  vectis_http_response metadata_response;
  vectis_http_response state_error_response;
  vectis_http_response json_source_response;
  vectis_http_response method_response;
  vectis_http_response no_body_response;
  vectis_http_response param_response;
  vectis_http_response oversized_response;
  vectis_http_response upload_response;
  vectis_http_response spooled_upload_response;
  vectis_http_response default_spooled_upload_response;
  vectis_http_response stream_response;
  vectis_http_response stream_reader_response;
  vectis_http_response stream_file_response;
  vectis_http_response xml_route_response;
  vectis_http_response dsv_route_response;
  vectis_http_response embedded_response;
  vectis_http_response embedded_not_modified_response;
  vectis_http_response embedded_if_none_match_miss_response;
  vectis_http_response embedded_range_response;
  vectis_http_response embedded_if_range_response;
  vectis_http_response embedded_if_range_miss_response;
  vectis_http_response embedded_suffix_range_response;
  vectis_http_response embedded_invalid_range_response;
  vectis_http_response embedded_head_response;
  vectis_http_response embedded_missing_response;
  vectis_http_response embedded_method_response;
  vectis_http_response embedded_webdav_response;
  vectis_http_response embedded_webdav_get_response;
  vectis_http_response webdav_response;
  vectis_http_response webdav_get_response;
  vectis_http_response webdav_propfind_response;
  vectis_http_response auth_login_response;
  vectis_http_response auth_bad_response;
  vectis_http_response auth_key_response;
  vectis_http_response auth_logout_response;
  vectis_http_response native_webdav_response;
  vectis_http_response native_webdav_get_response;
  source_json_doc json_source_doc;
  stream_probe_context stream_context;
  runtime_dsv_summary dsv_summary;
  vectis_route_config route;
  vectis_route_config limited_route;
  vectis_route_config upload_route;
  vectis_route_config spooled_upload_route;
  vectis_route_config default_spooled_upload_route;
  vectis_route_config file_route;
  vectis_upload_route_config stream_route;
  vectis_upload_reader_route_config stream_reader_route;
  vectis_upload_file_route_config stream_file_route;
  vectis_xml_route_config xml_route;
  vectis_dsv_route_config dsv_route;
  vectis_static_embedded_config embedded_mount;
  vectis_embedded_fs_config embedded_fs_config;
  vectis_webdav_embedded_site_config embedded_webdav_mount;
  vectis_webdav_config webdav_storage;
  vectis_webdav_mount_config webdav_mount;
  vectis_auth_store_config auth_store;
  vectis_auth_user_config auth_user;
  vectis_auth_user_enrollment auth_enrollment;
  vectis_auth_routes_config auth_routes;
  vectis_auth_native_provider_config native_auth;
  vectis_auth_provider native_auth_provider;
  vectis_webdav_auth_provider_config native_webdav_auth;
  vectis_webdav_mount_config native_webdav_mount;
  vectis_xml_config xml_config;
  vectis_dsv_config dsv_config;
  vectis_app_config second_config;
  vectis_route_config second_route;
  const char *headers[] = {"x-vectis-trace: runtime-smoke"};
  const char *embedded_if_none_match_headers[] = {
      "If-None-Match: "
      "\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d078875901\""};
  const char *embedded_if_none_match_miss_headers[] = {
      "If-None-Match: \"different\"", "Range: bytes=1-2"};
  const char *embedded_range_headers[] = {"Range: bytes=1-2"};
  const char *embedded_if_range_headers[] = {
      "Range: bytes=1-2",
      "If-Range: "
      "\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d078875901\""};
  const char *embedded_if_range_miss_headers[] = {"Range: bytes=1-2",
                                                  "If-Range: \"different\""};
  const char *embedded_suffix_range_headers[] = {"range: bytes=-2"};
  const char *embedded_invalid_range_headers[] = {"range: bytes=4-10"};
  const char upload_path[] = "/tmp/vectis-runtime-upload.bin";
  const char stream_upload_path[] = "/tmp/vectis-runtime-stream-source.bin";
  const char stream_file_path[] = "/tmp/vectis-runtime-stream-upload.bin";
  const char xml_upload_path[] = "/tmp/vectis-runtime-upload.xml";
  const char dsv_upload_path[] = "/tmp/vectis-runtime-upload.csv";
  const char json_source_path[] = "/tmp/vectis-runtime-json-source.txt";
  const char response_file_path[] = "/tmp/vectis-runtime-response.txt";
  const char auth_store_path[] = "/tmp/vectis-runtime-auth.json";
  const char response_file_body[] = "file-response";
  static const unsigned char embedded_payload[] = "hello\napp\n";
  static const char embedded_manifest[] =
      "{\"format\":\"vectis-pack\",\"assets\":["
      "{\"path\":\"/index.html\",\"offset\":0,\"size\":6,"
      "\"sha256\":"
      "\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\","
      "\"content_type\":\"text/html\"},"
      "{\"path\":\"/assets/app.txt\",\"offset\":6,\"size\":4,"
      "\"sha256\":"
      "\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d078875901\","
      "\"content_type\":\"text/plain\"}]}";
  char webdav_cache_dir[] = "/tmp/vectis-runtime-webdav.XXXXXX";
  const char *webdav_headers[] = {"x-vectis-webdav-auth: ok"};
  const char *webdav_required_headers[] = {"x-vectis-webdav-auth: required"};
  const char *webdav_deny_headers[] = {"x-vectis-webdav-auth: deny"};
  const char *native_webdav_headers[1];
  vectis_error error;
  vectis_error second_error;
  vectis_status status;
  vectis_status second_status;
  vectis_app *app;
  vectis_app *second_app;
  vectis_embedded_fs *embedded_fs;
  FILE *fp;
  char *default_spooled_body;
  char *stream_body;
  char size_text[64];
  char dsv_text[128];
  char auth_client_id[128];
  char auth_client_secret[128];
  char auth_clear[320];
  char auth_token[448];
  char auth_header[480];
  char auth_pending_transaction_id[128];
  char auth_totp_code[VECTIS_TOTP_CODE_LENGTH + 1u];
  char auth_totp_form[256];
  size_t default_spooled_body_size;
  size_t stream_body_size;
  size_t xml_body_size;
  size_t dsv_rows;
  long long dsv_total;
  size_t dsv_active;
  long stream_file_size;
  int attempt;
  int i;
  int written;
  vectis_totp auth_totp;

  memset(&response, 0, sizeof(response));
  memset(&metadata_response, 0, sizeof(metadata_response));
  memset(&state_error_response, 0, sizeof(state_error_response));
  memset(&json_source_response, 0, sizeof(json_source_response));
  memset(&method_response, 0, sizeof(method_response));
  memset(&no_body_response, 0, sizeof(no_body_response));
  memset(&param_response, 0, sizeof(param_response));
  memset(&oversized_response, 0, sizeof(oversized_response));
  memset(&upload_response, 0, sizeof(upload_response));
  memset(&spooled_upload_response, 0, sizeof(spooled_upload_response));
  memset(&default_spooled_upload_response, 0,
         sizeof(default_spooled_upload_response));
  memset(&stream_response, 0, sizeof(stream_response));
  memset(&stream_reader_response, 0, sizeof(stream_reader_response));
  memset(&stream_file_response, 0, sizeof(stream_file_response));
  memset(&xml_route_response, 0, sizeof(xml_route_response));
  memset(&dsv_route_response, 0, sizeof(dsv_route_response));
  memset(&embedded_response, 0, sizeof(embedded_response));
  memset(&embedded_not_modified_response, 0,
         sizeof(embedded_not_modified_response));
  memset(&embedded_if_none_match_miss_response, 0,
         sizeof(embedded_if_none_match_miss_response));
  memset(&embedded_if_range_response, 0, sizeof(embedded_if_range_response));
  memset(&embedded_if_range_miss_response, 0,
         sizeof(embedded_if_range_miss_response));
  memset(&embedded_head_response, 0, sizeof(embedded_head_response));
  memset(&embedded_missing_response, 0, sizeof(embedded_missing_response));
  memset(&embedded_method_response, 0, sizeof(embedded_method_response));
  memset(&embedded_webdav_response, 0, sizeof(embedded_webdav_response));
  memset(&embedded_webdav_get_response, 0,
         sizeof(embedded_webdav_get_response));
  memset(&webdav_response, 0, sizeof(webdav_response));
  memset(&webdav_get_response, 0, sizeof(webdav_get_response));
  memset(&webdav_propfind_response, 0, sizeof(webdav_propfind_response));
  memset(&auth_login_response, 0, sizeof(auth_login_response));
  memset(&auth_bad_response, 0, sizeof(auth_bad_response));
  memset(&auth_key_response, 0, sizeof(auth_key_response));
  memset(&auth_logout_response, 0, sizeof(auth_logout_response));
  memset(&native_webdav_response, 0, sizeof(native_webdav_response));
  memset(&native_webdav_get_response, 0, sizeof(native_webdav_get_response));
  memset(&stream_context, 0, sizeof(stream_context));
  memset(&dsv_summary, 0, sizeof(dsv_summary));
  memset(&json_source_request, 0, sizeof(json_source_request));
  memset(&no_body_request, 0, sizeof(no_body_request));
  memset(&json_source_doc, 0, sizeof(json_source_doc));
  embedded_fs = NULL;
  vectis_auth_user_enrollment_init(&auth_enrollment);
  vectis_auth_provider_init(&native_auth_provider);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = 28080u;
  config.server.max_request_header_bytes = 1024u;
  config.server.max_request_body_bytes = 2097152u;
  config.server.request_header_timeout_ms = 1000L;
  config.server.request_body_idle_timeout_ms = 5000L;
  config.server.request_body_min_rate_bytes_per_sec = 1024u;
  config.server.request_body_min_rate_grace_ms = 500L;
  config.server.keepalive_max_requests = 1u;
  fp = fopen(response_file_path, "wb");
  assert(fp != NULL);
  assert(fwrite(response_file_body, 1u, sizeof(response_file_body) - 1u, fp) ==
         sizeof(response_file_body) - 1u);
  assert(fclose(fp) == 0);
  assert(mkdtemp(webdav_cache_dir) != NULL);
  (void)remove(auth_store_path);
  vectis_auth_store_config_init(&auth_store);
  auth_store.credentials_path = auth_store_path;
  status = vectis_auth_store_init(&auth_store, &error);
  assert(status == VECTIS_OK);
  vectis_auth_user_config_init(&auth_user);
  auth_user.username = "runtime-user";
  auth_user.password = "runtime-password";
  status = vectis_auth_user_add_or_update(&auth_store, &auth_user,
                                          &auth_enrollment, &error);
  assert(status == VECTIS_OK);
  vectis_auth_user_enrollment_cleanup(&auth_enrollment);
  vectis_auth_user_config_init(&auth_user);
  auth_user.username = "runtime-totp";
  auth_user.password = "runtime-totp-password";
  auth_user.enable_totp = 1;
  auth_user.totp_secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
  status = vectis_auth_user_add_or_update(&auth_store, &auth_user,
                                          &auth_enrollment, &error);
  assert(status == VECTIS_OK);
  vectis_auth_user_enrollment_cleanup(&auth_enrollment);
  assert(vectis_totp_init(&auth_totp, "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ") ==
         VECTIS_TOTP_QR_OK);
  assert(vectis_totp_generate(&auth_totp, 59u, auth_totp_code) ==
         VECTIS_TOTP_QR_OK);
  vectis_webdav_config_init(&webdav_storage);
  webdav_storage.cache_dir = webdav_cache_dir;
  webdav_storage.site_id = "runtime";
  webdav_storage.max_file_bytes = 4096u;
  webdav_storage.max_total_bytes = 65536u;
  vectis_embedded_fs_config_init(&embedded_fs_config);
  embedded_fs_config.manifest_json = embedded_manifest;
  embedded_fs_config.manifest_json_size = sizeof(embedded_manifest) - 1u;
  embedded_fs_config.payload = embedded_payload;
  embedded_fs_config.payload_size = sizeof(embedded_payload) - 1u;
  status =
      vectis_embedded_fs_from_pack(&embedded_fs_config, &embedded_fs, &error);
  assert(status == VECTIS_OK);
  assert(embedded_fs != NULL);
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/health", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/metadata", metadata_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  route =
      vectis_route(VECTIS_HTTP_GET, "/state-error", state_error_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  route = vectis_json_body_route(VECTIS_HTTP_POST, "/json-source",
                                 json_source_handler, NULL);
  route.body.max_bytes = 4096u;
  route.body.memory_buffer_limit_bytes = 4096u;
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  route =
      vectis_route_methods(VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD |
                               VECTIS_HTTP_METHODS_OPTIONS,
                           "/methods", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_POST, "/no-body", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/orders/:id/items/:item_id?",
                       param_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  limited_route =
      vectis_route(VECTIS_HTTP_POST, "/limited", sample_handler, NULL);
  limited_route.body = vectis_body_buffered_max(4u);
  status = vectis_register_route(app, &limited_route, &error);
  assert(status == VECTIS_OK);
  upload_route = vectis_upload_route_max(VECTIS_HTTP_POST, "/upload", 4096u,
                                         upload_handler, NULL);
  upload_route.body.memory_buffer_limit_bytes = 4096u;
  upload_route.body.disk_spool_disabled = 1;
  status = vectis_register_route(app, &upload_route, &error);
  assert(status == VECTIS_OK);
  spooled_upload_route = vectis_upload_route_max(
      VECTIS_HTTP_POST, "/upload-spooled", 4096u, spooled_upload_handler, NULL);
  spooled_upload_route.body.memory_buffer_limit_bytes = 4u;
  spooled_upload_route.body.disk_spool_disabled = 0;
  status = vectis_register_route(app, &spooled_upload_route, &error);
  assert(status == VECTIS_OK);
  default_spooled_upload_route =
      vectis_upload_route_max(VECTIS_HTTP_POST, "/upload-default-spooled",
                              524288u, default_spooled_upload_handler, NULL);
  assert(default_spooled_upload_route.body.memory_buffer_limit_bytes ==
         VECTIS_BODY_DEFAULT_MEMORY_BUFFER_LIMIT_BYTES);
  assert(default_spooled_upload_route.body.disk_spool_disabled == 0);
  status = vectis_register_route(app, &default_spooled_upload_route, &error);
  assert(status == VECTIS_OK);
  stream_route = vectis_stream_upload_route(
      VECTIS_HTTP_POST, "/stream-upload", stream_probe_open, stream_probe_write,
      stream_probe_finish, stream_probe_close, &stream_context);
  stream_route.body.max_bytes = 2097152u;
  stream_route.body.memory_buffer_limit_bytes = 8u;
  status = app->upload_stream(app, &stream_route, &error);
  assert(status == VECTIS_OK);
  stream_reader_route = vectis_upload_reader_route(
      VECTIS_HTTP_POST, "/stream-reader", stream_reader_handler, NULL);
  stream_reader_route.body.max_bytes = 2097152u;
  stream_reader_route.body.memory_buffer_limit_bytes =
      VECTIS_TEST_ASAN ? 1024u : 4096u;
  stream_reader_route.buffer_bytes = VECTIS_TEST_ASAN ? 1024u : 4096u;
  status = app->upload_reader(app, &stream_reader_route, &error);
  assert(status == VECTIS_OK);
  xml_config = vectis_xml_default();
  xml_config.root_element = "doc";
  xml_route = vectis_xml_route(VECTIS_HTTP_POST, "/xml-upload",
                               &runtime_xml_doc_map, sizeof(runtime_xml_doc),
                               &xml_config, xml_route_handler, NULL);
  xml_route.body.max_bytes = 2097152u;
  xml_route.body.memory_buffer_limit_bytes = 8u;
  xml_route.buffer_bytes = 4096u;
  status = app->xml_route(app, &xml_route, &error);
  assert(status == VECTIS_OK);
  dsv_config = vectis_dsv_csv();
  dsv_route = vectis_dsv_route(VECTIS_HTTP_POST, "/dsv-upload",
                               &runtime_dsv_row_map, sizeof(runtime_dsv_row),
                               &dsv_config, dsv_route_handler, &dsv_summary);
  dsv_route.body.max_bytes = 2097152u;
  dsv_route.body.memory_buffer_limit_bytes = 8u;
  dsv_route.buffer_bytes = 4096u;
  status = app->dsv_route(app, &dsv_route, &error);
  assert(status == VECTIS_OK);
  stream_file_route =
      vectis_upload_file_route(VECTIS_HTTP_POST, "/stream-file",
                               stream_file_path, "application/octet-stream");
  stream_file_route.body.max_bytes = 2097152u;
  stream_file_route.body.memory_buffer_limit_bytes = 8u;
  status = app->upload_file(app, &stream_file_route, &error);
  assert(status == VECTIS_OK);
  file_route = vectis_route(VECTIS_HTTP_GET, "/file", file_handler,
                            (void *)response_file_path);
  status = vectis_register_route(app, &file_route, &error);
  assert(status == VECTIS_OK);
  vectis_static_embedded_config_init(&embedded_mount);
  embedded_mount.path_prefix = "/embedded";
  embedded_mount.fs = embedded_fs;
  status = app->static_embedded(app, &embedded_mount, &error);
  assert(status == VECTIS_OK);
  vectis_webdav_embedded_site_config_init(&embedded_webdav_mount);
  embedded_webdav_mount.path_prefix = "/dav-embedded";
  embedded_webdav_mount.storage = webdav_storage;
  embedded_webdav_mount.storage.site_id = "runtime-embedded";
  embedded_webdav_mount.fs = embedded_fs;
  embedded_webdav_mount.auth = runtime_webdav_auth;
  status = app->webdav_embedded_site(app, &embedded_webdav_mount, &error);
  assert(status == VECTIS_OK);
  vectis_webdav_mount_config_init(&webdav_mount);
  webdav_mount.path_prefix = "/dav";
  webdav_mount.storage = webdav_storage;
  webdav_mount.auth = runtime_webdav_auth;
  status = app->webdav(app, &webdav_mount, &error);
  assert(status == VECTIS_OK);
  vectis_auth_routes_config_init(&auth_routes);
  auth_routes.path_prefix = "/auth";
  auth_routes.store = auth_store;
  auth_routes.login_title = "Runtime Login";
  auth_routes.unix_seconds = 59u;
  status = app->auth_routes(app, &auth_routes, &error);
  assert(status == VECTIS_OK);
  vectis_auth_routes_config_init(&auth_routes);
  auth_routes.path_prefix = "/auth-totp-only";
  auth_routes.store = auth_store;
  auth_routes.required_factors = VECTIS_AUTH_ROUTE_FACTOR_TOTP;
  status = app->auth_routes(app, &auth_routes, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "required_factors") != NULL);
  vectis_error_clear(&error);
  vectis_auth_routes_config_init(&auth_routes);
  auth_routes.path_prefix = "/auth-totp-required";
  auth_routes.store = auth_store;
  auth_routes.login_title = "Runtime Explicit TOTP Login";
  auth_routes.unix_seconds = 59u;
  auth_routes.required_factors =
      VECTIS_AUTH_ROUTE_FACTOR_PASSWORD | VECTIS_AUTH_ROUTE_FACTOR_TOTP;
  status = app->auth_routes(app, &auth_routes, &error);
  assert(status == VECTIS_OK);
  vectis_auth_native_provider_config_init(&native_auth);
  native_auth.store = auth_store;
  native_auth.realm = "runtime";
  native_auth.allowed_auth_modes = VECTIS_AUTH_MODE_BASIC;
  status = vectis_auth_provider_from_native_store(&native_auth_provider,
                                                  &native_auth, &error);
  assert(status == VECTIS_OK);
  vectis_webdav_auth_provider_config_init(&native_webdav_auth);
  native_webdav_auth.provider = &native_auth_provider;
  native_webdav_auth.purpose = "webdav";
  native_webdav_auth.allowed_auth_modes = VECTIS_AUTH_MODE_BASIC;
  vectis_webdav_mount_config_init(&native_webdav_mount);
  native_webdav_mount.path_prefix = "/dav-native";
  native_webdav_mount.storage = webdav_storage;
  native_webdav_mount.storage.site_id = "runtime-native";
  native_webdav_mount.auth = vectis_webdav_auth_provider;
  native_webdav_mount.auth_userdata = &native_webdav_auth;
  status = app->webdav(app, &native_webdav_mount, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_OK);

  vectis_app_config_init(&second_config);
  second_config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  second_config.tls.bind = "127.0.0.1";
  second_config.tls.port = 28081u;
  second_app = vectis_app_new(&second_config, &second_error);
  assert(second_app != NULL);
  second_route = vectis_route(VECTIS_HTTP_GET, "/health", sample_handler, NULL);
  second_status =
      vectis_register_route(second_app, &second_route, &second_error);
  assert(second_status == VECTIS_OK);
  second_status = vectis_start(second_app, &second_error);
  assert(second_status == VECTIS_OK);
  second_status = vectis_stop(second_app, &second_error);
  assert(second_status == VECTIS_OK);
  vectis_destroy(second_app);

  vectis_http_client_config_init(&http);
  http.timeout_ms = 60000L;
  http.connect_timeout_ms = 200L;
  http.follow_redirects_disabled = 1;
  status = VECTIS_ERR_STATE;
  for (attempt = 0; attempt < 100 && status != VECTIS_OK; ++attempt) {
    vectis_http_response_cleanup(&response);
    status = vectis_http_get(&http, "http://127.0.0.1:28080/health", &response,
                             &error);
    if (status != VECTIS_OK) {
      usleep(100000u);
    }
  }
  assert(status == VECTIS_OK);
  assert(response.status_code == 200L);
  assert(response.body_size == 2u);
  assert(memcmp(response.body, "ok", 2u) == 0);

  status = vectis_http_get(&http, "http://127.0.0.1:28080/state-error",
                           &state_error_response, &error);
  assert(status == VECTIS_OK);
  assert(state_error_response.status_code == 500L);
  vectis_http_response_cleanup(&state_error_response);

  assert_large_header_rejected(28080u);
  assert_keepalive_limit(28080u);

  vectis_http_response_cleanup(&response);
  status =
      vectis_http_get(&http, "http://127.0.0.1:28080/file", &response, &error);
  assert(status == VECTIS_OK);
  assert(response.status_code == 200L);
  assert(response.body_size == sizeof(response_file_body) - 1u);
  assert(memcmp(response.body, response_file_body,
                sizeof(response_file_body) - 1u) == 0);
  vectis_http_response_cleanup(&response);
  assert_repeated_file_responses_do_not_leak_fds(
      &http, "http://127.0.0.1:28080/file", response_file_body,
      sizeof(response_file_body) - 1u, &error);

  status = vectis_http_get(&http, "http://127.0.0.1:28080/embedded",
                           &embedded_response, &error);
  assert(status == VECTIS_OK);
  assert(embedded_response.status_code == 200L);
  assert(embedded_response.content_type != NULL);
  assert(strcmp(embedded_response.content_type, "text/html") == 0);
  assert(strcmp(vectis_http_response_header(&embedded_response, "ETag"),
                "\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6"
                "be03\"") == 0);
  assert(
      strcmp(vectis_http_response_header(&embedded_response, "cache-control"),
             "no-cache") == 0);
  assert(embedded_response.body_size == 6u);
  assert(memcmp(embedded_response.body, "hello\n", 6u) == 0);
  vectis_http_response_cleanup(&embedded_response);

  status =
      vectis_http_get(&http, "http://127.0.0.1:28080/embedded/assets/app.txt",
                      &embedded_response, &error);
  assert(status == VECTIS_OK);
  assert(embedded_response.status_code == 200L);
  assert(embedded_response.content_type != NULL);
  assert(strcmp(embedded_response.content_type, "text/plain") == 0);
  assert(strcmp(vectis_http_response_header(&embedded_response, "etag"),
                "\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d07887"
                "5901\"") == 0);
  assert(
      strcmp(vectis_http_response_header(&embedded_response, "accept-ranges"),
             "bytes") == 0);
  assert(embedded_response.body_size == 4u);
  assert(memcmp(embedded_response.body, "app\n", 4u) == 0);
  vectis_http_response_cleanup(&embedded_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = "http://127.0.0.1:28080/embedded/assets/app.txt";
  request.headers = embedded_if_none_match_headers;
  request.header_count = 1u;
  status = vectis_http_execute(&http, &request, &embedded_not_modified_response,
                               &error);
  assert(status == VECTIS_OK);
  assert(embedded_not_modified_response.status_code == 304L);
  assert(strcmp(vectis_http_response_header(&embedded_not_modified_response,
                                            "etag"),
                "\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d07887"
                "5901\"") == 0);
  assert(strcmp(vectis_http_response_header(&embedded_not_modified_response,
                                            "accept-ranges"),
                "bytes") == 0);
  assert(embedded_not_modified_response.body_size == 0u);
  vectis_http_response_cleanup(&embedded_not_modified_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = "http://127.0.0.1:28080/embedded/assets/app.txt";
  request.headers = embedded_if_none_match_miss_headers;
  request.header_count = 2u;
  status = vectis_http_execute(&http, &request,
                               &embedded_if_none_match_miss_response, &error);
  assert(status == VECTIS_OK);
  assert(embedded_if_none_match_miss_response.status_code == 206L);
  assert(strcmp(vectis_http_response_header(
                    &embedded_if_none_match_miss_response, "content-range"),
                "bytes 1-2/4") == 0);
  assert(embedded_if_none_match_miss_response.body_size == 2u);
  assert(memcmp(embedded_if_none_match_miss_response.body, "pp", 2u) == 0);
  vectis_http_response_cleanup(&embedded_if_none_match_miss_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = "http://127.0.0.1:28080/embedded/assets/app.txt";
  request.headers = embedded_range_headers;
  request.header_count = 1u;
  status =
      vectis_http_execute(&http, &request, &embedded_range_response, &error);
  assert(status == VECTIS_OK);
  assert(embedded_range_response.status_code == 206L);
  assert(embedded_range_response.content_type != NULL);
  assert(strcmp(embedded_range_response.content_type, "text/plain") == 0);
  assert(strcmp(vectis_http_response_header(&embedded_range_response,
                                            "content-range"),
                "bytes 1-2/4") == 0);
  assert(strcmp(vectis_http_response_header(&embedded_range_response,
                                            "accept-ranges"),
                "bytes") == 0);
  assert(embedded_range_response.body_size == 2u);
  assert(memcmp(embedded_range_response.body, "pp", 2u) == 0);
  vectis_http_response_cleanup(&embedded_range_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = "http://127.0.0.1:28080/embedded/assets/app.txt";
  request.headers = embedded_if_range_headers;
  request.header_count = 2u;
  status =
      vectis_http_execute(&http, &request, &embedded_if_range_response, &error);
  assert(status == VECTIS_OK);
  assert(embedded_if_range_response.status_code == 206L);
  assert(strcmp(vectis_http_response_header(&embedded_if_range_response,
                                            "content-range"),
                "bytes 1-2/4") == 0);
  assert(embedded_if_range_response.body_size == 2u);
  assert(memcmp(embedded_if_range_response.body, "pp", 2u) == 0);
  vectis_http_response_cleanup(&embedded_if_range_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = "http://127.0.0.1:28080/embedded/assets/app.txt";
  request.headers = embedded_if_range_miss_headers;
  request.header_count = 2u;
  status = vectis_http_execute(&http, &request,
                               &embedded_if_range_miss_response, &error);
  assert(status == VECTIS_OK);
  assert(embedded_if_range_miss_response.status_code == 200L);
  assert(vectis_http_response_header(&embedded_if_range_miss_response,
                                     "content-range") == NULL);
  assert(embedded_if_range_miss_response.body_size == 4u);
  assert(memcmp(embedded_if_range_miss_response.body, "app\n", 4u) == 0);
  vectis_http_response_cleanup(&embedded_if_range_miss_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = "http://127.0.0.1:28080/embedded/assets/app.txt";
  request.headers = embedded_suffix_range_headers;
  request.header_count = 1u;
  status = vectis_http_execute(&http, &request, &embedded_suffix_range_response,
                               &error);
  assert(status == VECTIS_OK);
  assert(embedded_suffix_range_response.status_code == 206L);
  assert(strcmp(vectis_http_response_header(&embedded_suffix_range_response,
                                            "content-range"),
                "bytes 2-3/4") == 0);
  assert(embedded_suffix_range_response.body_size == 2u);
  assert(memcmp(embedded_suffix_range_response.body, "p\n", 2u) == 0);
  vectis_http_response_cleanup(&embedded_suffix_range_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = "http://127.0.0.1:28080/embedded/assets/app.txt";
  request.headers = embedded_invalid_range_headers;
  request.header_count = 1u;
  status = vectis_http_execute(&http, &request,
                               &embedded_invalid_range_response, &error);
  assert(status == VECTIS_OK);
  assert(embedded_invalid_range_response.status_code == 416L);
  assert(strcmp(vectis_http_response_header(&embedded_invalid_range_response,
                                            "content-range"),
                "bytes */4") == 0);
  assert(strcmp(vectis_http_response_header(&embedded_invalid_range_response,
                                            "accept-ranges"),
                "bytes") == 0);
  assert(embedded_invalid_range_response.body_size == 0u);
  vectis_http_response_cleanup(&embedded_invalid_range_response);

  status =
      vectis_http_head(&http, "http://127.0.0.1:28080/embedded/assets/app.txt",
                       &embedded_head_response, &error);
  assert(status == VECTIS_OK);
  assert(embedded_head_response.status_code == 200L);
  assert(embedded_head_response.content_type != NULL);
  assert(strcmp(embedded_head_response.content_type, "text/plain") == 0);
  assert(strcmp(vectis_http_response_header(&embedded_head_response, "etag"),
                "\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d07887"
                "5901\"") == 0);
  assert(embedded_head_response.body_size == 0u);
  vectis_http_response_cleanup(&embedded_head_response);

  status = vectis_http_get(&http, "http://127.0.0.1:28080/embedded/missing",
                           &embedded_missing_response, &error);
  assert(status == VECTIS_OK);
  assert(embedded_missing_response.status_code == 404L);
  assert(embedded_missing_response.content_type != NULL);
  assert(strcmp(embedded_missing_response.content_type,
                "text/plain; charset=utf-8") == 0);
  assert(strcmp(vectis_http_response_header(&embedded_missing_response,
                                            "cache-control"),
                "no-store") == 0);
  assert(embedded_missing_response.body_size == strlen("not found\n"));
  assert(memcmp(embedded_missing_response.body, "not found\n",
                strlen("not found\n")) == 0);
  vectis_http_response_cleanup(&embedded_missing_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_DELETE;
  request.url = "http://127.0.0.1:28080/embedded/assets/app.txt";
  status =
      vectis_http_execute(&http, &request, &embedded_method_response, &error);
  assert(status == VECTIS_OK);
  assert(embedded_method_response.status_code == 405L);
  assert(strcmp(vectis_http_response_header(&embedded_method_response, "allow"),
                "GET, HEAD") == 0);
  assert(embedded_method_response.body_size == 0u);
  vectis_http_response_cleanup(&embedded_method_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = "http://127.0.0.1:28080/dav-embedded/assets/app.txt";
  request.headers = webdav_headers;
  request.header_count = 1u;
  status = vectis_http_execute(&http, &request, &embedded_webdav_get_response,
                               &error);
  assert(status == VECTIS_OK);
  assert(embedded_webdav_get_response.status_code == 200L);
  assert(embedded_webdav_get_response.body_size == 4u);
  assert(memcmp(embedded_webdav_get_response.body, "app\n", 4u) == 0);
  vectis_http_response_cleanup(&embedded_webdav_get_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_PUT;
  request.url = "http://127.0.0.1:28080/dav-embedded/assets/app.txt";
  request.headers = webdav_headers;
  request.header_count = 1u;
  request.body = "mutated webdav asset\n";
  request.body_size = strlen("mutated webdav asset\n");
  status =
      vectis_http_execute(&http, &request, &embedded_webdav_response, &error);
  assert(status == VECTIS_OK);
  assert(embedded_webdav_response.status_code == 201L);
  vectis_http_response_cleanup(&embedded_webdav_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = "http://127.0.0.1:28080/dav-embedded/assets/app.txt";
  request.headers = webdav_headers;
  request.header_count = 1u;
  status = vectis_http_execute(&http, &request, &embedded_webdav_get_response,
                               &error);
  assert(status == VECTIS_OK);
  assert(embedded_webdav_get_response.status_code == 200L);
  assert(embedded_webdav_get_response.body_size ==
         strlen("mutated webdav asset\n"));
  assert(memcmp(embedded_webdav_get_response.body, "mutated webdav asset\n",
                strlen("mutated webdav asset\n")) == 0);
  vectis_http_response_cleanup(&embedded_webdav_get_response);

  status =
      vectis_http_get(&http, "http://127.0.0.1:28080/embedded/assets/app.txt",
                      &embedded_response, &error);
  assert(status == VECTIS_OK);
  assert(embedded_response.status_code == 200L);
  assert(embedded_response.body_size == 4u);
  assert(memcmp(embedded_response.body, "app\n", 4u) == 0);
  vectis_http_response_cleanup(&embedded_response);

  status = vectis_http_get(&http, "http://127.0.0.1:28080/dav/runtime.txt",
                           &webdav_get_response, &error);
  assert(status == VECTIS_OK);
  assert(webdav_get_response.status_code == 302L);
  vectis_http_response_cleanup(&webdav_get_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = "http://127.0.0.1:28080/dav/runtime.txt";
  request.headers = webdav_required_headers;
  request.header_count = 1u;
  status = vectis_http_execute(&http, &request, &webdav_get_response, &error);
  assert(status == VECTIS_OK);
  assert(webdav_get_response.status_code == 401L);
  assert(webdav_get_response.content_type != NULL);
  assert(strcmp(webdav_get_response.content_type, "text/plain") == 0);
  assert(webdav_get_response.body_size == strlen("runtime auth required"));
  assert(memcmp(webdav_get_response.body, "runtime auth required",
                strlen("runtime auth required")) == 0);
  vectis_http_response_cleanup(&webdav_get_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = "http://127.0.0.1:28080/dav/runtime.txt";
  request.headers = webdav_deny_headers;
  request.header_count = 1u;
  status = vectis_http_execute(&http, &request, &webdav_get_response, &error);
  assert(status == VECTIS_OK);
  assert(webdav_get_response.status_code == 404L);
  vectis_http_response_cleanup(&webdav_get_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_PUT;
  request.url = "http://127.0.0.1:28080/dav/runtime.txt";
  request.headers = webdav_headers;
  request.header_count = 1u;
  request.body = "webdav-body";
  request.body_size = 11u;
  status = vectis_http_execute(&http, &request, &webdav_response, &error);
  assert(status == VECTIS_OK);
  assert(webdav_response.status_code == 201L);
  vectis_http_response_cleanup(&webdav_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = "http://127.0.0.1:28080/dav/runtime.txt";
  request.headers = webdav_headers;
  request.header_count = 1u;
  status = vectis_http_execute(&http, &request, &webdav_get_response, &error);
  assert(status == VECTIS_OK);
  assert(webdav_get_response.status_code == 200L);
  assert(webdav_get_response.body_size == 11u);
  assert(memcmp(webdav_get_response.body, "webdav-body", 11u) == 0);
  vectis_http_response_cleanup(&webdav_get_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_PROPFIND;
  request.url = "http://127.0.0.1:28080/dav";
  request.headers = webdav_headers;
  request.header_count = 1u;
  status =
      vectis_http_execute(&http, &request, &webdav_propfind_response, &error);
  assert(status == VECTIS_OK);
  assert(webdav_propfind_response.status_code == 207L);
  assert(webdav_propfind_response.body_size > 0u);
  assert(bytes_contain(webdav_propfind_response.body,
                       webdav_propfind_response.body_size, "<D:multistatus"));
  assert(bytes_contain(webdav_propfind_response.body,
                       webdav_propfind_response.body_size, "/dav/runtime.txt"));
  vectis_http_response_cleanup(&webdav_propfind_response);

  status = vectis_http_get(&http, "http://127.0.0.1:28080/auth/login",
                           &auth_login_response, &error);
  assert(status == VECTIS_OK);
  assert(auth_login_response.status_code == 200L);
  assert(auth_login_response.content_type != NULL);
  assert(strcmp(auth_login_response.content_type, "text/html; charset=utf-8") ==
         0);
  assert(bytes_contain(auth_login_response.body, auth_login_response.body_size,
                       "action=\"/auth/continue\""));
  assert(!bytes_contain(auth_login_response.body, auth_login_response.body_size,
                        "name=\"email_token\""));
  vectis_http_response_cleanup(&auth_login_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_POST;
  request.url = "http://127.0.0.1:28080/auth/continue";
  request.content_type = "application/x-www-form-urlencoded";
  request.body = "username=runtime-user&password=wrong";
  request.body_size = strlen("username=runtime-user&password=wrong");
  status = vectis_http_execute(&http, &request, &auth_bad_response, &error);
  assert(status == VECTIS_OK);
  assert(auth_bad_response.status_code == 401L);
  assert(bytes_contain(auth_bad_response.body, auth_bad_response.body_size,
                       "login failed"));
  vectis_http_response_cleanup(&auth_bad_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_POST;
  request.url = "http://127.0.0.1:28080/auth/login";
  request.content_type = "application/x-www-form-urlencoded";
  request.body = "username=runtime-user&password=runtime-password";
  request.body_size = strlen("username=runtime-user&password=runtime-password");
  status = vectis_http_execute(&http, &request, &auth_key_response, &error);
  assert(status == VECTIS_OK);
  assert(auth_key_response.status_code == 200L);
  assert(runtime_response_line_value(auth_key_response.body,
                                     auth_key_response.body_size, "client_id",
                                     auth_client_id, sizeof(auth_client_id)));
  assert(runtime_response_line_value(
      auth_key_response.body, auth_key_response.body_size, "client_secret",
      auth_client_secret, sizeof(auth_client_secret)));
  assert(bytes_contain(auth_key_response.body, auth_key_response.body_size,
                       "\"purpose\":\"webdav\""));
  assert(bytes_contain(auth_key_response.body, auth_key_response.body_size,
                       "\"sub\":\"runtime-user\""));
  vectis_http_response_cleanup(&auth_key_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_POST;
  request.url = "http://127.0.0.1:28080/auth/continue";
  request.content_type = "application/x-www-form-urlencoded";
  request.body = "username=runtime-totp&password=runtime-totp-password";
  request.body_size =
      strlen("username=runtime-totp&password=runtime-totp-password");
  status = vectis_http_execute(&http, &request, &auth_bad_response, &error);
  assert(status == VECTIS_OK);
  assert(auth_bad_response.status_code == 202L);
  assert(bytes_contain(auth_bad_response.body, auth_bad_response.body_size,
                       "totp_required=1"));
  assert(runtime_response_line_value(
      auth_bad_response.body, auth_bad_response.body_size,
      "pending_transaction_id", auth_pending_transaction_id,
      sizeof(auth_pending_transaction_id)));
  vectis_http_response_cleanup(&auth_bad_response);

  written = snprintf(auth_totp_form, sizeof(auth_totp_form),
                     "username=runtime-totp&pending_transaction_id=%s&"
                     "totp_code=%s",
                     auth_pending_transaction_id, auth_totp_code);
  assert(written > 0 && (size_t)written < sizeof(auth_totp_form));
  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_POST;
  request.url = "http://127.0.0.1:28080/auth/continue";
  request.content_type = "application/x-www-form-urlencoded";
  request.body = auth_totp_form;
  request.body_size = strlen(auth_totp_form);
  status = vectis_http_execute(&http, &request, &auth_key_response, &error);
  assert(status == VECTIS_OK);
  assert(auth_key_response.status_code == 200L);
  assert(runtime_response_line_value(auth_key_response.body,
                                     auth_key_response.body_size, "client_id",
                                     auth_client_id, sizeof(auth_client_id)));
  assert(runtime_response_line_value(
      auth_key_response.body, auth_key_response.body_size, "client_secret",
      auth_client_secret, sizeof(auth_client_secret)));
  assert(bytes_contain(auth_key_response.body, auth_key_response.body_size,
                       "\"purpose\":\"webdav\""));
  assert(bytes_contain(auth_key_response.body, auth_key_response.body_size,
                       "\"sub\":\"runtime-totp\""));
  vectis_http_response_cleanup(&auth_key_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_POST;
  request.url = "http://127.0.0.1:28080/auth-totp-required/login";
  request.content_type = "application/x-www-form-urlencoded";
  request.body = "username=runtime-user&password=runtime-password&"
                 "totp_code=287082";
  request.body_size = strlen("username=runtime-user&password=runtime-password&"
                             "totp_code=287082");
  status = vectis_http_execute(&http, &request, &auth_bad_response, &error);
  assert(status == VECTIS_OK);
  assert(auth_bad_response.status_code == 401L);
  assert(bytes_contain(auth_bad_response.body, auth_bad_response.body_size,
                       "login failed"));
  vectis_http_response_cleanup(&auth_bad_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_POST;
  request.url = "http://127.0.0.1:28080/auth-totp-required/login";
  request.content_type = "application/x-www-form-urlencoded";
  request.body = "username=runtime-totp&password=runtime-totp-password";
  request.body_size =
      strlen("username=runtime-totp&password=runtime-totp-password");
  status = vectis_http_execute(&http, &request, &auth_bad_response, &error);
  assert(status == VECTIS_OK);
  assert(auth_bad_response.status_code == 202L);
  assert(bytes_contain(auth_bad_response.body, auth_bad_response.body_size,
                       "totp_required=1"));
  assert(runtime_response_line_value(
      auth_bad_response.body, auth_bad_response.body_size,
      "pending_transaction_id", auth_pending_transaction_id,
      sizeof(auth_pending_transaction_id)));
  vectis_http_response_cleanup(&auth_bad_response);

  written = snprintf(auth_totp_form, sizeof(auth_totp_form),
                     "username=runtime-totp&pending_transaction_id=%s&"
                     "totp_code=%s",
                     auth_pending_transaction_id, auth_totp_code);
  assert(written > 0 && (size_t)written < sizeof(auth_totp_form));
  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_POST;
  request.url = "http://127.0.0.1:28080/auth-totp-required/continue";
  request.content_type = "application/x-www-form-urlencoded";
  request.body = auth_totp_form;
  request.body_size = strlen(auth_totp_form);
  status = vectis_http_execute(&http, &request, &auth_key_response, &error);
  assert(status == VECTIS_OK);
  assert(auth_key_response.status_code == 200L);
  assert(runtime_response_line_value(auth_key_response.body,
                                     auth_key_response.body_size, "client_id",
                                     auth_client_id, sizeof(auth_client_id)));
  assert(runtime_response_line_value(
      auth_key_response.body, auth_key_response.body_size, "client_secret",
      auth_client_secret, sizeof(auth_client_secret)));
  assert(bytes_contain(auth_key_response.body, auth_key_response.body_size,
                       "\"purpose\":\"webdav\""));
  assert(bytes_contain(auth_key_response.body, auth_key_response.body_size,
                       "\"sub\":\"runtime-totp\""));
  vectis_http_response_cleanup(&auth_key_response);

  assert(snprintf(auth_clear, sizeof(auth_clear), "%s:%s", auth_client_id,
                  auth_client_secret) > 0);
  assert(runtime_base64_encode(auth_clear, auth_token, sizeof(auth_token)));
  assert(snprintf(auth_header, sizeof(auth_header), "Authorization: Basic %s",
                  auth_token) > 0);
  native_webdav_headers[0] = auth_header;

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_PUT;
  request.url = "http://127.0.0.1:28080/dav-native/from-auth.txt";
  request.headers = native_webdav_headers;
  request.header_count = 1u;
  request.body = "native-webdav-body";
  request.body_size = strlen("native-webdav-body");
  status =
      vectis_http_execute(&http, &request, &native_webdav_response, &error);
  assert(status == VECTIS_OK);
  assert(native_webdav_response.status_code == 201L);
  vectis_http_response_cleanup(&native_webdav_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = "http://127.0.0.1:28080/dav-native/from-auth.txt";
  request.headers = native_webdav_headers;
  request.header_count = 1u;
  status =
      vectis_http_execute(&http, &request, &native_webdav_get_response, &error);
  assert(status == VECTIS_OK);
  assert(native_webdav_get_response.status_code == 200L);
  assert(native_webdav_get_response.body_size == strlen("native-webdav-body"));
  assert(memcmp(native_webdav_get_response.body, "native-webdav-body",
                strlen("native-webdav-body")) == 0);
  vectis_http_response_cleanup(&native_webdav_get_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_POST;
  request.url = "http://127.0.0.1:28080/auth/logout";
  request.headers = native_webdav_headers;
  request.header_count = 1u;
  request.content_type = "application/x-www-form-urlencoded";
  request.body = "";
  request.body_size = 0u;
  status = vectis_http_execute(&http, &request, &auth_logout_response, &error);
  assert(status == VECTIS_OK);
  assert(auth_logout_response.status_code == 200L);
  assert(bytes_contain(auth_logout_response.body,
                       auth_logout_response.body_size, "logged_out=1"));
  vectis_http_response_cleanup(&auth_logout_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = "http://127.0.0.1:28080/dav-native/from-auth.txt";
  request.headers = native_webdav_headers;
  request.header_count = 1u;
  status =
      vectis_http_execute(&http, &request, &native_webdav_get_response, &error);
  assert(status == VECTIS_OK);
  assert(native_webdav_get_response.status_code == 401L);
  vectis_http_response_cleanup(&native_webdav_get_response);

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

  fp = fopen(json_source_path, "wb");
  assert(fp != NULL);
  assert(fwrite("abc", 1u, 3u, fp) == 3u);
  assert(fclose(fp) == 0);
  lonejson_source_init(&json_source_doc.payload);
  assert(lonejson_source_set_path(&json_source_doc.payload, json_source_path,
                                  NULL) == LONEJSON_STATUS_OK);
  vectis_http_request_init(&json_source_request);
  json_source_request.method = VECTIS_HTTP_POST;
  json_source_request.url = "http://127.0.0.1:28080/json-source";
  json_source_request.json_map = &source_json_doc_map;
  json_source_request.json_value = &json_source_doc;
  status = vectis_http_execute(&http, &json_source_request,
                               &json_source_response, &error);
  assert(status == VECTIS_OK);
  assert(json_source_response.status_code == 200L);
  assert(json_source_response.body_size == 11u);
  assert(memcmp(json_source_response.body, "json-source", 11u) == 0);
  vectis_http_response_cleanup(&json_source_response);
  lonejson_source_cleanup(&json_source_doc.payload);
  remove(json_source_path);

  status = vectis_http_head(&http, "http://127.0.0.1:28080/methods",
                            &method_response, &error);
  assert(status == VECTIS_OK);
  assert(method_response.status_code == 200L);
  assert(method_response.body_size == 0u);
  vectis_http_response_cleanup(&method_response);

  status = vectis_http_options(&http, "http://127.0.0.1:28080/methods",
                               &method_response, &error);
  assert(status == VECTIS_OK);
  assert(method_response.status_code == 200L);
  vectis_http_response_cleanup(&method_response);

  status = vectis_http_delete(&http, "http://127.0.0.1:28080/methods",
                              &method_response, &error);
  assert(status == VECTIS_OK);
  assert(method_response.status_code == 404L);
  vectis_http_response_cleanup(&method_response);

  vectis_http_request_init(&no_body_request);
  no_body_request.method = VECTIS_HTTP_POST;
  no_body_request.url = "http://127.0.0.1:28080/no-body";
  no_body_request.body = "not allowed";
  no_body_request.body_size = 11u;
  status =
      vectis_http_execute(&http, &no_body_request, &no_body_response, &error);
  assert(status == VECTIS_OK);
  assert(no_body_response.status_code == 413L);
  vectis_http_response_cleanup(&no_body_response);

  status = vectis_http_get(&http, "http://127.0.0.1:28080/orders/123/items/abc",
                           &param_response, &error);
  assert(status == VECTIS_OK);
  assert(param_response.status_code == 200L);
  assert(param_response.body_size == 3u);
  assert(memcmp(param_response.body, "abc", 3u) == 0);
  vectis_http_response_cleanup(&param_response);

  status = vectis_http_get(&http, "http://127.0.0.1:28080/orders/123/items",
                           &param_response, &error);
  assert(status == VECTIS_OK);
  assert(param_response.status_code == 200L);
  assert(param_response.body_size == 3u);
  assert(memcmp(param_response.body, "123", 3u) == 0);
  vectis_http_response_cleanup(&param_response);

  status = vectis_http_get(&http, "http://127.0.0.1:28080/orders/../items",
                           &param_response, &error);
  assert(status == VECTIS_OK);
  assert(param_response.status_code == 400L ||
         param_response.status_code == 404L);
  vectis_http_response_cleanup(&param_response);

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
  status = vectis_http_upload_file(
      &http, VECTIS_HTTP_POST, "http://127.0.0.1:28080/upload", upload_path,
      "application/octet-stream", &upload_response, &error);
  assert(status == VECTIS_OK);
  assert(upload_response.status_code == 200L);
  assert(upload_response.body_size == 8u);
  assert(memcmp(upload_response.body, "buffered", 8u) == 0);
  vectis_http_response_cleanup(&upload_response);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_POST;
  request.url = "http://127.0.0.1:28080/upload-spooled";
  request.body = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
  request.body_size = 32u;
  status =
      vectis_http_execute(&http, &request, &spooled_upload_response, &error);
  assert(status == VECTIS_OK);
  assert(spooled_upload_response.status_code == 200L);
  assert(spooled_upload_response.body_size == 7u);
  assert(memcmp(spooled_upload_response.body, "spooled", 7u) == 0);
  vectis_http_response_cleanup(&spooled_upload_response);

  default_spooled_body_size =
      VECTIS_BODY_DEFAULT_MEMORY_BUFFER_LIMIT_BYTES + 1u;
  default_spooled_body = (char *)malloc(default_spooled_body_size);
  assert(default_spooled_body != NULL);
  memset(default_spooled_body, 'x', default_spooled_body_size);
  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_POST;
  request.url = "http://127.0.0.1:28080/upload-default-spooled";
  request.body = default_spooled_body;
  request.body_size = default_spooled_body_size;
  status = vectis_http_execute(&http, &request,
                               &default_spooled_upload_response, &error);
  free(default_spooled_body);
  assert(status == VECTIS_OK);
  assert(default_spooled_upload_response.status_code == 200L);
  assert(default_spooled_upload_response.body_size == 15u);
  assert(memcmp(default_spooled_upload_response.body, "default-spooled", 15u) ==
         0);
  vectis_http_response_cleanup(&default_spooled_upload_response);

  stream_body_size = VECTIS_TEST_ASAN ? 8192u + 3u : 1048576u + 3u;
  stream_body = (char *)malloc(stream_body_size);
  assert(stream_body != NULL);
  for (i = 0; i < (int)stream_body_size; ++i) {
    stream_body[i] = (char)('a' + (i % 26));
  }
  fp = fopen(stream_upload_path, "wb");
  assert(fp != NULL);
  assert(fwrite(stream_body, 1u, stream_body_size, fp) == stream_body_size);
  assert(fclose(fp) == 0);
  (void)snprintf(size_text, sizeof(size_text), "%lu",
                 (unsigned long)stream_body_size);
  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_POST;
  request.url = "http://127.0.0.1:28080/stream-upload";
  request.body = stream_body;
  request.body_size = stream_body_size;
  status = vectis_http_execute(&http, &request, &stream_response, &error);
  assert(status == VECTIS_OK);
  assert(stream_response.status_code == 200L);
  assert(stream_response.body_size == strlen(size_text));
  assert(memcmp(stream_response.body, size_text, strlen(size_text)) == 0);
  vectis_http_response_cleanup(&stream_response);

  /*
   * ASAN-instrumented embedded Kore workers do not reliably complete
   * upload-reader-backed routes. The normal runtime test keeps the large-body
   * non-materialization coverage for upload reader, XML, and DSV routes.
   */
  if (!VECTIS_TEST_ASAN) {
    vectis_http_request_init(&request);
    request.method = VECTIS_HTTP_POST;
    request.url = "http://127.0.0.1:28080/stream-reader";
    request.body = stream_body;
    request.body_size = stream_body_size;
    status =
        vectis_http_execute(&http, &request, &stream_reader_response, &error);
    assert(status == VECTIS_OK);
    assert(stream_reader_response.status_code == 200L);
    assert(stream_reader_response.body_size == strlen(size_text));
    assert(memcmp(stream_reader_response.body, size_text, strlen(size_text)) ==
           0);
    vectis_http_response_cleanup(&stream_reader_response);

    status = vectis_http_upload_file(
        &http, VECTIS_HTTP_POST, "http://127.0.0.1:28080/stream-reader",
        stream_upload_path, "application/octet-stream", &stream_reader_response,
        &error);
    assert(status == VECTIS_OK);
    assert(stream_reader_response.status_code == 200L);
    assert(stream_reader_response.body_size == strlen(size_text));
    assert(memcmp(stream_reader_response.body, size_text, strlen(size_text)) ==
           0);
    vectis_http_response_cleanup(&stream_reader_response);

    xml_body_size = 1024u;
    fp = fopen(xml_upload_path, "wb");
    assert(fp != NULL);
    assert(fwrite("<doc><body>", 1u, 11u, fp) == 11u);
    for (i = 0; i < (int)xml_body_size; ++i) {
      assert(fputc('x', fp) == 'x');
    }
    assert(fwrite("</body></doc>", 1u, 13u, fp) == 13u);
    assert(fclose(fp) == 0);
    (void)snprintf(size_text, sizeof(size_text), "%lu",
                   (unsigned long)xml_body_size);
    status = vectis_http_upload_file(
        &http, VECTIS_HTTP_POST, "http://127.0.0.1:28080/xml-upload",
        xml_upload_path, "application/xml", &xml_route_response, &error);
    assert(status == VECTIS_OK);
    assert(xml_route_response.status_code == 200L);
    assert(xml_route_response.body_size == strlen(size_text));
    assert(memcmp(xml_route_response.body, size_text, strlen(size_text)) == 0);
    vectis_http_response_cleanup(&xml_route_response);

    dsv_rows = 3000u;
    dsv_total = 0;
    dsv_active = 0u;
    fp = fopen(dsv_upload_path, "wb");
    assert(fp != NULL);
    assert(fputs("id,count,active,pad\n", fp) >= 0);
    for (i = 0; i < (int)dsv_rows; ++i) {
      int active;
      int j;

      active = (i % 2) == 0;
      dsv_total += 1;
      if (active) {
        dsv_active++;
      }
      assert(fprintf(fp, "row-%05d,1,%s,", i, active ? "true" : "false") > 0);
      for (j = 0; j < 400; ++j) {
        assert(fputc('x', fp) == 'x');
      }
      assert(fputc('\n', fp) == '\n');
    }
    assert(fclose(fp) == 0);
    (void)snprintf(dsv_text, sizeof(dsv_text), "%lu:%lld:%lu",
                   (unsigned long)dsv_rows, dsv_total,
                   (unsigned long)dsv_active);
    status = vectis_http_upload_file(
        &http, VECTIS_HTTP_POST, "http://127.0.0.1:28080/dsv-upload",
        dsv_upload_path, "text/csv", &dsv_route_response, &error);
    assert(status == VECTIS_OK);
    assert(dsv_route_response.status_code == 200L);
    assert(dsv_route_response.body_size == strlen(dsv_text));
    assert(memcmp(dsv_route_response.body, dsv_text, strlen(dsv_text)) == 0);
    vectis_http_response_cleanup(&dsv_route_response);
  }

  (void)snprintf(size_text, sizeof(size_text), "%lu",
                 (unsigned long)stream_body_size);
  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_POST;
  request.url = "http://127.0.0.1:28080/stream-file";
  request.body = stream_body;
  request.body_size = stream_body_size;
  status = vectis_http_execute(&http, &request, &stream_file_response, &error);
  assert(status == VECTIS_OK);
  assert(stream_file_response.status_code == 200L);
  assert(stream_file_response.body_size == strlen(size_text));
  assert(memcmp(stream_file_response.body, size_text, strlen(size_text)) == 0);
  fp = fopen(stream_file_path, "rb");
  assert(fp != NULL);
  assert(fseek(fp, 0L, SEEK_END) == 0);
  stream_file_size = ftell(fp);
  assert(stream_file_size == (long)stream_body_size);
  assert(fclose(fp) == 0);
  vectis_http_response_cleanup(&stream_file_response);
  free(stream_body);

  remove(upload_path);
  remove(stream_upload_path);
  remove(stream_file_path);
  remove(xml_upload_path);
  remove(dsv_upload_path);
  remove(response_file_path);
  remove_tree(webdav_cache_dir);
  (void)remove(auth_store_path);

  status = vectis_stop(app, &error);
  assert(status == VECTIS_OK);
  app->close(app);
  vectis_embedded_fs_close(embedded_fs);
}

static void assert_consumer_service_declaration_before_routes(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  lc_consumer_config consumer;
  lc_consumer_service_config service_config;
  vectis_consumer_service *service;
  vectis_route_config route;

  vectis_app_config_init(&config);
  config.lockd.unix_socket_path = "/tmp/vectis-runtime-missing-lockd.sock";
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  lc_consumer_config_init(&consumer);
  lc_consumer_service_config_init(&service_config);
  consumer.name = "runtime-declared";
  consumer.request.queue = "runtime";
  consumer.request.owner = "runtime-declared";
  consumer.handle = sample_consumer_handler;
  service_config.consumers = &consumer;
  service_config.consumer_count = 1u;
  service = NULL;
  status = app->consumer_service(app, &service_config, &service, &error);
  assert(status == VECTIS_OK);
  assert(service != NULL);
  assert(service->native(service) == NULL);

  status = service->start(service, &error);
  assert(status == VECTIS_OK);
  assert(service->native(service) == NULL);

  route = vectis_route(VECTIS_HTTP_GET, "/declared", sample_handler, NULL);
  status = app->route(app, &route, &error);
  assert(status == VECTIS_OK);

  service->close(service);
  app->close(app);
}

static void assert_kore_start_rejects_extra_thread(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  vectis_route_config route;
  runtime_thread_probe probe;
  pthread_t thread;
  struct timespec delay;

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = 0u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/thread-guard", sample_handler, NULL);
  status = app->route(app, &route, &error);
  assert(status == VECTIS_OK);

  probe.done = 0;
  assert(pthread_create(&thread, NULL, runtime_probe_thread, &probe) == 0);
  delay.tv_sec = 0;
  delay.tv_nsec = 50000000L;
  (void)nanosleep(&delay, NULL);
  status = app->start(app, &error);
  probe.done = 1;
  (void)pthread_join(thread, NULL);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "single-threaded") != NULL);
  app->close(app);
}

static void assert_supervised_start_reports_child_readiness_failure(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  vectis_route_config route;
  unsigned short port;
  int reserved_fd;

  reserved_fd = reserve_loopback_port(&port);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = port;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/child-exit", sample_handler, NULL);
  status = app->route(app, &route, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "before readiness") != NULL);
  status = app->stop(app, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "app is not started") != NULL);
  app->close(app);
  close(reserved_fd);
}

static void assert_supervised_wait_reports_consumer_service_exit(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  vectis_route_config route;
  lc_consumer_config consumer;
  lc_consumer_service_config service_config;
  vectis_consumer_service *service;
  char pouch_dir[] = "/tmp/vectis-runtime-consumer.XXXXXX";
  char endpoint[4096];
  const char *endpoints[1];
  int handled_count;
  int reserved_fd;
  int written;
  unsigned short port;

  assert(mkdtemp(pouch_dir) != NULL);
  written = snprintf(endpoint, sizeof(endpoint),
                     "pouch://%s?single_writer=false", pouch_dir);
  assert(written > 0 && (size_t)written < sizeof(endpoint));
  endpoints[0] = endpoint;

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  reserved_fd = reserve_loopback_port(&port);
  close(reserved_fd);
  config.tls.port = port;
  config.lockd.endpoints = endpoints;
  config.lockd.endpoint_count = 1u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  handled_count = 0;
  lc_consumer_config_init(&consumer);
  lc_consumer_service_config_init(&service_config);
  consumer.name = "runtime-failing";
  consumer.request.queue = "runtime-failing";
  consumer.request.owner = "runtime-failing-owner";
  consumer.request.wait_seconds = 1L;
  consumer.request.visibility_timeout_seconds = 1L;
  consumer.handle = failing_consumer_handler;
  consumer.on_error = failing_consumer_on_error;
  consumer.context = &handled_count;
  service_config.consumers = &consumer;
  service_config.consumer_count = 1u;
  service = NULL;
  status = app->consumer_service(app, &service_config, &service, &error);
  assert(status == VECTIS_OK);
  assert(service != NULL);

  route = vectis_route(VECTIS_HTTP_GET, "/consumer-service-exit",
                       sample_handler, NULL);
  status = app->route(app, &route, &error);
  assert(status == VECTIS_OK);
  status = service->start(service, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_OK);

  enqueue_lockd_test_message(endpoint, "runtime-failing");

  (void)alarm(10u);
  status = app->wait(app, &error);
  (void)alarm(0u);
  assert(status == VECTIS_ERR_STATE);
  assert(error.source == VECTIS_ERROR_SOURCE_LOCKDC);
  assert(error.dependency_code != (long)LC_OK);
  assert(strstr(error.message, "lockd consumer service wait failed") != NULL);
  assert(handled_count == 1);

  service->close(service);
  app->close(app);
  remove_tree(pouch_dir);
}

static void assert_service_only_wait_reports_consumer_service_exit(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  lc_consumer_config consumer;
  lc_consumer_service_config service_config;
  vectis_consumer_service *service;
  char pouch_dir[] = "/tmp/vectis-runtime-service-only.XXXXXX";
  char endpoint[4096];
  const char *endpoints[1];
  int handled_count;
  int written;

  assert(mkdtemp(pouch_dir) != NULL);
  written = snprintf(endpoint, sizeof(endpoint),
                     "pouch://%s?single_writer=false", pouch_dir);
  assert(written > 0 && (size_t)written < sizeof(endpoint));
  endpoints[0] = endpoint;

  vectis_app_config_init(&config);
  config.lockd.endpoints = endpoints;
  config.lockd.endpoint_count = 1u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  handled_count = 0;
  lc_consumer_config_init(&consumer);
  lc_consumer_service_config_init(&service_config);
  consumer.name = "service-only-failing";
  consumer.request.queue = "service-only-failing";
  consumer.request.owner = "service-only-failing-owner";
  consumer.request.wait_seconds = 1L;
  consumer.request.visibility_timeout_seconds = 1L;
  consumer.handle = failing_consumer_handler;
  consumer.on_error = failing_consumer_on_error;
  consumer.context = &handled_count;
  service_config.consumers = &consumer;
  service_config.consumer_count = 1u;
  service = NULL;
  status = app->consumer_service(app, &service_config, &service, &error);
  assert(status == VECTIS_OK);
  assert(service != NULL);
  status = service->start(service, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_OK);

  enqueue_lockd_test_message(endpoint, "service-only-failing");

  (void)alarm(10u);
  status = app->wait(app, &error);
  (void)alarm(0u);
  assert(status == VECTIS_ERR_STATE);
  assert(error.source == VECTIS_ERROR_SOURCE_LOCKDC);
  assert(error.dependency_code != (long)LC_OK);
  assert(strstr(error.message, "lockd consumer service wait failed") != NULL);
  assert(handled_count == 1);

  service->close(service);
  app->close(app);
  remove_tree(pouch_dir);
}

#ifdef VECTIS_RUNTIME_HEADER_LIMIT_ONLY
int main(void) {
  assert_default_header_limit_accepts_64k();
  return 0;
}
#else
int main(void) {
  vectis_app_config config;
  vectis_error error;
  vectis_app *app;
  vectis_status status;
  vectis_route_config bad_route;
  vectis_route_config route;
  vectis_body_policy policy;

  assert_server_config_validation();
  assert_route_body_policy_validation();
  assert_metrics_surface();
  assert_consumer_service_declaration_before_routes();
  assert_kore_start_rejects_extra_thread();
  assert_supervised_start_reports_child_readiness_failure();
  assert_supervised_wait_reports_consumer_service_exit();
  assert_service_only_wait_reports_consumer_service_exit();

  vectis_app_config_init(&config);
  config.tls.mode = (vectis_tls_mode)99;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route =
      vectis_route(VECTIS_HTTP_GET, "/invalid-tls-mode", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "tls.mode") != NULL);
  app->close(app);

  vectis_app_config_init(&config);

  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  assert(vectis_internal_max_request_body_bytes(app) ==
         VECTIS_SERVER_DEFAULT_MAX_REQUEST_BODY_BYTES);
  assert(vectis_internal_lockd_client(app) == NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/requires-tls", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);

  status = app->start(app, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "manual TLS requires") != NULL);
  app->close(app);

  vectis_app_config_init(&config);
  config.lockd.unix_socket_path = "/tmp/vectis-missing-lockd.sock";
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  status = app->start(app, &error);
  assert(status == VECTIS_OK || status == VECTIS_ERR_STATE);
  if (status == VECTIS_ERR_STATE) {
    assert(error.source == VECTIS_ERROR_SOURCE_LOCKDC);
    assert(strstr(error.message, "manual TLS requires") == NULL);
  } else {
    status = vectis_stop(app, &error);
    assert(status == VECTIS_OK);
  }
  app->close(app);

  vectis_app_config_init(&config);
  config.tls.cert_key_bundle = vectis_source_from_path("/tmp/server.pem");
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  assert(vectis_internal_lockd_client(app) == NULL);

  status = app->start(app, &error);
  assert(status == VECTIS_OK);
  status = vectis_stop(app, &error);
  assert(status == VECTIS_OK);

  app->close(app);
  config.server.request_header_timeout_ms = -1L;
  app = vectis_app_new(&config, &error);
  assert(app == NULL);
  assert(strstr(error.message, "request_header_timeout_ms") != NULL);
  vectis_app_config_init(&config);
  config.tls.cert_key_bundle = vectis_source_from_path("/tmp/server.pem");
  config.lockd.unix_socket_path = "/tmp/lockd.sock";
  config.server.keepalive_timeout_ms = -1L;
  app = vectis_app_new(&config, &error);
  assert(app == NULL);
  assert(strstr(error.message, "keepalive_timeout_ms") != NULL);
  vectis_app_config_init(&config);
  config.tls.cert_key_bundle = vectis_source_from_path("/tmp/server.pem");
  config.lockd.unix_socket_path = "/tmp/lockd.sock";
  config.server.keepalive_disabled = 1;
  config.server.keepalive_timeout_ms = 0L;
  config.server.keepalive_max_requests = 0u;
  app = vectis_app_new(&config, &error);
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

  route = vectis_route_methods(VECTIS_HTTP_METHODS_NONE, "/bad-method",
                               sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "HTTP method") != NULL);

  route = vectis_route_methods(VECTIS_HTTP_METHODS_OPTIONS |
                                   VECTIS_HTTP_METHODS_HEAD,
                               "/multi", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_OPTIONS, "/multi", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_CONFLICT);

  route = vectis_upload_route(VECTIS_HTTP_POST, "/upload-default",
                              sample_handler, NULL);
  assert(route.body.mode == VECTIS_BODY_STREAMING_UPLOAD);
  assert(route.body.max_bytes == VECTIS_BODY_DEFAULT_UPLOAD_MAX_BYTES);
  assert(route.body.disk_spool_disabled == 0);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_max_request_body_bytes(app) ==
         VECTIS_BODY_DEFAULT_UPLOAD_MAX_BYTES);
  status = vectis_internal_route_body_policy(
      app, VECTIS_HTTP_POST, "/upload-default", &policy, &error);
  assert(status == VECTIS_OK);
  assert(policy.max_bytes == VECTIS_BODY_DEFAULT_UPLOAD_MAX_BYTES);
  assert(policy.memory_buffer_limit_bytes ==
         VECTIS_BODY_DEFAULT_MEMORY_BUFFER_LIMIT_BYTES);

  route = vectis_upload_route_max(VECTIS_HTTP_POST, "/upload", 4096u,
                                  sample_handler, NULL);
  assert(route.body.mode == VECTIS_BODY_STREAMING_UPLOAD);
  assert(route.body.max_bytes == 4096u);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);

  route = vectis_upload_route_max(VECTIS_HTTP_POST, "/bad-upload",
                                  (size_t)1024u, sample_handler, NULL);
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

  app->close(app);
  assert_kore_smoke();
  return 0;
}
#endif
