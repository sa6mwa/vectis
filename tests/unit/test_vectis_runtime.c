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

typedef struct runtime_managed_service_probe {
  int started;
  int stopped;
  int waited;
  int cleaned;
  int wait_fds[2];
  vectis_status wait_status;
  const char *wait_error;
} runtime_managed_service_probe;

typedef struct runtime_log_buffer {
  char data[8192];
  size_t size;
} runtime_log_buffer;

typedef struct runtime_http_mock_server {
  int listen_fd;
  unsigned short port;
  pthread_t thread;
  const char *body;
  char request[1024];
  size_t request_size;
} runtime_http_mock_server;

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

static vectis_status runtime_managed_service_start(void *context,
                                                   vectis_error *error) {
  runtime_managed_service_probe *probe;

  (void)error;
  probe = (runtime_managed_service_probe *)context;
  if (probe != NULL) {
    probe->started += 1;
  }
  return VECTIS_OK;
}

static vectis_status runtime_managed_service_stop(void *context,
                                                  vectis_error *error) {
  runtime_managed_service_probe *probe;
  const char byte = 'x';

  (void)error;
  probe = (runtime_managed_service_probe *)context;
  if (probe != NULL) {
    probe->stopped += 1;
    if (probe->wait_fds[1] >= 0) {
      assert(write(probe->wait_fds[1], &byte, 1u) == 1);
    }
  }
  return VECTIS_OK;
}

static vectis_status runtime_managed_service_wait(void *context,
                                                  vectis_error *error) {
  runtime_managed_service_probe *probe;
  char byte;

  probe = (runtime_managed_service_probe *)context;
  if (probe == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "managed service probe is required");
    return VECTIS_ERR_INVALID;
  }
  probe->waited += 1;
  if (probe->wait_status != VECTIS_OK) {
    vectis_set_error(error, probe->wait_status,
                     probe->wait_error != NULL ? probe->wait_error
                                               : "managed service failed");
    return probe->wait_status;
  }
  assert(read(probe->wait_fds[0], &byte, 1u) == 1);
  return VECTIS_OK;
}

static void runtime_managed_service_cleanup(void *context) {
  runtime_managed_service_probe *probe;

  probe = (runtime_managed_service_probe *)context;
  if (probe != NULL) {
    probe->cleaned += 1;
  }
}

static int runtime_log_write(void *userdata, const char *data, size_t len,
                             size_t *written) {
  runtime_log_buffer *buffer;
  size_t available;
  size_t copy_size;

  buffer = (runtime_log_buffer *)userdata;
  assert(buffer != NULL);
  available = buffer->size < sizeof(buffer->data) - 1u
                  ? sizeof(buffer->data) - 1u - buffer->size
                  : 0u;
  copy_size = len < available ? len : available;
  if (copy_size > 0u) {
    memcpy(buffer->data + buffer->size, data, copy_size);
    buffer->size += copy_size;
    buffer->data[buffer->size] = '\0';
  }
  if (written != NULL) {
    *written = len;
  }
  return 0;
}

static pslog_logger *runtime_test_logger(runtime_log_buffer *buffer) {
  pslog_config config;

  memset(buffer, 0, sizeof(*buffer));
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.output.write = runtime_log_write;
  config.output.close = NULL;
  config.output.isatty = NULL;
  config.output.userdata = buffer;
  config.output.owned = 0;
  return pslog_new(&config);
}

static void *runtime_http_mock_main(void *userdata) {
  runtime_http_mock_server *server;
  struct sockaddr_in peer;
  socklen_t peer_len;
  int client_fd;
  ssize_t nread;
  char response[512];
  int written;
  size_t body_size;

  server = (runtime_http_mock_server *)userdata;
  if (server == NULL) {
    return NULL;
  }
  peer_len = sizeof(peer);
  client_fd = accept(server->listen_fd, (struct sockaddr *)&peer, &peer_len);
  if (client_fd < 0) {
    return NULL;
  }
  nread = read(client_fd, server->request, sizeof(server->request) - 1u);
  if (nread > 0) {
    server->request_size = (size_t)nread;
    server->request[server->request_size] = '\0';
  }
  body_size = strlen(server->body != NULL ? server->body : "");
  written = snprintf(response, sizeof(response),
                     "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                     "Content-Length: %lu\r\nConnection: close\r\n\r\n%s",
                     (unsigned long)body_size,
                     server->body != NULL ? server->body : "");
  assert(written > 0 && (size_t)written < sizeof(response));
  assert(write(client_fd, response, (size_t)written) == written);
  (void)close(client_fd);
  return NULL;
}

