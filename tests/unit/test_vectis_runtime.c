#include "vectis_internal.h"
#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <lc/lc.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vectis/vectis.h>

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#define VECTIS_TEST_ASAN 1
#else
#define VECTIS_TEST_ASAN 0
#endif

extern u_int64_t http_body_disk_offload;
extern u_int64_t worker_idle_timeout;

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

static vectis_status stream_probe_open(vectis_app *app,
                                       vectis_request *request, void *userdata,
                                       void **state, vectis_error *error) {
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

static vectis_status stream_probe_write(
    vectis_app *app, vectis_request *request, const void *data, size_t size,
    void *state, void *userdata, vectis_error *error) {
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

static vectis_status stream_probe_finish(
    vectis_app *app, vectis_request *request, vectis_response *response,
    void *state, void *userdata, vectis_error *error) {
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
  (void)snprintf(text, sizeof(text), "%lu",
                 (unsigned long)context->total_size);
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

static vectis_status stream_reader_handler(
    vectis_app *app, vectis_request *request, struct lc_source *reader,
    vectis_response *response, void *userdata, vectis_error *error) {
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
  (void)snprintf(text, sizeof(text), "%lu",
                 (unsigned long)strlen(doc->body));
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
    status =
        vectis_dsv_rows_next(rows, &has_row, &row_number, &row_ptr, error);
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
  vectis_xml_config xml_config;
  vectis_dsv_config dsv_config;
  vectis_app_config second_config;
  vectis_route_config second_route;
  const char *headers[] = {"x-vectis-trace: runtime-smoke"};
  const char upload_path[] = "/tmp/vectis-runtime-upload.bin";
  const char stream_upload_path[] = "/tmp/vectis-runtime-stream-source.bin";
  const char stream_file_path[] = "/tmp/vectis-runtime-stream-upload.bin";
  const char xml_upload_path[] = "/tmp/vectis-runtime-upload.xml";
  const char dsv_upload_path[] = "/tmp/vectis-runtime-upload.csv";
  const char json_source_path[] = "/tmp/vectis-runtime-json-source.txt";
  const char response_file_path[] = "/tmp/vectis-runtime-response.txt";
  const char response_file_body[] = "file-response";
  vectis_error error;
  vectis_error second_error;
  vectis_status status;
  vectis_status second_status;
  vectis_app *app;
  vectis_app *second_app;
  FILE *fp;
  char *default_spooled_body;
  char *stream_body;
  char size_text[64];
  char dsv_text[128];
  size_t default_spooled_body_size;
  size_t stream_body_size;
  size_t xml_body_size;
  size_t dsv_rows;
  long long dsv_total;
  size_t dsv_active;
  long stream_file_size;
  int attempt;
  int i;

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
  memset(&stream_context, 0, sizeof(stream_context));
  memset(&dsv_summary, 0, sizeof(dsv_summary));
  memset(&json_source_request, 0, sizeof(json_source_request));
  memset(&no_body_request, 0, sizeof(no_body_request));
  memset(&json_source_doc, 0, sizeof(json_source_doc));
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
  stream_route =
      vectis_stream_upload_route(VECTIS_HTTP_POST, "/stream-upload",
                                 stream_probe_open, stream_probe_write,
                                 stream_probe_finish, stream_probe_close,
                                 &stream_context);
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
  stream_file_route = vectis_upload_file_route(
      VECTIS_HTTP_POST, "/stream-file", stream_file_path,
      "application/octet-stream");
  stream_file_route.body.max_bytes = 2097152u;
  stream_file_route.body.memory_buffer_limit_bytes = 8u;
  status = app->upload_file(app, &stream_file_route, &error);
  assert(status == VECTIS_OK);
  file_route = vectis_route(VECTIS_HTTP_GET, "/file", file_handler,
                            (void *)response_file_path);
  status = vectis_register_route(app, &file_route, &error);
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
  assert(second_status == VECTIS_ERR_STATE);
  assert(strstr(second_error.message, "Kore runtime is already running") !=
         NULL);
  vectis_destroy(second_app);

  vectis_http_client_config_init(&http);
  http.timeout_ms = 60000L;
  http.connect_timeout_ms = 200L;
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
  assert(http_body_disk_offload == 0u);
  assert(worker_idle_timeout == VECTIS_SERVER_DEFAULT_IDLE_TIMEOUT_MS);

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

  status = vectis_stop(app, &error);
  assert(status == VECTIS_OK);
  app->close(app);
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