static void runtime_http_mock_start(runtime_http_mock_server *server,
                                    const char *body) {
  struct sockaddr_in addr;
  socklen_t addr_len;
  int enabled;

  memset(server, 0, sizeof(*server));
  server->listen_fd = -1;
  server->body = body;
  server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(server->listen_fd >= 0);
  enabled = 1;
  (void)setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                   sizeof(enabled));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(0);
  assert(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
  assert(bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  assert(listen(server->listen_fd, 1) == 0);
  addr_len = sizeof(addr);
  assert(getsockname(server->listen_fd, (struct sockaddr *)&addr, &addr_len) ==
         0);
  server->port = ntohs(addr.sin_port);
  assert(pthread_create(&server->thread, NULL, runtime_http_mock_main,
                        server) == 0);
}

static void runtime_http_mock_stop(runtime_http_mock_server *server) {
  if (server == NULL) {
    return;
  }
  if (server->listen_fd >= 0) {
    (void)close(server->listen_fd);
    server->listen_fd = -1;
  }
  (void)pthread_join(server->thread, NULL);
}

typedef struct runtime_enqueue_after_delay {
  const char *endpoint;
  const char *queue;
  long delay_ms;
  vectis_consumer_service *service;
  int signal_after_failure;
} runtime_enqueue_after_delay;

static void enqueue_lockd_test_message(const char *endpoint, const char *queue);

static void *runtime_enqueue_after_delay_main(void *userdata) {
  runtime_enqueue_after_delay *task;
  struct timespec delay;
  vectis_consumer_service_state state;
  vectis_error error;
  int i;

  task = (runtime_enqueue_after_delay *)userdata;
  if (task == NULL) {
    return NULL;
  }
  delay.tv_sec = task->delay_ms / 1000L;
  delay.tv_nsec = (task->delay_ms % 1000L) * 1000000L;
  (void)nanosleep(&delay, NULL);
  enqueue_lockd_test_message(task->endpoint, task->queue);
  if (task->signal_after_failure && task->service != NULL) {
    delay.tv_sec = 0;
    delay.tv_nsec = 50000000L;
    for (i = 0; i < 100; ++i) {
      vectis_error_clear(&error);
      vectis_consumer_service_state_init(&state);
      if (task->service->state(task->service, &state, &error) == VECTIS_OK &&
          state.failed) {
        (void)kill(getpid(), SIGTERM);
        return NULL;
      }
      (void)nanosleep(&delay, NULL);
    }
    (void)kill(getpid(), SIGTERM);
  }
  return NULL;
}

static void assert_runtime_control_frame_contract(void) {
  vectis_error error;
  vectis_status status;
  vectis_runtime_control_type type;
  vectis_mutable_bytes payload;
  int fds[2];
  const char message[] = "metrics-delta";
  unsigned char bad_header[8];
  unsigned char partial_header[4];

  assert(pipe(fds) == 0);
  status = vectis_internal_runtime_control_write(
      fds[1], VECTIS_RUNTIME_CONTROL_METRICS, message, sizeof(message) - 1u,
      &error);
  assert(status == VECTIS_OK);
  memset(&payload, 0, sizeof(payload));
  status =
      vectis_internal_runtime_control_read(fds[0], &type, &payload, &error);
  assert(status == VECTIS_OK);
  assert(type == VECTIS_RUNTIME_CONTROL_METRICS);
  assert(payload.size == sizeof(message) - 1u);
  assert(memcmp(payload.data, message, payload.size) == 0);
  vectis_mutable_bytes_cleanup(&payload);
  close(fds[0]);
  close(fds[1]);

  assert(pipe(fds) == 0);
  status = vectis_internal_runtime_control_write(
      fds[1], VECTIS_RUNTIME_CONTROL_STOP, NULL, 0u, &error);
  assert(status == VECTIS_OK);
  memset(&payload, 0, sizeof(payload));
  status =
      vectis_internal_runtime_control_read(fds[0], &type, &payload, &error);
  assert(status == VECTIS_OK);
  assert(type == VECTIS_RUNTIME_CONTROL_STOP);
  assert(payload.size == 0u);
  vectis_mutable_bytes_cleanup(&payload);
  close(fds[0]);
  close(fds[1]);

  assert(pipe(fds) == 0);
  status = vectis_internal_runtime_control_write(
      fds[1], VECTIS_RUNTIME_CONTROL_SERVICE_FAILURE, message,
      sizeof(message) - 1u, &error);
  assert(status == VECTIS_OK);
  memset(&payload, 0, sizeof(payload));
  status =
      vectis_internal_runtime_control_read(fds[0], &type, &payload, &error);
  assert(status == VECTIS_OK);
  assert(type == VECTIS_RUNTIME_CONTROL_SERVICE_FAILURE);
  assert(payload.size == sizeof(message) - 1u);
  assert(memcmp(payload.data, message, payload.size) == 0);
  vectis_mutable_bytes_cleanup(&payload);
  close(fds[0]);
  close(fds[1]);

  assert(pipe(fds) == 0);
  status = vectis_internal_runtime_control_write(
      fds[1], (vectis_runtime_control_type)99, NULL, 0u, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "type") != NULL);
  close(fds[0]);
  close(fds[1]);

  assert(pipe(fds) == 0);
  memset(bad_header, 0, sizeof(bad_header));
  bad_header[0] = 'B';
  bad_header[1] = 'A';
  bad_header[2] = 'D';
  bad_header[3] = '1';
  bad_header[4] = (unsigned char)VECTIS_RUNTIME_CONTROL_READY;
  assert(write(fds[1], bad_header, sizeof(bad_header)) ==
         (ssize_t)sizeof(bad_header));
  memset(&payload, 0, sizeof(payload));
  status =
      vectis_internal_runtime_control_read(fds[0], &type, &payload, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "header") != NULL);
  close(fds[0]);
  close(fds[1]);

  assert(pipe(fds) == 0);
  partial_header[0] = 'V';
  partial_header[1] = 'R';
  partial_header[2] = 'C';
  partial_header[3] = '1';
  assert(write(fds[1], partial_header, sizeof(partial_header)) ==
         (ssize_t)sizeof(partial_header));
  close(fds[1]);
  memset(&payload, 0, sizeof(payload));
  status =
      vectis_internal_runtime_control_read(fds[0], &type, &payload, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "complete frame") != NULL);
  close(fds[0]);
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

static int metrics_pouch_snapshot_contains_text(const char *endpoint,
                                                const char *namespace_name,
                                                const char *needle);
static void remove_tree(const char *path);

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

static void assert_supervised_metrics_persistence_worker(void) {
  vectis_app_config config;
  vectis_metrics_config metrics;
  vectis_app *app;
  vectis_http_client_config http;
  vectis_http_response response;
  vectis_mutable_bytes supervisor_snapshot;
  vectis_error error;
  vectis_status status;
  char pouch_dir[] = "/tmp/vectis-runtime-metrics.XXXXXX";
  char endpoint[4096];
  char metrics_url[256];
  struct timespec pause_time;
  unsigned short port;
  int reserved_fd;
  int written;
  int found;
  int wrote;
  int i;

  assert(mkdtemp(pouch_dir) != NULL);
  written = snprintf(endpoint, sizeof(endpoint),
                     "pouch://%s?single_writer=false", pouch_dir);
  assert(written > 0 && (size_t)written < sizeof(endpoint));
  reserved_fd = reserve_loopback_port(&port);
  close(reserved_fd);

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = port;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  memset(&response, 0, sizeof(response));
  memset(&supervisor_snapshot, 0, sizeof(supervisor_snapshot));

  vectis_metrics_config_init(&metrics);
  metrics.path = "/.metrics";
  metrics.json_path = "/.metrics.json";
  metrics.title = "supervised metrics worker";
  metrics.persistence_enabled = 1;
  metrics.storage_endpoint = endpoint;
  metrics.storage_namespace = "vectis.runtime.metrics";
  metrics.storage_owner = "runtime-test";
  metrics.snapshot_interval_seconds = 300u;
  status = app->metrics(app, &metrics, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_OK);

  vectis_http_client_config_init(&http);
  http.timeout_ms = 2000L;
  http.connect_timeout_ms = 1000L;
  written = snprintf(metrics_url, sizeof(metrics_url),
                     "http://127.0.0.1:%u/.metrics.json", (unsigned)port);
  assert(written > 0 && (size_t)written < sizeof(metrics_url));
  status = vectis_http_get(&http, metrics_url, &response, &error);
  assert(status == VECTIS_OK);
  assert(response.status_code == 200L);
  vectis_http_response_cleanup(&response);
  status = vectis_metrics_snapshot_json(app, &supervisor_snapshot, &error);
  assert(status == VECTIS_OK);
  assert(strstr((const char *)supervisor_snapshot.data,
                "\"requests_total\":1") != NULL);
  assert(strstr((const char *)supervisor_snapshot.data, "\"2xx\":1") != NULL);
  vectis_mutable_bytes_cleanup(&supervisor_snapshot);

  pause_time.tv_sec = 0;
  pause_time.tv_nsec = 100000000L;
  wrote = 0;
  for (i = 0; i < 50 && !wrote; ++i) {
    vectis_mutable_bytes snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    status = vectis_metrics_snapshot_json(app, &snapshot, &error);
    assert(status == VECTIS_OK);
    wrote = snapshot.data != NULL &&
            strstr((const char *)snapshot.data, "\"writes\":1") != NULL;
    vectis_mutable_bytes_cleanup(&snapshot);
    if (!wrote) {
      (void)nanosleep(&pause_time, NULL);
    }
  }
  assert(wrote);
  found = metrics_pouch_snapshot_contains_text(
      endpoint, "vectis.runtime.metrics",
      "\"service\":\"supervised metrics worker\"");
  assert(found);

  status = app->stop(app, &error);
  assert(status == VECTIS_OK);
  app->close(app);
  remove_tree(pouch_dir);
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

static int metrics_pouch_snapshot_contains_text(const char *endpoint,
                                                const char *namespace_name,
                                                const char *needle) {
  const char *endpoints[1];
  lc_client_config client_config;
  lc_client *client;
  lc_error lcerr;
  time_t now;
  long offset;
  char key[64];
  int found;
  int rc;

  if (endpoint == NULL || namespace_name == NULL || needle == NULL) {
    return 0;
  }
  endpoints[0] = endpoint;
  lc_error_init(&lcerr);
  lc_client_config_init(&client_config);
  client_config.endpoints = endpoints;
  client_config.endpoint_count = 1u;
  client_config.default_namespace = namespace_name;
  client = NULL;
  rc = lc_client_open(&client_config, &client, &lcerr);
  if (rc != LC_OK) {
    lc_error_cleanup(&lcerr);
    return 0;
  }

  found = 0;
  now = time(NULL);
  for (offset = -10L; offset <= 10L && !found; ++offset) {
    lc_sink *sink;
    lc_get_res get_res;
    const void *bytes;
    size_t length;

    if (snprintf(key, sizeof(key), "snapshot.%llu",
                 (unsigned long long)(now + (time_t)offset)) <= 0) {
      continue;
    }
    sink = NULL;
    rc = lc_sink_to_memory(&sink, &lcerr);
    if (rc != LC_OK || sink == NULL) {
      continue;
    }
    memset(&get_res, 0, sizeof(get_res));
    rc = client->get(client, key, NULL, sink, &get_res, &lcerr);
    if (rc == LC_OK &&
        lc_sink_memory_bytes(sink, &bytes, &length, &lcerr) == LC_OK &&
        bytes != NULL && length > 0u) {
      char *copy;

      copy = (char *)malloc(length + 1u);
      assert(copy != NULL);
      memcpy(copy, bytes, length);
      copy[length] = '\0';
      found = strstr(copy, needle) != NULL;
      free(copy);
    }
    lc_get_res_cleanup(&get_res);
    sink->close(sink);
  }

  client->close(client);
  lc_error_cleanup(&lcerr);
  return found;
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
  assert(config.supervision_policy == VECTIS_SUPERVISION_AUTO);
  assert(config.service_failure_policy == VECTIS_SERVICE_FAILURE_FAIL_CLOSED);
  assert(config.quiescence_policy == VECTIS_QUIESCENCE_STRICT);
  assert(config.shutdown_grace_ms == VECTIS_APP_DEFAULT_SHUTDOWN_GRACE_MS);

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
  config.supervision_policy = VECTIS_SUPERVISION_DIRECT;
  assert_valid_server_config(&config);

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.supervision_policy = VECTIS_SUPERVISION_SUPERVISED;
  assert_valid_server_config(&config);

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.service_failure_policy = VECTIS_SERVICE_FAILURE_CONTINUE;
  assert_valid_server_config(&config);

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.quiescence_policy = VECTIS_QUIESCENCE_WARN_UNAVAILABLE;
  assert_valid_server_config(&config);

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
  config.supervision_policy = (vectis_supervision_policy)99;
  assert_invalid_server_config(&config, "supervision_policy");

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.service_failure_policy = (vectis_service_failure_policy)99;
  assert_invalid_server_config(&config, "service_failure_policy");

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.quiescence_policy = (vectis_quiescence_policy)99;
  assert_invalid_server_config(&config, "quiescence_policy");

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.shutdown_grace_ms = -1L;
  assert_invalid_server_config(&config, "shutdown_grace_ms");

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.server.keepalive_disabled = 1;
  config.server.keepalive_timeout_ms = 0L;
  config.server.keepalive_max_requests = 0u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  app->close(app);
}

static void assert_direct_supervision_policy_rejects_app_services(void) {
  vectis_app_config config;
  vectis_metrics_config metrics;
  vectis_route_config route;
  vectis_managed_service_config service_config;
  vectis_managed_service *service;
  runtime_managed_service_probe probe;
  vectis_app *app;
  vectis_error error;
  vectis_status status;

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.supervision_policy = VECTIS_SUPERVISION_DIRECT;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  vectis_metrics_config_init(&metrics);
  metrics.path = "/.metrics";
  metrics.json_path = "/.metrics.json";
  metrics.persistence_enabled = 1;
  metrics.storage_endpoint = "pouch:///tmp/vectis-direct-policy-not-used";
  status = app->metrics(app, &metrics, &error);
  assert(status == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/direct-policy", sample_handler, NULL);
  status = app->route(app, &route, &error);
  assert(status == VECTIS_OK);

  status = app->start(app, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "direct supervision_policy") != NULL);
  status = app->run(app, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "direct supervision_policy") != NULL);
  app->close(app);

  memset(&probe, 0, sizeof(probe));
  probe.wait_fds[0] = -1;
  probe.wait_fds[1] = -1;
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.supervision_policy = VECTIS_SUPERVISION_DIRECT;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  vectis_managed_service_config_init(&service_config);
  service_config.name = "direct-policy-managed";
  service_config.context = &probe;
  service_config.start = runtime_managed_service_start;
  service_config.start_with_app = 1;
  service = NULL;
  status = app->managed_service(app, &service_config, &service, &error);
  assert(status == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/direct-policy-managed",
                       sample_handler, NULL);
  status = app->route(app, &route, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "direct supervision_policy") != NULL);
  service->close(service);
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
  vectis_consumer_service_state service_state;
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
  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.declared);
  assert(!service_state.materialized);
  assert(!service_state.process_local);
  assert(!service_state.start_requested);
  assert(!service_state.started);
  assert(!service_state.failed);

  status = service->start(service, &error);
  assert(status == VECTIS_OK);
  assert(service->native(service) == NULL);
  status = vectis_consumer_service_state_get(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.start_requested);
  assert(!service_state.materialized);
  assert(!service_state.started);

  route = vectis_route(VECTIS_HTTP_GET, "/declared", sample_handler, NULL);
  status = app->route(app, &route, &error);
  assert(status == VECTIS_OK);

  service->close(service);
  app->close(app);
}

static void assert_managed_service_declaration_before_routes(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  vectis_managed_service_config service_config;
  vectis_managed_service *service;
  vectis_managed_service_state service_state;
  runtime_managed_service_probe probe;
  vectis_route_config route;

  memset(&probe, 0, sizeof(probe));
  probe.wait_fds[0] = -1;
  probe.wait_fds[1] = -1;
  vectis_app_config_init(&config);
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  vectis_managed_service_config_init(&service_config);
  service_config.name = "runtime-managed";
  service_config.context = &probe;
  service_config.start = runtime_managed_service_start;
  service_config.cleanup = runtime_managed_service_cleanup;
  service_config.start_with_app = 1;
  service = NULL;
  status = app->managed_service(app, &service_config, &service, &error);
  assert(status == VECTIS_OK);
  assert(service != NULL);
  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.declared);
  assert(!service_state.materialized);
  assert(!service_state.process_local);
  assert(service_state.start_requested);
  assert(!service_state.started);
  assert(!service_state.failed);

  route =
      vectis_route(VECTIS_HTTP_GET, "/managed-declared", sample_handler, NULL);
  status = app->route(app, &route, &error);
  assert(status == VECTIS_OK);

  service->close(service);
  assert(probe.cleaned == 1);
  app->close(app);
}

static void assert_managed_service_explicit_start_before_routes_defers(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  vectis_managed_service_config service_config;
  vectis_managed_service *service;
  vectis_managed_service_state service_state;
  runtime_managed_service_probe probe;
  vectis_route_config route;

  memset(&probe, 0, sizeof(probe));
  probe.wait_fds[0] = -1;
  probe.wait_fds[1] = -1;
  vectis_app_config_init(&config);
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  vectis_managed_service_config_init(&service_config);
  service_config.name = "runtime-managed-explicit-start";
  service_config.context = &probe;
  service_config.start = runtime_managed_service_start;
  service_config.cleanup = runtime_managed_service_cleanup;
  service_config.start_with_app = 0;
  service = NULL;
  status = app->managed_service(app, &service_config, &service, &error);
  assert(status == VECTIS_OK);

  status = service->start(service, &error);
  assert(status == VECTIS_OK);
  assert(probe.started == 0);
  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.declared);
  assert(!service_state.materialized);
  assert(!service_state.process_local);
  assert(service_state.start_requested);
  assert(!service_state.started);

  route = vectis_route(VECTIS_HTTP_GET, "/managed-explicit-start",
                       sample_handler, NULL);
  status = app->route(app, &route, &error);
  assert(status == VECTIS_OK);

  service->close(service);
  assert(probe.cleaned == 1);
  app->close(app);
}

static void assert_routes_reject_materialized_managed_services(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  vectis_managed_service_config service_config;
  vectis_managed_service *service;
  vectis_managed_service_state service_state;
  runtime_managed_service_probe probe;
  vectis_route_config route;

  memset(&probe, 0, sizeof(probe));
  probe.wait_fds[0] = -1;
  probe.wait_fds[1] = -1;
  vectis_app_config_init(&config);
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  vectis_managed_service_config_init(&service_config);
  service_config.name = "runtime-managed-materialized";
  service_config.context = &probe;
  service_config.start = runtime_managed_service_start;
  service_config.cleanup = runtime_managed_service_cleanup;
  service_config.start_with_app = 1;
  service = NULL;
  status = app->managed_service(app, &service_config, &service, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_OK);
  assert(probe.started == 1);
  status = app->stop(app, &error);
  assert(status == VECTIS_OK);
  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.materialized);
  assert(!service_state.started);

  route = vectis_route(VECTIS_HTTP_GET, "/managed-materialized", sample_handler,
                       NULL);
  status = app->route(app, &route, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "managed services") != NULL);

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
  config.shutdown_grace_ms = 250L;
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
  vectis_consumer_service_state service_state;
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
  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.declared);
  assert(service_state.materialized);
  assert(service_state.process_local);
  assert(service_state.start_requested);
  assert(service_state.started);
  assert(service_state.monitor_active);
  assert(!service_state.failed);

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

static void assert_supervised_child_exit_stops_consumer_service(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  vectis_route_config route;
  lc_consumer_config consumer;
  lc_consumer_service_config service_config;
  vectis_consumer_service *service;
  vectis_consumer_service_state service_state;
  char pouch_dir[] = "/tmp/vectis-runtime-child-exit.XXXXXX";
  char endpoint[4096];
  const char *endpoints[1];
  unsigned short port;
  pid_t child_pid;
  int reserved_fd;
  int written;

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
  config.shutdown_grace_ms = 500L;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  lc_consumer_config_init(&consumer);
  lc_consumer_service_config_init(&service_config);
  consumer.name = "runtime-child-exit";
  consumer.request.queue = "runtime-child-exit";
  consumer.request.owner = "runtime-child-exit-owner";
  consumer.request.wait_seconds = 1L;
  consumer.request.visibility_timeout_seconds = 1L;
  consumer.handle = sample_consumer_handler;
  service_config.consumers = &consumer;
  service_config.consumer_count = 1u;
  service = NULL;
  status = app->consumer_service(app, &service_config, &service, &error);
  assert(status == VECTIS_OK);
  assert(service != NULL);

  route = vectis_route(VECTIS_HTTP_GET, "/child-exit-after-readiness",
                       sample_handler, NULL);
  status = app->route(app, &route, &error);
  assert(status == VECTIS_OK);
  status = service->start(service, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_OK);
  child_pid = vectis_internal_kore_child_pid(app);
  assert(child_pid > 0);
  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.started);
  assert(service_state.monitor_active);

  assert(kill(child_pid, SIGTERM) == 0);
  (void)alarm(10u);
  status = app->wait(app, &error);
  (void)alarm(0u);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "supervised Kore runtime exited") != NULL);

  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.stop_requested);
  assert(!service_state.started);
  assert(!service_state.monitor_active);
  assert(service_state.monitor_done);
  assert(service_state.monitor_joined);

  service->close(service);
  app->close(app);
  remove_tree(pouch_dir);
}

static void assert_supervised_repeated_start_stop(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  vectis_route_config route;
  vectis_http_client_config http;
  vectis_http_response response;
  unsigned short port;
  char url[128];
  pid_t child_pid;
  int reserved_fd;
  int written;
  int i;

  reserved_fd = reserve_loopback_port(&port);
  close(reserved_fd);
  written =
      snprintf(url, sizeof(url), "http://127.0.0.1:%u/restart", (unsigned)port);
  assert(written > 0 && (size_t)written < sizeof(url));

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = port;
  config.supervision_policy = VECTIS_SUPERVISION_SUPERVISED;
  config.shutdown_grace_ms = 500L;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  route = vectis_route(VECTIS_HTTP_GET, "/restart", sample_handler, NULL);
  status = app->route(app, &route, &error);
  assert(status == VECTIS_OK);
  vectis_http_client_config_init(&http);
  http.timeout_ms = 2000L;
  http.connect_timeout_ms = 1000L;

  for (i = 0; i < 3; ++i) {
    memset(&response, 0, sizeof(response));
    status = app->start(app, &error);
    assert(status == VECTIS_OK);
    child_pid = vectis_internal_kore_child_pid(app);
    assert(child_pid > 0);

    status = vectis_http_get(&http, url, &response, &error);
    assert(status == VECTIS_OK);
    assert(response.status_code == 200);
    assert(response.body != NULL);
    assert(response.body_size == 2u);
    assert(memcmp(response.body, "ok", 2u) == 0);
    vectis_http_response_cleanup(&response);

    status = app->stop(app, &error);
    assert(status == VECTIS_OK);
    assert(vectis_internal_kore_child_pid(app) == 0);
  }

  app->close(app);
}

static void assert_supervised_managed_service_lifecycle(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  vectis_route_config route;
  vectis_http_client_config http;
  vectis_http_response response;
  vectis_managed_service_config service_config;
  vectis_managed_service *service;
  vectis_managed_service_state service_state;
  runtime_managed_service_probe probe;
  unsigned short port;
  char url[128];
  int reserved_fd;
  int written;

  memset(&probe, 0, sizeof(probe));
  assert(pipe(probe.wait_fds) == 0);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  reserved_fd = reserve_loopback_port(&port);
  close(reserved_fd);
  config.tls.port = port;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  vectis_managed_service_config_init(&service_config);
  service_config.name = "supervised-managed";
  service_config.context = &probe;
  service_config.start = runtime_managed_service_start;
  service_config.stop = runtime_managed_service_stop;
  service_config.wait = runtime_managed_service_wait;
  service_config.start_with_app = 1;
  service = NULL;
  status = app->managed_service(app, &service_config, &service, &error);
  assert(status == VECTIS_OK);

  route = vectis_route(VECTIS_HTTP_GET, "/managed", sample_handler, NULL);
  status = app->route(app, &route, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_OK);
  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.materialized);
  assert(service_state.process_local);
  assert(service_state.start_requested);
  assert(service_state.started);
  assert(service_state.monitor_active);
  assert(!service_state.failed);
  assert(probe.started == 1);

  vectis_http_client_config_init(&http);
  http.timeout_ms = 2000L;
  http.connect_timeout_ms = 1000L;
  written =
      snprintf(url, sizeof(url), "http://127.0.0.1:%u/managed", (unsigned)port);
  assert(written > 0 && (size_t)written < sizeof(url));
  memset(&response, 0, sizeof(response));
  status = vectis_http_get(&http, url, &response, &error);
  assert(status == VECTIS_OK);
  assert(response.status_code == 200);
  vectis_http_response_cleanup(&response);

  status = app->stop(app, &error);
  assert(status == VECTIS_OK);
  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.stop_requested);
  assert(!service_state.started);
  assert(!service_state.monitor_active);
  assert(service_state.monitor_done);
  assert(service_state.monitor_joined);
  assert(!service_state.failed);
  assert(probe.stopped == 1);
  assert(probe.waited == 1);

  service->close(service);
  app->close(app);
  close(probe.wait_fds[0]);
  close(probe.wait_fds[1]);
}

static void assert_managed_service_inherits_app_logger(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  vectis_managed_service_config service_config;
  vectis_managed_service *service;
  runtime_managed_service_probe probe;
  runtime_log_buffer log_buffer;
  pslog_logger *logger;

  logger = runtime_test_logger(&log_buffer);
  assert(logger != NULL);
  memset(&probe, 0, sizeof(probe));
  probe.wait_fds[0] = -1;
  probe.wait_fds[1] = -1;

  vectis_app_config_init(&config);
  config.logger = logger;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  vectis_managed_service_config_init(&service_config);
  service_config.name = "log-inherited-service";
  service_config.context = &probe;
  service_config.start = runtime_managed_service_start;
  service_config.stop = runtime_managed_service_stop;
  service_config.cleanup = runtime_managed_service_cleanup;
  service_config.start_with_app = 1;
  service = NULL;
  status = app->managed_service(app, &service_config, &service, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_OK);
  status = app->stop(app, &error);
  assert(status == VECTIS_OK);

  assert(strstr(log_buffer.data, "vectis.managed_service.started") != NULL);
  assert(strstr(log_buffer.data, "vectis.managed_service.stopped") != NULL);
  assert(strstr(log_buffer.data, "log-inherited-service") != NULL);

  service->close(service);
  app->close(app);
  logger->destroy(logger);
}

static void assert_managed_service_logger_disabled(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  vectis_managed_service_config service_config;
  vectis_managed_service *service;
  runtime_managed_service_probe probe;
  runtime_log_buffer log_buffer;
  pslog_logger *logger;

  logger = runtime_test_logger(&log_buffer);
  assert(logger != NULL);
  memset(&probe, 0, sizeof(probe));
  probe.wait_fds[0] = -1;
  probe.wait_fds[1] = -1;

  vectis_app_config_init(&config);
  config.logger = logger;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  vectis_managed_service_config_init(&service_config);
  service_config.name = "log-disabled-service";
  service_config.logger_disabled = 1;
  service_config.context = &probe;
  service_config.start = runtime_managed_service_start;
  service_config.stop = runtime_managed_service_stop;
  service_config.cleanup = runtime_managed_service_cleanup;
  service_config.start_with_app = 1;
  service = NULL;
  status = app->managed_service(app, &service_config, &service, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_OK);
  status = app->stop(app, &error);
  assert(status == VECTIS_OK);

  assert(strstr(log_buffer.data, "vectis.managed_service.started") == NULL);
  assert(strstr(log_buffer.data, "log-disabled-service") == NULL);

  service->close(service);
  app->close(app);
  logger->destroy(logger);
}

static void assert_service_only_curl_worker_mailbox_http(void) {
  vectis_app_config app_config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  vectis_mailbox_config mailbox_config;
  vectis_mailbox *mailbox;
  vectis_mailbox_broker_config broker_config;
  vectis_mailbox_broker *broker;
  vectis_curl_worker_service_config worker_config;
  vectis_managed_service *service;
  vectis_managed_service_state service_state;
  vectis_curl_worker_http_request request_config;
  vectis_curl_worker_event request_event;
  vectis_mailbox_event reply_event;
  vectis_curl_worker_http_response worker_response;
  runtime_http_mock_server http_server;
  char url[128];
  char failed_url[128];
  unsigned long correlation_id;
  unsigned short closed_port;
  int reserved_fd;
  int written;

  reserved_fd = reserve_loopback_port(&closed_port);
  close(reserved_fd);
  written = snprintf(failed_url, sizeof(failed_url), "http://127.0.0.1:%u/",
                     (unsigned)closed_port);
  assert(written > 0 && (size_t)written < sizeof(failed_url));

  runtime_http_mock_start(&http_server, "curl worker ok");
  written = snprintf(url, sizeof(url), "http://127.0.0.1:%u/worker",
                     http_server.port);
  assert(written > 0 && (size_t)written < sizeof(url));

  vectis_mailbox_config_init(&mailbox_config);
  mailbox_config.capacity = 4u;
  mailbox = NULL;
  status = vectis_mailbox_new(&mailbox_config, &mailbox, &error);
  assert(status == VECTIS_OK);
  vectis_mailbox_broker_config_init(&broker_config);
  broker_config.request_mailbox = mailbox;
  broker = NULL;
  status = vectis_mailbox_broker_new(&broker_config, &broker, &error);
  assert(status == VECTIS_OK);

  vectis_app_config_init(&app_config);
  app = vectis_app_new(&app_config, &error);
  assert(app != NULL);
  vectis_curl_worker_service_config_init(&worker_config);
  worker_config.name = "curl-worker-test";
  worker_config.request_mailbox = mailbox;
  worker_config.reply_broker = broker;
  worker_config.http.timeout_ms = 2000L;
  worker_config.http.connect_timeout_ms = 1000L;
  worker_config.poll_timeout_ms = 10L;
  service = NULL;
  status = app->curl_worker_service(app, &worker_config, &service, &error);
  assert(status == VECTIS_OK);
  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.declared);
  assert(service_state.start_requested);
  assert(!service_state.materialized);

  status = app->start(app, &error);
  assert(status == VECTIS_OK);
  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.materialized);
  assert(service_state.started);
  assert(service_state.monitor_active);

  vectis_curl_worker_http_response_init(&worker_response);
  vectis_curl_worker_http_request_init(&request_config);
  request_config.method = VECTIS_HTTP_GET;
  request_config.url = failed_url;
  request_config.timeout_ms = 500L;
  request_config.max_response_body_bytes = 1024u;
  status = vectis_curl_worker_http_event_build(&request_config, &request_event,
                                               &error);
  assert(status == VECTIS_OK);
  vectis_mailbox_event_init(&reply_event);
  status = broker->request(broker, &request_event.message, 3000L, &reply_event,
                           &correlation_id, &error);
  assert(status == VECTIS_OK);
  assert(correlation_id != 0u);
  status = vectis_curl_worker_http_response_decode(&reply_event,
                                                   &worker_response, &error);
  assert(status == VECTIS_OK);
  assert(worker_response.transfer_status != VECTIS_OK);
  assert(worker_response.status_code == 0L);
  vectis_mailbox_event_cleanup(&reply_event);
  vectis_curl_worker_event_cleanup(&request_event);

  vectis_curl_worker_http_request_init(&request_config);
  request_config.method = VECTIS_HTTP_GET;
  request_config.url = url;
  request_config.max_response_body_bytes = 1024u;
  status = vectis_curl_worker_http_event_build(&request_config, &request_event,
                                               &error);
  assert(status == VECTIS_OK);
  vectis_mailbox_event_init(&reply_event);
  status = broker->request(broker, &request_event.message, 3000L, &reply_event,
                           &correlation_id, &error);
  assert(status == VECTIS_OK);
  assert(correlation_id != 0u);
  status = vectis_curl_worker_http_response_decode(&reply_event,
                                                   &worker_response, &error);
  assert(status == VECTIS_OK);
  assert(worker_response.transfer_status == VECTIS_OK);
  assert(worker_response.status_code == 200L);
  assert(worker_response.content_type != NULL);
  assert(strcmp(worker_response.content_type, "text/plain") == 0);
  assert(worker_response.body_size == sizeof("curl worker ok") - 1u);
  assert(memcmp(worker_response.body, "curl worker ok",
                sizeof("curl worker ok") - 1u) == 0);
  status = vectis_curl_worker_http_response_decode(&reply_event,
                                                   &worker_response, &error);
  assert(status == VECTIS_OK);
  assert(worker_response.transfer_status == VECTIS_OK);
  assert(worker_response.status_code == 200L);
  assert(worker_response.content_type != NULL);
  assert(strcmp(worker_response.content_type, "text/plain") == 0);
  assert(worker_response.body_size == sizeof("curl worker ok") - 1u);
  assert(memcmp(worker_response.body, "curl worker ok",
                sizeof("curl worker ok") - 1u) == 0);
  assert(strstr(http_server.request, "GET /worker HTTP/1.1") != NULL);

  vectis_curl_worker_http_response_cleanup(&worker_response);
  vectis_mailbox_event_cleanup(&reply_event);
  vectis_curl_worker_event_cleanup(&request_event);
  status = app->stop(app, &error);
  assert(status == VECTIS_OK);
  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.stop_requested);
  assert(!service_state.started);
  assert(service_state.monitor_done);
  assert(service_state.monitor_joined);
  assert(!service_state.failed);

  service->close(service);
  app->close(app);
  broker->destroy(broker);
  mailbox->destroy(mailbox);
  runtime_http_mock_stop(&http_server);
}

static void assert_supervised_opcua_server_service_lifecycle(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  vectis_route_config route;
  vectis_http_client_config http;
  vectis_http_response response;
  vectis_opcua_server_service_config service_config;
  vectis_managed_service *service;
  vectis_managed_service_state service_state;
  cpkt_opcua_server *opcua_server;
  cpkt_opcua_result opcua_result;
  unsigned short port;
  char url[128];
  int reserved_fd;
  int written;

  opcua_server = NULL;
  opcua_result = cpkt_opcua_server_new(&opcua_server, 0u);
  assert(opcua_result == CPKT_OPCUA_OK);
  assert(opcua_server != NULL);

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  reserved_fd = reserve_loopback_port(&port);
  close(reserved_fd);
  config.tls.port = port;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  vectis_opcua_server_service_config_init(&service_config);
  service_config.name = "runtime-opcua-server";
  service_config.server = opcua_server;
  service_config.start_with_app = 1;
  service = NULL;
  status = app->opcua_server_service(app, &service_config, &service, &error);
  assert(status == VECTIS_OK);
  assert(service != NULL);
  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.declared);
  assert(!service_state.materialized);
  assert(service_state.start_requested);

  route = vectis_route(VECTIS_HTTP_GET, "/opcua-service", sample_handler, NULL);
  status = app->route(app, &route, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_OK);
  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.materialized);
  assert(service_state.process_local);
  assert(service_state.started);
  assert(service_state.monitor_active);
  assert(!service_state.failed);

  vectis_http_client_config_init(&http);
  http.timeout_ms = 2000L;
  http.connect_timeout_ms = 1000L;
  written = snprintf(url, sizeof(url), "http://127.0.0.1:%u/opcua-service",
                     (unsigned)port);
  assert(written > 0 && (size_t)written < sizeof(url));
  memset(&response, 0, sizeof(response));
  status = vectis_http_get(&http, url, &response, &error);
  assert(status == VECTIS_OK);
  assert(response.status_code == 200);
  vectis_http_response_cleanup(&response);

  status = app->stop(app, &error);
  assert(status == VECTIS_OK);
  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.stop_requested);
  assert(!service_state.started);
  assert(!service_state.monitor_active);
  assert(service_state.monitor_done);
  assert(service_state.monitor_joined);
  assert(!service_state.failed);

  service->close(service);
  app->close(app);
  cpkt_opcua_server_free(opcua_server);
}

static void assert_supervised_shutdown_deadline_kills_stopped_runtime(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  vectis_route_config route;
  vectis_http_client_config http;
  vectis_http_response response;
  unsigned short port;
  char url[128];
  pid_t child_pid;
  int reserved_fd;
  int written;

  reserved_fd = reserve_loopback_port(&port);
  close(reserved_fd);
  written = snprintf(url, sizeof(url), "http://127.0.0.1:%u/deadline",
                     (unsigned)port);
  assert(written > 0 && (size_t)written < sizeof(url));

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = port;
  config.supervision_policy = VECTIS_SUPERVISION_SUPERVISED;
  config.shutdown_grace_ms = 100L;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  route = vectis_route(VECTIS_HTTP_GET, "/deadline", sample_handler, NULL);
  status = app->route(app, &route, &error);
  assert(status == VECTIS_OK);
  vectis_http_client_config_init(&http);
  http.timeout_ms = 2000L;
  http.connect_timeout_ms = 1000L;

  memset(&response, 0, sizeof(response));
  status = app->start(app, &error);
  assert(status == VECTIS_OK);
  child_pid = vectis_internal_kore_child_pid(app);
  assert(child_pid > 0);
  assert(kill(-child_pid, 0) == 0);
  status = vectis_http_get(&http, url, &response, &error);
  assert(status == VECTIS_OK);
  assert(response.status_code == 200);
  vectis_http_response_cleanup(&response);

  assert(kill(-child_pid, SIGSTOP) == 0);
  status = app->stop(app, &error);
  if (status != VECTIS_OK) {
    (void)kill(-child_pid, SIGKILL);
  }
  assert(status == VECTIS_OK);
  assert(vectis_internal_kore_child_pid(app) == 0);

  memset(&response, 0, sizeof(response));
  status = app->start(app, &error);
  assert(status == VECTIS_OK);
  status = vectis_http_get(&http, url, &response, &error);
  assert(status == VECTIS_OK);
  assert(response.status_code == 200);
  vectis_http_response_cleanup(&response);
  status = app->stop(app, &error);
  assert(status == VECTIS_OK);

  app->close(app);
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

static void assert_service_failure_continue_waits_for_signal(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  lc_consumer_config consumer;
  lc_consumer_service_config service_config;
  vectis_consumer_service *service;
  vectis_consumer_service_state service_state;
  char pouch_dir[] = "/tmp/vectis-runtime-service-continue.XXXXXX";
  char endpoint[4096];
  const char *endpoints[1];
  runtime_enqueue_after_delay enqueue_task;
  pthread_t enqueue_thread;
  int handled_count;
  int written;

  assert(mkdtemp(pouch_dir) != NULL);
  written = snprintf(endpoint, sizeof(endpoint),
                     "pouch://%s?single_writer=false", pouch_dir);
  assert(written > 0 && (size_t)written < sizeof(endpoint));
  endpoints[0] = endpoint;

  vectis_app_config_init(&config);
  config.service_failure_policy = VECTIS_SERVICE_FAILURE_CONTINUE;
  config.lockd.endpoints = endpoints;
  config.lockd.endpoint_count = 1u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  handled_count = 0;
  lc_consumer_config_init(&consumer);
  lc_consumer_service_config_init(&service_config);
  consumer.name = "service-continue-failing";
  consumer.request.queue = "service-continue-failing";
  consumer.request.owner = "service-continue-failing-owner";
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

  enqueue_task.endpoint = endpoint;
  enqueue_task.queue = "service-continue-failing";
  enqueue_task.delay_ms = 100L;
  enqueue_task.service = service;
  enqueue_task.signal_after_failure = 1;
  assert(pthread_create(&enqueue_thread, NULL, runtime_enqueue_after_delay_main,
                        &enqueue_task) == 0);

  (void)alarm(10u);
  status = app->wait(app, &error);
  (void)alarm(0u);
  assert(pthread_join(enqueue_thread, NULL) == 0);
  assert(status == VECTIS_OK);
  assert(handled_count == 1);
  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.failed);
  assert(service_state.dependency_code != (long)LC_OK);

  service->close(service);
  app->close(app);
  remove_tree(pouch_dir);
}

static void assert_supervised_wait_reports_managed_service_exit(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  vectis_route_config route;
  vectis_managed_service_config service_config;
  vectis_managed_service *service;
  vectis_managed_service_state service_state;
  runtime_managed_service_probe probe;
  unsigned short port;
  int reserved_fd;

  memset(&probe, 0, sizeof(probe));
  probe.wait_fds[0] = -1;
  probe.wait_fds[1] = -1;
  probe.wait_status = VECTIS_ERR_STATE;
  probe.wait_error = "managed service wait failed";

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  reserved_fd = reserve_loopback_port(&port);
  close(reserved_fd);
  config.tls.port = port;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  vectis_managed_service_config_init(&service_config);
  service_config.name = "runtime-managed-failing";
  service_config.context = &probe;
  service_config.start = runtime_managed_service_start;
  service_config.stop = runtime_managed_service_stop;
  service_config.wait = runtime_managed_service_wait;
  service_config.start_with_app = 1;
  service = NULL;
  status = app->managed_service(app, &service_config, &service, &error);
  assert(status == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/managed-service-exit", sample_handler,
                       NULL);
  status = app->route(app, &route, &error);
  assert(status == VECTIS_OK);

  status = app->start(app, &error);
  assert(status == VECTIS_OK);
  (void)alarm(10u);
  status = app->wait(app, &error);
  (void)alarm(0u);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "managed service wait failed") != NULL);
  assert(probe.started == 1);
  assert(probe.waited == 1);

  status = service->state(service, &service_state, &error);
  assert(status == VECTIS_OK);
  assert(service_state.failed);
  assert(service_state.terminal_status == VECTIS_ERR_STATE);
  assert(!service_state.started);

  service->close(service);
  app->close(app);
}

#ifdef VECTIS_RUNTIME_HEADER_LIMIT_ONLY
int main(void) {
  assert_default_header_limit_accepts_64k();
  return 0;
}
#else
static int run_named_runtime_test(const char *name) {
  if (name == NULL || name[0] == '\0') {
    return 0;
  }
  if (strcmp(name, "server_config_validation") == 0) {
    assert_server_config_validation();
    return 1;
  }
  if (strcmp(name, "runtime_control_frame_contract") == 0) {
    assert_runtime_control_frame_contract();
    return 1;
  }
  if (strcmp(name, "route_body_policy_validation") == 0) {
    assert_route_body_policy_validation();
    return 1;
  }
  if (strcmp(name, "metrics_surface") == 0) {
    assert_metrics_surface();
    return 1;
  }
  if (strcmp(name, "supervised_metrics_persistence_worker") == 0) {
    assert_supervised_metrics_persistence_worker();
    return 1;
  }
  if (strcmp(name, "direct_supervision_policy_rejects_app_services") == 0) {
    assert_direct_supervision_policy_rejects_app_services();
    return 1;
  }
  if (strcmp(name, "consumer_service_declaration_before_routes") == 0) {
    assert_consumer_service_declaration_before_routes();
    return 1;
  }
  if (strcmp(name, "managed_service_declaration_before_routes") == 0) {
    assert_managed_service_declaration_before_routes();
    return 1;
  }
  if (strcmp(name, "managed_service_explicit_start_before_routes_defers") ==
      0) {
    assert_managed_service_explicit_start_before_routes_defers();
    return 1;
  }
  if (strcmp(name, "routes_reject_materialized_managed_services") == 0) {
    assert_routes_reject_materialized_managed_services();
    return 1;
  }
  if (strcmp(name, "kore_start_rejects_extra_thread") == 0) {
    assert_kore_start_rejects_extra_thread();
    return 1;
  }
  if (strcmp(name, "supervised_start_reports_child_readiness_failure") == 0) {
    assert_supervised_start_reports_child_readiness_failure();
    return 1;
  }
  if (strcmp(name, "supervised_wait_reports_consumer_service_exit") == 0) {
    assert_supervised_wait_reports_consumer_service_exit();
    return 1;
  }
  if (strcmp(name, "supervised_child_exit_stops_consumer_service") == 0) {
    assert_supervised_child_exit_stops_consumer_service();
    return 1;
  }
  if (strcmp(name, "supervised_repeated_start_stop") == 0) {
    assert_supervised_repeated_start_stop();
    return 1;
  }
  if (strcmp(name, "supervised_managed_service_lifecycle") == 0) {
    assert_supervised_managed_service_lifecycle();
    return 1;
  }
  if (strcmp(name, "managed_service_inherits_app_logger") == 0) {
    assert_managed_service_inherits_app_logger();
    return 1;
  }
  if (strcmp(name, "managed_service_logger_disabled") == 0) {
    assert_managed_service_logger_disabled();
    return 1;
  }
  if (strcmp(name, "service_only_curl_worker_mailbox_http") == 0) {
    assert_service_only_curl_worker_mailbox_http();
    return 1;
  }
  if (strcmp(name, "supervised_opcua_server_service_lifecycle") == 0) {
    assert_supervised_opcua_server_service_lifecycle();
    return 1;
  }
  if (strcmp(name, "supervised_shutdown_deadline_kills_stopped_runtime") == 0) {
    assert_supervised_shutdown_deadline_kills_stopped_runtime();
    return 1;
  }
  if (strcmp(name, "service_only_wait_reports_consumer_service_exit") == 0) {
    assert_service_only_wait_reports_consumer_service_exit();
    return 1;
  }
  if (strcmp(name, "service_failure_continue_waits_for_signal") == 0) {
    assert_service_failure_continue_waits_for_signal();
    return 1;
  }
  if (strcmp(name, "supervised_wait_reports_managed_service_exit") == 0) {
    assert_supervised_wait_reports_managed_service_exit();
    return 1;
  }
  if (strcmp(name, "kore_smoke") == 0) {
    assert_kore_smoke();
    return 1;
  }
  fprintf(stderr, "unknown VECTIS_RUNTIME_TEST case: %s\n", name);
  abort();
  return 1;
}

int main(void) {
  vectis_app_config config;
  vectis_error error;
  vectis_app *app;
  vectis_status status;
  vectis_route_config bad_route;
  vectis_route_config route;
  vectis_body_policy policy;

  if (run_named_runtime_test(getenv("VECTIS_RUNTIME_TEST"))) {
    return 0;
  }

  assert_server_config_validation();
  assert_runtime_control_frame_contract();
  assert_route_body_policy_validation();
  assert_metrics_surface();
  assert_supervised_metrics_persistence_worker();
  assert_direct_supervision_policy_rejects_app_services();
  assert_consumer_service_declaration_before_routes();
  assert_managed_service_declaration_before_routes();
  assert_managed_service_explicit_start_before_routes_defers();
  assert_routes_reject_materialized_managed_services();
  assert_kore_start_rejects_extra_thread();
  assert_supervised_start_reports_child_readiness_failure();
  assert_supervised_wait_reports_consumer_service_exit();
  assert_supervised_child_exit_stops_consumer_service();
  assert_supervised_repeated_start_stop();
  assert_supervised_managed_service_lifecycle();
  assert_managed_service_inherits_app_logger();
  assert_managed_service_logger_disabled();
  assert_service_only_curl_worker_mailbox_http();
  assert_supervised_opcua_server_service_lifecycle();
  assert_supervised_shutdown_deadline_kills_stopped_runtime();
  assert_service_only_wait_reports_consumer_service_exit();
  assert_service_failure_continue_waits_for_signal();
  assert_supervised_wait_reports_managed_service_exit();

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
