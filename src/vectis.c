#include "vectis_internal.h"

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <lc/lc.h>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <libxml/parser.h>
#include <libxml/xmlreader.h>
#include <limits.h>
#include <lonejson.h>
#include <netdb.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <pthread.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <vectis/embedded_fs.h>
#include <vectis/webdav.h>

typedef enum vectis_route_entry_kind {
  VECTIS_ROUTE_ENTRY_HANDLER = 0,
  VECTIS_ROUTE_ENTRY_UPLOAD_STREAM = 1
} vectis_route_entry_kind;

typedef struct vectis_route_entry {
  vectis_route_entry_kind kind;
  vectis_http_method method;
  vectis_http_methods methods;
  char *path;
  vectis_route_path_kind path_kind;
  vectis_body_policy body;
  vectis_route_handler_fn handler;
  vectis_upload_open_fn upload_open;
  vectis_upload_write_fn upload_write;
  vectis_upload_finish_fn upload_finish;
  vectis_upload_close_fn upload_close;
  void *userdata;
  int owns_userdata;
} vectis_route_entry;

typedef struct vectis_json_route_adapter {
  const lonejson_map *input_map;
  size_t input_size;
  const lonejson_map *output_map;
  size_t output_size;
  vectis_json_route_handler_fn handler;
  void *userdata;
} vectis_json_route_adapter;

typedef struct vectis_json_typed_route_adapter {
  const lonejson_map *input_map;
  size_t input_size;
  vectis_json_typed_route_handler_fn handler;
  void *userdata;
} vectis_json_typed_route_adapter;

typedef struct vectis_xml_route_adapter {
  const lonejson_map *input_map;
  size_t input_size;
  vectis_xml_config config;
  vectis_xml_route_handler_fn handler;
  void *userdata;
  size_t buffer_bytes;
} vectis_xml_route_adapter;

typedef struct vectis_dsv_route_adapter {
  const lonejson_map *row_map;
  size_t row_size;
  vectis_dsv_config config;
  vectis_dsv_route_handler_fn handler;
  void *userdata;
  size_t buffer_bytes;
} vectis_dsv_route_adapter;

typedef struct vectis_static_route_data {
  int directory;
  const char *path_prefix;
  const char *file_path;
  const char *root_dir;
  const char *content_type;
  const char *index_file;
  const vectis_embedded_fs *embedded_fs;
  const char *cache_control;
  const char *not_found_body;
  const char *not_found_content_type;
  vectis_http_methods allowed_methods;
} vectis_static_route_data;

typedef struct vectis_upload_file_adapter {
  char *file_path;
  char *content_type;
} vectis_upload_file_adapter;

typedef struct vectis_upload_file_state {
  FILE *file;
  size_t size;
} vectis_upload_file_state;

typedef struct vectis_upload_reader_chunk {
  struct vectis_upload_reader_chunk *next;
  size_t offset;
  size_t size;
  unsigned char data[1];
} vectis_upload_reader_chunk;

typedef struct vectis_upload_reader_adapter {
  vectis_upload_reader_handler_fn handler;
  void *userdata;
  size_t buffer_bytes;
  void (*free_userdata)(void *userdata);
} vectis_upload_reader_adapter;

typedef struct vectis_upload_reader_state {
  pthread_mutex_t mutex;
  pthread_cond_t readable;
  pthread_cond_t writable;
  vectis_upload_reader_chunk *head;
  vectis_upload_reader_chunk *tail;
  size_t queued;
  size_t capacity;
  int eof;
  int failed;
  int closed;
  int handler_done;
  int sync_initialized;
  int thread_started;
  pthread_t thread;
  lc_source *source;
  vectis_app *app;
  vectis_request *request;
  vectis_response *response;
  vectis_error error;
  vectis_status status;
  vectis_upload_reader_handler_fn handler;
  void *userdata;
} vectis_upload_reader_state;

typedef struct vectis_kv {
  char *name;
  char *value;
} vectis_kv;

typedef struct vectis_openapi_doc_entry {
  vectis_http_methods methods;
  char *path;
  vectis_openapi_route_doc doc;
} vectis_openapi_doc_entry;

typedef struct vectis_string_builder {
  char *data;
  size_t size;
  size_t capacity;
} vectis_string_builder;

typedef struct vectis_lonejson_builder_sink {
  vectis_string_builder *builder;
  vectis_error *error;
} vectis_lonejson_builder_sink;

typedef struct vectis_lonejson_fd_sink {
  int fd;
  vectis_error *error;
} vectis_lonejson_fd_sink;

typedef struct vectis_dsv_fields {
  char **items;
  size_t *sizes;
  size_t count;
  size_t capacity;
} vectis_dsv_fields;

typedef struct vectis_dsv_parser {
  vectis_dsv_config config;
  lc_source *source;
  vectis_dsv_fields fields;
  vectis_string_builder field;
  char buffer[4096];
  size_t offset;
  size_t size;
  size_t physical_row;
  int first_field_quoted;
  int eof;
  int skip_next_lf;
} vectis_dsv_parser;

struct vectis_dsv_rows {
  vectis_dsv_parser parser;
  vectis_dsv_fields headers;
  const lonejson_map *map;
  const char *const *columns;
  const char **header_columns;
  size_t column_count;
  size_t row_size;
  size_t data_row;
  void *value;
  lonejson *runtime;
  int done;
};

typedef struct vectis_xml_field_state {
  size_t count;
  int array;
  int array_open;
  int array_closed;
} vectis_xml_field_state;

typedef struct vectis_xml_lc_reader {
  lc_source *source;
  vectis_status status;
  lc_error error;
  int has_error;
} vectis_xml_lc_reader;

typedef struct vectis_spill_writer {
  unsigned char *memory;
  size_t memory_size;
  size_t memory_capacity;
  size_t memory_limit;
  size_t total;
  FILE *fp;
  char *path;
  int spooled;
} vectis_spill_writer;

typedef struct vectis_app_impl {
  pthread_mutex_t mutex;
  int started;
  int owns_logger;
  char *app_name;
  char *bind;
  char *domain;
  char **domains;
  size_t domain_count;
  char *cert_key_bundle_path;
  void *cert_key_bundle_pem;
  size_t cert_key_bundle_pem_size;
  struct lc_source *cert_key_bundle_source;
  char *certificate_path;
  void *certificate_pem;
  size_t certificate_pem_size;
  struct lc_source *certificate_source;
  char *private_key_path;
  void *private_key_pem;
  size_t private_key_pem_size;
  struct lc_source *private_key_source;
  char *ca_bundle_path;
  void *ca_bundle_pem;
  size_t ca_bundle_pem_size;
  struct lc_source *ca_bundle_source;
  char *client_ca_bundle_path;
  void *client_ca_bundle_pem;
  size_t client_ca_bundle_pem_size;
  struct lc_source *client_ca_bundle_source;
  char *acme_email;
  char *acme_directory_url;
  char *acme_state_dir;
  char *unix_socket_path;
  void *client_bundle_pem;
  size_t client_bundle_pem_size;
  struct lc_source *client_bundle_source;
  char *client_bundle_path;
  char *default_namespace;
  char **endpoints;
  size_t endpoint_count;
  pslog_logger *lockd_logger;
  int lockd_logger_disabled;
  long timeout_ms;
  unsigned short port;
  vectis_tls_mode tls_mode;
  int require_client_certificate;
  vectis_server_config server;
  pslog_logger *logger;
  vectis_route_entry *routes;
  size_t route_count;
  size_t route_capacity;
  vectis_openapi_doc_entry *openapi_docs;
  size_t openapi_doc_count;
  size_t openapi_doc_capacity;
  struct lc_client *lockd_client;
  pid_t lockd_client_pid;
  struct vectis_consumer_receiver_entry *consumer_receivers;
} vectis_app_impl;

struct vectis_request {
  vectis_http_method method;
  char *path;
  struct http_request *kore_request;
  vectis_bytes body;
  struct lc_source *body_reader;
  int owns_body_reader;
  char *body_path;
  int body_spooled;
  int body_streaming_upload;
  vectis_body_policy body_policy;
  vectis_kv *path_params;
  size_t path_param_count;
  size_t path_param_capacity;
  vectis_kv *query;
  size_t query_count;
  size_t query_capacity;
  vectis_kv *headers;
  size_t header_count;
  size_t header_capacity;
};

struct vectis_response {
  int status_code;
  char *content_type;
  void *body;
  size_t body_size;
  char *file_path;
  int file_path_temporary;
  vectis_kv *headers;
  size_t header_count;
  size_t header_capacity;
  int sent;
};

typedef struct vectis_file_sink {
  FILE *fp;
} vectis_file_sink;

struct vectis_json_response {
  vectis_response *response;
  int sent;
};

typedef struct vectis_error_response_body {
  char code[64];
  char message[256];
  char detail[256];
} vectis_error_response_body;

static const lonejson_field vectis_error_response_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(vectis_error_response_body, code, "code",
                                    LONEJSON_OVERFLOW_TRUNCATE),
    LONEJSON_FIELD_STRING_FIXED_REQ(vectis_error_response_body, message,
                                    "message", LONEJSON_OVERFLOW_TRUNCATE),
    LONEJSON_FIELD_STRING_FIXED(vectis_error_response_body, detail, "detail",
                                LONEJSON_OVERFLOW_TRUNCATE)};

LONEJSON_MAP_DEFINE(vectis_error_response_map, vectis_error_response_body,
                    vectis_error_response_fields);

typedef struct vectis_consumer_service_impl {
  lc_consumer_service *service;
  struct vectis_consumer_receiver_runtime *receivers;
} vectis_consumer_service_impl;

typedef struct vectis_consumer_receiver_entry {
  char *kind;
  vectis_consumer_receiver_adapter adapter;
  struct vectis_consumer_receiver_entry *next;
} vectis_consumer_receiver_entry;

typedef struct vectis_consumer_receiver_runtime {
  vectis_consumer_receiver receiver;
  struct vectis_consumer_receiver_runtime *next;
} vectis_consumer_receiver_runtime;

typedef struct vectis_webdav_marker_receiver {
  vectis_webdav_config storage;
  char *cache_dir;
  char *site_id;
  char *processing_path;
  char *done_path;
  char *processing_body;
  char *done_body;
  long processing_delay_seconds;
} vectis_webdav_marker_receiver;

typedef struct vectis_curl_buffer {
  char *data;
  size_t size;
  int failed;
} vectis_curl_buffer;

typedef struct vectis_curl_response_stream {
  vectis_http_response_body_fn callback;
  void *userdata;
  vectis_error *error;
  int failed;
} vectis_curl_response_stream;

typedef struct vectis_curl_header_capture {
  vectis_http_response *response;
  int failed;
} vectis_curl_header_capture;

typedef struct vectis_curl_request_body {
  const void *data;
  size_t size;
  size_t offset;
  char *owned_data;
  lonejson *json_runtime;
  lonejson_curl_upload json_upload;
  curl_off_t json_size;
  int json_upload_active;
  FILE *file;
  long file_size;
} vectis_curl_request_body;

static vectis_status vectis_app_start_impl(vectis_app *app,
                                           vectis_error *error);
static vectis_status vectis_app_stop_impl(vectis_app *app, vectis_error *error);
static void
vectis_close_lockd_client_for_current_process(vectis_app_impl *impl);
static vectis_status vectis_app_register_route_impl(
    vectis_app *app, const vectis_route_config *route, vectis_error *error);
static vectis_status vectis_app_register_route_owned_userdata(
    vectis_app *app, const vectis_route_config *route, int owns_userdata,
    vectis_error *error);
static vectis_status vectis_app_register_upload_owned_userdata(
    vectis_app *app, const vectis_upload_route_config *route, int owns_userdata,
    vectis_error *error);
static vectis_status vectis_set_lonejson_error(vectis_error *error,
                                               lonejson_status status,
                                               const lonejson_error *json_error,
                                               const char *message);
static vectis_status vectis_register_consumer_receiver_impl(
    vectis_app_impl *impl, const vectis_consumer_receiver_adapter *adapter,
    vectis_error *error);
static vectis_status vectis_upload_file_write(vectis_app *app,
                                              vectis_request *request,
                                              const void *data, size_t size,
                                              void *state, void *userdata,
                                              vectis_error *error);
static vectis_status vectis_upload_reader_write(vectis_app *app,
                                                vectis_request *request,
                                                const void *data, size_t size,
                                                void *state, void *userdata,
                                                vectis_error *error);
static vectis_status
vectis_dsv_rows_open(vectis_dsv_rows *rows, struct lc_source *source,
                     const lonejson_map *map, size_t row_size,
                     const vectis_dsv_config *config, vectis_error *error);
static void vectis_dsv_rows_cleanup(vectis_dsv_rows *rows);
vectis_status
vectis_register_upload_stream(vectis_app *app,
                              const vectis_upload_route_config *route,
                              vectis_error *error);
vectis_status
vectis_register_upload_file(vectis_app *app,
                            const vectis_upload_file_route_config *route,
                            vectis_error *error);
vectis_status
vectis_register_upload_reader(vectis_app *app,
                              const vectis_upload_reader_route_config *route,
                              vectis_error *error);
vectis_status vectis_register_xml_route(vectis_app *app,
                                        const vectis_xml_route_config *route,
                                        vectis_error *error);
vectis_status vectis_register_dsv_route(vectis_app *app,
                                        const vectis_dsv_route_config *route,
                                        vectis_error *error);
vectis_status
vectis_register_prefixed_xml_route(vectis_app *app, const char *prefix,
                                   const vectis_xml_route_config *route,
                                   vectis_error *error);
vectis_status
vectis_register_prefixed_dsv_route(vectis_app *app, const char *prefix,
                                   const vectis_dsv_route_config *route,
                                   vectis_error *error);
static size_t vectis_app_route_count_impl(const vectis_app *app);
static size_t vectis_app_max_request_body_bytes(vectis_app_impl *impl);
static size_t vectis_app_body_disk_offload_bytes(vectis_app_impl *impl,
                                                 int *configured);
static pslog_logger *vectis_app_logger_impl(vectis_app *app);
static vectis_status vectis_http_client_send_json(
    vectis_http_client *client, vectis_http_method method, const char *url,
    const lonejson_map *map, const void *value, vectis_http_response *response,
    vectis_error *error);
static vectis_status vectis_read_source_bytes(const vectis_source *source,
                                              void **out, size_t *out_size,
                                              const char *label,
                                              vectis_error *error);

static pthread_once_t vectis_curl_once = PTHREAD_ONCE_INIT;
static pthread_once_t vectis_libssh2_once = PTHREAD_ONCE_INIT;

static void vectis_curl_global_init_once(void) {
  (void)curl_global_init(CURL_GLOBAL_DEFAULT);
}

static void vectis_libssh2_global_init_once(void) { (void)libssh2_init(0); }

static void vectis_set_errorf(vectis_error *error, vectis_status code,
                              const char *fmt, ...);

static lonejson *vectis_lonejson_new(vectis_error *error) {
  lonejson_error json_error;
  lonejson *runtime;

  runtime = lonejson_new(NULL, &json_error);
  if (runtime == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_NOMEM,
                      "failed to initialize lonejson runtime: %s",
                      json_error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
      error->dependency_code = (long)LONEJSON_STATUS_ALLOCATION_FAILED;
    }
  }
  return runtime;
}

void vectis_error_clear(vectis_error *error) {
  if (error == NULL) {
    return;
  }
  error->code = VECTIS_OK;
  error->source = VECTIS_ERROR_SOURCE_NONE;
  error->dependency_code = 0L;
  error->http_status = 0L;
  error->message[0] = '\0';
  error->detail[0] = '\0';
}

void vectis_set_error(vectis_error *error, vectis_status code,
                      const char *message) {
  vectis_error_clear(error);
  if (error == NULL) {
    return;
  }
  error->code = code;
  error->source = VECTIS_ERROR_SOURCE_VECTIS;
  if (message == NULL) {
    return;
  }
  (void)snprintf(error->message, sizeof(error->message), "%s", message);
}

static void vectis_set_errorf(vectis_error *error, vectis_status code,
                              const char *fmt, ...) {
  va_list ap;

  vectis_error_clear(error);
  if (error == NULL) {
    return;
  }
  error->code = code;
  error->source = VECTIS_ERROR_SOURCE_VECTIS;
  if (fmt == NULL) {
    return;
  }
  va_start(ap, fmt);
  (void)vsnprintf(error->message, sizeof(error->message), fmt, ap);
  va_end(ap);
}

static vectis_status vectis_set_lockdc_error(vectis_error *error, int rc,
                                             const lc_error *lcerr,
                                             const char *message) {
  vectis_set_errorf(error, VECTIS_ERR_STATE, "%s: %s",
                    message != NULL ? message : "lockdc operation failed",
                    lcerr != NULL && lcerr->message != NULL
                        ? lcerr->message
                        : "unknown lockdc error");
  if (error != NULL) {
    error->source = VECTIS_ERROR_SOURCE_LOCKDC;
    error->dependency_code = (long)rc;
    if (lcerr != NULL) {
      error->http_status = lcerr->http_status;
      if (lcerr->detail != NULL) {
        (void)snprintf(error->detail, sizeof(error->detail), "%s",
                       lcerr->detail);
      }
    }
  }
  return VECTIS_ERR_STATE;
}

static void vectis_request_close_body_reader(vectis_request *request) {
  if (request == NULL) {
    return;
  }
  if (request->body_reader != NULL && request->owns_body_reader) {
    lc_source_close(request->body_reader);
  }
  request->body_reader = NULL;
  request->owns_body_reader = 0;
}

static vectis_status vectis_source_error(vectis_error *error, int rc,
                                         const lc_error *lcerr,
                                         const char *message) {
  vectis_set_errorf(
      error, rc == LC_ERR_NOMEM ? VECTIS_ERR_NOMEM : VECTIS_ERR_STATE, "%s: %s",
      message != NULL ? message : "source operation failed",
      lcerr != NULL && lcerr->message != NULL ? lcerr->message
                                              : "unknown source error");
  if (error != NULL) {
    error->source = VECTIS_ERROR_SOURCE_LOCKDC;
    error->dependency_code = (long)rc;
  }
  return rc == LC_ERR_NOMEM ? VECTIS_ERR_NOMEM : VECTIS_ERR_STATE;
}

static vectis_status vectis_request_reset_body_reader(vectis_request *request,
                                                      vectis_error *error) {
  lc_error lcerr;
  int rc;

  if (request == NULL || request->body_reader == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "request body reader is not available");
    return VECTIS_ERR_INVALID;
  }
  if (request->body_reader->reset == NULL) {
    return VECTIS_OK;
  }
  lc_error_init(&lcerr);
  rc = request->body_reader->reset(request->body_reader, &lcerr);
  if (rc != LC_OK) {
    (void)vectis_source_error(error, rc, &lcerr,
                              "failed to reset request body reader");
    lc_error_cleanup(&lcerr);
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  return VECTIS_OK;
}

static int vectis_tmp_template(char *buffer, size_t buffer_size,
                               const vectis_body_materialize_config *config) {
  const char *directory;
  const char *prefix;
  int n;

  if (buffer == NULL || buffer_size == 0u) {
    return 0;
  }
  directory = config != NULL && config->directory != NULL &&
                      config->directory[0] != '\0'
                  ? config->directory
                  : "/tmp";
  prefix = config != NULL && config->prefix != NULL && config->prefix[0] != '\0'
               ? config->prefix
               : "vectis-body";
  n = snprintf(buffer, buffer_size, "%s/%s-XXXXXX", directory, prefix);
  return n > 0 && (size_t)n < buffer_size;
}

static int vectis_response_tmp_template(char *buffer, size_t buffer_size) {
  int n;

  if (buffer == NULL || buffer_size == 0u) {
    return 0;
  }
  n = snprintf(buffer, buffer_size, "/tmp/vectis-response-XXXXXX");
  return n > 0 && (size_t)n < buffer_size;
}

static void vectis_string_builder_cleanup(vectis_string_builder *builder) {
  if (builder == NULL) {
    return;
  }
  free(builder->data);
  builder->data = NULL;
  builder->size = 0u;
  builder->capacity = 0u;
}

static vectis_status
vectis_string_builder_reserve(vectis_string_builder *builder, size_t extra,
                              vectis_error *error) {
  char *next;
  size_t needed;
  size_t capacity;

  if (builder == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "string builder is required");
    return VECTIS_ERR_INVALID;
  }
  if ((size_t)-1 - builder->size < extra + 1u) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "string builder size overflow");
    return VECTIS_ERR_NOMEM;
  }
  needed = builder->size + extra + 1u;
  if (needed <= builder->capacity) {
    return VECTIS_OK;
  }
  capacity = builder->capacity == 0u ? 256u : builder->capacity;
  while (capacity < needed) {
    if (capacity > ((size_t)-1 / 2u)) {
      capacity = needed;
      break;
    }
    capacity *= 2u;
  }
  next = (char *)realloc(builder->data, capacity);
  if (next == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to grow string builder");
    return VECTIS_ERR_NOMEM;
  }
  builder->data = next;
  builder->capacity = capacity;
  return VECTIS_OK;
}

static vectis_status
vectis_string_builder_append_n(vectis_string_builder *builder, const char *text,
                               size_t length, vectis_error *error) {
  if (text == NULL && length > 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "string builder text is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_string_builder_reserve(builder, length, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  if (length > 0u) {
    memcpy(builder->data + builder->size, text, length);
  }
  builder->size += length;
  builder->data[builder->size] = '\0';
  return VECTIS_OK;
}

static vectis_status
vectis_string_builder_append(vectis_string_builder *builder, const char *text,
                             vectis_error *error) {
  return vectis_string_builder_append_n(builder, text != NULL ? text : "",
                                        text != NULL ? strlen(text) : 0u,
                                        error);
}

static lonejson_status
vectis_lonejson_builder_sink_write(void *user, const void *data, size_t size,
                                   lonejson_error *json_error) {
  vectis_lonejson_builder_sink *sink;
  const char *message;
  vectis_status status;

  sink = (vectis_lonejson_builder_sink *)user;
  if (sink == NULL || sink->builder == NULL || (data == NULL && size > 0u)) {
    if (json_error != NULL) {
      lonejson_error_init(json_error);
      json_error->code = LONEJSON_STATUS_CALLBACK_FAILED;
      (void)snprintf(json_error->message, sizeof(json_error->message), "%s",
                     "JSON builder sink is required");
    }
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  status = vectis_string_builder_append_n(sink->builder, (const char *)data,
                                          size, sink->error);
  if (status != VECTIS_OK) {
    if (json_error != NULL) {
      lonejson_error_init(json_error);
      json_error->code = LONEJSON_STATUS_CALLBACK_FAILED;
      message = sink->error != NULL && sink->error->message[0] != '\0'
                    ? sink->error->message
                    : "JSON builder sink write failed";
      strncpy(json_error->message, message, sizeof(json_error->message) - 1u);
      json_error->message[sizeof(json_error->message) - 1u] = '\0';
    }
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static vectis_status
vectis_string_builder_appendf(vectis_string_builder *builder,
                              vectis_error *error, const char *fmt, ...) {
  va_list ap;
  int n;
  size_t old_size;

  if (fmt == NULL) {
    return VECTIS_OK;
  }
  va_start(ap, fmt);
  n = vsnprintf(NULL, 0u, fmt, ap);
  va_end(ap);
  if (n < 0) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to format string builder content");
    return VECTIS_ERR_STATE;
  }
  if (vectis_string_builder_reserve(builder, (size_t)n, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  old_size = builder->size;
  va_start(ap, fmt);
  (void)vsnprintf(builder->data + builder->size,
                  builder->capacity - builder->size, fmt, ap);
  va_end(ap);
  builder->size = old_size + (size_t)n;
  return VECTIS_OK;
}

const char *vectis_status_string(vectis_status status) {
  switch (status) {
  case VECTIS_OK:
    return "ok";
  case VECTIS_ERR_INVALID:
    return "invalid";
  case VECTIS_ERR_NOMEM:
    return "nomem";
  case VECTIS_ERR_STATE:
    return "state";
  case VECTIS_ERR_CONFLICT:
    return "conflict";
  case VECTIS_ERR_NOT_IMPLEMENTED:
    return "not_implemented";
  case VECTIS_ERR_TIMEOUT:
    return "timeout";
  default:
    return "unknown";
  }
}

const char *vectis_error_source_string(vectis_error_source source) {
  switch (source) {
  case VECTIS_ERROR_SOURCE_NONE:
    return "none";
  case VECTIS_ERROR_SOURCE_VECTIS:
    return "vectis";
  case VECTIS_ERROR_SOURCE_KORE:
    return "kore";
  case VECTIS_ERROR_SOURCE_LOCKDC:
    return "lockdc";
  case VECTIS_ERROR_SOURCE_LONEJSON:
    return "lonejson";
  case VECTIS_ERROR_SOURCE_PSLOG:
    return "pslog";
  case VECTIS_ERROR_SOURCE_CURL:
    return "curl";
  case VECTIS_ERROR_SOURCE_OPENSSL:
    return "openssl";
  case VECTIS_ERROR_SOURCE_LIBSSH2:
    return "libssh2";
  default:
    return "unknown";
  }
}

const char *vectis_http_method_string(vectis_http_method method) {
  switch (method) {
  case VECTIS_HTTP_ANY:
    return "ANY";
  case VECTIS_HTTP_GET:
    return "GET";
  case VECTIS_HTTP_POST:
    return "POST";
  case VECTIS_HTTP_PUT:
    return "PUT";
  case VECTIS_HTTP_PATCH:
    return "PATCH";
  case VECTIS_HTTP_DELETE:
    return "DELETE";
  case VECTIS_HTTP_HEAD:
    return "HEAD";
  case VECTIS_HTTP_OPTIONS:
    return "OPTIONS";
  case VECTIS_HTTP_PROPFIND:
    return "PROPFIND";
  case VECTIS_HTTP_MKCOL:
    return "MKCOL";
  case VECTIS_HTTP_COPY:
    return "COPY";
  case VECTIS_HTTP_MOVE:
    return "MOVE";
  default:
    return "UNKNOWN";
  }
}

const char *vectis_body_mode_string(vectis_body_mode mode) {
  switch (mode) {
  case VECTIS_BODY_NONE:
    return "none";
  case VECTIS_BODY_JSON:
    return "json";
  case VECTIS_BODY_BUFFERED:
    return "buffered";
  case VECTIS_BODY_STREAMING_UPLOAD:
    return "streaming_upload";
  default:
    return "unknown";
  }
}

void vectis_source_init(vectis_source *source) {
  if (source == NULL) {
    return;
  }
  memset(source, 0, sizeof(*source));
}

vectis_source vectis_source_from_path(const char *path) {
  vectis_source source;

  vectis_source_init(&source);
  source.path = path;
  return source;
}

vectis_source vectis_source_from_memory(const void *memory,
                                        size_t memory_size) {
  vectis_source source;

  vectis_source_init(&source);
  source.memory = memory;
  source.memory_size = memory_size;
  return source;
}

vectis_source vectis_source_from_lc(struct lc_source *lc_bundle_source) {
  vectis_source source;

  vectis_source_init(&source);
  source.source = lc_bundle_source;
  return source;
}

static const char *vectis_source_path_or_old(const vectis_source *source,
                                             const char *old_path) {
  if (source != NULL && source->path != NULL) {
    return source->path;
  }
  return old_path;
}

static struct lc_source *vectis_source_lc_or_old(const vectis_source *source,
                                                 struct lc_source *old_source) {
  if (source != NULL && source->source != NULL) {
    return source->source;
  }
  return old_source;
}

static const void *vectis_source_memory_or_old(const vectis_source *source,
                                               const void *old_memory,
                                               size_t *out_size,
                                               size_t old_size) {
  if (source != NULL && source->memory != NULL && source->memory_size > 0u) {
    if (out_size != NULL) {
      *out_size = source->memory_size;
    }
    return source->memory;
  }
  if (out_size != NULL) {
    *out_size = old_size;
  }
  return old_memory;
}

static size_t vectis_min_size(size_t a, size_t b) { return a < b ? a : b; }

void vectis_tls_config_init(vectis_tls_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->mode = VECTIS_TLS_MODE_MANUAL;
  config->bind = "0.0.0.0";
  config->port = 8443u;
  config->domain = "*";
  config->acme_directory_url = VECTIS_ACME_DIRECTORY_LETSENCRYPT_PRODUCTION;
}

void vectis_lockd_config_init(vectis_lockd_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->timeout_ms = 30000L;
}

void vectis_server_config_init(vectis_server_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->max_connections = VECTIS_SERVER_DEFAULT_MAX_CONNECTIONS;
  config->max_request_header_bytes =
      VECTIS_SERVER_DEFAULT_MAX_REQUEST_HEADER_BYTES;
  config->max_request_body_bytes = 0u;
  config->request_header_timeout_ms =
      VECTIS_SERVER_DEFAULT_REQUEST_HEADER_TIMEOUT_MS;
  config->request_body_idle_timeout_ms =
      VECTIS_SERVER_DEFAULT_REQUEST_BODY_IDLE_TIMEOUT_MS;
  config->response_write_idle_timeout_ms =
      VECTIS_SERVER_DEFAULT_RESPONSE_WRITE_IDLE_TIMEOUT_MS;
  config->request_body_min_rate_bytes_per_sec =
      VECTIS_SERVER_DEFAULT_REQUEST_BODY_MIN_RATE_BYTES_PER_SEC;
  config->request_body_min_rate_grace_ms =
      VECTIS_SERVER_DEFAULT_REQUEST_BODY_MIN_RATE_GRACE_MS;
  config->idle_timeout_ms = VECTIS_SERVER_DEFAULT_IDLE_TIMEOUT_MS;
  config->keepalive_timeout_ms = VECTIS_SERVER_DEFAULT_KEEPALIVE_TIMEOUT_MS;
  config->keepalive_max_requests = VECTIS_SERVER_DEFAULT_KEEPALIVE_MAX_REQUESTS;
  vectis_autoblock_config_init(&config->autoblock);
}

void vectis_autoblock_config_init(vectis_autoblock_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->window_seconds = 1800u;
  config->block_seconds = 3600u;
  config->max_entries = 8192u;
}

void vectis_app_config_init(vectis_app_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->app_name = "vectis";
  config->log_mode = PSLOG_MODE_JSON;
  config->min_log_level = PSLOG_LEVEL_INFO;
  vectis_server_config_init(&config->server);
  vectis_tls_config_init(&config->tls);
  vectis_lockd_config_init(&config->lockd);
}

void vectis_body_policy_init(vectis_body_policy *policy) {
  if (policy == NULL) {
    return;
  }
  memset(policy, 0, sizeof(*policy));
  policy->mode = VECTIS_BODY_NONE;
}

vectis_body_policy vectis_body_none(void) {
  vectis_body_policy policy;

  vectis_body_policy_init(&policy);
  return policy;
}

vectis_body_policy vectis_body_json_default(void) {
  vectis_body_policy policy;

  vectis_body_policy_init(&policy);
  policy.mode = VECTIS_BODY_JSON;
  policy.max_bytes = VECTIS_SERVER_DEFAULT_MAX_REQUEST_BODY_BYTES;
  policy.memory_buffer_limit_bytes =
      VECTIS_BODY_DEFAULT_MEMORY_BUFFER_LIMIT_BYTES;
  policy.disk_spool_disabled = 0;
  return policy;
}

vectis_body_policy vectis_body_buffered_max(size_t max_bytes) {
  vectis_body_policy policy;

  policy = vectis_body_json_default();
  policy.mode = VECTIS_BODY_BUFFERED;
  policy.max_bytes = max_bytes;
  policy.memory_buffer_limit_bytes =
      max_bytes == 0u
          ? VECTIS_BODY_DEFAULT_MEMORY_BUFFER_LIMIT_BYTES
          : vectis_min_size(max_bytes,
                            VECTIS_BODY_DEFAULT_MEMORY_BUFFER_LIMIT_BYTES);
  return policy;
}

vectis_body_policy vectis_body_upload_max(size_t max_bytes) {
  vectis_body_policy policy;

  vectis_body_policy_init(&policy);
  policy.mode = VECTIS_BODY_STREAMING_UPLOAD;
  policy.max_bytes = max_bytes;
  policy.memory_buffer_limit_bytes =
      VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
  return policy;
}

vectis_body_policy vectis_body_upload(void) {
  return vectis_body_upload_max(VECTIS_BODY_DEFAULT_UPLOAD_MAX_BYTES);
}

static size_t vectis_default_size(size_t value, size_t default_value) {
  return value == 0u ? default_value : value;
}

static long vectis_default_long(long value, long default_value) {
  return value == 0L ? default_value : value;
}

static unsigned vectis_default_unsigned(unsigned value,
                                        unsigned default_value) {
  return value == 0u ? default_value : value;
}

static unsigned short vectis_default_ushort(unsigned short value,
                                            unsigned short default_value) {
  return value == 0u ? default_value : value;
}

static vectis_server_config
vectis_effective_server_config(const vectis_server_config *config) {
  vectis_server_config effective;

  vectis_server_config_init(&effective);
  if (config == NULL) {
    return effective;
  }
  effective.max_connections = vectis_default_size(
      config->max_connections, VECTIS_SERVER_DEFAULT_MAX_CONNECTIONS);
  effective.max_request_header_bytes =
      vectis_default_size(config->max_request_header_bytes,
                          VECTIS_SERVER_DEFAULT_MAX_REQUEST_HEADER_BYTES);
  effective.max_request_body_bytes =
      vectis_default_size(config->max_request_body_bytes,
                          VECTIS_SERVER_DEFAULT_MAX_REQUEST_BODY_BYTES);
  effective.request_header_timeout_ms =
      vectis_default_long(config->request_header_timeout_ms,
                          VECTIS_SERVER_DEFAULT_REQUEST_HEADER_TIMEOUT_MS);
  effective.request_body_idle_timeout_ms =
      vectis_default_long(config->request_body_idle_timeout_ms,
                          VECTIS_SERVER_DEFAULT_REQUEST_BODY_IDLE_TIMEOUT_MS);
  effective.response_write_idle_timeout_ms =
      vectis_default_long(config->response_write_idle_timeout_ms,
                          VECTIS_SERVER_DEFAULT_RESPONSE_WRITE_IDLE_TIMEOUT_MS);
  effective.request_body_min_rate_bytes_per_sec = vectis_default_size(
      config->request_body_min_rate_bytes_per_sec,
      VECTIS_SERVER_DEFAULT_REQUEST_BODY_MIN_RATE_BYTES_PER_SEC);
  effective.request_body_min_rate_grace_ms =
      vectis_default_long(config->request_body_min_rate_grace_ms,
                          VECTIS_SERVER_DEFAULT_REQUEST_BODY_MIN_RATE_GRACE_MS);
  effective.idle_timeout_ms = vectis_default_long(
      config->idle_timeout_ms, VECTIS_SERVER_DEFAULT_IDLE_TIMEOUT_MS);
  effective.keepalive_disabled = config->keepalive_disabled;
  effective.keepalive_timeout_ms = vectis_default_long(
      config->keepalive_timeout_ms, VECTIS_SERVER_DEFAULT_KEEPALIVE_TIMEOUT_MS);
  effective.keepalive_max_requests =
      vectis_default_unsigned(config->keepalive_max_requests,
                              VECTIS_SERVER_DEFAULT_KEEPALIVE_MAX_REQUESTS);
  effective.autoblock = config->autoblock;
  effective.autoblock.window_seconds =
      vectis_default_unsigned(config->autoblock.window_seconds, 1800u);
  effective.autoblock.block_seconds =
      vectis_default_unsigned(config->autoblock.block_seconds, 3600u);
  effective.autoblock.max_entries =
      vectis_default_unsigned(config->autoblock.max_entries, 8192u);
  return effective;
}

static vectis_body_policy
vectis_effective_body_policy(const vectis_body_policy *policy,
                             const vectis_server_config *server) {
  vectis_body_policy effective;
  size_t server_max_body;

  vectis_body_policy_init(&effective);
  if (policy == NULL) {
    return effective;
  }
  effective = *policy;
  if (effective.mode == VECTIS_BODY_NONE) {
    return effective;
  }

  server_max_body = server != NULL && server->max_request_body_bytes > 0u
                        ? server->max_request_body_bytes
                        : VECTIS_SERVER_DEFAULT_MAX_REQUEST_BODY_BYTES;

  if (effective.mode == VECTIS_BODY_STREAMING_UPLOAD) {
    effective.max_bytes = vectis_default_size(
        effective.max_bytes, VECTIS_BODY_DEFAULT_UPLOAD_MAX_BYTES);
    effective.memory_buffer_limit_bytes =
        vectis_default_size(effective.memory_buffer_limit_bytes,
                            VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES);
  } else {
    effective.max_bytes =
        vectis_default_size(effective.max_bytes, server_max_body);
    effective.memory_buffer_limit_bytes = vectis_default_size(
        effective.memory_buffer_limit_bytes,
        vectis_min_size(effective.max_bytes,
                        VECTIS_BODY_DEFAULT_MEMORY_BUFFER_LIMIT_BYTES));
  }
  return effective;
}

void vectis_route_config_init(vectis_route_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->method = VECTIS_HTTP_ANY;
  config->methods = VECTIS_HTTP_METHODS_NONE;
  config->path_kind = VECTIS_ROUTE_PATH_LITERAL;
  config->body = vectis_body_none();
}

void vectis_upload_route_config_init(vectis_upload_route_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->method = VECTIS_HTTP_ANY;
  config->methods = VECTIS_HTTP_METHODS_NONE;
  config->path_kind = VECTIS_ROUTE_PATH_LITERAL;
  config->body = vectis_body_upload();
}

void vectis_upload_file_route_config_init(
    vectis_upload_file_route_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->method = VECTIS_HTTP_ANY;
  config->methods = VECTIS_HTTP_METHODS_NONE;
  config->path_kind = VECTIS_ROUTE_PATH_LITERAL;
  config->body = vectis_body_upload();
  config->content_type = "application/octet-stream";
}

void vectis_upload_reader_route_config_init(
    vectis_upload_reader_route_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->method = VECTIS_HTTP_ANY;
  config->methods = VECTIS_HTTP_METHODS_NONE;
  config->path_kind = VECTIS_ROUTE_PATH_LITERAL;
  config->body = vectis_body_upload();
  config->buffer_bytes = VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
}

void vectis_json_route_config_init(vectis_json_route_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->method = VECTIS_HTTP_ANY;
  config->methods = VECTIS_HTTP_METHODS_NONE;
  config->path_kind = VECTIS_ROUTE_PATH_LITERAL;
  config->body = vectis_body_json_default();
}

void vectis_json_typed_route_config_init(
    vectis_json_typed_route_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->method = VECTIS_HTTP_ANY;
  config->methods = VECTIS_HTTP_METHODS_NONE;
  config->path_kind = VECTIS_ROUTE_PATH_LITERAL;
  config->body = vectis_body_json_default();
}

void vectis_xml_route_config_init(vectis_xml_route_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->method = VECTIS_HTTP_ANY;
  config->methods = VECTIS_HTTP_METHODS_NONE;
  config->path_kind = VECTIS_ROUTE_PATH_LITERAL;
  config->body = vectis_body_upload();
  config->buffer_bytes = VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
  vectis_xml_config_init(&config->config);
}

void vectis_dsv_route_config_init(vectis_dsv_route_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->method = VECTIS_HTTP_ANY;
  config->methods = VECTIS_HTTP_METHODS_NONE;
  config->path_kind = VECTIS_ROUTE_PATH_LITERAL;
  config->body = vectis_body_upload();
  config->buffer_bytes = VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
  vectis_dsv_config_init(&config->config);
}

void vectis_openapi_document_init(vectis_openapi_document *document) {
  if (document == NULL) {
    return;
  }
  document->title = "Vectis API";
  document->version = "0.0.0";
}

void vectis_openapi_route_doc_init(vectis_openapi_route_doc *doc) {
  if (doc == NULL) {
    return;
  }
  memset(doc, 0, sizeof(*doc));
}

void vectis_openapi_route_doc_cleanup(vectis_openapi_route_doc *doc) {
  if (doc == NULL) {
    return;
  }
  free(doc->responses);
  doc->responses = NULL;
  doc->response_count = 0u;
  doc->response_capacity = 0u;
}

vectis_openapi_schema vectis_openapi_lonejson_schema(const char *name,
                                                     const lonejson_map *map) {
  vectis_openapi_schema schema;

  schema.name = name;
  schema.map = map;
  return schema;
}

vectis_status vectis_openapi_request_json(vectis_openapi_route_doc *doc,
                                          vectis_openapi_schema schema,
                                          vectis_error *error) {
  if (doc == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OpenAPI route doc is required");
    return VECTIS_ERR_INVALID;
  }
  if (schema.map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OpenAPI request schema map is required");
    return VECTIS_ERR_INVALID;
  }
  doc->request_schema = schema;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_openapi_response_json(vectis_openapi_route_doc *doc,
                                           int status_code,
                                           const char *description,
                                           vectis_openapi_schema schema,
                                           vectis_error *error) {
  vectis_openapi_response *grown;
  size_t next_capacity;

  if (doc == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OpenAPI route doc is required");
    return VECTIS_ERR_INVALID;
  }
  if (status_code < 100 || status_code > 599) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OpenAPI response status is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (schema.map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OpenAPI response schema map is required");
    return VECTIS_ERR_INVALID;
  }
  if (doc->response_count == doc->response_capacity) {
    next_capacity =
        doc->response_capacity == 0u ? 4u : doc->response_capacity * 2u;
    grown = (vectis_openapi_response *)realloc(doc->responses,
                                               next_capacity * sizeof(*grown));
    if (grown == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to grow OpenAPI responses");
      return VECTIS_ERR_NOMEM;
    }
    doc->responses = grown;
    doc->response_capacity = next_capacity;
  }
  doc->responses[doc->response_count].status_code = status_code;
  doc->responses[doc->response_count].description = description;
  doc->responses[doc->response_count].schema = schema;
  doc->response_count++;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_route_path_kind vectis_infer_route_path_kind(const char *path) {
  if (path != NULL && path[0] == '^') {
    return VECTIS_ROUTE_PATH_REGEX;
  }
  if (path != NULL && strchr(path, ':') != NULL) {
    return VECTIS_ROUTE_PATH_PARAMS;
  }
  return VECTIS_ROUTE_PATH_LITERAL;
}

static vectis_http_methods vectis_method_mask(vectis_http_method method) {
  if (method == VECTIS_HTTP_ANY) {
    return VECTIS_HTTP_METHODS_ALL;
  }
  if (method < VECTIS_HTTP_GET || method > VECTIS_HTTP_MOVE) {
    return VECTIS_HTTP_METHODS_NONE;
  }
  return VECTIS_HTTP_METHOD_MASK(method);
}

static vectis_http_methods
vectis_normalize_methods(vectis_http_method method,
                         vectis_http_methods methods) {
  if (methods != VECTIS_HTTP_METHODS_NONE) {
    return methods;
  }
  return vectis_method_mask(method);
}

static vectis_http_method vectis_first_method(vectis_http_methods methods) {
  if (methods & VECTIS_HTTP_METHODS_GET) {
    return VECTIS_HTTP_GET;
  }
  if (methods & VECTIS_HTTP_METHODS_POST) {
    return VECTIS_HTTP_POST;
  }
  if (methods & VECTIS_HTTP_METHODS_PUT) {
    return VECTIS_HTTP_PUT;
  }
  if (methods & VECTIS_HTTP_METHODS_PATCH) {
    return VECTIS_HTTP_PATCH;
  }
  if (methods & VECTIS_HTTP_METHODS_DELETE) {
    return VECTIS_HTTP_DELETE;
  }
  if (methods & VECTIS_HTTP_METHODS_HEAD) {
    return VECTIS_HTTP_HEAD;
  }
  if (methods & VECTIS_HTTP_METHODS_OPTIONS) {
    return VECTIS_HTTP_OPTIONS;
  }
  if (methods & VECTIS_HTTP_METHODS_PROPFIND) {
    return VECTIS_HTTP_PROPFIND;
  }
  if (methods & VECTIS_HTTP_METHODS_MKCOL) {
    return VECTIS_HTTP_MKCOL;
  }
  if (methods & VECTIS_HTTP_METHODS_COPY) {
    return VECTIS_HTTP_COPY;
  }
  if (methods & VECTIS_HTTP_METHODS_MOVE) {
    return VECTIS_HTTP_MOVE;
  }
  return VECTIS_HTTP_ANY;
}

vectis_route_config vectis_route(vectis_http_method method, const char *path,
                                 vectis_route_handler_fn handler,
                                 void *userdata) {
  vectis_route_config route;

  vectis_route_config_init(&route);
  route.method = method;
  route.methods = vectis_method_mask(method);
  route.path = path;
  route.path_kind = vectis_infer_route_path_kind(path);
  route.handler = handler;
  route.userdata = userdata;
  return route;
}

vectis_route_config vectis_route_methods(vectis_http_methods methods,
                                         const char *path,
                                         vectis_route_handler_fn handler,
                                         void *userdata) {
  vectis_route_config route;

  vectis_route_config_init(&route);
  route.method = methods == VECTIS_HTTP_METHODS_NONE
                     ? (vectis_http_method)-1
                     : vectis_first_method(methods);
  route.methods = methods;
  route.path = path;
  route.path_kind = vectis_infer_route_path_kind(path);
  route.handler = handler;
  route.userdata = userdata;
  return route;
}

vectis_route_config vectis_json_body_route(vectis_http_method method,
                                           const char *path,
                                           vectis_route_handler_fn handler,
                                           void *userdata) {
  vectis_route_config route;

  route = vectis_route(method, path, handler, userdata);
  route.body = vectis_body_json_default();
  return route;
}

vectis_route_config
vectis_json_body_route_methods(vectis_http_methods methods, const char *path,
                               vectis_route_handler_fn handler,
                               void *userdata) {
  vectis_route_config route;

  route = vectis_route_methods(methods, path, handler, userdata);
  route.body = vectis_body_json_default();
  return route;
}

vectis_route_config vectis_route_regex(vectis_http_method method,
                                       const char *pattern,
                                       vectis_route_handler_fn handler,
                                       void *userdata) {
  vectis_route_config route;

  vectis_route_config_init(&route);
  route.method = method;
  route.methods = vectis_method_mask(method);
  route.path = pattern;
  route.path_kind = VECTIS_ROUTE_PATH_REGEX;
  route.handler = handler;
  route.userdata = userdata;
  return route;
}

vectis_route_config vectis_route_regex_methods(vectis_http_methods methods,
                                               const char *pattern,
                                               vectis_route_handler_fn handler,
                                               void *userdata) {
  vectis_route_config route;

  route = vectis_route_methods(methods, pattern, handler, userdata);
  route.path_kind = VECTIS_ROUTE_PATH_REGEX;
  return route;
}

vectis_route_config vectis_upload_route(vectis_http_method method,
                                        const char *path,
                                        vectis_route_handler_fn handler,
                                        void *userdata) {
  vectis_route_config route;

  route = vectis_route(method, path, handler, userdata);
  route.body = vectis_body_upload();
  return route;
}

vectis_route_config vectis_upload_route_methods(vectis_http_methods methods,
                                                const char *path,
                                                vectis_route_handler_fn handler,
                                                void *userdata) {
  vectis_route_config route;

  route = vectis_route_methods(methods, path, handler, userdata);
  route.body = vectis_body_upload();
  return route;
}

vectis_route_config vectis_upload_route_max(vectis_http_method method,
                                            const char *path, size_t max_bytes,
                                            vectis_route_handler_fn handler,
                                            void *userdata) {
  vectis_route_config route;

  route = vectis_route(method, path, handler, userdata);
  route.body = vectis_body_upload_max(max_bytes);
  return route;
}

vectis_route_config vectis_upload_route_max_methods(
    vectis_http_methods methods, const char *path, size_t max_bytes,
    vectis_route_handler_fn handler, void *userdata) {
  vectis_route_config route;

  route = vectis_route_methods(methods, path, handler, userdata);
  route.body = vectis_body_upload_max(max_bytes);
  return route;
}

vectis_upload_route_config vectis_stream_upload_route(
    vectis_http_method method, const char *path, vectis_upload_open_fn open,
    vectis_upload_write_fn write, vectis_upload_finish_fn finish,
    vectis_upload_close_fn close, void *userdata) {
  vectis_upload_route_config route;

  vectis_upload_route_config_init(&route);
  route.method = method;
  route.methods = vectis_method_mask(method);
  route.path = path;
  route.path_kind = vectis_infer_route_path_kind(path);
  route.open = open;
  route.write = write;
  route.finish = finish;
  route.close = close;
  route.userdata = userdata;
  return route;
}

vectis_upload_route_config vectis_stream_upload_route_methods(
    vectis_http_methods methods, const char *path, vectis_upload_open_fn open,
    vectis_upload_write_fn write, vectis_upload_finish_fn finish,
    vectis_upload_close_fn close, void *userdata) {
  vectis_upload_route_config route;

  route = vectis_stream_upload_route(
      methods == VECTIS_HTTP_METHODS_NONE ? (vectis_http_method)-1
                                          : vectis_first_method(methods),
      path, open, write, finish, close, userdata);
  route.methods = methods;
  return route;
}

vectis_upload_file_route_config
vectis_upload_file_route(vectis_http_method method, const char *path,
                         const char *file_path, const char *content_type) {
  vectis_upload_file_route_config route;

  vectis_upload_file_route_config_init(&route);
  route.method = method;
  route.methods = vectis_method_mask(method);
  route.path = path;
  route.path_kind = vectis_infer_route_path_kind(path);
  route.file_path = file_path;
  route.content_type =
      content_type != NULL ? content_type : "application/octet-stream";
  return route;
}

vectis_upload_reader_route_config
vectis_upload_reader_route(vectis_http_method method, const char *path,
                           vectis_upload_reader_handler_fn handler,
                           void *userdata) {
  vectis_upload_reader_route_config route;

  vectis_upload_reader_route_config_init(&route);
  route.method = method;
  route.methods = vectis_method_mask(method);
  route.path = path;
  route.path_kind = vectis_infer_route_path_kind(path);
  route.handler = handler;
  route.userdata = userdata;
  return route;
}

vectis_upload_reader_route_config vectis_upload_reader_route_methods(
    vectis_http_methods methods, const char *path,
    vectis_upload_reader_handler_fn handler, void *userdata) {
  vectis_upload_reader_route_config route;

  route = vectis_upload_reader_route(methods == VECTIS_HTTP_METHODS_NONE
                                         ? (vectis_http_method)-1
                                         : vectis_first_method(methods),
                                     path, handler, userdata);
  route.methods = methods;
  return route;
}

vectis_json_route_config
vectis_json_route(vectis_http_method method, const char *path,
                  const lonejson_map *input_map, size_t input_size,
                  const lonejson_map *output_map, size_t output_size,
                  vectis_json_route_handler_fn handler, void *userdata) {
  vectis_json_route_config route;

  vectis_json_route_config_init(&route);
  route.method = method;
  route.methods = vectis_method_mask(method);
  route.path = path;
  route.path_kind = vectis_infer_route_path_kind(path);
  route.body = vectis_body_json_default();
  route.input_map = input_map;
  route.input_size = input_size;
  route.output_map = output_map;
  route.output_size = output_size;
  route.handler = handler;
  route.userdata = userdata;
  return route;
}

vectis_json_route_config
vectis_json_route_methods(vectis_http_methods methods, const char *path,
                          const lonejson_map *input_map, size_t input_size,
                          const lonejson_map *output_map, size_t output_size,
                          vectis_json_route_handler_fn handler,
                          void *userdata) {
  vectis_json_route_config route;

  route = vectis_json_route(
      methods == VECTIS_HTTP_METHODS_NONE ? (vectis_http_method)-1
                                          : vectis_first_method(methods),
      path, input_map, input_size, output_map, output_size, handler, userdata);
  route.methods = methods;
  return route;
}

vectis_json_typed_route_config
vectis_json_typed_route(vectis_http_method method, const char *path,
                        const lonejson_map *input_map, size_t input_size,
                        vectis_json_typed_route_handler_fn handler,
                        void *userdata) {
  vectis_json_typed_route_config route;

  vectis_json_typed_route_config_init(&route);
  route.method = method;
  route.methods = vectis_method_mask(method);
  route.path = path;
  route.path_kind = vectis_infer_route_path_kind(path);
  route.body = vectis_body_json_default();
  route.input_map = input_map;
  route.input_size = input_size;
  route.handler = handler;
  route.userdata = userdata;
  return route;
}

vectis_json_typed_route_config vectis_json_typed_route_methods(
    vectis_http_methods methods, const char *path,
    const lonejson_map *input_map, size_t input_size,
    vectis_json_typed_route_handler_fn handler, void *userdata) {
  vectis_json_typed_route_config route;

  route = vectis_json_typed_route(
      methods == VECTIS_HTTP_METHODS_NONE ? (vectis_http_method)-1
                                          : vectis_first_method(methods),
      path, input_map, input_size, handler, userdata);
  route.methods = methods;
  return route;
}

vectis_xml_route_config
vectis_xml_route(vectis_http_method method, const char *path,
                 const lonejson_map *input_map, size_t input_size,
                 const vectis_xml_config *config,
                 vectis_xml_route_handler_fn handler, void *userdata) {
  vectis_xml_route_config route;

  vectis_xml_route_config_init(&route);
  route.method = method;
  route.methods = vectis_method_mask(method);
  route.path = path;
  route.path_kind = vectis_infer_route_path_kind(path);
  route.input_map = input_map;
  route.input_size = input_size;
  if (config != NULL) {
    route.config = *config;
  }
  route.handler = handler;
  route.userdata = userdata;
  return route;
}

vectis_xml_route_config
vectis_xml_route_methods(vectis_http_methods methods, const char *path,
                         const lonejson_map *input_map, size_t input_size,
                         const vectis_xml_config *config,
                         vectis_xml_route_handler_fn handler, void *userdata) {
  vectis_xml_route_config route;

  route = vectis_xml_route(
      methods == VECTIS_HTTP_METHODS_NONE ? (vectis_http_method)-1
                                          : vectis_first_method(methods),
      path, input_map, input_size, config, handler, userdata);
  route.methods = methods;
  return route;
}

vectis_dsv_route_config
vectis_dsv_route(vectis_http_method method, const char *path,
                 const lonejson_map *row_map, size_t row_size,
                 const vectis_dsv_config *config,
                 vectis_dsv_route_handler_fn handler, void *userdata) {
  vectis_dsv_route_config route;

  vectis_dsv_route_config_init(&route);
  route.method = method;
  route.methods = vectis_method_mask(method);
  route.path = path;
  route.path_kind = vectis_infer_route_path_kind(path);
  route.row_map = row_map;
  route.row_size = row_size;
  if (config != NULL) {
    route.config = *config;
  }
  route.handler = handler;
  route.userdata = userdata;
  return route;
}

vectis_dsv_route_config
vectis_dsv_route_methods(vectis_http_methods methods, const char *path,
                         const lonejson_map *row_map, size_t row_size,
                         const vectis_dsv_config *config,
                         vectis_dsv_route_handler_fn handler, void *userdata) {
  vectis_dsv_route_config route;

  route = vectis_dsv_route(methods == VECTIS_HTTP_METHODS_NONE
                               ? (vectis_http_method)-1
                               : vectis_first_method(methods),
                           path, row_map, row_size, config, handler, userdata);
  route.methods = methods;
  return route;
}

void vectis_static_file_config_init(vectis_static_file_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->content_type = "application/octet-stream";
  config->methods = VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD;
}

void vectis_static_directory_config_init(
    vectis_static_directory_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->content_type = "application/octet-stream";
  config->index_file = "index.html";
  config->methods = VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD;
}

void vectis_static_embedded_config_init(vectis_static_embedded_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->content_type = "application/octet-stream";
  config->cache_control = "no-cache";
  config->not_found_body = "not found\n";
  config->not_found_content_type = "text/plain; charset=utf-8";
  config->methods = VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD;
}

static char *vectis_strdup(const char *value) {
  size_t len;
  char *copy;

  if (value == NULL) {
    return NULL;
  }
  len = strlen(value) + 1u;
  copy = (char *)malloc(len);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value, len);
  return copy;
}

static char *vectis_strndup(const char *value, size_t len) {
  char *copy;

  if (value == NULL) {
    return NULL;
  }
  copy = (char *)malloc(len + 1u);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value, len);
  copy[len] = '\0';
  return copy;
}

static void
vectis_consumer_receiver_runtime_cleanup(vectis_consumer_receiver_runtime *rt) {
  vectis_consumer_receiver_runtime *next;

  while (rt != NULL) {
    next = rt->next;
    if (rt->receiver.cleanup != NULL) {
      rt->receiver.cleanup(rt->receiver.context);
    }
    free(rt);
    rt = next;
  }
}

static void
vectis_consumer_receiver_entries_cleanup(vectis_consumer_receiver_entry *entry) {
  vectis_consumer_receiver_entry *next;

  while (entry != NULL) {
    next = entry->next;
    free(entry->kind);
    free(entry);
    entry = next;
  }
}

static vectis_consumer_receiver_entry *
vectis_find_consumer_receiver(vectis_app_impl *impl, const char *kind) {
  vectis_consumer_receiver_entry *entry;

  if (impl == NULL || kind == NULL || kind[0] == '\0') {
    return NULL;
  }
  entry = impl->consumer_receivers;
  while (entry != NULL) {
    if (strcmp(entry->kind, kind) == 0) {
      return entry;
    }
    entry = entry->next;
  }
  return NULL;
}

static int
vectis_webdav_marker_receiver_put(vectis_webdav_marker_receiver *receiver,
                                  const char *path, const char *body) {
  vectis_webdav_status status;

  if (receiver == NULL || path == NULL || body == NULL) {
    return LC_ERR_PROTOCOL;
  }
  status = vectis_webdav_put(&receiver->storage, path,
                             (const unsigned char *)body, strlen(body));
  return status == VECTIS_WEBDAV_OK ? LC_OK : LC_ERR_PROTOCOL;
}

static int vectis_webdav_marker_receiver_handle(void *context,
                                                lc_consumer_message *message,
                                                lc_error *error) {
  vectis_webdav_marker_receiver *receiver;
  int rc;

  receiver = (vectis_webdav_marker_receiver *)context;
  rc = vectis_webdav_marker_receiver_put(receiver, receiver->processing_path,
                                         receiver->processing_body);
  if (rc == LC_OK && receiver->processing_delay_seconds > 0L) {
    (void)sleep((unsigned int)receiver->processing_delay_seconds);
  }
  if (rc == LC_OK && message != NULL && message->message != NULL) {
    rc = message->message->ack(message->message, error);
  }
  if (rc == LC_OK) {
    rc = vectis_webdav_marker_receiver_put(receiver, receiver->done_path,
                                           receiver->done_body);
  }
  return rc;
}

static void vectis_webdav_marker_receiver_cleanup(void *context) {
  vectis_webdav_marker_receiver *receiver;

  receiver = (vectis_webdav_marker_receiver *)context;
  if (receiver == NULL) {
    return;
  }
  free(receiver->cache_dir);
  free(receiver->site_id);
  free(receiver->processing_path);
  free(receiver->done_path);
  free(receiver->processing_body);
  free(receiver->done_body);
  free(receiver);
}

static vectis_status vectis_webdav_marker_receiver_create(
    void *adapter_context, const void *receiver_config,
    vectis_consumer_receiver *out, vectis_error *error) {
  const vectis_webdav_marker_receiver_config *config;
  vectis_webdav_marker_receiver *receiver;

  (void)adapter_context;
  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "receiver output is required");
    return VECTIS_ERR_INVALID;
  }
  memset(out, 0, sizeof(*out));
  if (receiver_config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "webdav_marker receiver config is required");
    return VECTIS_ERR_INVALID;
  }
  config = (const vectis_webdav_marker_receiver_config *)receiver_config;
  if (config->cache_dir == NULL || config->cache_dir[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "webdav_marker receiver cache_dir is required");
    return VECTIS_ERR_INVALID;
  }
  receiver = (vectis_webdav_marker_receiver *)calloc(1u, sizeof(*receiver));
  if (receiver == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate webdav_marker receiver");
    return VECTIS_ERR_NOMEM;
  }
  receiver->cache_dir = vectis_strdup(config->cache_dir);
  receiver->site_id = vectis_strdup(config->site_id != NULL ? config->site_id
                                                            : "consumer");
  receiver->processing_path =
      vectis_strdup(config->processing_path != NULL ? config->processing_path
                                                    : "/consumer-processing.txt");
  receiver->done_path = vectis_strdup(config->done_path != NULL
                                          ? config->done_path
                                          : "/consumer-done.txt");
  receiver->processing_body =
      vectis_strdup(config->processing_body != NULL ? config->processing_body
                                                    : "processing\n");
  receiver->done_body =
      vectis_strdup(config->done_body != NULL ? config->done_body : "handled\n");
  if (receiver->cache_dir == NULL || receiver->site_id == NULL ||
      receiver->processing_path == NULL || receiver->done_path == NULL ||
      receiver->processing_body == NULL || receiver->done_body == NULL) {
    vectis_webdav_marker_receiver_cleanup(receiver);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy webdav_marker receiver config");
    return VECTIS_ERR_NOMEM;
  }
  vectis_webdav_config_init(&receiver->storage);
  receiver->storage.cache_dir = receiver->cache_dir;
  receiver->storage.site_id = receiver->site_id;
  receiver->storage.max_file_bytes = config->max_file_bytes;
  receiver->storage.max_total_bytes = config->max_total_bytes;
  receiver->storage.max_resources = config->max_resources;
  receiver->processing_delay_seconds = config->processing_delay_seconds;
  out->handle = vectis_webdav_marker_receiver_handle;
  out->context = receiver;
  out->cleanup = vectis_webdav_marker_receiver_cleanup;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static int vectis_consumer_receiver_bridge(void *context,
                                           lc_consumer_message *message,
                                           lc_error *error) {
  vectis_consumer_receiver_runtime *rt;

  rt = (vectis_consumer_receiver_runtime *)context;
  if (rt == NULL || rt->receiver.handle == NULL) {
    return LC_ERR_PROTOCOL;
  }
  return rt->receiver.handle(rt->receiver.context, message, error);
}

static void
vectis_http_response_headers_cleanup(vectis_http_response *response) {
  size_t i;

  if (response == NULL) {
    return;
  }
  for (i = 0u; i < response->header_count; ++i) {
    free(response->headers[i].name);
    free(response->headers[i].value);
  }
  free(response->headers);
  response->headers = NULL;
  response->header_count = 0u;
}

static int vectis_http_response_add_header(vectis_http_response *response,
                                           const char *name, size_t name_len,
                                           const char *value,
                                           size_t value_len) {
  vectis_http_header *headers;
  char *name_copy;
  char *value_copy;

  if (response == NULL || name == NULL || name_len == 0u || value == NULL) {
    return -1;
  }
  headers = (vectis_http_header *)realloc(
      response->headers, (response->header_count + 1u) * sizeof(headers[0]));
  if (headers == NULL) {
    return -1;
  }
  response->headers = headers;
  name_copy = vectis_strndup(name, name_len);
  value_copy = vectis_strndup(value, value_len);
  if (name_copy == NULL || value_copy == NULL) {
    free(name_copy);
    free(value_copy);
    return -1;
  }
  response->headers[response->header_count].name = name_copy;
  response->headers[response->header_count].value = value_copy;
  response->header_count++;
  return 0;
}

static size_t vectis_curl_header_callback(char *buffer, size_t size,
                                          size_t nitems, void *userdata) {
  vectis_curl_header_capture *capture;
  const char *line;
  const char *colon;
  const char *value;
  size_t len;
  size_t line_len;
  size_t name_len;
  size_t value_len;

  len = size * nitems;
  capture = (vectis_curl_header_capture *)userdata;
  if (capture == NULL || capture->response == NULL) {
    return len;
  }
  line = buffer;
  line_len = len;
  while (line_len > 0u &&
         (line[line_len - 1u] == '\r' || line[line_len - 1u] == '\n')) {
    line_len--;
  }
  if (line_len == 0u) {
    return len;
  }
  if (line_len >= 5u && strncmp(line, "HTTP/", 5u) == 0) {
    vectis_http_response_headers_cleanup(capture->response);
    return len;
  }
  colon = memchr(line, ':', line_len);
  if (colon == NULL || colon == line) {
    return len;
  }
  name_len = (size_t)(colon - line);
  value = colon + 1;
  value_len = line_len - name_len - 1u;
  while (value_len > 0u && (*value == ' ' || *value == '\t')) {
    value++;
    value_len--;
  }
  while (value_len > 0u &&
         (value[value_len - 1u] == ' ' || value[value_len - 1u] == '\t')) {
    value_len--;
  }
  if (vectis_http_response_add_header(capture->response, line, name_len, value,
                                      value_len) != 0) {
    capture->failed = 1;
    return 0u;
  }
  return len;
}

static void vectis_kv_free_all(vectis_kv *items, size_t count) {
  size_t i;

  if (items == NULL) {
    return;
  }
  for (i = 0u; i < count; ++i) {
    free(items[i].name);
    free(items[i].value);
  }
  free(items);
}

static const char *vectis_kv_find(const vectis_kv *items, size_t count,
                                  const char *name) {
  size_t i;

  if (items == NULL || name == NULL) {
    return NULL;
  }
  for (i = 0u; i < count; ++i) {
    if (items[i].name != NULL && strcmp(items[i].name, name) == 0) {
      return items[i].value;
    }
  }
  return NULL;
}

static int vectis_header_name_valid(const char *name) {
  const unsigned char *cursor;

  if (name == NULL || name[0] == '\0') {
    return 0;
  }
  for (cursor = (const unsigned char *)name; *cursor != '\0'; ++cursor) {
    if (*cursor <= 32u || *cursor == 127u || *cursor == ':') {
      return 0;
    }
  }
  return 1;
}

static int vectis_header_value_valid(const char *value) {
  const unsigned char *cursor;

  if (value == NULL) {
    return 0;
  }
  for (cursor = (const unsigned char *)value; *cursor != '\0'; ++cursor) {
    if (*cursor == '\r' || *cursor == '\n') {
      return 0;
    }
  }
  return 1;
}

static vectis_status vectis_kv_add(vectis_kv **items, size_t *count,
                                   size_t *capacity, const char *name,
                                   const char *value, const char *label,
                                   vectis_error *error) {
  vectis_kv *grown;
  char *name_copy;
  char *value_copy;
  size_t next_capacity;

  if (items == NULL || count == NULL || capacity == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "key/value storage is required");
    return VECTIS_ERR_INVALID;
  }
  if (name == NULL || name[0] == '\0') {
    vectis_set_errorf(error, VECTIS_ERR_INVALID, "%s name is required", label);
    return VECTIS_ERR_INVALID;
  }
  if (value == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID, "%s value is required", label);
    return VECTIS_ERR_INVALID;
  }
  if (*count == *capacity) {
    next_capacity = *capacity == 0u ? 4u : *capacity * 2u;
    grown = (vectis_kv *)realloc(*items, next_capacity * sizeof(*grown));
    if (grown == NULL) {
      vectis_set_errorf(error, VECTIS_ERR_NOMEM, "failed to grow %s storage",
                        label);
      return VECTIS_ERR_NOMEM;
    }
    *items = grown;
    *capacity = next_capacity;
  }
  name_copy = vectis_strdup(name);
  value_copy = vectis_strdup(value);
  if (name_copy == NULL || value_copy == NULL) {
    free(name_copy);
    free(value_copy);
    vectis_set_errorf(error, VECTIS_ERR_NOMEM, "failed to copy %s", label);
    return VECTIS_ERR_NOMEM;
  }
  (*items)[*count].name = name_copy;
  (*items)[*count].value = value_copy;
  (*count)++;
  return VECTIS_OK;
}

static void vectis_kv_truncate(vectis_kv **items, size_t *count, size_t keep) {
  size_t i;

  if (items == NULL || *items == NULL || count == NULL || keep >= *count) {
    return;
  }
  for (i = keep; i < *count; ++i) {
    free((*items)[i].name);
    free((*items)[i].value);
    (*items)[i].name = NULL;
    (*items)[i].value = NULL;
  }
  *count = keep;
}

static int vectis_has_url_scheme(const char *url) {
  const char *p;

  if (url == NULL) {
    return 0;
  }
  p = url;
  while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
         (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '.') {
    p++;
  }
  return p > url && p[0] == ':' && p[1] == '/' && p[2] == '/';
}

static char *vectis_join_url(const char *base_url, const char *url,
                             vectis_error *error) {
  size_t base_len;
  size_t url_len;
  int base_slash;
  int url_slash;
  size_t len;
  char *joined;
  char *out;

  if (url != NULL && vectis_has_url_scheme(url)) {
    return vectis_strdup(url);
  }
  if (base_url == NULL || base_url[0] == '\0') {
    return vectis_strdup(url);
  }
  if (url == NULL || url[0] == '\0') {
    return vectis_strdup(base_url);
  }

  base_len = strlen(base_url);
  url_len = strlen(url);
  base_slash = base_len > 0u && base_url[base_len - 1u] == '/';
  url_slash = url[0] == '/';
  len = base_len + url_len + 2u;
  joined = (char *)malloc(len);
  if (joined == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate joined URL");
    return NULL;
  }
  out = joined;
  memcpy(out, base_url, base_len);
  out += base_len;
  if (base_slash && url_slash) {
    url++;
    url_len--;
  } else if (!base_slash && !url_slash) {
    *out = '/';
    out++;
  }
  memcpy(out, url, url_len);
  out += url_len;
  *out = '\0';
  return joined;
}

static vectis_status vectis_copy_bytes(const void *bytes, size_t size,
                                       void **out, size_t *out_size,
                                       const char *label, vectis_error *error) {
  void *copy;

  if (out != NULL) {
    *out = NULL;
  }
  if (out_size != NULL) {
    *out_size = 0u;
  }
  if (bytes == NULL || size == 0u) {
    return VECTIS_OK;
  }
  copy = malloc(size);
  if (copy == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_NOMEM, "failed to copy %s", label);
    return VECTIS_ERR_NOMEM;
  }
  memcpy(copy, bytes, size);
  *out = copy;
  *out_size = size;
  return VECTIS_OK;
}

static int vectis_tls_material_present(const char *path, const void *pem,
                                       struct lc_source *source) {
  return path != NULL || pem != NULL || source != NULL;
}

static vectis_status
vectis_copy_source_bytes(const vectis_source *source, const void *old_bytes,
                         size_t old_size, void **out, size_t *out_size,
                         const char *label, vectis_error *error) {
  const void *bytes;
  size_t size;

  size = 0u;
  bytes = vectis_source_memory_or_old(source, old_bytes, &size, old_size);
  return vectis_copy_bytes(bytes, size, out, out_size, label, error);
}

static int vectis_is_param_start(int ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

static int vectis_is_param_char(int ch) {
  return vectis_is_param_start(ch) || (ch >= '0' && ch <= '9');
}

static vectis_status vectis_validate_param_path(const char *path,
                                                vectis_error *error) {
  const char *p;
  int segment_start;

  p = path + 1;
  segment_start = 1;
  while (*p != '\0') {
    if (*p == '/') {
      if (segment_start) {
        vectis_set_error(error, VECTIS_ERR_INVALID,
                         "route path must not contain empty segments");
        return VECTIS_ERR_INVALID;
      }
      segment_start = 1;
      p++;
      continue;
    }
    if (segment_start && p[0] == '.' &&
        (p[1] == '/' || p[1] == '\0' ||
         (p[1] == '.' && (p[2] == '/' || p[2] == '\0')))) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "route path must not contain dot segments");
      return VECTIS_ERR_INVALID;
    }
    if (*p == ':') {
      if (!segment_start) {
        vectis_set_error(error, VECTIS_ERR_INVALID,
                         "route path parameters must occupy a full segment");
        return VECTIS_ERR_INVALID;
      }
      p++;
      if (!vectis_is_param_start((unsigned char)*p)) {
        vectis_set_error(error, VECTIS_ERR_INVALID,
                         "route path parameter name is invalid");
        return VECTIS_ERR_INVALID;
      }
      while (vectis_is_param_char((unsigned char)*p)) {
        p++;
      }
      if (*p == '?') {
        p++;
      }
      if (*p != '/' && *p != '\0') {
        vectis_set_error(error, VECTIS_ERR_INVALID,
                         "route path parameter name is invalid");
        return VECTIS_ERR_INVALID;
      }
      segment_start = 0;
      continue;
    }
    if (*p == '?') {
      vectis_set_error(
          error, VECTIS_ERR_INVALID,
          "route path '?' is only allowed after a path parameter name");
      return VECTIS_ERR_INVALID;
    }
    if (*p == '%' || *p == '\\') {
      vectis_set_error(
          error, VECTIS_ERR_INVALID,
          "route path must not contain percent escapes or backslashes");
      return VECTIS_ERR_INVALID;
    }
    segment_start = 0;
    p++;
  }
  if (segment_start && path[1] != '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "route path must not end with an empty segment");
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static vectis_status
vectis_validate_route_path(const char *path, vectis_route_path_kind path_kind,
                           vectis_error *error) {
  regex_t compiled;
  int regex_rc;

  if (path_kind == VECTIS_ROUTE_PATH_LITERAL) {
    if (path == NULL || path[0] != '/') {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "route path must start with '/'");
      return VECTIS_ERR_INVALID;
    }
    if (strchr(path, ':') != NULL) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "route path contains ':' but path_kind is not "
                       "VECTIS_ROUTE_PATH_PARAMS");
      return VECTIS_ERR_INVALID;
    }
    return vectis_validate_param_path(path, error);
  }
  if (path_kind == VECTIS_ROUTE_PATH_PARAMS) {
    if (path == NULL || path[0] != '/') {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "route path must start with '/'");
      return VECTIS_ERR_INVALID;
    }
    return vectis_validate_param_path(path, error);
  }
  if (path_kind == VECTIS_ROUTE_PATH_REGEX) {
    if (path == NULL || path[0] == '\0') {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "regex route path is required");
      return VECTIS_ERR_INVALID;
    }
    if (path[0] != '^' || path[1] != '/') {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "regex route path must start with '^/'");
      return VECTIS_ERR_INVALID;
    }
    regex_rc = regcomp(&compiled, path, REG_EXTENDED | REG_NOSUB);
    if (regex_rc != 0) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "regex route path is invalid POSIX extended regex");
      return VECTIS_ERR_INVALID;
    }
    regfree(&compiled);
    return VECTIS_OK;
  }
  vectis_set_error(error, VECTIS_ERR_INVALID, "route path_kind is invalid");
  return VECTIS_ERR_INVALID;
}

static vectis_status vectis_validate_request_path(const char *path,
                                                  vectis_error *error) {
  if (path == NULL || path[0] != '/') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "request path must start with '/'");
    return VECTIS_ERR_INVALID;
  }
  if (strchr(path, ':') != NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "request path must not contain ':'");
    return VECTIS_ERR_INVALID;
  }
  return vectis_validate_param_path(path, error);
}

static vectis_status vectis_validate_methods(vectis_http_method method,
                                             vectis_http_methods methods,
                                             vectis_error *error) {
  vectis_http_methods normalized;

  normalized = vectis_normalize_methods(method, methods);
  if (normalized == VECTIS_HTTP_METHODS_NONE) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "route requires at least one HTTP method");
    return VECTIS_ERR_INVALID;
  }
  if ((normalized & ~VECTIS_HTTP_METHODS_ALL) != 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "route HTTP methods contain unsupported bits");
    return VECTIS_ERR_INVALID;
  }
  if (method != VECTIS_HTTP_ANY &&
      vectis_method_mask(method) == VECTIS_HTTP_METHODS_NONE) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "route HTTP method is invalid");
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static vectis_status
vectis_validate_body_policy(const vectis_body_policy *policy,
                            const vectis_server_config *server,
                            vectis_error *error) {
  vectis_body_policy effective;

  if (policy == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "body policy is required");
    return VECTIS_ERR_INVALID;
  }
  if (policy->mode == VECTIS_BODY_NONE) {
    return VECTIS_OK;
  }
  if (policy->mode != VECTIS_BODY_JSON &&
      policy->mode != VECTIS_BODY_BUFFERED &&
      policy->mode != VECTIS_BODY_STREAMING_UPLOAD) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "body policy mode is invalid");
    return VECTIS_ERR_INVALID;
  }
  effective = vectis_effective_body_policy(policy, server);
  if (effective.mode != VECTIS_BODY_STREAMING_UPLOAD) {
    if (effective.memory_buffer_limit_bytes > effective.max_bytes) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "buffered body policy memory_buffer_limit_bytes must "
                       "not exceed max_bytes");
      return VECTIS_ERR_INVALID;
    }
  } else if (effective.disk_spool_disabled &&
             effective.memory_buffer_limit_bytes < effective.max_bytes) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "streaming upload body policy cannot disable spooling "
                     "unless fully buffered");
    return VECTIS_ERR_INVALID;
  }
  if (server != NULL && server->max_request_body_bytes > 0u &&
      effective.max_bytes > server->max_request_body_bytes) {
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "body policy max_bytes exceeds server max_request_body_bytes");
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static void vectis_free_endpoints(vectis_app_impl *impl) {
  size_t i;

  if (impl->endpoints == NULL) {
    return;
  }
  for (i = 0u; i < impl->endpoint_count; ++i) {
    free(impl->endpoints[i]);
  }
  free(impl->endpoints);
  impl->endpoints = NULL;
  impl->endpoint_count = 0u;
}

static void vectis_free_domains(vectis_app_impl *impl) {
  size_t i;

  if (impl->domains == NULL) {
    return;
  }
  for (i = 0u; i < impl->domain_count; ++i) {
    free(impl->domains[i]);
  }
  free(impl->domains);
  impl->domains = NULL;
  impl->domain_count = 0u;
}

static int vectis_is_dns_hostname(const char *name) {
  size_t label_len;
  size_t total_len;
  int label_has_char;
  char c;
  char previous;

  if (name == NULL || name[0] == '\0') {
    return 0;
  }
  label_len = 0u;
  total_len = 0u;
  label_has_char = 0;
  previous = '\0';
  while (*name != '\0') {
    c = *name++;
    total_len++;
    if (total_len > 253u) {
      return 0;
    }
    if (c == '.') {
      if (!label_has_char || label_len == 0u || label_len > 63u ||
          previous == '-') {
        return 0;
      }
      label_len = 0u;
      label_has_char = 0;
      previous = c;
      continue;
    }
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-')) {
      return 0;
    }
    if (label_len == 0u && c == '-') {
      return 0;
    }
    label_len++;
    label_has_char = 1;
    if (label_len > 63u) {
      return 0;
    }
    previous = c;
  }
  return label_has_char && label_len > 0u && label_len <= 63u &&
         previous != '-';
}

static vectis_status vectis_copy_domains(vectis_app_impl *impl,
                                         const vectis_tls_config *tls,
                                         vectis_error *error) {
  size_t i;
  size_t j;

  if (tls->domain_count == 0u) {
    return VECTIS_OK;
  }
  if (tls->domains == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "tls.domains is required");
    return VECTIS_ERR_INVALID;
  }
  for (i = 0u; i < tls->domain_count; ++i) {
    if (!vectis_is_dns_hostname(tls->domains[i])) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "tls.domains entries must be DNS hostnames");
      return VECTIS_ERR_INVALID;
    }
    for (j = 0u; j < i; ++j) {
      if (strcmp(tls->domains[i], tls->domains[j]) == 0) {
        vectis_set_error(error, VECTIS_ERR_INVALID,
                         "tls.domains contains a duplicate name");
        return VECTIS_ERR_INVALID;
      }
    }
  }
  impl->domains = (char **)calloc(tls->domain_count, sizeof(*impl->domains));
  if (impl->domains == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate tls.domains");
    return VECTIS_ERR_NOMEM;
  }
  impl->domain_count = tls->domain_count;
  for (i = 0u; i < tls->domain_count; ++i) {
    impl->domains[i] = vectis_strdup(tls->domains[i]);
    if (impl->domains[i] == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy tls.domain");
      return VECTIS_ERR_NOMEM;
    }
  }
  return VECTIS_OK;
}

static vectis_status vectis_copy_endpoints(vectis_app_impl *impl,
                                           const vectis_lockd_config *lockd,
                                           vectis_error *error) {
  size_t i;

  if (lockd->endpoint_count == 0u) {
    return VECTIS_OK;
  }
  if (lockd->endpoints == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lockd endpoints are required");
    return VECTIS_ERR_INVALID;
  }
  for (i = 0u; i < lockd->endpoint_count; ++i) {
    if (lockd->endpoints[i] == NULL || lockd->endpoints[i][0] == '\0') {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "lockd endpoints must not be empty");
      return VECTIS_ERR_INVALID;
    }
  }
  impl->endpoints =
      (char **)calloc(lockd->endpoint_count, sizeof(*impl->endpoints));
  if (impl->endpoints == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate lockd endpoints");
    return VECTIS_ERR_NOMEM;
  }
  impl->endpoint_count = lockd->endpoint_count;
  for (i = 0u; i < lockd->endpoint_count; ++i) {
    impl->endpoints[i] = vectis_strdup(lockd->endpoints[i]);
    if (impl->endpoints[i] == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy lockd endpoint");
      return VECTIS_ERR_NOMEM;
    }
  }
  return VECTIS_OK;
}

static pslog_logger *vectis_make_owned_logger(const vectis_app_config *config,
                                              vectis_error *error) {
  pslog_config psconf;
  pslog_logger *root;
  pslog_logger *scoped;
  const char *service_name;

  pslog_default_config(&psconf);
  psconf.mode = config->log_mode;
  psconf.min_level = config->min_log_level;
  psconf.output = pslog_output_from_fp(stderr, 0);

  root = pslog_new(&psconf);
  if (root == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to create vectis logger");
    return NULL;
  }

  service_name = config->app_name != NULL ? config->app_name : "vectis";
  scoped = pslog_withf(root, "service=%s component=%s", service_name, "vectis");
  root->destroy(root);
  if (scoped == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to derive vectis logger fields");
    return NULL;
  }

  return scoped;
}

static void vectis_route_entry_free_userdata(vectis_route_entry *route) {
  vectis_upload_file_adapter *file_adapter;
  vectis_upload_reader_adapter *reader_adapter;

  if (route == NULL || !route->owns_userdata || route->userdata == NULL) {
    return;
  }
  if (route->kind == VECTIS_ROUTE_ENTRY_UPLOAD_STREAM &&
      route->upload_write == vectis_upload_file_write) {
    file_adapter = (vectis_upload_file_adapter *)route->userdata;
    free(file_adapter->file_path);
    free(file_adapter->content_type);
    free(file_adapter);
  } else if (route->kind == VECTIS_ROUTE_ENTRY_UPLOAD_STREAM &&
             route->upload_write == vectis_upload_reader_write) {
    reader_adapter = (vectis_upload_reader_adapter *)route->userdata;
    if (reader_adapter->free_userdata != NULL) {
      reader_adapter->free_userdata(reader_adapter->userdata);
    }
    free(reader_adapter);
  } else {
    free(route->userdata);
  }
  route->userdata = NULL;
}

static void vectis_free_routes(vectis_app_impl *impl) {
  size_t i;

  for (i = 0u; i < impl->route_count; ++i) {
    free(impl->routes[i].path);
    vectis_route_entry_free_userdata(&impl->routes[i]);
  }
  free(impl->routes);
  impl->routes = NULL;
  impl->route_count = 0u;
  impl->route_capacity = 0u;
}

static void vectis_free_const_string(const char *value) {
  union {
    const char *const_value;
    void *mutable_value;
  } ptr;

  ptr.const_value = value;
  free(ptr.mutable_value);
}

static void vectis_free_const_string_array(const char *const *value) {
  union {
    const char *const *const_value;
    void *mutable_value;
  } ptr;

  ptr.const_value = value;
  free(ptr.mutable_value);
}

static void
vectis_openapi_route_doc_deep_cleanup(vectis_openapi_route_doc *doc) {
  size_t i;

  if (doc == NULL) {
    return;
  }
  vectis_free_const_string(doc->summary);
  vectis_free_const_string(doc->operation_id);
  for (i = 0u; i < doc->tag_count; ++i) {
    vectis_free_const_string(doc->tags[i]);
  }
  vectis_free_const_string_array(doc->tags);
  vectis_free_const_string(doc->request_schema.name);
  for (i = 0u; i < doc->response_count; ++i) {
    vectis_free_const_string(doc->responses[i].description);
    vectis_free_const_string(doc->responses[i].schema.name);
  }
  free(doc->responses);
  memset(doc, 0, sizeof(*doc));
}

static void vectis_free_openapi_docs(vectis_app_impl *impl) {
  size_t i;

  if (impl == NULL) {
    return;
  }
  for (i = 0u; i < impl->openapi_doc_count; ++i) {
    free(impl->openapi_docs[i].path);
    vectis_openapi_route_doc_deep_cleanup(&impl->openapi_docs[i].doc);
  }
  free(impl->openapi_docs);
  impl->openapi_docs = NULL;
  impl->openapi_doc_count = 0u;
  impl->openapi_doc_capacity = 0u;
}

static vectis_status
vectis_openapi_route_doc_deep_copy(vectis_openapi_route_doc *dst,
                                   const vectis_openapi_route_doc *src,
                                   vectis_error *error) {
  const char **tags;
  vectis_openapi_response *responses;
  size_t i;

  if (dst == NULL || src == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OpenAPI route doc copy requires source and destination");
    return VECTIS_ERR_INVALID;
  }
  memset(dst, 0, sizeof(*dst));
  if (src->summary != NULL) {
    dst->summary = vectis_strdup(src->summary);
    if (dst->summary == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy OpenAPI summary");
      return VECTIS_ERR_NOMEM;
    }
  }
  if (src->operation_id != NULL) {
    dst->operation_id = vectis_strdup(src->operation_id);
    if (dst->operation_id == NULL) {
      vectis_openapi_route_doc_deep_cleanup(dst);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy OpenAPI operation id");
      return VECTIS_ERR_NOMEM;
    }
  }
  if (src->tag_count > 0u) {
    tags = (const char **)calloc(src->tag_count, sizeof(*tags));
    if (tags == NULL) {
      vectis_openapi_route_doc_deep_cleanup(dst);
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy OpenAPI tags");
      return VECTIS_ERR_NOMEM;
    }
    dst->tags = tags;
    dst->tag_count = src->tag_count;
    for (i = 0u; i < src->tag_count; ++i) {
      if (src->tags[i] != NULL) {
        tags[i] = vectis_strdup(src->tags[i]);
        if (tags[i] == NULL) {
          vectis_openapi_route_doc_deep_cleanup(dst);
          vectis_set_error(error, VECTIS_ERR_NOMEM,
                           "failed to copy OpenAPI tag");
          return VECTIS_ERR_NOMEM;
        }
      }
    }
  }
  dst->request_schema = src->request_schema;
  if (src->request_schema.name != NULL) {
    dst->request_schema.name = vectis_strdup(src->request_schema.name);
    if (dst->request_schema.name == NULL) {
      vectis_openapi_route_doc_deep_cleanup(dst);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy OpenAPI request schema name");
      return VECTIS_ERR_NOMEM;
    }
  }
  if (src->response_count > 0u) {
    responses = (vectis_openapi_response *)calloc(src->response_count,
                                                  sizeof(*responses));
    if (responses == NULL) {
      vectis_openapi_route_doc_deep_cleanup(dst);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy OpenAPI responses");
      return VECTIS_ERR_NOMEM;
    }
    dst->responses = responses;
    dst->response_count = src->response_count;
    dst->response_capacity = src->response_count;
    for (i = 0u; i < src->response_count; ++i) {
      responses[i] = src->responses[i];
      if (src->responses[i].description != NULL) {
        responses[i].description = vectis_strdup(src->responses[i].description);
        if (responses[i].description == NULL) {
          vectis_openapi_route_doc_deep_cleanup(dst);
          vectis_set_error(error, VECTIS_ERR_NOMEM,
                           "failed to copy OpenAPI response description");
          return VECTIS_ERR_NOMEM;
        }
      }
      if (src->responses[i].schema.name != NULL) {
        responses[i].schema.name = vectis_strdup(src->responses[i].schema.name);
        if (responses[i].schema.name == NULL) {
          vectis_openapi_route_doc_deep_cleanup(dst);
          vectis_set_error(error, VECTIS_ERR_NOMEM,
                           "failed to copy OpenAPI response schema name");
          return VECTIS_ERR_NOMEM;
        }
      }
    }
  }
  return VECTIS_OK;
}

static void vectis_destroy_impl(vectis_app_impl *impl) {
  if (impl == NULL) {
    return;
  }

  vectis_close_lockd_client_for_current_process(impl);

  vectis_free_routes(impl);
  vectis_free_openapi_docs(impl);
  vectis_free_endpoints(impl);
  vectis_free_domains(impl);
  vectis_consumer_receiver_entries_cleanup(impl->consumer_receivers);

  free(impl->app_name);
  free(impl->bind);
  free(impl->domain);
  free(impl->cert_key_bundle_path);
  free(impl->cert_key_bundle_pem);
  free(impl->certificate_path);
  free(impl->certificate_pem);
  free(impl->private_key_path);
  free(impl->private_key_pem);
  free(impl->ca_bundle_path);
  free(impl->ca_bundle_pem);
  free(impl->client_ca_bundle_path);
  free(impl->client_ca_bundle_pem);
  free(impl->acme_email);
  free(impl->acme_directory_url);
  free(impl->acme_state_dir);
  free(impl->unix_socket_path);
  free(impl->client_bundle_pem);
  free(impl->client_bundle_path);
  free(impl->default_namespace);

  if (impl->owns_logger && impl->logger != NULL) {
    impl->logger->destroy(impl->logger);
  }

  (void)pthread_mutex_destroy(&impl->mutex);
  free(impl);
}

static vectis_status vectis_validate_route(const vectis_route_config *route,
                                           vectis_error *error) {
  if (route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "route is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_validate_methods(route->method, route->methods, error) !=
      VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (vectis_validate_route_path(route->path, route->path_kind, error) !=
      VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (route->handler == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "route handler is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_validate_body_policy(&route->body, NULL, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static vectis_status
vectis_validate_upload_route(const vectis_upload_route_config *route,
                             vectis_error *error) {
  if (route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "upload route is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_validate_methods(route->method, route->methods, error) !=
      VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (vectis_validate_route_path(route->path, route->path_kind, error) !=
      VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (route->write == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "upload route write callback is required");
    return VECTIS_ERR_INVALID;
  }
  if (route->finish == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "upload route finish callback is required");
    return VECTIS_ERR_INVALID;
  }
  if (route->body.mode != VECTIS_BODY_STREAMING_UPLOAD) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "upload route requires streaming upload body policy");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_validate_body_policy(&route->body, NULL, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static vectis_status
vectis_validate_server_config(const vectis_server_config *config,
                              vectis_error *error) {
  vectis_server_config effective;

  if (config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "server config is required");
    return VECTIS_ERR_INVALID;
  }
  effective = vectis_effective_server_config(config);
  if (effective.max_request_header_bytes < 1024u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "server max_request_header_bytes must be at least 1024");
    return VECTIS_ERR_INVALID;
  }
  if (config->request_header_timeout_ms < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "server request_header_timeout_ms must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  if (config->request_body_idle_timeout_ms < 0L) {
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "server request_body_idle_timeout_ms must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  if (config->response_write_idle_timeout_ms < 0L) {
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "server response_write_idle_timeout_ms must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  if (config->request_body_min_rate_grace_ms < 0L) {
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "server request_body_min_rate_grace_ms must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  if (config->idle_timeout_ms < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "server idle_timeout_ms must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  if (!config->keepalive_disabled) {
    if (config->keepalive_timeout_ms < 0L) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "server keepalive_timeout_ms must be non-negative when "
                       "keepalive is enabled");
      return VECTIS_ERR_INVALID;
    }
  }
  return VECTIS_OK;
}

static vectis_status vectis_validate_startable(const vectis_app_impl *impl,
                                               vectis_error *error) {
  int has_cert_key_bundle;
  int has_split_certificate;
  int has_split_private_key;
  int has_client_ca;

  if (impl->tls_mode != VECTIS_TLS_MODE_DISABLED &&
      impl->tls_mode != VECTIS_TLS_MODE_MANUAL &&
      impl->tls_mode != VECTIS_TLS_MODE_ACME) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "tls.mode is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (impl->tls_mode == VECTIS_TLS_MODE_MANUAL) {
    has_cert_key_bundle = vectis_tls_material_present(
        impl->cert_key_bundle_path, impl->cert_key_bundle_pem,
        impl->cert_key_bundle_source);
    has_split_certificate = vectis_tls_material_present(
        impl->certificate_path, impl->certificate_pem,
        impl->certificate_source);
    has_split_private_key = vectis_tls_material_present(
        impl->private_key_path, impl->private_key_pem,
        impl->private_key_source);
    if (!has_cert_key_bundle &&
        (!has_split_certificate || !has_split_private_key)) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "manual TLS requires cert_key_bundle or certificate + "
                       "private_key from path, source, or memory");
      return VECTIS_ERR_INVALID;
    }
    has_client_ca = vectis_tls_material_present(impl->client_ca_bundle_path,
                                                impl->client_ca_bundle_pem,
                                                impl->client_ca_bundle_source);
    if (impl->require_client_certificate && !has_client_ca) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "client certificate verification requires "
                       "client_ca_bundle from path, source, or memory");
      return VECTIS_ERR_INVALID;
    }
  } else if (impl->tls_mode == VECTIS_TLS_MODE_ACME) {
    if (impl->acme_email == NULL || impl->acme_email[0] == '\0') {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "ACME mode requires acme_email");
      return VECTIS_ERR_INVALID;
    }
    if (impl->domain_count == 0u) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "ACME mode requires tls.domains");
      return VECTIS_ERR_INVALID;
    }
  }

  return VECTIS_OK;
}

static vectis_status
vectis_validate_lockd_startable(const vectis_app_impl *impl,
                                vectis_error *error) {
  int has_lockd_transport;

  if (impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  has_lockd_transport =
      (impl->endpoint_count > 0u) || (impl->unix_socket_path != NULL);
  if (has_lockd_transport && impl->client_bundle_path == NULL &&
      impl->client_bundle_source == NULL && impl->client_bundle_pem == NULL &&
      impl->unix_socket_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "tcp lockd transport requires client_bundle_path, "
                     "client_bundle_source, or client_bundle_pem");
    return VECTIS_ERR_INVALID;
  }

  return VECTIS_OK;
}

static int vectis_lockd_is_configured(const vectis_app_impl *impl) {
  return impl != NULL &&
         (impl->endpoint_count > 0u || impl->unix_socket_path != NULL);
}

static void
vectis_close_lockd_client_for_current_process(vectis_app_impl *impl) {
  if (impl == NULL || impl->lockd_client == NULL) {
    return;
  }
  if (impl->lockd_client_pid == 0 || impl->lockd_client_pid == getpid()) {
    lc_client_close(impl->lockd_client);
  }
  impl->lockd_client = NULL;
  impl->lockd_client_pid = 0;
}

static vectis_status vectis_open_lockd_client(vectis_app_impl *impl,
                                              vectis_error *error) {
  lc_client_config config;
  lc_source *memory_source;
  lc_error lcerr;
  int rc;

  if (!vectis_lockd_is_configured(impl)) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  if (impl->lockd_client != NULL && impl->lockd_client_pid != getpid()) {
    impl->lockd_client = NULL;
    impl->lockd_client_pid = 0;
  }
  if (impl->lockd_client != NULL) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }

  memory_source = NULL;
  lc_error_init(&lcerr);
  lc_client_config_init(&config);
  config.endpoints = (const char *const *)impl->endpoints;
  config.endpoint_count = impl->endpoint_count;
  config.unix_socket_path = impl->unix_socket_path;
  config.client_bundle_path = impl->client_bundle_path;
  config.client_bundle_source = impl->client_bundle_source;
  config.default_namespace = impl->default_namespace;
  config.timeout_ms = impl->timeout_ms;
  config.logger =
      impl->lockd_logger_disabled
          ? NULL
          : (impl->lockd_logger != NULL ? impl->lockd_logger : impl->logger);

  if (impl->client_bundle_pem != NULL && impl->client_bundle_pem_size > 0u) {
    rc = lc_source_from_memory(impl->client_bundle_pem,
                               impl->client_bundle_pem_size, &memory_source,
                               &lcerr);
    if (rc != LC_OK) {
      vectis_set_errorf(error, VECTIS_ERR_STATE,
                        "failed to create lockd client bundle source: %s",
                        lcerr.message != NULL ? lcerr.message
                                              : "unknown lockdc error");
      if (error != NULL) {
        error->source = VECTIS_ERROR_SOURCE_LOCKDC;
        error->dependency_code = (long)rc;
        error->http_status = lcerr.http_status;
        if (lcerr.detail != NULL) {
          (void)snprintf(error->detail, sizeof(error->detail), "%s",
                         lcerr.detail);
        }
      }
      lc_error_cleanup(&lcerr);
      return VECTIS_ERR_STATE;
    }
    config.client_bundle_source = memory_source;
  }

  rc = lc_client_open(&config, &impl->lockd_client, &lcerr);
  if (rc == LC_OK) {
    impl->lockd_client_pid = getpid();
  }
  if (memory_source != NULL) {
    lc_source_close(memory_source);
  }
  if (rc != LC_OK) {
    vectis_set_errorf(
        error, VECTIS_ERR_STATE, "failed to open lockd client: %s",
        lcerr.message != NULL ? lcerr.message : "unknown lockdc error");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LOCKDC;
      error->dependency_code = (long)rc;
      error->http_status = lcerr.http_status;
      if (lcerr.detail != NULL) {
        (void)snprintf(error->detail, sizeof(error->detail), "%s",
                       lcerr.detail);
      }
    }
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }

  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_app *vectis_app_new(const vectis_app_config *config,
                           vectis_error *error) {
  vectis_app_config defaults;
  const vectis_app_config *effective;
  vectis_server_config effective_server;
  vectis_app *app;
  vectis_app_impl *impl;
  vectis_status status;

  vectis_error_clear(error);
  vectis_app_config_init(&defaults);
  effective = config != NULL ? config : &defaults;
  effective_server = vectis_effective_server_config(&effective->server);
  status = vectis_validate_server_config(&effective->server, error);
  if (status != VECTIS_OK) {
    return NULL;
  }
  if (effective->lockd.timeout_ms < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "lockd timeout_ms must be non-negative");
    return NULL;
  }

  app = (vectis_app *)calloc(1u, sizeof(*app));
  impl = (vectis_app_impl *)calloc(1u, sizeof(*impl));
  if (app == NULL || impl == NULL) {
    free(app);
    free(impl);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate vectis app");
    return NULL;
  }

  if (pthread_mutex_init(&impl->mutex, NULL) != 0) {
    free(app);
    free(impl);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to initialize app mutex");
    return NULL;
  }

  impl->app_name = vectis_strdup(
      effective->app_name != NULL ? effective->app_name : "vectis");
  impl->bind = vectis_strdup(effective->tls.bind != NULL ? effective->tls.bind
                                                         : "0.0.0.0");
  impl->domain = vectis_strdup(
      effective->tls.domain != NULL ? effective->tls.domain : "*");
  impl->cert_key_bundle_path = vectis_strdup(vectis_source_path_or_old(
      &effective->tls.cert_key_bundle, effective->tls.cert_key_bundle_path));
  impl->cert_key_bundle_source = vectis_source_lc_or_old(
      &effective->tls.cert_key_bundle, effective->tls.cert_key_bundle_source);
  impl->certificate_path = vectis_strdup(vectis_source_path_or_old(
      &effective->tls.certificate, effective->tls.certificate_path));
  impl->certificate_source = vectis_source_lc_or_old(
      &effective->tls.certificate, effective->tls.certificate_source);
  impl->private_key_path = vectis_strdup(vectis_source_path_or_old(
      &effective->tls.private_key, effective->tls.private_key_path));
  impl->private_key_source = vectis_source_lc_or_old(
      &effective->tls.private_key, effective->tls.private_key_source);
  impl->ca_bundle_path = vectis_strdup(vectis_source_path_or_old(
      &effective->tls.ca_bundle, effective->tls.ca_bundle_path));
  impl->ca_bundle_source = vectis_source_lc_or_old(
      &effective->tls.ca_bundle, effective->tls.ca_bundle_source);
  impl->client_ca_bundle_path = vectis_strdup(vectis_source_path_or_old(
      &effective->tls.client_ca_bundle, effective->tls.client_ca_bundle_path));
  impl->client_ca_bundle_source = vectis_source_lc_or_old(
      &effective->tls.client_ca_bundle, effective->tls.client_ca_bundle_source);
  impl->acme_email = vectis_strdup(effective->tls.acme_email);
  impl->acme_directory_url = vectis_strdup(effective->tls.acme_directory_url);
  impl->acme_state_dir = vectis_strdup(effective->tls.acme_state_dir);
  impl->unix_socket_path = vectis_strdup(effective->lockd.unix_socket_path);
  impl->client_bundle_path = vectis_strdup(vectis_source_path_or_old(
      &effective->lockd.client_bundle, effective->lockd.client_bundle_path));
  impl->client_bundle_source = vectis_source_lc_or_old(
      &effective->lockd.client_bundle, effective->lockd.client_bundle_source);
  impl->default_namespace = vectis_strdup(effective->lockd.default_namespace);
  impl->lockd_logger = effective->lockd.logger;
  impl->lockd_logger_disabled = effective->lockd.logger_disabled;
  impl->timeout_ms = vectis_default_long(effective->lockd.timeout_ms, 30000L);
  impl->port = vectis_default_ushort(effective->tls.port, 8443u);
  impl->tls_mode = effective->tls.mode;
  impl->require_client_certificate = effective->tls.require_client_certificate;
  impl->server = effective_server;
  impl->server.max_request_body_bytes =
      effective->server.max_request_body_bytes;

  status = vectis_copy_source_bytes(
      &effective->tls.cert_key_bundle, effective->tls.cert_key_bundle_pem,
      effective->tls.cert_key_bundle_pem_size, &impl->cert_key_bundle_pem,
      &impl->cert_key_bundle_pem_size, "TLS cert/key bundle PEM", error);
  if (status != VECTIS_OK) {
    vectis_destroy_impl(impl);
    free(app);
    return NULL;
  }
  status = vectis_copy_source_bytes(
      &effective->tls.certificate, effective->tls.certificate_pem,
      effective->tls.certificate_pem_size, &impl->certificate_pem,
      &impl->certificate_pem_size, "TLS certificate PEM", error);
  if (status != VECTIS_OK) {
    vectis_destroy_impl(impl);
    free(app);
    return NULL;
  }
  status = vectis_copy_source_bytes(
      &effective->tls.private_key, effective->tls.private_key_pem,
      effective->tls.private_key_pem_size, &impl->private_key_pem,
      &impl->private_key_pem_size, "TLS private key PEM", error);
  if (status != VECTIS_OK) {
    vectis_destroy_impl(impl);
    free(app);
    return NULL;
  }
  status = vectis_copy_source_bytes(
      &effective->tls.ca_bundle, effective->tls.ca_bundle_pem,
      effective->tls.ca_bundle_pem_size, &impl->ca_bundle_pem,
      &impl->ca_bundle_pem_size, "TLS CA bundle PEM", error);
  if (status != VECTIS_OK) {
    vectis_destroy_impl(impl);
    free(app);
    return NULL;
  }
  status = vectis_copy_source_bytes(
      &effective->tls.client_ca_bundle, effective->tls.client_ca_bundle_pem,
      effective->tls.client_ca_bundle_pem_size, &impl->client_ca_bundle_pem,
      &impl->client_ca_bundle_pem_size, "TLS client CA bundle PEM", error);
  if (status != VECTIS_OK) {
    vectis_destroy_impl(impl);
    free(app);
    return NULL;
  }

  status = vectis_copy_source_bytes(
      &effective->lockd.client_bundle, effective->lockd.client_bundle_pem,
      effective->lockd.client_bundle_pem_size, &impl->client_bundle_pem,
      &impl->client_bundle_pem_size, "lockd client bundle PEM", error);
  if (status != VECTIS_OK) {
    vectis_destroy_impl(impl);
    free(app);
    return NULL;
  }

  status = vectis_copy_endpoints(impl, &effective->lockd, error);
  if (status != VECTIS_OK) {
    vectis_destroy_impl(impl);
    free(app);
    return NULL;
  }
  status = vectis_copy_domains(impl, &effective->tls, error);
  if (status != VECTIS_OK) {
    vectis_destroy_impl(impl);
    free(app);
    return NULL;
  }

  if (effective->logger != NULL) {
    impl->logger = effective->logger;
    impl->owns_logger = 0;
  } else {
    impl->logger = vectis_make_owned_logger(effective, error);
    if (impl->logger == NULL) {
      vectis_destroy_impl(impl);
      free(app);
      return NULL;
    }
    impl->owns_logger = 1;
  }

  {
    vectis_consumer_receiver_adapter adapter;

    adapter.kind = "webdav_marker";
    adapter.create = vectis_webdav_marker_receiver_create;
    adapter.context = NULL;
    status = vectis_register_consumer_receiver_impl(impl, &adapter, error);
    if (status != VECTIS_OK) {
      vectis_destroy_impl(impl);
      free(app);
      return NULL;
    }
  }

  app->start = vectis_start;
  app->stop = vectis_stop;
  app->route = vectis_register_route;
  app->json_route = vectis_register_json_route;
  app->json_typed_route = vectis_register_json_typed_route;
  app->xml_route = vectis_register_xml_route;
  app->dsv_route = vectis_register_dsv_route;
  app->upload_stream = vectis_register_upload_stream;
  app->upload_file = vectis_register_upload_file;
  app->upload_reader = vectis_register_upload_reader;
  app->prefixed_route = vectis_register_prefixed_route;
  app->prefixed_json_route = vectis_register_prefixed_json_route;
  app->prefixed_json_typed_route = vectis_register_prefixed_json_typed_route;
  app->prefixed_xml_route = vectis_register_prefixed_xml_route;
  app->prefixed_dsv_route = vectis_register_prefixed_dsv_route;
  app->static_file = vectis_register_static_file;
  app->static_directory = vectis_register_static_directory;
  app->static_embedded = vectis_register_static_embedded;
  app->webdav = vectis_register_webdav;
  app->webdav_embedded_site = vectis_register_webdav_embedded_site;
  app->auth_routes = vectis_register_auth_routes;
  app->openapi_doc = vectis_attach_openapi_doc;
  app->openapi = vectis_generate_openapi;
  app->route_count = vectis_route_count;
  app->logger = vectis_logger;
  app->lockd_client = vectis_lockd_client;
  app->consumer_service = vectis_consumer_service_new;
  app->register_consumer_receiver = vectis_register_consumer_receiver;
  app->consumer_service_receiver = vectis_consumer_service_new_receiver;
  app->close = vectis_destroy;
  app->impl = impl;
  return app;
}

vectis_app *vectis_new(const vectis_app_config *config, vectis_error *error) {
  return vectis_app_new(config, error);
}

void vectis_app_close(vectis_app *app) { vectis_destroy(app); }

void vectis_destroy(vectis_app *app) {
  vectis_app_impl *impl;
  vectis_error error;

  if (app == NULL) {
    return;
  }
  impl = (vectis_app_impl *)app->impl;
  if (impl != NULL && impl->started) {
    (void)vectis_stop(app, &error);
  }
  vectis_destroy_impl(impl);
  free(app);
}

static vectis_status vectis_app_start_impl(vectis_app *app,
                                           vectis_error *error) {
  vectis_app_impl *impl;
  vectis_status status;
  vectis_kore_runtime_config kore_config;
  size_t route_count;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  impl = (vectis_app_impl *)app->impl;

  status = vectis_validate_lockd_startable(impl, error);
  if (status != VECTIS_OK) {
    return status;
  }

  (void)pthread_mutex_lock(&impl->mutex);
  if (impl->started) {
    (void)pthread_mutex_unlock(&impl->mutex);
    vectis_set_error(error, VECTIS_ERR_STATE, "app is already started");
    return VECTIS_ERR_STATE;
  }
  (void)pthread_mutex_unlock(&impl->mutex);

  (void)pthread_mutex_lock(&impl->mutex);
  route_count = impl->route_count;
  (void)pthread_mutex_unlock(&impl->mutex);

  if (route_count > 0u) {
    status = vectis_validate_startable(impl, error);
    if (status != VECTIS_OK) {
      return status;
    }
  }

  if (route_count == 0u) {
    status = vectis_open_lockd_client(impl, error);
    if (status != VECTIS_OK) {
      return status;
    }
  }

  if (route_count > 0u) {
    memset(&kore_config, 0, sizeof(kore_config));
    kore_config.app = app;
    kore_config.app_name = impl->app_name;
    kore_config.bind = impl->bind;
    kore_config.port = impl->port;
    kore_config.domain = impl->domain;
    kore_config.domains = (const char *const *)impl->domains;
    kore_config.domain_count = impl->domain_count;
    kore_config.tls_mode = impl->tls_mode;
    kore_config.acme_email = impl->acme_email;
    kore_config.acme_directory_url = impl->acme_directory_url;
    kore_config.acme_state_dir = impl->acme_state_dir;
    kore_config.cert_key_bundle_path = impl->cert_key_bundle_path;
    kore_config.cert_key_bundle_pem = impl->cert_key_bundle_pem;
    kore_config.cert_key_bundle_pem_size = impl->cert_key_bundle_pem_size;
    kore_config.cert_key_bundle_source = impl->cert_key_bundle_source;
    kore_config.certificate_path = impl->certificate_path;
    kore_config.certificate_pem = impl->certificate_pem;
    kore_config.certificate_pem_size = impl->certificate_pem_size;
    kore_config.certificate_source = impl->certificate_source;
    kore_config.private_key_path = impl->private_key_path;
    kore_config.private_key_pem = impl->private_key_pem;
    kore_config.private_key_pem_size = impl->private_key_pem_size;
    kore_config.private_key_source = impl->private_key_source;
    kore_config.ca_bundle_path = impl->ca_bundle_path;
    kore_config.ca_bundle_pem = impl->ca_bundle_pem;
    kore_config.ca_bundle_pem_size = impl->ca_bundle_pem_size;
    kore_config.ca_bundle_source = impl->ca_bundle_source;
    kore_config.client_ca_bundle_path = impl->client_ca_bundle_path;
    kore_config.client_ca_bundle_pem = impl->client_ca_bundle_pem;
    kore_config.client_ca_bundle_pem_size = impl->client_ca_bundle_pem_size;
    kore_config.client_ca_bundle_source = impl->client_ca_bundle_source;
    kore_config.require_client_certificate = impl->require_client_certificate;
    kore_config.server = impl->server;
    kore_config.server.max_request_body_bytes =
        vectis_app_max_request_body_bytes(impl);
    kore_config.body_disk_offload_bytes = vectis_app_body_disk_offload_bytes(
        impl, &kore_config.body_disk_offload_configured);
    kore_config.logger = impl->logger;
    status = vectis_internal_kore_start(&kore_config, error);
    if (status != VECTIS_OK) {
      vectis_close_lockd_client_for_current_process(impl);
      return status;
    }
  }

  (void)pthread_mutex_lock(&impl->mutex);
  impl->started = 1;
  (void)pthread_mutex_unlock(&impl->mutex);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_start(vectis_app *app, vectis_error *error) {
  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_app_start_impl(app, error);
}

static vectis_status vectis_app_stop_impl(vectis_app *app,
                                          vectis_error *error) {
  vectis_app_impl *impl;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  impl = (vectis_app_impl *)app->impl;

  (void)pthread_mutex_lock(&impl->mutex);
  if (!impl->started) {
    (void)pthread_mutex_unlock(&impl->mutex);
    vectis_set_error(error, VECTIS_ERR_STATE, "app is not started");
    return VECTIS_ERR_STATE;
  }
  impl->started = 0;
  (void)pthread_mutex_unlock(&impl->mutex);

  if (vectis_internal_kore_stop(app, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }

  (void)pthread_mutex_lock(&impl->mutex);
  vectis_close_lockd_client_for_current_process(impl);
  (void)pthread_mutex_unlock(&impl->mutex);

  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_stop(vectis_app *app, vectis_error *error) {
  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_app_stop_impl(app, error);
}

static int vectis_route_conflicts(const vectis_route_entry *existing,
                                  vectis_http_method method,
                                  vectis_http_methods methods,
                                  vectis_route_path_kind path_kind,
                                  const char *path) {
  vectis_http_methods mask;

  mask = vectis_normalize_methods(method, methods);
  return (existing->methods & mask) != 0u && existing->path_kind == path_kind &&
         strcmp(existing->path, path) == 0;
}

static vectis_status vectis_app_register_route_impl(
    vectis_app *app, const vectis_route_config *route, vectis_error *error) {
  return vectis_app_register_route_owned_userdata(app, route, 0, error);
}

static vectis_status vectis_app_register_route_owned_userdata(
    vectis_app *app, const vectis_route_config *route, int owns_userdata,
    vectis_error *error) {
  vectis_app_impl *impl;
  vectis_route_entry *grown;
  size_t i;
  size_t next_capacity;
  vectis_status status;
  vectis_body_policy effective_body;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }

  status = vectis_validate_route(route, error);
  if (status != VECTIS_OK) {
    return status;
  }

  impl = (vectis_app_impl *)app->impl;
  (void)pthread_mutex_lock(&impl->mutex);
  if (impl->started) {
    (void)pthread_mutex_unlock(&impl->mutex);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "routes cannot be registered after app start");
    return VECTIS_ERR_STATE;
  }
  (void)pthread_mutex_unlock(&impl->mutex);

  status = vectis_validate_body_policy(
      &route->body,
      impl->server.max_request_body_bytes > 0u ? &impl->server : NULL, error);
  if (status != VECTIS_OK) {
    return status;
  }
  effective_body = vectis_effective_body_policy(&route->body, &impl->server);
  (void)pthread_mutex_lock(&impl->mutex);
  for (i = 0u; i < impl->route_count; ++i) {
    if (vectis_route_conflicts(&impl->routes[i], route->method, route->methods,
                               route->path_kind, route->path)) {
      (void)pthread_mutex_unlock(&impl->mutex);
      vectis_set_errorf(error, VECTIS_ERR_CONFLICT,
                        "duplicate route registration for %s", route->path);
      return VECTIS_ERR_CONFLICT;
    }
  }

  if (impl->route_count == impl->route_capacity) {
    next_capacity = impl->route_capacity == 0u ? 4u : impl->route_capacity * 2u;
    grown = (vectis_route_entry *)realloc(impl->routes,
                                          next_capacity * sizeof(*grown));
    if (grown == NULL) {
      (void)pthread_mutex_unlock(&impl->mutex);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to grow route registry");
      return VECTIS_ERR_NOMEM;
    }
    impl->routes = grown;
    impl->route_capacity = next_capacity;
  }

  impl->routes[impl->route_count].kind = VECTIS_ROUTE_ENTRY_HANDLER;
  impl->routes[impl->route_count].method = route->method;
  impl->routes[impl->route_count].methods =
      vectis_normalize_methods(route->method, route->methods);
  impl->routes[impl->route_count].path_kind = route->path_kind;
  impl->routes[impl->route_count].path = vectis_strdup(route->path);
  impl->routes[impl->route_count].body = effective_body;
  impl->routes[impl->route_count].handler = route->handler;
  impl->routes[impl->route_count].userdata = route->userdata;
  impl->routes[impl->route_count].owns_userdata = owns_userdata;
  if (impl->routes[impl->route_count].path == NULL) {
    (void)pthread_mutex_unlock(&impl->mutex);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy route path");
    return VECTIS_ERR_NOMEM;
  }
  impl->route_count++;
  (void)pthread_mutex_unlock(&impl->mutex);

  return VECTIS_OK;
}

vectis_status vectis_register_route(vectis_app *app,
                                    const vectis_route_config *route,
                                    vectis_error *error) {
  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_app_register_route_impl(app, route, error);
}

static vectis_status vectis_app_register_upload_owned_userdata(
    vectis_app *app, const vectis_upload_route_config *route, int owns_userdata,
    vectis_error *error) {
  vectis_app_impl *impl;
  vectis_route_entry *grown;
  size_t i;
  size_t next_capacity;
  vectis_status status;
  vectis_body_policy effective_body;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }

  status = vectis_validate_upload_route(route, error);
  if (status != VECTIS_OK) {
    return status;
  }

  impl = (vectis_app_impl *)app->impl;
  (void)pthread_mutex_lock(&impl->mutex);
  if (impl->started) {
    (void)pthread_mutex_unlock(&impl->mutex);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "routes cannot be registered after app start");
    return VECTIS_ERR_STATE;
  }
  (void)pthread_mutex_unlock(&impl->mutex);

  status = vectis_validate_body_policy(
      &route->body,
      impl->server.max_request_body_bytes > 0u ? &impl->server : NULL, error);
  if (status != VECTIS_OK) {
    return status;
  }
  effective_body = vectis_effective_body_policy(&route->body, &impl->server);
  (void)pthread_mutex_lock(&impl->mutex);
  for (i = 0u; i < impl->route_count; ++i) {
    if (vectis_route_conflicts(&impl->routes[i], route->method, route->methods,
                               route->path_kind, route->path)) {
      (void)pthread_mutex_unlock(&impl->mutex);
      vectis_set_errorf(error, VECTIS_ERR_CONFLICT,
                        "duplicate route registration for %s", route->path);
      return VECTIS_ERR_CONFLICT;
    }
  }

  if (impl->route_count == impl->route_capacity) {
    next_capacity = impl->route_capacity == 0u ? 4u : impl->route_capacity * 2u;
    grown = (vectis_route_entry *)realloc(impl->routes,
                                          next_capacity * sizeof(*grown));
    if (grown == NULL) {
      (void)pthread_mutex_unlock(&impl->mutex);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to grow route registry");
      return VECTIS_ERR_NOMEM;
    }
    impl->routes = grown;
    impl->route_capacity = next_capacity;
  }

  impl->routes[impl->route_count].kind = VECTIS_ROUTE_ENTRY_UPLOAD_STREAM;
  impl->routes[impl->route_count].method = route->method;
  impl->routes[impl->route_count].methods =
      vectis_normalize_methods(route->method, route->methods);
  impl->routes[impl->route_count].path_kind = route->path_kind;
  impl->routes[impl->route_count].path = vectis_strdup(route->path);
  impl->routes[impl->route_count].body = effective_body;
  impl->routes[impl->route_count].handler = NULL;
  impl->routes[impl->route_count].upload_open = route->open;
  impl->routes[impl->route_count].upload_write = route->write;
  impl->routes[impl->route_count].upload_finish = route->finish;
  impl->routes[impl->route_count].upload_close = route->close;
  impl->routes[impl->route_count].userdata = route->userdata;
  impl->routes[impl->route_count].owns_userdata = owns_userdata;
  if (impl->routes[impl->route_count].path == NULL) {
    (void)pthread_mutex_unlock(&impl->mutex);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy route path");
    return VECTIS_ERR_NOMEM;
  }
  impl->route_count++;
  (void)pthread_mutex_unlock(&impl->mutex);

  return VECTIS_OK;
}

vectis_status
vectis_register_upload_stream(vectis_app *app,
                              const vectis_upload_route_config *route,
                              vectis_error *error) {
  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_app_register_upload_owned_userdata(app, route, 0, error);
}

static char *vectis_join_route_prefix(const char *prefix, const char *path,
                                      vectis_error *error) {
  size_t prefix_len;
  size_t path_len;
  size_t need_slash;
  char *joined;

  if (path == NULL || path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "route path is required");
    return NULL;
  }
  if (prefix == NULL || prefix[0] == '\0') {
    return vectis_strdup(path);
  }
  if (prefix[0] != '/') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "route prefix must start with /");
    return NULL;
  }
  prefix_len = strlen(prefix);
  path_len = strlen(path);
  need_slash = prefix[prefix_len - 1u] == '/' || path[0] == '/' ? 0u : 1u;
  joined = (char *)malloc(prefix_len + need_slash + path_len + 1u);
  if (joined == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate prefixed route path");
    return NULL;
  }
  (void)snprintf(joined, prefix_len + need_slash + path_len + 1u, "%s%s%s",
                 prefix, need_slash ? "/" : "",
                 path[0] == '/' && prefix[prefix_len - 1u] == '/' ? path + 1
                                                                  : path);
  return joined;
}

static char *vectis_join_regex_route_prefix(const char *prefix,
                                            const char *path,
                                            vectis_error *error) {
  size_t prefix_len;
  size_t path_len;
  size_t need_slash;
  char *joined;

  if (path == NULL || path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "route path is required");
    return NULL;
  }
  if (path[0] != '^' || path[1] != '/') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "regex route path must start with '^/'");
    return NULL;
  }
  if (prefix == NULL || prefix[0] == '\0' || strcmp(prefix, "/") == 0) {
    return vectis_strdup(path);
  }
  if (prefix[0] != '/') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "route prefix must start with /");
    return NULL;
  }
  prefix_len = strlen(prefix);
  path_len = strlen(path);
  need_slash = prefix[prefix_len - 1u] == '/' ? 0u : 1u;
  joined = (char *)malloc(1u + prefix_len + need_slash + path_len - 1u + 1u);
  if (joined == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate prefixed route path");
    return NULL;
  }
  (void)snprintf(joined, 1u + prefix_len + need_slash + path_len - 1u + 1u,
                 "^%s%s%s", prefix, need_slash ? "/" : "", path + 2);
  return joined;
}

vectis_status vectis_register_prefixed_route(vectis_app *app,
                                             const char *prefix,
                                             const vectis_route_config *route,
                                             vectis_error *error) {
  vectis_route_config prefixed;
  char *path;
  vectis_status status;

  if (route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "route is required");
    return VECTIS_ERR_INVALID;
  }
  if (route->path_kind == VECTIS_ROUTE_PATH_REGEX) {
    path = vectis_join_regex_route_prefix(prefix, route->path, error);
  } else {
    path = vectis_join_route_prefix(prefix, route->path, error);
  }
  if (path == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  prefixed = *route;
  prefixed.path = path;
  prefixed.path_kind = vectis_infer_route_path_kind(path);
  if (route->path_kind == VECTIS_ROUTE_PATH_REGEX) {
    prefixed.path_kind = VECTIS_ROUTE_PATH_REGEX;
  }
  status = vectis_register_route(app, &prefixed, error);
  free(path);
  return status;
}

static vectis_http_methods
vectis_static_methods_or_default(vectis_http_methods methods) {
  if (methods == VECTIS_HTTP_METHODS_NONE) {
    return VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD;
  }
  return methods;
}

static vectis_static_route_data *vectis_static_route_data_new(
    int directory, const char *path_prefix, const char *file_path,
    const char *root_dir, const char *content_type, const char *index_file,
    const vectis_embedded_fs *embedded_fs, const char *cache_control,
    const char *not_found_body, const char *not_found_content_type,
    vectis_http_methods allowed_methods, vectis_error *error) {
  vectis_static_route_data *data;
  char *cursor;
  size_t total;
  size_t len;

  total = sizeof(*data);
  total += path_prefix != NULL ? strlen(path_prefix) + 1u : 0u;
  total += file_path != NULL ? strlen(file_path) + 1u : 0u;
  total += root_dir != NULL ? strlen(root_dir) + 1u : 0u;
  total += content_type != NULL ? strlen(content_type) + 1u : 0u;
  total += index_file != NULL ? strlen(index_file) + 1u : 0u;
  total += cache_control != NULL ? strlen(cache_control) + 1u : 0u;
  total += not_found_body != NULL ? strlen(not_found_body) + 1u : 0u;
  total +=
      not_found_content_type != NULL ? strlen(not_found_content_type) + 1u : 0u;
  data = (vectis_static_route_data *)calloc(1u, total);
  if (data == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate static route data");
    return NULL;
  }
  data->directory = directory;
  data->embedded_fs = embedded_fs;
  data->allowed_methods = allowed_methods;
  cursor = (char *)(data + 1);
#define VECTIS_COPY_STATIC_FIELD(field, value)                                 \
  do {                                                                         \
    if ((value) != NULL) {                                                     \
      len = strlen(value) + 1u;                                                \
      memcpy(cursor, value, len);                                              \
      data->field = cursor;                                                    \
      cursor += len;                                                           \
    }                                                                          \
  } while (0)
  VECTIS_COPY_STATIC_FIELD(path_prefix, path_prefix);
  VECTIS_COPY_STATIC_FIELD(file_path, file_path);
  VECTIS_COPY_STATIC_FIELD(root_dir, root_dir);
  VECTIS_COPY_STATIC_FIELD(content_type, content_type);
  VECTIS_COPY_STATIC_FIELD(index_file, index_file);
  VECTIS_COPY_STATIC_FIELD(cache_control, cache_control);
  VECTIS_COPY_STATIC_FIELD(not_found_body, not_found_body);
  VECTIS_COPY_STATIC_FIELD(not_found_content_type, not_found_content_type);
#undef VECTIS_COPY_STATIC_FIELD
  (void)len;
  return data;
}

static vectis_status vectis_static_response(vectis_request *request,
                                            vectis_response *response,
                                            const char *content_type,
                                            const char *file_path,
                                            vectis_error *error) {
  struct stat st;

  if (vectis_request_method(request) == VECTIS_HTTP_HEAD) {
    if (stat(file_path, &st) != 0 || !S_ISREG(st.st_mode)) {
      return vectis_response_status(response, 404, error);
    }
    if (content_type != NULL &&
        vectis_response_header(response, "content-type", content_type, error) !=
            VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
    return vectis_response_status(response, 200, error);
  }
  return vectis_response_file(response, 200, content_type, file_path, error);
}

static vectis_status vectis_static_file_dispatch(vectis_app *app,
                                                 vectis_request *request,
                                                 vectis_response *response,
                                                 void *userdata,
                                                 vectis_error *error) {
  vectis_static_route_data *data;

  (void)app;
  data = (vectis_static_route_data *)userdata;
  if (data == NULL || data->file_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "static file route is invalid");
    return VECTIS_ERR_INVALID;
  }
  return vectis_static_response(request, response, data->content_type,
                                data->file_path, error);
}

static int vectis_static_relative_path_safe(const char *path) {
  const char *segment;
  const char *p;
  size_t len;

  if (path == NULL || path[0] == '\0' || path[0] == '/') {
    return 0;
  }
  segment = path;
  p = path;
  for (;;) {
    if (*p == '/' || *p == '\0') {
      len = (size_t)(p - segment);
      if (len == 0u || (len == 1u && segment[0] == '.') ||
          (len == 2u && segment[0] == '.' && segment[1] == '.')) {
        return 0;
      }
      if (*p == '\0') {
        return 1;
      }
      segment = p + 1;
    } else if (*p == '\\' || *p == '%') {
      return 0;
    }
    p++;
  }
}

static char *vectis_join_file_path(const char *root, const char *relative,
                                   vectis_error *error) {
  size_t root_len;
  size_t relative_len;
  size_t need_slash;
  char *path;

  root_len = strlen(root);
  relative_len = strlen(relative);
  need_slash = root_len > 0u && root[root_len - 1u] == '/' ? 0u : 1u;
  path = (char *)malloc(root_len + need_slash + relative_len + 1u);
  if (path == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate static file path");
    return NULL;
  }
  (void)snprintf(path, root_len + need_slash + relative_len + 1u, "%s%s%s",
                 root, need_slash ? "/" : "", relative);
  return path;
}

static vectis_status vectis_static_directory_dispatch(vectis_app *app,
                                                      vectis_request *request,
                                                      vectis_response *response,
                                                      void *userdata,
                                                      vectis_error *error) {
  vectis_static_route_data *data;
  const char *path;
  const char *relative;
  size_t prefix_len;
  char *file_path;
  vectis_status status;

  (void)app;
  data = (vectis_static_route_data *)userdata;
  path = vectis_request_path(request);
  if (data == NULL || data->path_prefix == NULL || data->root_dir == NULL ||
      path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "static directory route is invalid");
    return VECTIS_ERR_INVALID;
  }
  prefix_len = strlen(data->path_prefix);
  if (strncmp(path, data->path_prefix, prefix_len) != 0) {
    return vectis_response_status(response, 404, error);
  }
  if (strcmp(data->path_prefix, "/") == 0) {
    relative = path[1] == '\0' ? data->index_file : path + 1u;
  } else if (path[prefix_len] == '\0') {
    relative = data->index_file;
  } else if (path[prefix_len] == '/') {
    relative = path + prefix_len + 1u;
  } else {
    return vectis_response_status(response, 404, error);
  }
  if (!vectis_static_relative_path_safe(relative)) {
    return vectis_response_status(response, 404, error);
  }
  file_path = vectis_join_file_path(data->root_dir, relative, error);
  if (file_path == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  status = vectis_static_response(request, response, data->content_type,
                                  file_path, error);
  free(file_path);
  return status;
}

static const char *vectis_static_embedded_allow(vectis_http_methods methods) {
  if ((methods & VECTIS_HTTP_METHODS_GET) != 0u &&
      (methods & VECTIS_HTTP_METHODS_HEAD) != 0u) {
    return "GET, HEAD";
  }
  if ((methods & VECTIS_HTTP_METHODS_HEAD) != 0u) {
    return "HEAD";
  }
  return "GET";
}

static vectis_status vectis_static_embedded_header(vectis_response *response,
                                                   const char *name,
                                                   const char *value,
                                                   vectis_error *error) {
  if (value == NULL || value[0] == '\0') {
    return VECTIS_OK;
  }
  return vectis_response_header(response, name, value, error);
}

typedef struct vectis_static_embedded_range {
  int requested;
  int satisfiable;
  size_t start;
  size_t end;
} vectis_static_embedded_range;

static int vectis_static_embedded_parse_size(const char **cursor, size_t *out) {
  const char *p;
  size_t value;
  unsigned int digit;

  if (cursor == NULL || *cursor == NULL || out == NULL ||
      !isdigit((unsigned char)**cursor)) {
    return 0;
  }
  p = *cursor;
  value = 0u;
  while (isdigit((unsigned char)*p)) {
    digit = (unsigned int)(*p - '0');
    if (value > (((size_t)-1) - (size_t)digit) / 10u) {
      return 0;
    }
    value = value * 10u + (size_t)digit;
    p++;
  }
  *cursor = p;
  *out = value;
  return 1;
}

static int
vectis_static_embedded_parse_range(const char *header_value, size_t size,
                                   vectis_static_embedded_range *out) {
  const char *cursor;
  size_t start;
  size_t end;
  size_t suffix;

  if (out == NULL) {
    return 0;
  }
  memset(out, 0, sizeof(*out));
  if (header_value == NULL || header_value[0] == '\0') {
    return 1;
  }
  out->requested = 1;
  if (strncmp(header_value, "bytes=", 6u) != 0 ||
      strchr(header_value, ',') != NULL) {
    return 1;
  }
  cursor = header_value + 6u;
  if (*cursor == '-') {
    cursor++;
    if (!vectis_static_embedded_parse_size(&cursor, &suffix) ||
        *cursor != '\0' || suffix == 0u || size == 0u) {
      return 1;
    }
    out->satisfiable = 1;
    out->start = suffix >= size ? 0u : size - suffix;
    out->end = size - 1u;
    return 1;
  }
  if (!vectis_static_embedded_parse_size(&cursor, &start) || *cursor != '-') {
    return 1;
  }
  cursor++;
  if (*cursor == '\0') {
    if (start >= size || size == 0u) {
      return 1;
    }
    out->satisfiable = 1;
    out->start = start;
    out->end = size - 1u;
    return 1;
  }
  if (!vectis_static_embedded_parse_size(&cursor, &end) || *cursor != '\0' ||
      end < start || start >= size || size == 0u) {
    return 1;
  }
  out->satisfiable = 1;
  out->start = start;
  out->end = end >= size ? size - 1u : end;
  return 1;
}

static int vectis_static_embedded_content_range(char *buffer,
                                                size_t buffer_size,
                                                const char *prefix,
                                                size_t start, size_t end,
                                                size_t size) {
  int n;

  if (prefix != NULL) {
    n = snprintf(buffer, buffer_size, "%s%llu", prefix,
                 (unsigned long long)size);
  } else {
    n = snprintf(buffer, buffer_size, "bytes %llu-%llu/%llu",
                 (unsigned long long)start, (unsigned long long)end,
                 (unsigned long long)size);
  }
  return n >= 0 && (size_t)n < buffer_size;
}

static int vectis_static_embedded_if_none_match_token(const char *start,
                                                      size_t len,
                                                      const char *etag) {
  while (len > 0u && (start[0] == ' ' || start[0] == '\t')) {
    start++;
    len--;
  }
  while (len > 0u && (start[len - 1u] == ' ' || start[len - 1u] == '\t')) {
    len--;
  }
  if (len == 1u && start[0] == '*') {
    return 1;
  }
  if (etag == NULL || etag[0] == '\0') {
    return 0;
  }
  if (strlen(etag) == len && memcmp(start, etag, len) == 0) {
    return 1;
  }
  if (len > 2u && start[0] == 'W' && start[1] == '/' &&
      strlen(etag) == len - 2u && memcmp(start + 2u, etag, len - 2u) == 0) {
    return 1;
  }
  return 0;
}

static int vectis_static_embedded_if_none_match(const char *header_value,
                                                const char *etag) {
  const char *token;
  const char *cursor;

  if (header_value == NULL || header_value[0] == '\0') {
    return 0;
  }
  cursor = header_value;
  for (;;) {
    token = cursor;
    while (*cursor != '\0' && *cursor != ',') {
      cursor++;
    }
    if (vectis_static_embedded_if_none_match_token(
            token, (size_t)(cursor - token), etag)) {
      return 1;
    }
    if (*cursor == '\0') {
      return 0;
    }
    cursor++;
  }
}

static int vectis_static_embedded_if_range_matches(const char *header_value,
                                                   const char *etag) {
  const char *start;
  size_t len;

  if (header_value == NULL || header_value[0] == '\0' || etag == NULL ||
      etag[0] == '\0') {
    return 0;
  }
  start = header_value;
  while (*start == ' ' || *start == '\t') {
    start++;
  }
  len = strlen(start);
  while (len > 0u && (start[len - 1u] == ' ' || start[len - 1u] == '\t')) {
    len--;
  }
  return strlen(etag) == len && memcmp(start, etag, len) == 0;
}

static vectis_status vectis_static_embedded_not_found(
    const vectis_static_route_data *data, vectis_request *request,
    vectis_response *response, vectis_error *error) {
  vectis_bytes body;
  const char *not_found_body;
  const char *content_type;
  size_t body_size;

  not_found_body = data->not_found_body != NULL ? data->not_found_body : "";
  body_size = strlen(not_found_body);
  content_type = data->not_found_content_type != NULL
                     ? data->not_found_content_type
                     : "text/plain; charset=utf-8";
  if (vectis_response_header(response, "cache-control", "no-store", error) !=
          VECTIS_OK ||
      vectis_static_embedded_header(response, "content-type", content_type,
                                    error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (vectis_request_method(request) == VECTIS_HTTP_HEAD) {
    return vectis_response_status(response, 404, error);
  }
  body.data = not_found_body;
  body.size = body_size;
  return vectis_response_bytes(response, 404, content_type, body, error);
}

static vectis_status vectis_static_embedded_response(
    const vectis_static_route_data *data, vectis_request *request,
    vectis_response *response, const vectis_embedded_fs_entry *entry,
    vectis_error *error) {
  vectis_bytes body;
  const char *content_type;
  const char *range_header;
  const char *if_range_header;
  vectis_static_embedded_range range;
  char content_range[96];
  int has_etag;

  content_type =
      entry->content_type != NULL ? entry->content_type : data->content_type;
  if (content_type == NULL || content_type[0] == '\0') {
    content_type = "application/octet-stream";
  }
  if (data->cache_control != NULL &&
      vectis_response_header(response, "cache-control", data->cache_control,
                             error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  has_etag = 0;
  if (entry->etag != NULL && entry->etag[0] != '\0') {
    if (vectis_response_header(response, "etag", entry->etag, error) !=
        VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_INVALID;
    }
    has_etag = 1;
  }
  if (vectis_response_header(response, "accept-ranges", "bytes", error) !=
      VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (has_etag && vectis_static_embedded_if_none_match(
                      vectis_request_header(request, "if-none-match"),
                      entry->etag)) {
    return vectis_response_status(response, 304, error);
  }
  range_header = vectis_request_header(request, "range");
  if (range_header != NULL && range_header[0] != '\0') {
    if_range_header = vectis_request_header(request, "if-range");
    if (if_range_header != NULL && if_range_header[0] != '\0' &&
        (!has_etag ||
         !vectis_static_embedded_if_range_matches(if_range_header,
                                                  entry->etag))) {
      range_header = NULL;
    }
  }
  if (!vectis_static_embedded_parse_range(range_header, entry->size, &range)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "failed to parse embedded static byte range");
    return VECTIS_ERR_INVALID;
  }
  if (range.requested && !range.satisfiable) {
    if (!vectis_static_embedded_content_range(content_range,
                                              sizeof(content_range), "bytes */",
                                              0u, 0u, entry->size) ||
        vectis_response_header(response, "content-range", content_range,
                               error) != VECTIS_OK ||
        vectis_static_embedded_header(response, "content-type", content_type,
                                      error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_INVALID;
    }
    return vectis_response_status(response, 416, error);
  }
  if (range.requested) {
    if (!vectis_static_embedded_content_range(
            content_range, sizeof(content_range), NULL, range.start, range.end,
            entry->size) ||
        vectis_response_header(response, "content-range", content_range,
                               error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_INVALID;
    }
    body.data = (const unsigned char *)entry->data + range.start;
    body.size = range.end - range.start + 1u;
    if (vectis_request_method(request) == VECTIS_HTTP_HEAD) {
      if (vectis_static_embedded_header(response, "content-type", content_type,
                                        error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_INVALID;
      }
      return vectis_response_status(response, 206, error);
    }
    return vectis_response_bytes(response, 206, content_type, body, error);
  }
  if (vectis_request_method(request) == VECTIS_HTTP_HEAD) {
    if (vectis_static_embedded_header(response, "content-type", content_type,
                                      error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_INVALID;
    }
    return vectis_response_status(response, 200, error);
  }
  body.data = entry->data;
  body.size = entry->size;
  return vectis_response_bytes(response, 200, content_type, body, error);
}

static vectis_status vectis_static_embedded_dispatch(vectis_app *app,
                                                     vectis_request *request,
                                                     vectis_response *response,
                                                     void *userdata,
                                                     vectis_error *error) {
  vectis_static_route_data *data;
  vectis_embedded_fs_entry entry;
  vectis_http_method method;
  vectis_http_methods method_mask;
  const char *path;
  const char *embedded_path;
  size_t prefix_len;
  int found;
  vectis_status status;

  (void)app;
  data = (vectis_static_route_data *)userdata;
  path = vectis_request_path(request);
  if (data == NULL || data->path_prefix == NULL || data->embedded_fs == NULL ||
      path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "static embedded route is invalid");
    return VECTIS_ERR_INVALID;
  }
  method = vectis_request_method(request);
  method_mask = vectis_method_mask(method);
  if (method_mask == VECTIS_HTTP_METHODS_NONE ||
      (data->allowed_methods & method_mask) == 0u) {
    if (vectis_response_header(
            response, "allow",
            vectis_static_embedded_allow(data->allowed_methods),
            error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_INVALID;
    }
    return vectis_response_status(response, 405, error);
  }
  prefix_len = strlen(data->path_prefix);
  if (strcmp(data->path_prefix, "/") == 0) {
    embedded_path = path;
  } else if (path[prefix_len] == '\0') {
    embedded_path = "/";
  } else if (path[prefix_len] == '/') {
    embedded_path = path + prefix_len;
  } else {
    return vectis_static_embedded_not_found(data, request, response, error);
  }
  memset(&entry, 0, sizeof(entry));
  found = 0;
  status = data->embedded_fs->lookup(data->embedded_fs, embedded_path, &found,
                                     &entry, error);
  if (status == VECTIS_ERR_INVALID) {
    return vectis_static_embedded_not_found(data, request, response, error);
  }
  if (status != VECTIS_OK) {
    return status;
  }
  if (!found) {
    return vectis_static_embedded_not_found(data, request, response, error);
  }
  if (entry.kind == VECTIS_EMBEDDED_FS_ENTRY_DIRECTORY) {
    return vectis_static_embedded_not_found(data, request, response, error);
  }
  return vectis_static_embedded_response(data, request, response, &entry,
                                         error);
}

static char *vectis_static_directory_regex(const char *prefix,
                                           vectis_error *error) {
  const char *p;
  char *regex;
  char *out;
  size_t extra;
  size_t len;

  extra = 0u;
  for (p = prefix; *p != '\0'; ++p) {
    if (strchr(".+*?^$()[]{}|\\", *p) != NULL) {
      extra++;
    }
  }
  if (strcmp(prefix, "/") == 0) {
    regex = vectis_strdup("^/.*$");
    if (regex == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to allocate static directory route regex");
    }
    return regex;
  }
  len = 1u + strlen(prefix) + extra + strlen("(/.*)?$") + 1u;
  regex = (char *)malloc(len);
  if (regex == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate static directory route regex");
    return NULL;
  }
  out = regex;
  *out = '^';
  out++;
  for (p = prefix; *p != '\0'; ++p) {
    if (strchr(".+*?^$()[]{}|\\", *p) != NULL) {
      *out = '\\';
      out++;
    }
    *out = *p;
    out++;
  }
  memcpy(out, "(/.*)?$", strlen("(/.*)?$") + 1u);
  return regex;
}

static char *vectis_normalize_static_directory_prefix(const char *prefix,
                                                      vectis_error *error) {
  char *normalized;
  size_t len;

  if (prefix == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "static directory path_prefix is required");
    return NULL;
  }
  len = strlen(prefix);
  while (len > 1u && prefix[len - 1u] == '/') {
    len--;
  }
  normalized = (char *)malloc(len + 1u);
  if (normalized == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to normalize static directory path_prefix");
    return NULL;
  }
  memcpy(normalized, prefix, len);
  normalized[len] = '\0';
  return normalized;
}

vectis_status
vectis_register_static_file(vectis_app *app,
                            const vectis_static_file_config *config,
                            vectis_error *error) {
  vectis_route_config route;
  vectis_static_route_data *data;
  vectis_status status;

  if (config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "static file config is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->path == NULL || config->file_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "static file path and file_path are required");
    return VECTIS_ERR_INVALID;
  }
  data = vectis_static_route_data_new(
      0, NULL, config->file_path, NULL,
      config->content_type != NULL ? config->content_type
                                   : "application/octet-stream",
      NULL, NULL, NULL, NULL, NULL, VECTIS_HTTP_METHODS_NONE, error);
  if (data == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  route =
      vectis_route_methods(vectis_static_methods_or_default(config->methods),
                           config->path, vectis_static_file_dispatch, data);
  status = vectis_app_register_route_owned_userdata(app, &route, 1, error);
  if (status != VECTIS_OK) {
    free(data);
  }
  return status;
}

vectis_status
vectis_register_static_directory(vectis_app *app,
                                 const vectis_static_directory_config *config,
                                 vectis_error *error) {
  vectis_route_config route;
  vectis_static_route_data *data;
  vectis_status status;
  char *regex;
  char *path_prefix;

  path_prefix = NULL;
  if (config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "static directory config is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->path_prefix == NULL || config->root_dir == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "static directory path_prefix and root_dir are required");
    return VECTIS_ERR_INVALID;
  }
  path_prefix =
      vectis_normalize_static_directory_prefix(config->path_prefix, error);
  if (path_prefix == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  if (vectis_validate_route_path(path_prefix, VECTIS_ROUTE_PATH_LITERAL,
                                 error) != VECTIS_OK) {
    free(path_prefix);
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  regex = vectis_static_directory_regex(path_prefix, error);
  if (regex == NULL) {
    free(path_prefix);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  data = vectis_static_route_data_new(
      1, path_prefix, NULL, config->root_dir,
      config->content_type != NULL ? config->content_type
                                   : "application/octet-stream",
      config->index_file != NULL ? config->index_file : "index.html", NULL,
      NULL, NULL, NULL, VECTIS_HTTP_METHODS_NONE, error);
  if (data == NULL) {
    free(path_prefix);
    free(regex);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  free(path_prefix);
  vectis_route_config_init(&route);
  route.method =
      vectis_first_method(vectis_static_methods_or_default(config->methods));
  route.methods = vectis_static_methods_or_default(config->methods);
  route.path = regex;
  route.path_kind = VECTIS_ROUTE_PATH_REGEX;
  route.body = vectis_body_none();
  route.handler = vectis_static_directory_dispatch;
  route.userdata = data;
  status = vectis_app_register_route_owned_userdata(app, &route, 1, error);
  free(regex);
  if (status != VECTIS_OK) {
    free(data);
  }
  return status;
}

vectis_status
vectis_register_static_embedded(vectis_app *app,
                                const vectis_static_embedded_config *config,
                                vectis_error *error) {
  vectis_route_config route;
  vectis_static_route_data *data;
  vectis_status status;
  char *regex;
  char *path_prefix;
  vectis_http_methods allowed_methods;

  path_prefix = NULL;
  if (config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "static embedded config is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->path_prefix == NULL || config->fs == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "static embedded path_prefix and fs are required");
    return VECTIS_ERR_INVALID;
  }
  allowed_methods = vectis_static_methods_or_default(config->methods);
  if (allowed_methods == VECTIS_HTTP_METHODS_NONE ||
      (allowed_methods &
       ~(VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD)) != 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "static embedded methods must be GET and/or HEAD");
    return VECTIS_ERR_INVALID;
  }
  path_prefix =
      vectis_normalize_static_directory_prefix(config->path_prefix, error);
  if (path_prefix == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  if (vectis_validate_route_path(path_prefix, VECTIS_ROUTE_PATH_LITERAL,
                                 error) != VECTIS_OK) {
    free(path_prefix);
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  regex = vectis_static_directory_regex(path_prefix, error);
  if (regex == NULL) {
    free(path_prefix);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  data = vectis_static_route_data_new(
      2, path_prefix, NULL, NULL,
      config->content_type != NULL ? config->content_type
                                   : "application/octet-stream",
      NULL, config->fs,
      config->cache_control != NULL ? config->cache_control : "no-cache",
      config->not_found_body != NULL ? config->not_found_body : "not found\n",
      config->not_found_content_type != NULL ? config->not_found_content_type
                                             : "text/plain; charset=utf-8",
      allowed_methods, error);
  if (data == NULL) {
    free(path_prefix);
    free(regex);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  free(path_prefix);
  vectis_route_config_init(&route);
  route.method = VECTIS_HTTP_GET;
  route.methods = VECTIS_HTTP_METHODS_ALL;
  route.path = regex;
  route.path_kind = VECTIS_ROUTE_PATH_REGEX;
  route.body = vectis_body_none();
  route.handler = vectis_static_embedded_dispatch;
  route.userdata = data;
  status = vectis_app_register_route_owned_userdata(app, &route, 1, error);
  free(regex);
  if (status != VECTIS_OK) {
    free(data);
  }
  return status;
}

typedef struct vectis_webdav_route_data {
  vectis_webdav_config storage;
  char *path_prefix;
  vectis_webdav_auth_fn auth;
  void *auth_userdata;
  int auth_required;
  int conceal_unauthorized;
} vectis_webdav_route_data;

typedef struct vectis_auth_route_data {
  vectis_auth_store_config store;
  const char *path_prefix;
  const char *realm;
  const char *login_title;
  const char *login_template_html;
  size_t max_body_bytes;
  uint64_t unix_seconds;
  unsigned int totp_window;
  unsigned int required_factors;
  int require_email_token;
  uint64_t email_token_ttl_seconds;
  unsigned int email_token_max_attempts;
  uint64_t pending_login_ttl_seconds;
  vectis_auth_smtp_config email_smtp;
} vectis_auth_route_data;

typedef struct vectis_auth_form_fields {
  char *username;
  char *password;
  char *totp_code;
  char *email;
  char *email_transaction_id;
  char *email_token;
  char *pending_transaction_id;
} vectis_auth_form_fields;

typedef struct vectis_webdav_propfind_state {
  const vectis_webdav_route_data *data;
  vectis_string_builder *xml;
  vectis_error *error;
  vectis_status status;
} vectis_webdav_propfind_state;

void vectis_webdav_mount_config_init(vectis_webdav_mount_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->path_prefix = "/";
  vectis_webdav_config_init(&config->storage);
  config->auth_required = 1;
  config->conceal_unauthorized = 1;
}

void vectis_webdav_embedded_site_config_init(
    vectis_webdav_embedded_site_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->path_prefix = "/";
  vectis_webdav_config_init(&config->storage);
  config->extract_policy = VECTIS_EMBEDDED_FS_EXTRACT_SKIP_EXISTING;
  config->auth_required = 1;
  config->conceal_unauthorized = 1;
}

void vectis_webdav_auth_response_init(vectis_webdav_auth_response *response) {
  if (response == NULL) {
    return;
  }
  memset(response, 0, sizeof(*response));
  response->action = VECTIS_WEBDAV_AUTH_DENY;
}

void vectis_webdav_auth_provider_config_init(
    vectis_webdav_auth_provider_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->allowed_auth_modes = VECTIS_AUTH_MODE_DEFAULT;
}

static void vectis_webdav_copy_fixed(char *out, size_t out_size,
                                     const char *value) {
  size_t len;

  if (out == NULL || out_size == 0u) {
    return;
  }
  out[0] = '\0';
  if (value == NULL) {
    return;
  }
  len = strlen(value);
  if (len >= out_size) {
    len = out_size - 1u;
  }
  memcpy(out, value, len);
  out[len] = '\0';
}

vectis_status
vectis_webdav_auth_provider(const vectis_webdav_auth_request *request,
                            vectis_webdav_auth_response *response,
                            void *userdata, vectis_error *error) {
  const vectis_webdav_auth_provider_config *config;
  vectis_auth_provider_request auth_request;
  vectis_auth_provider_response auth_response;
  vectis_status status;

  config = (const vectis_webdav_auth_provider_config *)userdata;
  if (request == NULL || response == NULL || config == NULL ||
      config->provider == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "WebDAV auth provider config is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_provider_request_init(&auth_request);
  auth_request.request = request->request;
  if (request->request != NULL) {
    auth_request.authorization =
        vectis_request_header(request->request, "authorization");
  }
  auth_request.purpose = config->purpose;
  auth_request.resource = request->resource_path;
  auth_request.allowed_auth_modes = config->allowed_auth_modes;
  vectis_auth_provider_response_init(&auth_response);
  status = vectis_auth_provider_authenticate(config->provider, &auth_request,
                                             &auth_response, error);
  if (status != VECTIS_OK) {
    vectis_auth_provider_response_cleanup(&auth_response);
    return status;
  }
  vectis_webdav_auth_response_init(response);
  response->status_code = auth_response.status_code;
  response->location = auth_response.location;
  response->content_type = auth_response.content_type;
  response->body = auth_response.body;
  response->body_size = auth_response.body_size;
  if (auth_response.www_authenticate[0] != '\0') {
    vectis_webdav_copy_fixed(response->www_authenticate_value,
                             sizeof(response->www_authenticate_value),
                             auth_response.www_authenticate);
    response->www_authenticate = response->www_authenticate_value;
  }
  vectis_webdav_copy_fixed(response->principal, sizeof(response->principal),
                           auth_response.principal);
  switch (auth_response.action) {
  case VECTIS_AUTH_ALLOW:
    response->action = VECTIS_WEBDAV_AUTH_ALLOW;
    break;
  case VECTIS_AUTH_REQUIRED:
    response->action = VECTIS_WEBDAV_AUTH_REQUIRED;
    break;
  case VECTIS_AUTH_REDIRECT:
    response->action = VECTIS_WEBDAV_AUTH_REDIRECT;
    break;
  case VECTIS_AUTH_DENY:
  default:
    response->action = VECTIS_WEBDAV_AUTH_DENY;
    break;
  }
  vectis_auth_provider_response_cleanup(&auth_response);
  return VECTIS_OK;
}

static int vectis_webdav_prefix_valid(const char *prefix) {
  return prefix != NULL && prefix[0] == '/' &&
         vectis_static_relative_path_safe(prefix + 1u);
}

static int
vectis_webdav_storage_config_present(const vectis_webdav_config *storage) {
  return storage != NULL && storage->cache_dir != NULL &&
         storage->cache_dir[0] == '/' && storage->site_id != NULL &&
         storage->site_id[0] != '\0';
}

static vectis_webdav_route_data *
vectis_webdav_route_data_new(const vectis_webdav_mount_config *config,
                             const char *path_prefix, vectis_error *error) {
  vectis_webdav_route_data *data;
  char *cursor;
  size_t prefix_len;
  size_t cache_len;
  size_t site_len;
  size_t total;

  prefix_len = strlen(path_prefix) + 1u;
  cache_len = strlen(config->storage.cache_dir) + 1u;
  site_len = strlen(config->storage.site_id) + 1u;
  total = sizeof(*data) + prefix_len + cache_len + site_len;
  data = (vectis_webdav_route_data *)calloc(1u, total);
  if (data == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate WebDAV route data");
    return NULL;
  }
  cursor = (char *)(data + 1);
  data->path_prefix = cursor;
  memcpy(cursor, path_prefix, prefix_len);
  cursor += prefix_len;
  data->storage = config->storage;
  data->storage.cache_dir = cursor;
  memcpy(cursor, config->storage.cache_dir, cache_len);
  cursor += cache_len;
  data->storage.site_id = cursor;
  memcpy(cursor, config->storage.site_id, site_len);
  data->auth = config->auth;
  data->auth_userdata = config->auth_userdata;
  data->auth_required = config->auth_required ? 1 : 0;
  data->conceal_unauthorized = config->conceal_unauthorized ? 1 : 0;
  return data;
}

static int
vectis_webdav_request_resource_path(const vectis_webdav_route_data *data,
                                    const char *request_path,
                                    char out[VECTIS_WEBDAV_PATH_MAX + 1u]) {
  const char *resource;
  size_t prefix_len;

  if (data == NULL || data->path_prefix == NULL || request_path == NULL) {
    return 0;
  }
  if (strcmp(data->path_prefix, "/") == 0) {
    resource = request_path;
  } else {
    prefix_len = strlen(data->path_prefix);
    if (strncmp(request_path, data->path_prefix, prefix_len) != 0) {
      return 0;
    }
    if (request_path[prefix_len] == '\0') {
      resource = "/";
    } else if (request_path[prefix_len] == '/') {
      resource = request_path + prefix_len;
    } else {
      return 0;
    }
  }
  return vectis_webdav_path_normalize(resource, out);
}

static vectis_status
vectis_webdav_auth_contract_response(const vectis_webdav_route_data *data,
                                     const vectis_webdav_auth_response *auth,
                                     vectis_response *response,
                                     vectis_error *error) {
  int status_code;
  const char *content_type;
  vectis_bytes body;

  if (auth == NULL) {
    return vectis_response_status(
        response, data != NULL && data->conceal_unauthorized ? 404 : 403,
        error);
  }
  switch (auth->action) {
  case VECTIS_WEBDAV_AUTH_REQUIRED:
    status_code = auth->status_code != 0 ? auth->status_code : 401;
    break;
  case VECTIS_WEBDAV_AUTH_REDIRECT:
    status_code = auth->status_code != 0 ? auth->status_code : 302;
    break;
  case VECTIS_WEBDAV_AUTH_DENY:
  default:
    status_code =
        auth->status_code != 0
            ? auth->status_code
            : (data != NULL && data->conceal_unauthorized ? 404 : 403);
    break;
  }
  if (auth->www_authenticate != NULL &&
      vectis_response_header(response, "www-authenticate",
                             auth->www_authenticate, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (auth->location != NULL &&
      vectis_response_header(response, "location", auth->location, error) !=
          VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (auth->body != NULL && auth->body_size > 0u) {
    content_type =
        auth->content_type != NULL ? auth->content_type : "text/plain";
    body.data = auth->body;
    body.size = auth->body_size;
    return vectis_response_bytes(response, status_code, content_type, body,
                                 error);
  }
  return vectis_response_status(response, status_code, error);
}

static vectis_status vectis_webdav_authenticate_request(
    const vectis_webdav_route_data *data, vectis_request *request,
    vectis_http_method method, const char *resource, vectis_response *response,
    int *authorized, vectis_error *error) {
  vectis_webdav_auth_request auth_request;
  vectis_webdav_auth_response auth_response;
  vectis_status status;

  if (data == NULL || request == NULL || resource == NULL ||
      authorized == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "WebDAV route is invalid");
    return VECTIS_ERR_INVALID;
  }
  *authorized = 0;
  if (!data->auth_required) {
    *authorized = 1;
    return VECTIS_OK;
  }
  vectis_webdav_auth_response_init(&auth_response);
  auth_request.request = request;
  auth_request.method = method;
  auth_request.mount_path_prefix = data->path_prefix;
  auth_request.resource_path = resource;
  status =
      data->auth(&auth_request, &auth_response, data->auth_userdata, error);
  if (status != VECTIS_OK) {
    return status;
  }
  if (auth_response.action == VECTIS_WEBDAV_AUTH_ALLOW) {
    *authorized = 1;
    return VECTIS_OK;
  }
  return vectis_webdav_auth_contract_response(data, &auth_response, response,
                                              error);
}

static vectis_status vectis_webdav_status_response(vectis_webdav_status status,
                                                   vectis_response *response,
                                                   vectis_error *error) {
  switch (status) {
  case VECTIS_WEBDAV_OK:
    return vectis_response_status(response, 204, error);
  case VECTIS_WEBDAV_NOT_FOUND:
    return vectis_response_status(response, 404, error);
  case VECTIS_WEBDAV_EXISTS:
    return vectis_response_status(response, 412, error);
  case VECTIS_WEBDAV_INVALID:
    return vectis_response_status(response, 400, error);
  case VECTIS_WEBDAV_LIMIT:
    return vectis_response_status(response, 413, error);
  case VECTIS_WEBDAV_CONFLICT:
  case VECTIS_WEBDAV_TOMBSTONED:
    return vectis_response_status(response, 409, error);
  default:
    return vectis_response_status(response, 500, error);
  }
}

static vectis_status vectis_webdav_xml_escape(vectis_string_builder *xml,
                                              const char *value,
                                              vectis_error *error) {
  const char *p;

  for (p = value != NULL ? value : ""; *p != '\0'; ++p) {
    switch (*p) {
    case '&':
      if (vectis_string_builder_append(xml, "&amp;", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
      break;
    case '<':
      if (vectis_string_builder_append(xml, "&lt;", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
      break;
    case '>':
      if (vectis_string_builder_append(xml, "&gt;", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
      break;
    case '"':
      if (vectis_string_builder_append(xml, "&quot;", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
      break;
    default:
      if (vectis_string_builder_append_n(xml, p, 1u, error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
      break;
    }
  }
  return VECTIS_OK;
}

static vectis_status vectis_webdav_href_for_resource(
    const vectis_webdav_route_data *data, const char *resource,
    vectis_string_builder *href, vectis_error *error) {
  if (strcmp(data->path_prefix, "/") == 0) {
    return vectis_webdav_xml_escape(href, resource, error);
  }
  if (strcmp(resource, "/") == 0) {
    return vectis_webdav_xml_escape(href, data->path_prefix, error);
  }
  if (vectis_webdav_xml_escape(href, data->path_prefix, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  return vectis_webdav_xml_escape(href, resource, error);
}

static vectis_status
vectis_webdav_append_prop(const vectis_webdav_route_data *data,
                          const char *resource, vectis_webdav_entry_kind kind,
                          size_t size, vectis_string_builder *xml,
                          vectis_error *error) {
  if (vectis_string_builder_append(xml, "<D:response><D:href>", error) !=
          VECTIS_OK ||
      vectis_webdav_href_for_resource(data, resource, xml, error) !=
          VECTIS_OK ||
      vectis_string_builder_append(
          xml, "</D:href><D:propstat><D:prop><D:resourcetype>", error) !=
          VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  if (kind == VECTIS_WEBDAV_ENTRY_COLLECTION &&
      vectis_string_builder_append(xml, "<D:collection/>", error) !=
          VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  if (vectis_string_builder_append(xml, "</D:resourcetype>", error) !=
      VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  if (kind == VECTIS_WEBDAV_ENTRY_FILE &&
      vectis_string_builder_appendf(xml, error,
                                    "<D:getcontentlength>%lu"
                                    "</D:getcontentlength>",
                                    (unsigned long)size) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  return vectis_string_builder_append(
      xml,
      "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
      "</D:response>",
      error);
}

static int vectis_webdav_propfind_entry(const char *path,
                                        vectis_webdav_entry_kind kind,
                                        size_t size, void *userdata) {
  vectis_webdav_propfind_state *state;

  state = (vectis_webdav_propfind_state *)userdata;
  if (state == NULL || state->status != VECTIS_OK) {
    return 0;
  }
  state->status = vectis_webdav_append_prop(state->data, path, kind, size,
                                            state->xml, state->error);
  return state->status == VECTIS_OK;
}

static vectis_status
vectis_webdav_propfind(const vectis_webdav_route_data *data,
                       const char *resource, vectis_request *request,
                       vectis_response *response, vectis_error *error) {
  vectis_webdav_entry entry;
  vectis_webdav_propfind_state state;
  vectis_string_builder xml;
  vectis_status response_status;
  vectis_bytes bytes;
  vectis_webdav_status status;
  const char *depth;

  memset(&xml, 0, sizeof(xml));
  status = vectis_webdav_lookup(&data->storage, resource, &entry);
  if (status != VECTIS_WEBDAV_OK ||
      entry.kind == VECTIS_WEBDAV_ENTRY_TOMBSTONE) {
    return vectis_webdav_status_response(
        entry.kind == VECTIS_WEBDAV_ENTRY_TOMBSTONE ? VECTIS_WEBDAV_NOT_FOUND
                                                    : status,
        response, error);
  }
  if (vectis_string_builder_append(&xml,
                                   "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                                   "<D:multistatus xmlns:D=\"DAV:\">",
                                   error) != VECTIS_OK ||
      vectis_webdav_append_prop(data, resource, entry.kind, entry.size, &xml,
                                error) != VECTIS_OK) {
    vectis_string_builder_cleanup(&xml);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  depth = vectis_request_header(request, "depth");
  if ((depth == NULL || strcmp(depth, "0") != 0) &&
      entry.kind == VECTIS_WEBDAV_ENTRY_COLLECTION) {
    state.data = data;
    state.xml = &xml;
    state.error = error;
    state.status = VECTIS_OK;
    status = vectis_webdav_list(&data->storage, resource,
                                vectis_webdav_propfind_entry, &state);
    if (status != VECTIS_WEBDAV_OK || state.status != VECTIS_OK) {
      vectis_string_builder_cleanup(&xml);
      return status != VECTIS_WEBDAV_OK
                 ? vectis_webdav_status_response(status, response, error)
                 : state.status;
    }
  }
  if (vectis_string_builder_append(&xml, "</D:multistatus>", error) !=
      VECTIS_OK) {
    vectis_string_builder_cleanup(&xml);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  bytes.data = xml.data;
  bytes.size = xml.size;
  response_status = vectis_response_bytes(
      response, 207, "application/xml; charset=utf-8", bytes, error);
  vectis_string_builder_cleanup(&xml);
  return response_status;
}

static int
vectis_webdav_destination_path(const vectis_webdav_route_data *data,
                               const char *destination,
                               char out[VECTIS_WEBDAV_PATH_MAX + 1u]) {
  const char *path;
  const char *scheme;
  const char *end;
  char request_path[VECTIS_WEBDAV_PATH_MAX + 1u];
  size_t length;

  if (destination == NULL) {
    return 0;
  }
  path = destination;
  scheme = strstr(destination, "://");
  if (scheme != NULL) {
    path = strchr(scheme + 3u, '/');
    if (path == NULL) {
      return 0;
    }
  }
  end = strpbrk(path, "?#");
  length = end != NULL ? (size_t)(end - path) : strlen(path);
  if (length == 0u || length > VECTIS_WEBDAV_PATH_MAX) {
    return 0;
  }
  memcpy(request_path, path, length);
  request_path[length] = '\0';
  return vectis_webdav_request_resource_path(data, request_path, out);
}

static vectis_status vectis_webdav_dispatch(vectis_app *app,
                                            vectis_request *request,
                                            vectis_response *response,
                                            void *userdata,
                                            vectis_error *error) {
  vectis_webdav_route_data *data;
  vectis_http_method method;
  vectis_webdav_status webdav_status;
  vectis_webdav_entry entry;
  vectis_mutable_bytes body;
  vectis_bytes response_body;
  const char *path;
  const char *destination;
  const char *overwrite_header;
  char resource[VECTIS_WEBDAV_PATH_MAX + 1u];
  char target[VECTIS_WEBDAV_PATH_MAX + 1u];
  int authorized;
  int overwrite;

  (void)app;
  data = (vectis_webdav_route_data *)userdata;
  path = vectis_request_path(request);
  if (!vectis_webdav_request_resource_path(data, path, resource)) {
    return vectis_response_status(response, 404, error);
  }
  method = vectis_request_method(request);
  if (vectis_webdav_authenticate_request(data, request, method, resource,
                                         response, &authorized,
                                         error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (!authorized) {
    return VECTIS_OK;
  }
  if (method == VECTIS_HTTP_OPTIONS) {
    if (vectis_response_header(response, "dav", "1", error) != VECTIS_OK ||
        vectis_response_header(
            response, "allow",
            "OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL, COPY, MOVE",
            error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_INVALID;
    }
    return vectis_response_status(response, 204, error);
  }
  if (method == VECTIS_HTTP_PROPFIND) {
    return vectis_webdav_propfind(data, resource, request, response, error);
  }
  if (method == VECTIS_HTTP_GET || method == VECTIS_HTTP_HEAD) {
    webdav_status = vectis_webdav_lookup(&data->storage, resource, &entry);
    if (webdav_status != VECTIS_WEBDAV_OK ||
        entry.kind != VECTIS_WEBDAV_ENTRY_FILE) {
      return vectis_response_status(response, 404, error);
    }
    if (vectis_response_header(response, "etag", entry.etag, error) !=
        VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_INVALID;
    }
    if (method == VECTIS_HTTP_HEAD) {
      return vectis_response_status(response, 200, error);
    }
    return vectis_response_file(response, 200, "application/octet-stream",
                                entry.storage_path, error);
  }
  if (method == VECTIS_HTTP_PUT) {
    memset(&body, 0, sizeof(body));
    if (vectis_request_body_read_all(request, &body, error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_INVALID;
    }
    response_body.data = body.data;
    response_body.size = body.size;
    webdav_status = vectis_webdav_put(&data->storage, resource,
                                      (const unsigned char *)response_body.data,
                                      response_body.size);
    vectis_mutable_bytes_cleanup(&body);
    return webdav_status == VECTIS_WEBDAV_OK
               ? vectis_response_status(response, 201, error)
               : vectis_webdav_status_response(webdav_status, response, error);
  }
  if (method == VECTIS_HTTP_DELETE) {
    webdav_status = vectis_webdav_delete(&data->storage, resource);
    return webdav_status == VECTIS_WEBDAV_OK
               ? vectis_response_status(response, 204, error)
               : vectis_webdav_status_response(webdav_status, response, error);
  }
  if (method == VECTIS_HTTP_MKCOL) {
    webdav_status = vectis_webdav_mkcol(&data->storage, resource);
    return webdav_status == VECTIS_WEBDAV_OK
               ? vectis_response_status(response, 201, error)
               : vectis_webdav_status_response(webdav_status, response, error);
  }
  if (method == VECTIS_HTTP_COPY || method == VECTIS_HTTP_MOVE) {
    destination = vectis_request_header(request, "destination");
    if (!vectis_webdav_destination_path(data, destination, target)) {
      return vectis_response_status(response, 400, error);
    }
    overwrite_header = vectis_request_header(request, "overwrite");
    overwrite =
        overwrite_header == NULL || strcasecmp(overwrite_header, "F") != 0;
    webdav_status =
        method == VECTIS_HTTP_COPY
            ? vectis_webdav_copy(&data->storage, resource, target, overwrite)
            : vectis_webdav_move(&data->storage, resource, target, overwrite);
    return webdav_status == VECTIS_WEBDAV_OK
               ? vectis_response_status(response, 201, error)
               : vectis_webdav_status_response(webdav_status, response, error);
  }
  return vectis_response_status(response, 405, error);
}

vectis_status vectis_register_webdav(vectis_app *app,
                                     const vectis_webdav_mount_config *config,
                                     vectis_error *error) {
  vectis_route_config route;
  vectis_webdav_route_data *data;
  vectis_status status;
  char *regex;
  char *path_prefix;

  path_prefix = NULL;
  if (app == NULL || config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "WebDAV app and config are required");
    return VECTIS_ERR_INVALID;
  }
  if (!vectis_webdav_storage_config_present(&config->storage)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "WebDAV storage cache_dir and site_id are required");
    return VECTIS_ERR_INVALID;
  }
  if (!vectis_webdav_prefix_valid(config->path_prefix)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "WebDAV path_prefix is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (config->auth_required && config->auth == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "WebDAV auth callback is required");
    return VECTIS_ERR_INVALID;
  }
  path_prefix =
      vectis_normalize_static_directory_prefix(config->path_prefix, error);
  if (path_prefix == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  if (vectis_validate_route_path(path_prefix, VECTIS_ROUTE_PATH_LITERAL,
                                 error) != VECTIS_OK) {
    free(path_prefix);
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  regex = vectis_static_directory_regex(path_prefix, error);
  if (regex == NULL) {
    free(path_prefix);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  data = vectis_webdav_route_data_new(config, path_prefix, error);
  free(path_prefix);
  if (data == NULL) {
    free(regex);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  vectis_route_config_init(&route);
  route.method = VECTIS_HTTP_OPTIONS;
  route.methods = VECTIS_HTTP_METHODS_OPTIONS | VECTIS_HTTP_METHODS_PROPFIND |
                  VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD |
                  VECTIS_HTTP_METHODS_PUT | VECTIS_HTTP_METHODS_DELETE |
                  VECTIS_HTTP_METHODS_MKCOL | VECTIS_HTTP_METHODS_COPY |
                  VECTIS_HTTP_METHODS_MOVE;
  route.path = regex;
  route.path_kind = VECTIS_ROUTE_PATH_REGEX;
  route.body = vectis_body_buffered_max(config->storage.max_file_bytes);
  route.handler = vectis_webdav_dispatch;
  route.userdata = data;
  status = vectis_app_register_route_owned_userdata(app, &route, 1, error);
  free(regex);
  if (status != VECTIS_OK) {
    free(data);
  }
  return status;
}

vectis_status vectis_register_webdav_site(vectis_app *app,
                                          const char *path_prefix,
                                          const vectis_webdav_config *storage,
                                          vectis_error *error) {
  vectis_webdav_mount_config config;

  if (storage == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "WebDAV storage config is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_webdav_mount_config_init(&config);
  config.path_prefix = path_prefix;
  config.storage = *storage;
  return vectis_register_webdav(app, &config, error);
}

vectis_status vectis_register_webdav_embedded_site(
    vectis_app *app, const vectis_webdav_embedded_site_config *config,
    vectis_error *error) {
  vectis_webdav_mount_config mount;
  vectis_embedded_fs_extract_config extract;
  char content_dir[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  vectis_status status;

  if (app == NULL || config == NULL || config->fs == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "WebDAV embedded site app, config, and fs are required");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_webdav_content_dir(&config->storage, content_dir, error);
  if (status != VECTIS_OK) {
    return status;
  }
  vectis_embedded_fs_extract_config_init(&extract);
  extract.output_dir = content_dir;
  extract.policy = config->extract_policy;
  status = vectis_embedded_fs_extract(config->fs, &extract, error);
  if (status != VECTIS_OK) {
    return status;
  }
  vectis_webdav_mount_config_init(&mount);
  mount.path_prefix = config->path_prefix;
  mount.storage = config->storage;
  mount.auth_required = config->auth_required;
  mount.conceal_unauthorized = config->conceal_unauthorized;
  mount.auth = config->auth;
  mount.auth_userdata = config->auth_userdata;
  return vectis_register_webdav(app, &mount, error);
}

static vectis_auth_route_data *
vectis_auth_route_data_new(const vectis_auth_routes_config *config,
                           const char *path_prefix, vectis_error *error) {
  vectis_auth_route_data *data;
  char *cursor;
  const char **recipient_copy;
  size_t total;
  size_t len;
  size_t i;

  total = sizeof(*data);
  total += config->store.credentials_path != NULL
               ? strlen(config->store.credentials_path) + 1u
               : 0u;
  total += config->store.state_path != NULL
               ? strlen(config->store.state_path) + 1u
               : 0u;
  total += path_prefix != NULL ? strlen(path_prefix) + 1u : 0u;
  total += config->realm != NULL ? strlen(config->realm) + 1u : 0u;
  total += config->login_title != NULL ? strlen(config->login_title) + 1u : 0u;
  total += config->login_template_html != NULL
               ? strlen(config->login_template_html) + 1u
               : 0u;
  total +=
      config->email_smtp.url != NULL ? strlen(config->email_smtp.url) + 1u : 0u;
  total += config->email_smtp.mail_from != NULL
               ? strlen(config->email_smtp.mail_from) + 1u
               : 0u;
  total += config->email_smtp.username != NULL
               ? strlen(config->email_smtp.username) + 1u
               : 0u;
  total += config->email_smtp.password != NULL
               ? strlen(config->email_smtp.password) + 1u
               : 0u;
  total += config->email_smtp.subject != NULL
               ? strlen(config->email_smtp.subject) + 1u
               : 0u;
  total += config->email_smtp.ca_bundle_path != NULL
               ? strlen(config->email_smtp.ca_bundle_path) + 1u
               : 0u;
  total += config->email_smtp.allowed_recipient_domain != NULL
               ? strlen(config->email_smtp.allowed_recipient_domain) + 1u
               : 0u;
  if (config->email_smtp.allowed_recipients != NULL &&
      config->email_smtp.allowed_recipient_count > 0u) {
    total +=
        config->email_smtp.allowed_recipient_count * sizeof(*recipient_copy);
    for (i = 0u; i < config->email_smtp.allowed_recipient_count; ++i) {
      total += config->email_smtp.allowed_recipients[i] != NULL
                   ? strlen(config->email_smtp.allowed_recipients[i]) + 1u
                   : 0u;
    }
  }
  data = (vectis_auth_route_data *)calloc(1u, total);
  if (data == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate auth route data");
    return NULL;
  }
  data->store = config->store;
  data->max_body_bytes = config->max_body_bytes;
  data->unix_seconds = config->unix_seconds;
  data->totp_window = config->totp_window;
  data->required_factors = config->required_factors;
  data->require_email_token = config->require_email_token;
  data->email_token_ttl_seconds = config->email_token_ttl_seconds;
  data->email_token_max_attempts = config->email_token_max_attempts;
  data->pending_login_ttl_seconds = config->pending_login_ttl_seconds;
  data->email_smtp = config->email_smtp;
  cursor = (char *)(data + 1);
  if (config->email_smtp.allowed_recipients != NULL &&
      config->email_smtp.allowed_recipient_count > 0u) {
    recipient_copy = (const char **)(void *)cursor;
    data->email_smtp.allowed_recipients =
        (const char *const *)(void *)recipient_copy;
    cursor +=
        config->email_smtp.allowed_recipient_count * sizeof(*recipient_copy);
  } else {
    recipient_copy = NULL;
    data->email_smtp.allowed_recipients = NULL;
    data->email_smtp.allowed_recipient_count = 0u;
  }
#define VECTIS_COPY_AUTH_ROUTE_FIELD(field, value)                             \
  do {                                                                         \
    if ((value) != NULL) {                                                     \
      len = strlen(value) + 1u;                                                \
      memcpy(cursor, value, len);                                              \
      data->field = cursor;                                                    \
      cursor += len;                                                           \
    }                                                                          \
  } while (0)
  VECTIS_COPY_AUTH_ROUTE_FIELD(store.credentials_path,
                               config->store.credentials_path);
  VECTIS_COPY_AUTH_ROUTE_FIELD(store.state_path, config->store.state_path);
  VECTIS_COPY_AUTH_ROUTE_FIELD(path_prefix, path_prefix);
  VECTIS_COPY_AUTH_ROUTE_FIELD(realm, config->realm);
  VECTIS_COPY_AUTH_ROUTE_FIELD(login_title, config->login_title);
  VECTIS_COPY_AUTH_ROUTE_FIELD(login_template_html,
                               config->login_template_html);
  VECTIS_COPY_AUTH_ROUTE_FIELD(email_smtp.url, config->email_smtp.url);
  VECTIS_COPY_AUTH_ROUTE_FIELD(email_smtp.mail_from,
                               config->email_smtp.mail_from);
  VECTIS_COPY_AUTH_ROUTE_FIELD(email_smtp.username,
                               config->email_smtp.username);
  VECTIS_COPY_AUTH_ROUTE_FIELD(email_smtp.password,
                               config->email_smtp.password);
  VECTIS_COPY_AUTH_ROUTE_FIELD(email_smtp.subject, config->email_smtp.subject);
  VECTIS_COPY_AUTH_ROUTE_FIELD(email_smtp.ca_bundle_path,
                               config->email_smtp.ca_bundle_path);
  VECTIS_COPY_AUTH_ROUTE_FIELD(email_smtp.allowed_recipient_domain,
                               config->email_smtp.allowed_recipient_domain);
  if (recipient_copy != NULL) {
    for (i = 0u; i < config->email_smtp.allowed_recipient_count; ++i) {
      if (config->email_smtp.allowed_recipients[i] != NULL) {
        len = strlen(config->email_smtp.allowed_recipients[i]) + 1u;
        memcpy(cursor, config->email_smtp.allowed_recipients[i], len);
        recipient_copy[i] = cursor;
        cursor += len;
      }
    }
  }
#undef VECTIS_COPY_AUTH_ROUTE_FIELD
  (void)len;
  return data;
}

static int
vectis_auth_template_source_count(const vectis_auth_routes_config *config) {
  int count;

  count = 0;
  if (config->login_template_html != NULL) {
    count++;
  }
  if (config->login_template_path != NULL) {
    count++;
  }
  if (config->login_template_embedded_path != NULL) {
    count++;
  }
  return count;
}

static vectis_status vectis_auth_read_template_file(const char *path,
                                                    char **out,
                                                    vectis_error *error) {
  FILE *file;
  long size;
  char *data;
  size_t nread;

  if (path == NULL || path[0] == '\0' || out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "login template path is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  file = fopen(path, "rb");
  if (file == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_STATE,
                      "failed to open login template path: %s", path);
    return VECTIS_ERR_STATE;
  }
  if (fseek(file, 0L, SEEK_END) != 0) {
    (void)fclose(file);
    vectis_set_errorf(error, VECTIS_ERR_STATE,
                      "failed to seek login template path: %s", path);
    return VECTIS_ERR_STATE;
  }
  size = ftell(file);
  if (size < 0L || fseek(file, 0L, SEEK_SET) != 0) {
    (void)fclose(file);
    vectis_set_errorf(error, VECTIS_ERR_STATE,
                      "failed to size login template path: %s", path);
    return VECTIS_ERR_STATE;
  }
  data = (char *)malloc((size_t)size + 1u);
  if (data == NULL) {
    (void)fclose(file);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate login template");
    return VECTIS_ERR_NOMEM;
  }
  nread = fread(data, 1u, (size_t)size, file);
  if (nread != (size_t)size || ferror(file)) {
    (void)fclose(file);
    free(data);
    vectis_set_errorf(error, VECTIS_ERR_STATE,
                      "failed to read login template path: %s", path);
    return VECTIS_ERR_STATE;
  }
  if (fclose(file) != 0) {
    free(data);
    vectis_set_errorf(error, VECTIS_ERR_STATE,
                      "failed to close login template path: %s", path);
    return VECTIS_ERR_STATE;
  }
  data[(size_t)size] = '\0';
  *out = data;
  return VECTIS_OK;
}

static vectis_status
vectis_auth_resolve_login_template(const vectis_auth_routes_config *config,
                                   char **out, vectis_error *error) {
  vectis_bytes body;
  vectis_status status;
  int found;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "login template output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  if (vectis_auth_template_source_count(config) > 1) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "set only one login template source");
    return VECTIS_ERR_INVALID;
  }
  if (config->login_template_path != NULL) {
    return vectis_auth_read_template_file(config->login_template_path, out,
                                          error);
  }
  if (config->login_template_embedded_path != NULL) {
    if (config->login_template_fs == NULL) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "login template embedded fs is required");
      return VECTIS_ERR_INVALID;
    }
    status = vectis_embedded_fs_read(config->login_template_fs,
                                     config->login_template_embedded_path,
                                     &found, &body, error);
    if (status != VECTIS_OK) {
      return status;
    }
    if (!found) {
      vectis_set_errorf(error, VECTIS_ERR_INVALID,
                        "embedded login template not found: %s",
                        config->login_template_embedded_path);
      return VECTIS_ERR_INVALID;
    }
    *out = (char *)malloc(body.size + 1u);
    if (*out == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to allocate embedded login template");
      return VECTIS_ERR_NOMEM;
    }
    memcpy(*out, body.data, body.size);
    (*out)[body.size] = '\0';
  }
  return VECTIS_OK;
}

static int vectis_auth_form_hex(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

static char *vectis_auth_form_decode(const char *text, size_t size,
                                     vectis_error *error) {
  char *out;
  size_t i;
  size_t j;
  int hi;
  int lo;

  out = (char *)malloc(size + 1u);
  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate decoded form field");
    return NULL;
  }
  i = 0u;
  j = 0u;
  while (i < size) {
    if (text[i] == '+') {
      out[j++] = ' ';
      i++;
    } else if (text[i] == '%') {
      if (i + 2u >= size) {
        free(out);
        vectis_set_error(error, VECTIS_ERR_INVALID,
                         "form field percent escape is incomplete");
        return NULL;
      }
      hi = vectis_auth_form_hex(text[i + 1u]);
      lo = vectis_auth_form_hex(text[i + 2u]);
      if (hi < 0 || lo < 0) {
        free(out);
        vectis_set_error(error, VECTIS_ERR_INVALID,
                         "form field percent escape is invalid");
        return NULL;
      }
      out[j++] = (char)((hi << 4) | lo);
      i += 3u;
    } else {
      out[j++] = text[i++];
    }
  }
  out[j] = '\0';
  return out;
}

static void vectis_auth_form_cleanup(vectis_auth_form_fields *fields) {
  if (fields == NULL) {
    return;
  }
  free(fields->username);
  free(fields->password);
  free(fields->totp_code);
  free(fields->email);
  free(fields->email_transaction_id);
  free(fields->email_token);
  free(fields->pending_transaction_id);
  memset(fields, 0, sizeof(*fields));
}

static vectis_status vectis_auth_form_set(vectis_auth_form_fields *fields,
                                          char *key, char *value) {
  char **target;

  target = NULL;
  if (strcmp(key, "username") == 0) {
    target = &fields->username;
  } else if (strcmp(key, "password") == 0) {
    target = &fields->password;
  } else if (strcmp(key, "totp_code") == 0 || strcmp(key, "totp") == 0) {
    target = &fields->totp_code;
  } else if (strcmp(key, "email") == 0) {
    target = &fields->email;
  } else if (strcmp(key, "email_transaction_id") == 0 ||
             strcmp(key, "transaction_id") == 0) {
    target = &fields->email_transaction_id;
  } else if (strcmp(key, "email_token") == 0 || strcmp(key, "token") == 0) {
    target = &fields->email_token;
  } else if (strcmp(key, "pending_transaction_id") == 0 ||
             strcmp(key, "pending_login_transaction_id") == 0 ||
             strcmp(key, "auth_transaction_id") == 0) {
    target = &fields->pending_transaction_id;
  }
  if (target == NULL) {
    free(value);
    return VECTIS_OK;
  }
  free(*target);
  *target = value;
  return VECTIS_OK;
}

static vectis_status vectis_auth_parse_form(const char *body, size_t size,
                                            vectis_auth_form_fields *fields,
                                            vectis_error *error) {
  size_t cursor;
  size_t pair_end;
  size_t equals;
  char *key;
  char *value;

  if (fields == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "form output is required");
    return VECTIS_ERR_INVALID;
  }
  memset(fields, 0, sizeof(*fields));
  cursor = 0u;
  while (cursor <= size) {
    pair_end = cursor;
    while (pair_end < size && body[pair_end] != '&') {
      pair_end++;
    }
    if (pair_end > cursor) {
      equals = cursor;
      while (equals < pair_end && body[equals] != '=') {
        equals++;
      }
      key = vectis_auth_form_decode(body + cursor, equals - cursor, error);
      if (key == NULL) {
        vectis_auth_form_cleanup(fields);
        return error != NULL ? error->code : VECTIS_ERR_INVALID;
      }
      value = equals < pair_end
                  ? vectis_auth_form_decode(body + equals + 1u,
                                            pair_end - equals - 1u, error)
                  : vectis_strdup("");
      if (value == NULL) {
        free(key);
        vectis_auth_form_cleanup(fields);
        if (error != NULL && error->code != VECTIS_OK) {
          return error->code;
        }
        vectis_set_error(error, VECTIS_ERR_NOMEM,
                         "failed to allocate decoded form value");
        return VECTIS_ERR_NOMEM;
      }
      (void)vectis_auth_form_set(fields, key, value);
      free(key);
    }
    if (pair_end == size) {
      break;
    }
    cursor = pair_end + 1u;
  }
  return VECTIS_OK;
}

static vectis_status vectis_auth_html_escape(vectis_string_builder *builder,
                                             const char *text,
                                             vectis_error *error) {
  const char *p;

  for (p = text != NULL ? text : ""; *p != '\0'; ++p) {
    switch (*p) {
    case '&':
      if (vectis_string_builder_append(builder, "&amp;", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
      break;
    case '<':
      if (vectis_string_builder_append(builder, "&lt;", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
      break;
    case '>':
      if (vectis_string_builder_append(builder, "&gt;", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
      break;
    case '"':
      if (vectis_string_builder_append(builder, "&quot;", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
      break;
    default:
      if (vectis_string_builder_append_n(builder, p, 1u, error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
      break;
    }
  }
  return VECTIS_OK;
}

static int vectis_auth_template_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f';
}

static int vectis_auth_template_name_equals(const char *name, size_t name_len,
                                            const char *expected) {
  size_t expected_len;

  expected_len = strlen(expected);
  return name_len == expected_len && strncmp(name, expected, name_len) == 0;
}

static vectis_status
vectis_auth_template_action(vectis_string_builder *builder,
                            const vectis_auth_route_data *data,
                            const char *suffix, vectis_error *error) {
  vectis_string_builder action;
  vectis_status status;

  memset(&action, 0, sizeof(action));
  status = vectis_string_builder_append(
      &action, data->path_prefix != NULL ? data->path_prefix : "", error);
  if (status == VECTIS_OK) {
    status = vectis_string_builder_append(&action, suffix, error);
  }
  if (status == VECTIS_OK) {
    status = vectis_auth_html_escape(builder, action.data, error);
  }
  vectis_string_builder_cleanup(&action);
  return status;
}

static vectis_status vectis_auth_template_placeholder(
    vectis_string_builder *builder, const vectis_auth_route_data *data,
    const char *name, size_t name_len, int *matched, vectis_error *error) {
  const char *value;

  *matched = 1;
  value = NULL;
  if (vectis_auth_template_name_equals(name, name_len, "login_title")) {
    value = data->login_title;
  } else if (vectis_auth_template_name_equals(name, name_len, "realm")) {
    value = data->realm;
  } else if (vectis_auth_template_name_equals(name, name_len, "path_prefix")) {
    value = data->path_prefix;
  } else if (vectis_auth_template_name_equals(name, name_len,
                                              "email_token_action")) {
    return vectis_auth_template_action(builder, data, "/email-token", error);
  } else if (vectis_auth_template_name_equals(name, name_len,
                                              "continue_action")) {
    return vectis_auth_template_action(builder, data, "/continue", error);
  } else if (vectis_auth_template_name_equals(name, name_len,
                                              "webdav_key_action")) {
    return vectis_auth_template_action(builder, data, "/webdav-key", error);
  } else {
    *matched = 0;
    return VECTIS_OK;
  }
  return vectis_auth_html_escape(builder, value, error);
}

static vectis_status
vectis_auth_render_login_template(const vectis_auth_route_data *data,
                                  vectis_string_builder *html,
                                  vectis_error *error) {
  const char *cursor;
  const char *open;
  const char *close;
  const char *name_start;
  const char *name_end;
  vectis_status status;
  size_t literal_len;
  int matched;

  cursor = data->login_template_html;
  while (cursor != NULL && *cursor != '\0') {
    open = strstr(cursor, "{{");
    if (open == NULL) {
      return vectis_string_builder_append(html, cursor, error);
    }
    literal_len = (size_t)(open - cursor);
    status = vectis_string_builder_append_n(html, cursor, literal_len, error);
    if (status != VECTIS_OK) {
      return status;
    }
    close = strstr(open + 2, "}}");
    if (close == NULL) {
      return vectis_string_builder_append(html, open, error);
    }
    name_start = open + 2;
    name_end = close;
    while (name_start < name_end && vectis_auth_template_space(*name_start)) {
      name_start++;
    }
    while (name_end > name_start && vectis_auth_template_space(name_end[-1])) {
      name_end--;
    }
    status = vectis_auth_template_placeholder(html, data, name_start,
                                              (size_t)(name_end - name_start),
                                              &matched, error);
    if (status != VECTIS_OK) {
      return status;
    }
    if (!matched) {
      status = vectis_string_builder_append_n(
          html, open, (size_t)((close + 2) - open), error);
      if (status != VECTIS_OK) {
        return status;
      }
    }
    cursor = close + 2;
  }
  return VECTIS_OK;
}

static unsigned int
vectis_auth_route_required_factors(const vectis_auth_route_data *data) {
  unsigned int required_factors;

  required_factors = data->required_factors != 0u
                         ? data->required_factors
                         : VECTIS_AUTH_ROUTE_FACTOR_PASSWORD;
  if (data->require_email_token) {
    required_factors |= VECTIS_AUTH_ROUTE_FACTOR_EMAIL_TOKEN;
  }
  return required_factors;
}

static int vectis_auth_route_required_factors_valid(unsigned int factors) {
  if ((factors & ~(VECTIS_AUTH_ROUTE_FACTOR_PASSWORD |
                   VECTIS_AUTH_ROUTE_FACTOR_EMAIL_TOKEN |
                   VECTIS_AUTH_ROUTE_FACTOR_TOTP)) != 0u) {
    return 0;
  }
  if ((factors & VECTIS_AUTH_ROUTE_FACTOR_TOTP) != 0u &&
      (factors & VECTIS_AUTH_ROUTE_FACTOR_PASSWORD) == 0u) {
    return 0;
  }
  return 1;
}

static vectis_status
vectis_auth_login_form_response(const vectis_auth_route_data *data,
                                vectis_response *response,
                                vectis_error *error) {
  vectis_string_builder html;
  vectis_bytes body;
  vectis_status status;
  unsigned int required_factors;

  if (data->login_template_html != NULL) {
    memset(&html, 0, sizeof(html));
    status = vectis_auth_render_login_template(data, &html, error);
    if (status != VECTIS_OK) {
      vectis_string_builder_cleanup(&html);
      return status;
    }
    body.data = html.data;
    body.size = html.size;
    status = vectis_response_bytes(response, 200, "text/html; charset=utf-8",
                                   body, error);
    vectis_string_builder_cleanup(&html);
    return status;
  }
  required_factors = vectis_auth_route_required_factors(data);
  memset(&html, 0, sizeof(html));
  status = vectis_string_builder_append(
      &html, "<!doctype html><html><head><meta charset=\"utf-8\"><title>",
      error);
  if (status == VECTIS_OK) {
    status = vectis_auth_html_escape(&html, data->login_title, error);
  }
  if (status == VECTIS_OK) {
    status = vectis_string_builder_append(
        &html, "</title></head><body><main><h1>", error);
  }
  if (status == VECTIS_OK) {
    status = vectis_auth_html_escape(&html, data->login_title, error);
  }
  if (status == VECTIS_OK) {
    status = vectis_string_builder_append(
        &html, "</h1><form method=\"post\" action=\"", error);
  }
  if (status == VECTIS_OK) {
    status = vectis_auth_template_action(&html, data, "/continue", error);
  }
  if (status == VECTIS_OK) {
    status = vectis_string_builder_append(
        &html,
        "\"><label>Username <input name=\"username\" "
        "autocomplete=\"username\"></label>",
        error);
  }
  if (status == VECTIS_OK &&
      (required_factors & VECTIS_AUTH_ROUTE_FACTOR_PASSWORD) != 0u) {
    status = vectis_string_builder_append(
        &html,
        "<label>Password <input type=\"password\" name=\"password\" "
        "autocomplete=\"current-password\"></label><label>TOTP <input "
        "name=\"totp_code\" inputmode=\"numeric\" "
        "autocomplete=\"one-time-code\"></label>",
        error);
  }
  if (status == VECTIS_OK &&
      (required_factors & VECTIS_AUTH_ROUTE_FACTOR_EMAIL_TOKEN) != 0u) {
    status = vectis_string_builder_append(
        &html,
        "<label>Email transaction <input name=\"email_transaction_id\" "
        "autocomplete=\"one-time-code\"></label><label>Email token <input "
        "name=\"email_token\" autocomplete=\"one-time-code\"></label>",
        error);
  }
  if (status == VECTIS_OK) {
    status = vectis_string_builder_append(
        &html, "<button type=\"submit\">Create WebDAV key</button></form>",
        error);
  }
  if (status == VECTIS_OK &&
      (required_factors & VECTIS_AUTH_ROUTE_FACTOR_EMAIL_TOKEN) != 0u) {
    status = vectis_string_builder_append(
        &html, "<form method=\"post\" action=\"", error);
  }
  if (status == VECTIS_OK &&
      (required_factors & VECTIS_AUTH_ROUTE_FACTOR_EMAIL_TOKEN) != 0u) {
    status = vectis_auth_template_action(&html, data, "/email-token", error);
  }
  if (status == VECTIS_OK &&
      (required_factors & VECTIS_AUTH_ROUTE_FACTOR_EMAIL_TOKEN) != 0u) {
    status = vectis_string_builder_append(
        &html,
        "\"><label>Username <input name=\"username\" "
        "autocomplete=\"username\"></label><label>Email <input "
        "type=\"email\" name=\"email\" autocomplete=\"email\"></label>"
        "<button type=\"submit\">Send email token</button></form>",
        error);
  }
  if (status == VECTIS_OK) {
    status =
        vectis_string_builder_append(&html, "</main></body></html>", error);
  }
  if (status != VECTIS_OK) {
    vectis_string_builder_cleanup(&html);
    return status;
  }
  body.data = html.data;
  body.size = html.size;
  status = vectis_response_bytes(response, 200, "text/html; charset=utf-8",
                                 body, error);
  vectis_string_builder_cleanup(&html);
  return status;
}

static vectis_status vectis_auth_no_store(vectis_response *response,
                                          vectis_error *error) {
  vectis_status status;

  status = vectis_response_header(response, "cache-control", "no-store", error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_response_header(response, "pragma", "no-cache", error);
  if (status != VECTIS_OK) {
    return status;
  }
  return vectis_response_header(response, "expires", "0", error);
}

static vectis_status
vectis_auth_webdav_key_response(vectis_auth_issued_credential *credential,
                                vectis_response *response,
                                vectis_error *error) {
  vectis_string_builder text;
  vectis_bytes body;
  vectis_status status;

  memset(&text, 0, sizeof(text));
  status = vectis_string_builder_appendf(
      &text, error, "client_id=%s\nclient_secret=%s\n",
      credential->client_id != NULL ? credential->client_id : "",
      credential->client_secret != NULL ? credential->client_secret : "");
  if (status == VECTIS_OK && credential->claim_json != NULL) {
    status = vectis_string_builder_append(&text, "claim_json=", error);
    if (status == VECTIS_OK) {
      status =
          vectis_string_builder_append(&text, credential->claim_json, error);
    }
    if (status == VECTIS_OK) {
      status = vectis_string_builder_append(&text, "\n", error);
    }
  }
  if (status != VECTIS_OK) {
    vectis_string_builder_cleanup(&text);
    return status;
  }
  body.data = text.data;
  body.size = text.size;
  status = vectis_response_bytes(response, 200, "text/plain; charset=utf-8",
                                 body, error);
  vectis_string_builder_cleanup(&text);
  return status;
}

static vectis_status vectis_auth_email_token_route_response(
    vectis_auth_email_token *token, int include_token,
    vectis_response *response, vectis_error *error) {
  vectis_string_builder text;
  vectis_bytes body;
  vectis_status status;

  memset(&text, 0, sizeof(text));
  status = vectis_string_builder_appendf(
      &text, error, "transaction_id=%s\n",
      token->transaction_id != NULL ? token->transaction_id : "");
  if (status == VECTIS_OK && include_token) {
    status = vectis_string_builder_appendf(
        &text, error, "token=%s\n", token->token != NULL ? token->token : "");
  }
  if (status == VECTIS_OK) {
    status =
        vectis_string_builder_appendf(&text, error, "expires_at=%llu\n",
                                      (unsigned long long)token->expires_at);
  }
  if (status != VECTIS_OK) {
    vectis_string_builder_cleanup(&text);
    return status;
  }
  body.data = text.data;
  body.size = text.size;
  status = vectis_response_bytes(response, 200, "text/plain; charset=utf-8",
                                 body, error);
  vectis_string_builder_cleanup(&text);
  return status;
}

static vectis_status
vectis_auth_pending_login_response(const vectis_auth_pending_login *pending,
                                   vectis_response *response,
                                   vectis_error *error) {
  vectis_string_builder text;
  vectis_bytes body;
  vectis_status status;

  memset(&text, 0, sizeof(text));
  status = vectis_string_builder_appendf(
      &text, error, "pending_transaction_id=%s\n",
      pending->transaction_id != NULL ? pending->transaction_id : "");
  if (status == VECTIS_OK) {
    status =
        vectis_string_builder_appendf(&text, error, "expires_at=%llu\n",
                                      (unsigned long long)pending->expires_at);
  }
  if (status == VECTIS_OK) {
    status = vectis_string_builder_appendf(&text, error, "totp_required=%d\n",
                                           pending->totp_required ? 1 : 0);
  }
  if (status != VECTIS_OK) {
    vectis_string_builder_cleanup(&text);
    return status;
  }
  body.data = text.data;
  body.size = text.size;
  status = vectis_response_bytes(response, 202, "text/plain; charset=utf-8",
                                 body, error);
  vectis_string_builder_cleanup(&text);
  return status;
}

static vectis_status vectis_auth_login_dispatch(vectis_app *app,
                                                vectis_request *request,
                                                vectis_response *response,
                                                void *userdata,
                                                vectis_error *error) {
  vectis_auth_route_data *data;
  vectis_status status;

  (void)app;
  (void)request;
  data = (vectis_auth_route_data *)userdata;
  if (data == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "auth route is invalid");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_auth_no_store(response, error);
  if (status != VECTIS_OK) {
    return status;
  }
  return vectis_auth_login_form_response(data, response, error);
}

static vectis_status vectis_auth_email_token_dispatch(vectis_app *app,
                                                      vectis_request *request,
                                                      vectis_response *response,
                                                      void *userdata,
                                                      vectis_error *error) {
  vectis_auth_route_data *data;
  vectis_mutable_bytes body;
  vectis_auth_form_fields fields;
  vectis_auth_email_token_issue_config issue;
  vectis_auth_email_token token;
  vectis_auth_email_message message;
  const char *content_type;
  vectis_status status;
  int smtp_enabled;
  int user_exists;

  (void)app;
  data = (vectis_auth_route_data *)userdata;
  if (data == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "auth route is invalid");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_auth_no_store(response, error);
  if (status != VECTIS_OK) {
    return status;
  }
  content_type = vectis_request_header(request, "content-type");
  if (content_type != NULL &&
      strncasecmp(content_type, "application/x-www-form-urlencoded",
                  strlen("application/x-www-form-urlencoded")) != 0) {
    return vectis_response_status(response, 415, error);
  }
  memset(&body, 0, sizeof(body));
  status = vectis_request_body_read_all(request, &body, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_auth_parse_form((const char *)body.data, body.size, &fields,
                                  error);
  vectis_mutable_bytes_cleanup(&body);
  if (status != VECTIS_OK) {
    return status;
  }
  if (fields.username == NULL || fields.username[0] == '\0' ||
      fields.email == NULL || fields.email[0] == '\0') {
    vectis_auth_form_cleanup(&fields);
    return vectis_response_text(response, 400, "text/plain; charset=utf-8",
                                "username and email are required\n", error);
  }
  user_exists = 0;
  status = vectis_auth_user_exists(&data->store, fields.username, &user_exists,
                                   error);
  if (status != VECTIS_OK) {
    vectis_auth_form_cleanup(&fields);
    return status;
  }
  if (!user_exists) {
    vectis_auth_form_cleanup(&fields);
    vectis_error_clear(error);
    return vectis_response_text(response, 401, "text/plain; charset=utf-8",
                                "login failed\n", error);
  }
  vectis_auth_email_token_issue_config_init(&issue);
  issue.store = data->store;
  issue.username = fields.username;
  issue.realm = data->realm;
  issue.email = fields.email;
  issue.pending_transaction_id = fields.pending_transaction_id;
  issue.now_seconds = data->unix_seconds;
  issue.ttl_seconds = data->email_token_ttl_seconds;
  issue.max_attempts = data->email_token_max_attempts;
  vectis_auth_email_token_init(&token);
  status = vectis_auth_email_token_issue(&issue, &token, error);
  if (status != VECTIS_OK) {
    vectis_auth_form_cleanup(&fields);
    vectis_auth_email_token_cleanup(&token);
    return status;
  }
  smtp_enabled =
      data->email_smtp.url != NULL && data->email_smtp.url[0] != '\0';
  if (smtp_enabled) {
    memset(&message, 0, sizeof(message));
    message.username = fields.username;
    message.realm = data->realm;
    message.email = fields.email;
    message.transaction_id = token.transaction_id;
    message.token = token.token;
    message.expires_at = token.expires_at;
    status = vectis_auth_email_token_deliver_smtp(&data->email_smtp, &message,
                                                  error);
    if (status != VECTIS_OK) {
      vectis_auth_email_token_verify_config consume;
      vectis_auth_email_token_result consume_result;
      const char *message_body;

      vectis_auth_email_token_verify_config_init(&consume);
      consume.store = data->store;
      consume.transaction_id = token.transaction_id;
      consume.username = fields.username;
      consume.realm = data->realm;
      consume.pending_transaction_id = fields.pending_transaction_id;
      consume.token = token.token;
      consume.now_seconds = data->unix_seconds;
      vectis_auth_email_token_result_init(&consume_result);
      (void)vectis_auth_email_token_verify(&consume, &consume_result, NULL);
      vectis_auth_email_token_result_cleanup(&consume_result);
      vectis_auth_form_cleanup(&fields);
      vectis_auth_email_token_cleanup(&token);
      if (status == VECTIS_ERR_INVALID) {
        message_body = error != NULL && error->message[0] != '\0'
                           ? error->message
                           : "email token delivery failed";
        return vectis_response_text(response, 400, "text/plain; charset=utf-8",
                                    message_body, error);
      }
      return status;
    }
  }
  vectis_auth_form_cleanup(&fields);
  status = vectis_auth_email_token_route_response(&token, !smtp_enabled,
                                                  response, error);
  vectis_auth_email_token_cleanup(&token);
  return status;
}

static vectis_status vectis_auth_webdav_key_dispatch(vectis_app *app,
                                                     vectis_request *request,
                                                     vectis_response *response,
                                                     void *userdata,
                                                     vectis_error *error) {
  vectis_auth_route_data *data;
  vectis_mutable_bytes body;
  vectis_auth_form_fields fields;
  vectis_auth_login_config login;
  vectis_auth_result login_result;
  vectis_auth_password_check_config password_check;
  vectis_auth_password_check_result password_result;
  vectis_auth_pending_login_issue_config pending_issue;
  vectis_auth_pending_login pending_login;
  vectis_auth_pending_login_consume_config pending_consume;
  vectis_auth_pending_login_result pending_result;
  vectis_auth_email_token_verify_config email_token;
  vectis_auth_email_token_result email_result;
  vectis_auth_issue_config issue;
  vectis_auth_issued_credential credential;
  const char *content_type;
  vectis_status status;
  unsigned int required_factors;
  int has_pending;
  int missing_email_token;
  int needs_pending;
  int pending_verified;

  (void)app;
  data = (vectis_auth_route_data *)userdata;
  if (data == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "auth route is invalid");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_auth_no_store(response, error);
  if (status != VECTIS_OK) {
    return status;
  }
  content_type = vectis_request_header(request, "content-type");
  if (content_type != NULL &&
      strncasecmp(content_type, "application/x-www-form-urlencoded",
                  strlen("application/x-www-form-urlencoded")) != 0) {
    return vectis_response_status(response, 415, error);
  }
  memset(&body, 0, sizeof(body));
  status = vectis_request_body_read_all(request, &body, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_auth_parse_form((const char *)body.data, body.size, &fields,
                                  error);
  vectis_mutable_bytes_cleanup(&body);
  if (status != VECTIS_OK) {
    return status;
  }
  required_factors = vectis_auth_route_required_factors(data);
  if (!vectis_auth_route_required_factors_valid(required_factors)) {
    vectis_auth_form_cleanup(&fields);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth route required_factors is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (fields.username == NULL || fields.username[0] == '\0') {
    vectis_auth_form_cleanup(&fields);
    return vectis_response_text(response, 400, "text/plain; charset=utf-8",
                                "username is required\n", error);
  }
  has_pending = fields.pending_transaction_id != NULL &&
                fields.pending_transaction_id[0] != '\0';
  pending_verified = 0;
  missing_email_token =
      (required_factors & VECTIS_AUTH_ROUTE_FACTOR_EMAIL_TOKEN) != 0u &&
      (fields.email_transaction_id == NULL ||
       fields.email_transaction_id[0] == '\0' || fields.email_token == NULL ||
       fields.email_token[0] == '\0');
  if (has_pending && missing_email_token) {
    vectis_auth_form_cleanup(&fields);
    return vectis_response_text(
        response, 400, "text/plain; charset=utf-8",
        "email_transaction_id and email_token are required\n", error);
  }
  if ((required_factors & VECTIS_AUTH_ROUTE_FACTOR_PASSWORD) != 0u &&
      !has_pending && (fields.password == NULL || fields.password[0] == '\0')) {
    vectis_auth_form_cleanup(&fields);
    return vectis_response_text(response, 400, "text/plain; charset=utf-8",
                                "password or pending_transaction_id is "
                                "required\n",
                                error);
  }
  vectis_auth_login_config_init(&login);
  login.username = fields.username;
  login.password = fields.password;
  login.totp_code = fields.totp_code;
  login.unix_seconds = data->unix_seconds;
  if (data->totp_window != 0u) {
    login.totp_window = data->totp_window;
  }
  vectis_auth_issued_credential_init(&credential);
  if ((required_factors & VECTIS_AUTH_ROUTE_FACTOR_PASSWORD) != 0u) {
    if (has_pending) {
      vectis_auth_pending_login_consume_config_init(&pending_consume);
      pending_consume.store = data->store;
      pending_consume.transaction_id = fields.pending_transaction_id;
      pending_consume.username = fields.username;
      pending_consume.realm = data->realm;
      pending_consume.totp_code = fields.totp_code;
      pending_consume.now_seconds = data->unix_seconds;
      if (data->totp_window != 0u) {
        pending_consume.totp_window = data->totp_window;
      }
      vectis_auth_pending_login_result_init(&pending_result);
      status = vectis_auth_pending_login_verify(&pending_consume,
                                                &pending_result, error);
      if (status != VECTIS_OK) {
        vectis_auth_form_cleanup(&fields);
        vectis_auth_pending_login_result_cleanup(&pending_result);
        vectis_auth_issued_credential_cleanup(&credential);
        return status;
      }
      if (!pending_result.authenticated) {
        if (pending_result.expired ||
            (fields.totp_code != NULL && fields.totp_code[0] != '\0')) {
          vectis_auth_pending_login_result_cleanup(&pending_result);
          status = vectis_auth_pending_login_consume(&pending_consume,
                                                     &pending_result, error);
          if (status != VECTIS_OK) {
            vectis_auth_form_cleanup(&fields);
            vectis_auth_pending_login_result_cleanup(&pending_result);
            vectis_auth_issued_credential_cleanup(&credential);
            return status;
          }
        }
        vectis_auth_form_cleanup(&fields);
        vectis_auth_pending_login_result_cleanup(&pending_result);
        vectis_auth_issued_credential_cleanup(&credential);
        vectis_error_clear(error);
        return vectis_response_text(response, 401, "text/plain; charset=utf-8",
                                    "login failed\n", error);
      }
      pending_verified = 1;
      if ((required_factors & VECTIS_AUTH_ROUTE_FACTOR_TOTP) != 0u &&
          !pending_result.totp_required) {
        vectis_auth_pending_login_result_cleanup(&pending_result);
        vectis_auth_form_cleanup(&fields);
        vectis_auth_issued_credential_cleanup(&credential);
        vectis_error_clear(error);
        return vectis_response_text(response, 401, "text/plain; charset=utf-8",
                                    "login failed\n", error);
      }
      vectis_auth_pending_login_result_cleanup(&pending_result);
    } else {
      vectis_auth_password_check_config_init(&password_check);
      password_check.store = data->store;
      password_check.username = fields.username;
      password_check.password = fields.password;
      vectis_auth_password_check_result_init(&password_result);
      status = vectis_auth_user_password_check(&password_check,
                                               &password_result, error);
      if (status != VECTIS_OK) {
        vectis_auth_form_cleanup(&fields);
        vectis_auth_issued_credential_cleanup(&credential);
        return status;
      }
      if (!password_result.authenticated) {
        vectis_auth_form_cleanup(&fields);
        vectis_auth_issued_credential_cleanup(&credential);
        vectis_error_clear(error);
        return vectis_response_text(response, 401, "text/plain; charset=utf-8",
                                    "login failed\n", error);
      }
      if ((required_factors & VECTIS_AUTH_ROUTE_FACTOR_TOTP) != 0u &&
          !password_result.totp_required) {
        vectis_auth_form_cleanup(&fields);
        vectis_auth_issued_credential_cleanup(&credential);
        vectis_error_clear(error);
        return vectis_response_text(response, 401, "text/plain; charset=utf-8",
                                    "login failed\n", error);
      }
      needs_pending =
          missing_email_token ||
          ((password_result.totp_required ||
            (required_factors & VECTIS_AUTH_ROUTE_FACTOR_TOTP) != 0u) &&
           (fields.totp_code == NULL || fields.totp_code[0] == '\0'));
      if (needs_pending) {
        vectis_auth_pending_login_issue_config_init(&pending_issue);
        pending_issue.store = data->store;
        pending_issue.username = fields.username;
        pending_issue.password = fields.password;
        pending_issue.realm = data->realm;
        pending_issue.now_seconds = data->unix_seconds;
        pending_issue.ttl_seconds = data->pending_login_ttl_seconds;
        vectis_auth_pending_login_init(&pending_login);
        status = vectis_auth_pending_login_issue(&pending_issue, &pending_login,
                                                 error);
        if (status != VECTIS_OK) {
          vectis_auth_form_cleanup(&fields);
          vectis_auth_pending_login_cleanup(&pending_login);
          vectis_auth_issued_credential_cleanup(&credential);
          return status;
        }
        if (!pending_login.authenticated) {
          vectis_auth_form_cleanup(&fields);
          vectis_auth_pending_login_cleanup(&pending_login);
          vectis_auth_issued_credential_cleanup(&credential);
          vectis_error_clear(error);
          return vectis_response_text(response, 401,
                                      "text/plain; charset=utf-8",
                                      "login failed\n", error);
        }
        status =
            vectis_auth_pending_login_response(&pending_login, response, error);
        vectis_auth_form_cleanup(&fields);
        vectis_auth_pending_login_cleanup(&pending_login);
        vectis_auth_issued_credential_cleanup(&credential);
        return status;
      }
      vectis_auth_result_init(&login_result);
      status =
          vectis_auth_user_login(&data->store, &login, &login_result, error);
      if (status != VECTIS_OK) {
        vectis_auth_form_cleanup(&fields);
        vectis_auth_result_cleanup(&login_result);
        vectis_auth_issued_credential_cleanup(&credential);
        return status;
      }
      if (!login_result.authenticated) {
        vectis_auth_form_cleanup(&fields);
        vectis_auth_result_cleanup(&login_result);
        vectis_auth_issued_credential_cleanup(&credential);
        vectis_error_clear(error);
        return vectis_response_text(response, 401, "text/plain; charset=utf-8",
                                    "login failed\n", error);
      }
      vectis_auth_result_cleanup(&login_result);
    }
  }
  if ((required_factors & VECTIS_AUTH_ROUTE_FACTOR_EMAIL_TOKEN) != 0u) {
    if (missing_email_token) {
      vectis_auth_form_cleanup(&fields);
      vectis_auth_issued_credential_cleanup(&credential);
      return vectis_response_text(
          response, 400, "text/plain; charset=utf-8",
          "email_transaction_id and email_token are required\n", error);
    }
    vectis_auth_email_token_verify_config_init(&email_token);
    email_token.store = data->store;
    email_token.transaction_id = fields.email_transaction_id;
    email_token.username = fields.username;
    email_token.realm = data->realm;
    email_token.pending_transaction_id = fields.pending_transaction_id;
    email_token.token = fields.email_token;
    email_token.now_seconds = data->unix_seconds;
    vectis_auth_email_token_result_init(&email_result);
    status = vectis_auth_email_token_verify(&email_token, &email_result, error);
    if (status != VECTIS_OK) {
      vectis_auth_form_cleanup(&fields);
      vectis_auth_email_token_result_cleanup(&email_result);
      vectis_auth_issued_credential_cleanup(&credential);
      return status;
    }
    if (!email_result.verified) {
      vectis_auth_form_cleanup(&fields);
      vectis_auth_email_token_result_cleanup(&email_result);
      vectis_auth_issued_credential_cleanup(&credential);
      vectis_error_clear(error);
      return vectis_response_text(response, 401, "text/plain; charset=utf-8",
                                  "login failed\n", error);
    }
    vectis_auth_email_token_result_cleanup(&email_result);
  }
  if (pending_verified) {
    vectis_auth_pending_login_result_init(&pending_result);
    status = vectis_auth_pending_login_consume(&pending_consume,
                                               &pending_result, error);
    if (status != VECTIS_OK) {
      vectis_auth_form_cleanup(&fields);
      vectis_auth_pending_login_result_cleanup(&pending_result);
      vectis_auth_issued_credential_cleanup(&credential);
      return status;
    }
    if (!pending_result.authenticated) {
      vectis_auth_form_cleanup(&fields);
      vectis_auth_pending_login_result_cleanup(&pending_result);
      vectis_auth_issued_credential_cleanup(&credential);
      vectis_error_clear(error);
      return vectis_response_text(response, 401, "text/plain; charset=utf-8",
                                  "login failed\n", error);
    }
    vectis_auth_pending_login_result_cleanup(&pending_result);
  }
  vectis_auth_issue_config_init(&issue);
  issue.subject = fields.username;
  issue.purpose = "webdav";
  issue.auth_modes = VECTIS_AUTH_MODE_BASIC;
  status =
      vectis_auth_issue_credential(&data->store, &issue, &credential, error);
  vectis_auth_form_cleanup(&fields);
  if (status != VECTIS_OK) {
    vectis_auth_issued_credential_cleanup(&credential);
    if (status == VECTIS_ERR_INVALID) {
      vectis_error_clear(error);
      return vectis_response_text(response, 401, "text/plain; charset=utf-8",
                                  "login failed\n", error);
    }
    return status;
  }
  if (credential.client_id == NULL || credential.client_secret == NULL) {
    vectis_auth_issued_credential_cleanup(&credential);
    return vectis_response_status(response, 401, error);
  }
  status = vectis_auth_webdav_key_response(&credential, response, error);
  vectis_auth_issued_credential_cleanup(&credential);
  return status;
}

static vectis_status vectis_auth_logout_dispatch(vectis_app *app,
                                                 vectis_request *request,
                                                 vectis_response *response,
                                                 void *userdata,
                                                 vectis_error *error) {
  vectis_auth_route_data *data;
  vectis_auth_result result;
  const char *authorization;
  vectis_status status;

  (void)app;
  data = (vectis_auth_route_data *)userdata;
  if (data == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "auth route is invalid");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_auth_no_store(response, error);
  if (status != VECTIS_OK) {
    return status;
  }
  authorization = vectis_request_header(request, "authorization");
  if (authorization == NULL || authorization[0] == '\0') {
    if (vectis_response_header(response, "www-authenticate", "Basic", error) !=
        VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_INVALID;
    }
    return vectis_response_text(response, 401, "text/plain; charset=utf-8",
                                "authorization is required\n", error);
  }
  vectis_auth_result_init(&result);
  status = vectis_auth_verify_authorization(
      &data->store, authorization,
      VECTIS_AUTH_MODE_BASIC | VECTIS_AUTH_MODE_BEARER, &result, error);
  if (status != VECTIS_OK) {
    vectis_auth_result_cleanup(&result);
    return status;
  }
  if (!result.authenticated || result.client_id == NULL) {
    vectis_auth_result_cleanup(&result);
    if (vectis_response_header(response, "www-authenticate", "Basic", error) !=
        VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_INVALID;
    }
    return vectis_response_text(response, 401, "text/plain; charset=utf-8",
                                "login failed\n", error);
  }
  status = vectis_auth_revoke_client(&data->store, result.client_id, error);
  vectis_auth_result_cleanup(&result);
  if (status != VECTIS_OK) {
    return status;
  }
  return vectis_response_text(response, 200, "text/plain; charset=utf-8",
                              "logged_out=1\n", error);
}

static char *vectis_auth_route_prefix_normalize(const char *prefix,
                                                vectis_error *error) {
  char *normalized;
  size_t len;

  if (prefix == NULL || prefix[0] != '/') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth route path_prefix is required");
    return NULL;
  }
  len = strlen(prefix);
  while (len > 1u && prefix[len - 1u] == '/') {
    len--;
  }
  normalized = (char *)malloc(len + 1u);
  if (normalized == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to normalize auth route path_prefix");
    return NULL;
  }
  memcpy(normalized, prefix, len);
  normalized[len] = '\0';
  return normalized;
}

static vectis_status vectis_register_auth_route_one(
    vectis_app *app, const vectis_auth_routes_config *config,
    const char *path_prefix, const char *suffix, vectis_http_method method,
    vectis_route_handler_fn handler, vectis_error *error) {
  vectis_route_config route;
  vectis_auth_route_data *data;
  vectis_status status;
  char *path;

  path = vectis_join_route_prefix(path_prefix, suffix, error);
  if (path == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  data = vectis_auth_route_data_new(config, path_prefix, error);
  if (data == NULL) {
    free(path);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  route = vectis_route(method, path, handler, data);
  if (method == VECTIS_HTTP_POST) {
    route.body = vectis_body_buffered_max(
        config->max_body_bytes > 0u ? config->max_body_bytes : 8192u);
  }
  status = vectis_app_register_route_owned_userdata(app, &route, 1, error);
  free(path);
  if (status != VECTIS_OK) {
    free(data);
  }
  return status;
}

vectis_status
vectis_register_auth_routes(vectis_app *app,
                            const vectis_auth_routes_config *config,
                            vectis_error *error) {
  vectis_auth_routes_config defaults;
  const vectis_auth_routes_config *effective;
  vectis_auth_routes_config resolved;
  vectis_status status;
  char *login_template_html;
  char *path_prefix;
  unsigned int required_factors;

  login_template_html = NULL;
  if (app == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  if (config == NULL) {
    vectis_auth_routes_config_init(&defaults);
    effective = &defaults;
  } else {
    effective = config;
  }
  if (effective->store.credentials_path == NULL ||
      effective->store.credentials_path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth route credentials_path is required");
    return VECTIS_ERR_INVALID;
  }
  path_prefix =
      vectis_auth_route_prefix_normalize(effective->path_prefix, error);
  if (path_prefix == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (vectis_validate_route_path(path_prefix, VECTIS_ROUTE_PATH_LITERAL,
                                 error) != VECTIS_OK) {
    free(path_prefix);
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  required_factors = effective->required_factors != 0u
                         ? effective->required_factors
                         : VECTIS_AUTH_ROUTE_FACTOR_PASSWORD;
  if (effective->require_email_token) {
    required_factors |= VECTIS_AUTH_ROUTE_FACTOR_EMAIL_TOKEN;
  }
  if (!vectis_auth_route_required_factors_valid(required_factors)) {
    free(path_prefix);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth route required_factors is invalid");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_auth_resolve_login_template(effective, &login_template_html,
                                              error);
  if (status != VECTIS_OK) {
    free(path_prefix);
    return status;
  }
  resolved = *effective;
  if (login_template_html != NULL) {
    resolved.login_template_html = login_template_html;
    resolved.login_template_path = NULL;
    resolved.login_template_embedded_path = NULL;
    resolved.login_template_fs = NULL;
  }
  status = vectis_register_auth_route_one(app, &resolved, path_prefix, "/login",
                                          VECTIS_HTTP_GET,
                                          vectis_auth_login_dispatch, error);
  if (status == VECTIS_OK) {
    status = vectis_register_auth_route_one(
        app, &resolved, path_prefix, "/login", VECTIS_HTTP_POST,
        vectis_auth_webdav_key_dispatch, error);
  }
  if (status == VECTIS_OK) {
    status = vectis_register_auth_route_one(
        app, &resolved, path_prefix, "/email-token", VECTIS_HTTP_POST,
        vectis_auth_email_token_dispatch, error);
  }
  if (status == VECTIS_OK) {
    status = vectis_register_auth_route_one(
        app, &resolved, path_prefix, "/continue", VECTIS_HTTP_POST,
        vectis_auth_webdav_key_dispatch, error);
  }
  if (status == VECTIS_OK) {
    status = vectis_register_auth_route_one(
        app, &resolved, path_prefix, "/webdav-key", VECTIS_HTTP_POST,
        vectis_auth_webdav_key_dispatch, error);
  }
  if (status == VECTIS_OK) {
    status = vectis_register_auth_route_one(app, &resolved, path_prefix,
                                            "/logout", VECTIS_HTTP_POST,
                                            vectis_auth_logout_dispatch, error);
  }
  free(login_template_html);
  free(path_prefix);
  return status;
}

static vectis_status vectis_upload_file_open(vectis_app *app,
                                             vectis_request *request,
                                             void *userdata, void **state,
                                             vectis_error *error) {
  vectis_upload_file_adapter *adapter;
  vectis_upload_file_state *file_state;

  (void)app;
  (void)request;
  adapter = (vectis_upload_file_adapter *)userdata;
  if (adapter == NULL || adapter->file_path == NULL || state == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "upload file route is invalid");
    return VECTIS_ERR_INVALID;
  }
  file_state = (vectis_upload_file_state *)calloc(1u, sizeof(*file_state));
  if (file_state == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate upload file state");
    return VECTIS_ERR_NOMEM;
  }
  file_state->file = fopen(adapter->file_path, "wb");
  if (file_state->file == NULL) {
    free(file_state);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to open upload output file");
    return VECTIS_ERR_STATE;
  }
  *state = file_state;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status vectis_upload_file_write(vectis_app *app,
                                              vectis_request *request,
                                              const void *data, size_t size,
                                              void *state, void *userdata,
                                              vectis_error *error) {
  vectis_upload_file_state *file_state;

  (void)app;
  (void)request;
  (void)userdata;
  file_state = (vectis_upload_file_state *)state;
  if (file_state == NULL || file_state->file == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "upload file state is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (size > 0u && fwrite(data, 1u, size, file_state->file) != size) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to write upload output file");
    return VECTIS_ERR_STATE;
  }
  file_state->size += size;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status vectis_upload_file_finish(vectis_app *app,
                                               vectis_request *request,
                                               vectis_response *response,
                                               void *state, void *userdata,
                                               vectis_error *error) {
  vectis_upload_file_adapter *adapter;
  vectis_upload_file_state *file_state;
  char message[64];

  (void)app;
  (void)request;
  adapter = (vectis_upload_file_adapter *)userdata;
  file_state = (vectis_upload_file_state *)state;
  if (adapter == NULL || file_state == NULL || file_state->file == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "upload file state is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (fflush(file_state->file) != 0) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to flush upload output file");
    return VECTIS_ERR_STATE;
  }
  (void)snprintf(message, sizeof(message), "%lu",
                 (unsigned long)file_state->size);
  return vectis_response_text(response, 200, "text/plain", message, error);
}

static void vectis_upload_file_close(vectis_app *app, vectis_request *request,
                                     void *state, void *userdata) {
  vectis_upload_file_state *file_state;

  (void)app;
  (void)request;
  (void)userdata;
  file_state = (vectis_upload_file_state *)state;
  if (file_state != NULL) {
    if (file_state->file != NULL) {
      (void)fclose(file_state->file);
    }
    free(file_state);
  }
}

static void vectis_upload_reader_set_lc_error(lc_error *error,
                                              const char *message) {
  if (error == NULL) {
    return;
  }
  lc_error_cleanup(error);
  error->code = LC_ERR_TRANSPORT;
  error->message =
      vectis_strdup(message != NULL ? message : "upload reader failed");
}

static void
vectis_upload_reader_signal_failure(vectis_upload_reader_state *reader_state) {
  if (reader_state == NULL || !reader_state->sync_initialized) {
    return;
  }
  (void)pthread_mutex_lock(&reader_state->mutex);
  reader_state->failed = 1;
  (void)pthread_cond_broadcast(&reader_state->readable);
  (void)pthread_cond_broadcast(&reader_state->writable);
  (void)pthread_mutex_unlock(&reader_state->mutex);
}

static size_t vectis_upload_reader_source_read(void *context, void *buffer,
                                               size_t count, lc_error *error) {
  vectis_upload_reader_state *reader_state;
  vectis_upload_reader_chunk *chunk;
  size_t nread;

  reader_state = (vectis_upload_reader_state *)context;
  if (reader_state == NULL || (buffer == NULL && count > 0u)) {
    vectis_upload_reader_set_lc_error(error, "upload reader is invalid");
    return 0u;
  }
  if (count == 0u) {
    return 0u;
  }

  (void)pthread_mutex_lock(&reader_state->mutex);
  while (reader_state->head == NULL && !reader_state->eof &&
         !reader_state->failed && !reader_state->closed) {
    (void)pthread_cond_wait(&reader_state->readable, &reader_state->mutex);
  }
  if (reader_state->failed || reader_state->closed) {
    (void)pthread_mutex_unlock(&reader_state->mutex);
    vectis_upload_reader_set_lc_error(error, "upload reader was closed");
    return 0u;
  }
  chunk = reader_state->head;
  if (chunk == NULL) {
    (void)pthread_mutex_unlock(&reader_state->mutex);
    return 0u;
  }
  nread = chunk->size - chunk->offset;
  if (nread > count) {
    nread = count;
  }
  memcpy(buffer, chunk->data + chunk->offset, nread);
  chunk->offset += nread;
  reader_state->queued -= nread;
  if (chunk->offset == chunk->size) {
    reader_state->head = chunk->next;
    if (reader_state->head == NULL) {
      reader_state->tail = NULL;
    }
  } else {
    chunk = NULL;
  }
  (void)pthread_cond_signal(&reader_state->writable);
  (void)pthread_mutex_unlock(&reader_state->mutex);
  free(chunk);
  if (error != NULL) {
    lc_error_init(error);
  }
  return nread;
}

static void vectis_upload_reader_source_close(void *context) {
  vectis_upload_reader_state *reader_state;

  reader_state = (vectis_upload_reader_state *)context;
  if (reader_state == NULL || !reader_state->sync_initialized) {
    return;
  }
  (void)pthread_mutex_lock(&reader_state->mutex);
  reader_state->closed = 1;
  (void)pthread_cond_broadcast(&reader_state->readable);
  (void)pthread_cond_broadcast(&reader_state->writable);
  (void)pthread_mutex_unlock(&reader_state->mutex);
}

static void *vectis_upload_reader_thread_main(void *userdata) {
  vectis_upload_reader_state *reader_state;
  vectis_status status;

  reader_state = (vectis_upload_reader_state *)userdata;
  status = reader_state->handler(reader_state->app, reader_state->request,
                                 reader_state->source, reader_state->response,
                                 reader_state->userdata, &reader_state->error);
  (void)pthread_mutex_lock(&reader_state->mutex);
  reader_state->status = status;
  reader_state->handler_done = 1;
  if (status != VECTIS_OK) {
    reader_state->failed = 1;
  }
  (void)pthread_cond_broadcast(&reader_state->readable);
  (void)pthread_cond_broadcast(&reader_state->writable);
  (void)pthread_mutex_unlock(&reader_state->mutex);
  return NULL;
}

static vectis_status vectis_upload_reader_open(vectis_app *app,
                                               vectis_request *request,
                                               void *userdata, void **state,
                                               vectis_error *error) {
  vectis_upload_reader_adapter *adapter;
  vectis_upload_reader_state *reader_state;
  lc_error lcerr;
  int mutex_ready;
  int readable_ready;
  int writable_ready;
  int rc;

  mutex_ready = 0;
  readable_ready = 0;
  writable_ready = 0;
  adapter = (vectis_upload_reader_adapter *)userdata;
  if (adapter == NULL || adapter->handler == NULL || state == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "upload reader route is invalid");
    return VECTIS_ERR_INVALID;
  }
  reader_state =
      (vectis_upload_reader_state *)calloc(1u, sizeof(*reader_state));
  if (reader_state == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate upload reader state");
    return VECTIS_ERR_NOMEM;
  }
  reader_state->capacity = adapter->buffer_bytes > 0u
                               ? adapter->buffer_bytes
                               : VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
  reader_state->app = app;
  reader_state->request = request;
  reader_state->handler = adapter->handler;
  reader_state->userdata = adapter->userdata;
  reader_state->status = VECTIS_OK;
  reader_state->response = vectis_internal_response_new(error);
  if (reader_state->response == NULL) {
    free(reader_state);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  vectis_error_clear(&reader_state->error);
  if (pthread_mutex_init(&reader_state->mutex, NULL) == 0) {
    mutex_ready = 1;
  }
  if (mutex_ready && pthread_cond_init(&reader_state->readable, NULL) == 0) {
    readable_ready = 1;
  }
  if (readable_ready && pthread_cond_init(&reader_state->writable, NULL) == 0) {
    writable_ready = 1;
  }
  if (!mutex_ready || !readable_ready || !writable_ready) {
    vectis_internal_response_free(reader_state->response);
    if (writable_ready) {
      (void)pthread_cond_destroy(&reader_state->writable);
    }
    if (readable_ready) {
      (void)pthread_cond_destroy(&reader_state->readable);
    }
    if (mutex_ready) {
      (void)pthread_mutex_destroy(&reader_state->mutex);
    }
    free(reader_state);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to initialize upload reader synchronization");
    return VECTIS_ERR_STATE;
  }
  reader_state->sync_initialized = 1;
  lc_error_init(&lcerr);
  rc = lc_source_from_callbacks(vectis_upload_reader_source_read, NULL,
                                vectis_upload_reader_source_close, reader_state,
                                &reader_state->source, &lcerr);
  if (rc != LC_OK) {
    vectis_internal_response_free(reader_state->response);
    (void)pthread_cond_destroy(&reader_state->writable);
    (void)pthread_cond_destroy(&reader_state->readable);
    (void)pthread_mutex_destroy(&reader_state->mutex);
    free(reader_state);
    (void)vectis_source_error(error, rc, &lcerr,
                              "failed to create upload reader source");
    lc_error_cleanup(&lcerr);
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  if (pthread_create(&reader_state->thread, NULL,
                     vectis_upload_reader_thread_main, reader_state) != 0) {
    lc_source_close(reader_state->source);
    reader_state->source = NULL;
    vectis_internal_response_free(reader_state->response);
    (void)pthread_cond_destroy(&reader_state->writable);
    (void)pthread_cond_destroy(&reader_state->readable);
    (void)pthread_mutex_destroy(&reader_state->mutex);
    free(reader_state);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to start upload reader handler");
    return VECTIS_ERR_STATE;
  }
  reader_state->thread_started = 1;
  *state = reader_state;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status vectis_upload_reader_write(vectis_app *app,
                                                vectis_request *request,
                                                const void *data, size_t size,
                                                void *state, void *userdata,
                                                vectis_error *error) {
  vectis_upload_reader_state *reader_state;
  vectis_upload_reader_chunk *chunk;
  const unsigned char *bytes;
  size_t offset;
  size_t ncopy;
  size_t room;

  (void)app;
  (void)request;
  (void)userdata;
  reader_state = (vectis_upload_reader_state *)state;
  if (reader_state == NULL || (data == NULL && size > 0u)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "upload reader chunk is invalid");
    return VECTIS_ERR_INVALID;
  }
  bytes = (const unsigned char *)data;
  offset = 0u;
  while (offset < size) {
    (void)pthread_mutex_lock(&reader_state->mutex);
    while (reader_state->queued >= reader_state->capacity &&
           !reader_state->failed && !reader_state->closed &&
           !reader_state->handler_done) {
      (void)pthread_cond_wait(&reader_state->writable, &reader_state->mutex);
    }
    if (reader_state->failed || reader_state->closed ||
        reader_state->handler_done) {
      (void)pthread_mutex_unlock(&reader_state->mutex);
      vectis_set_error(error, VECTIS_ERR_STATE,
                       "upload reader handler stopped before consuming body");
      return VECTIS_ERR_STATE;
    }
    room = reader_state->capacity - reader_state->queued;
    ncopy = size - offset;
    if (ncopy > room) {
      ncopy = room;
    }
    if (ncopy == 0u) {
      (void)pthread_mutex_unlock(&reader_state->mutex);
      continue;
    }
    chunk = (vectis_upload_reader_chunk *)malloc(sizeof(*chunk) + ncopy - 1u);
    if (chunk == NULL) {
      reader_state->failed = 1;
      (void)pthread_cond_broadcast(&reader_state->readable);
      (void)pthread_cond_broadcast(&reader_state->writable);
      (void)pthread_mutex_unlock(&reader_state->mutex);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to allocate upload reader chunk");
      return VECTIS_ERR_NOMEM;
    }
    chunk->next = NULL;
    chunk->offset = 0u;
    chunk->size = ncopy;
    memcpy(chunk->data, bytes + offset, ncopy);
    if (reader_state->tail != NULL) {
      reader_state->tail->next = chunk;
    } else {
      reader_state->head = chunk;
    }
    reader_state->tail = chunk;
    reader_state->queued += ncopy;
    offset += ncopy;
    (void)pthread_cond_signal(&reader_state->readable);
    (void)pthread_mutex_unlock(&reader_state->mutex);
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

static void vectis_response_move(vectis_response *dst, vectis_response *src) {
  if (dst == NULL || src == NULL) {
    return;
  }
  vectis_internal_response_cleanup(dst);
  *dst = *src;
  memset(src, 0, sizeof(*src));
}

static vectis_status vectis_upload_reader_finish(vectis_app *app,
                                                 vectis_request *request,
                                                 vectis_response *response,
                                                 void *state, void *userdata,
                                                 vectis_error *error) {
  vectis_upload_reader_state *reader_state;

  (void)app;
  (void)request;
  (void)userdata;
  reader_state = (vectis_upload_reader_state *)state;
  if (reader_state == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "upload reader stream is not initialized");
    return VECTIS_ERR_INVALID;
  }
  (void)pthread_mutex_lock(&reader_state->mutex);
  reader_state->eof = 1;
  (void)pthread_cond_broadcast(&reader_state->readable);
  (void)pthread_mutex_unlock(&reader_state->mutex);
  if (reader_state->thread_started) {
    (void)pthread_join(reader_state->thread, NULL);
    reader_state->thread_started = 0;
  }
  if (reader_state->status != VECTIS_OK) {
    if (error != NULL) {
      *error = reader_state->error;
    }
    return reader_state->status;
  }
  if (reader_state->response == NULL ||
      reader_state->response->status_code == 0) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "upload reader handler did not produce a response");
    return VECTIS_ERR_STATE;
  }
  vectis_response_move(response, reader_state->response);
  vectis_error_clear(error);
  return VECTIS_OK;
}

static void vectis_upload_reader_close(vectis_app *app, vectis_request *request,
                                       void *state, void *userdata) {
  vectis_upload_reader_state *reader_state;
  vectis_upload_reader_chunk *chunk;
  vectis_upload_reader_chunk *next;

  (void)app;
  (void)request;
  (void)userdata;
  reader_state = (vectis_upload_reader_state *)state;
  if (reader_state == NULL) {
    return;
  }
  vectis_upload_reader_signal_failure(reader_state);
  if (reader_state->thread_started) {
    (void)pthread_join(reader_state->thread, NULL);
    reader_state->thread_started = 0;
  }
  if (reader_state->source != NULL) {
    lc_source_close(reader_state->source);
    reader_state->source = NULL;
  }
  chunk = reader_state->head;
  while (chunk != NULL) {
    next = chunk->next;
    free(chunk);
    chunk = next;
  }
  vectis_internal_response_free(reader_state->response);
  if (reader_state->sync_initialized) {
    (void)pthread_cond_destroy(&reader_state->writable);
    (void)pthread_cond_destroy(&reader_state->readable);
    (void)pthread_mutex_destroy(&reader_state->mutex);
  }
  free(reader_state);
}

vectis_status
vectis_register_upload_file(vectis_app *app,
                            const vectis_upload_file_route_config *route,
                            vectis_error *error) {
  vectis_upload_file_adapter *adapter;
  vectis_upload_route_config stream_route;
  vectis_status status;

  if (route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "upload file route is required");
    return VECTIS_ERR_INVALID;
  }
  if (route->file_path == NULL || route->file_path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "upload file route file_path is required");
    return VECTIS_ERR_INVALID;
  }
  adapter = (vectis_upload_file_adapter *)calloc(1u, sizeof(*adapter));
  if (adapter == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate upload file adapter");
    return VECTIS_ERR_NOMEM;
  }
  adapter->file_path = vectis_strdup(route->file_path);
  adapter->content_type =
      vectis_strdup(route->content_type != NULL ? route->content_type
                                                : "application/octet-stream");
  if (adapter->file_path == NULL || adapter->content_type == NULL) {
    free(adapter->file_path);
    free(adapter->content_type);
    free(adapter);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy upload file adapter");
    return VECTIS_ERR_NOMEM;
  }
  vectis_upload_route_config_init(&stream_route);
  stream_route.method = route->method;
  stream_route.methods = route->methods;
  stream_route.path = route->path;
  stream_route.path_kind = route->path_kind;
  stream_route.body = route->body;
  stream_route.open = vectis_upload_file_open;
  stream_route.write = vectis_upload_file_write;
  stream_route.finish = vectis_upload_file_finish;
  stream_route.close = vectis_upload_file_close;
  stream_route.userdata = adapter;
  status =
      vectis_app_register_upload_owned_userdata(app, &stream_route, 1, error);
  if (status != VECTIS_OK) {
    free(adapter->file_path);
    free(adapter->content_type);
    free(adapter);
  }
  return status;
}

static vectis_status vectis_register_upload_reader_adapter(
    vectis_app *app, const vectis_upload_reader_route_config *route,
    vectis_upload_reader_adapter *adapter, vectis_error *error) {
  vectis_upload_route_config stream_route;
  vectis_status status;

  vectis_upload_route_config_init(&stream_route);
  stream_route.method = route->method;
  stream_route.methods = route->methods;
  stream_route.path = route->path;
  stream_route.path_kind = route->path_kind;
  stream_route.body = route->body;
  stream_route.open = vectis_upload_reader_open;
  stream_route.write = vectis_upload_reader_write;
  stream_route.finish = vectis_upload_reader_finish;
  stream_route.close = vectis_upload_reader_close;
  stream_route.userdata = adapter;
  status =
      vectis_app_register_upload_owned_userdata(app, &stream_route, 1, error);
  return status;
}

vectis_status
vectis_register_upload_reader(vectis_app *app,
                              const vectis_upload_reader_route_config *route,
                              vectis_error *error) {
  vectis_upload_reader_adapter *adapter;
  vectis_status status;

  if (route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "upload reader route is required");
    return VECTIS_ERR_INVALID;
  }
  if (route->handler == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "upload reader route handler is required");
    return VECTIS_ERR_INVALID;
  }
  adapter = (vectis_upload_reader_adapter *)calloc(1u, sizeof(*adapter));
  if (adapter == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate upload reader adapter");
    return VECTIS_ERR_NOMEM;
  }
  adapter->handler = route->handler;
  adapter->userdata = route->userdata;
  adapter->buffer_bytes = route->buffer_bytes > 0u
                              ? route->buffer_bytes
                              : VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
  status = vectis_register_upload_reader_adapter(app, route, adapter, error);
  if (status != VECTIS_OK) {
    free(adapter);
  }
  return status;
}

static vectis_status vectis_validate_lonejson_struct(const lonejson_map *map,
                                                     size_t size,
                                                     const char *label,
                                                     vectis_error *error) {
  if (map == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID, "%s map is required", label);
    return VECTIS_ERR_INVALID;
  }
  if (size == 0u || size > 10485760u) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID, "%s size is invalid", label);
    return VECTIS_ERR_INVALID;
  }
  if (size != map->struct_size) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "%s size does not match lonejson map", label);
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static vectis_status
vectis_xml_route_dispatch(vectis_app *app, vectis_request *request,
                          struct lc_source *reader, vectis_response *response,
                          void *userdata, vectis_error *error) {
  vectis_xml_route_adapter *adapter;
  vectis_source source;
  void *input;
  vectis_status status;

  adapter = (vectis_xml_route_adapter *)userdata;
  if (adapter == NULL || adapter->handler == NULL || reader == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML route is invalid");
    return VECTIS_ERR_INVALID;
  }
  input = calloc(1u, adapter->input_size);
  if (input == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate XML route input");
    return VECTIS_ERR_NOMEM;
  }
  source = vectis_source_from_lc(reader);
  status = vectis_xml_parse_lonejson_source(&source, adapter->input_map,
                                            &adapter->config, input, error);
  if (status == VECTIS_OK) {
    status = adapter->handler(app, request, input, response, adapter->userdata,
                              error);
    lonejson_cleanup(adapter->input_map, input);
  }
  free(input);
  return status;
}

static vectis_status
vectis_dsv_route_dispatch(vectis_app *app, vectis_request *request,
                          struct lc_source *reader, vectis_response *response,
                          void *userdata, vectis_error *error) {
  vectis_dsv_route_adapter *adapter;
  vectis_dsv_rows rows;
  vectis_status status;

  adapter = (vectis_dsv_route_adapter *)userdata;
  if (adapter == NULL || adapter->handler == NULL || reader == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV route is invalid");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_dsv_rows_open(&rows, reader, adapter->row_map,
                                adapter->row_size, &adapter->config, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status =
      adapter->handler(app, request, &rows, response, adapter->userdata, error);
  vectis_dsv_rows_cleanup(&rows);
  return status;
}

vectis_status vectis_register_xml_route(vectis_app *app,
                                        const vectis_xml_route_config *route,
                                        vectis_error *error) {
  vectis_xml_route_adapter *adapter;
  vectis_upload_reader_adapter *upload_adapter;
  vectis_upload_reader_route_config upload_route;
  vectis_status status;

  if (route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML route is required");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_validate_lonejson_struct(route->input_map, route->input_size,
                                           "XML route input", error);
  if (status != VECTIS_OK) {
    return status;
  }
  if (route->handler == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "XML route handler is required");
    return VECTIS_ERR_INVALID;
  }
  adapter = (vectis_xml_route_adapter *)calloc(1u, sizeof(*adapter));
  if (adapter == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate XML route adapter");
    return VECTIS_ERR_NOMEM;
  }
  adapter->input_map = route->input_map;
  adapter->input_size = route->input_size;
  adapter->config = route->config;
  adapter->handler = route->handler;
  adapter->userdata = route->userdata;
  adapter->buffer_bytes = route->buffer_bytes > 0u
                              ? route->buffer_bytes
                              : VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
  upload_adapter =
      (vectis_upload_reader_adapter *)calloc(1u, sizeof(*upload_adapter));
  if (upload_adapter == NULL) {
    free(adapter);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate XML upload reader adapter");
    return VECTIS_ERR_NOMEM;
  }
  upload_adapter->handler = vectis_xml_route_dispatch;
  upload_adapter->userdata = adapter;
  upload_adapter->buffer_bytes = adapter->buffer_bytes;
  upload_adapter->free_userdata = free;
  vectis_upload_reader_route_config_init(&upload_route);
  upload_route.method = route->method;
  upload_route.methods = route->methods;
  upload_route.path = route->path;
  upload_route.path_kind = route->path_kind;
  upload_route.body = route->body;
  upload_route.buffer_bytes = adapter->buffer_bytes;
  upload_route.handler = vectis_xml_route_dispatch;
  upload_route.userdata = adapter;
  status = vectis_register_upload_reader_adapter(app, &upload_route,
                                                 upload_adapter, error);
  if (status != VECTIS_OK) {
    free(upload_adapter);
    free(adapter);
  }
  return status;
}

vectis_status vectis_register_dsv_route(vectis_app *app,
                                        const vectis_dsv_route_config *route,
                                        vectis_error *error) {
  vectis_dsv_route_adapter *adapter;
  vectis_upload_reader_adapter *upload_adapter;
  vectis_upload_reader_route_config upload_route;
  vectis_status status;

  if (route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV route is required");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_validate_lonejson_struct(route->row_map, route->row_size,
                                           "DSV route row", error);
  if (status != VECTIS_OK) {
    return status;
  }
  if (route->handler == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV route handler is required");
    return VECTIS_ERR_INVALID;
  }
  adapter = (vectis_dsv_route_adapter *)calloc(1u, sizeof(*adapter));
  if (adapter == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate DSV route adapter");
    return VECTIS_ERR_NOMEM;
  }
  adapter->row_map = route->row_map;
  adapter->row_size = route->row_size;
  adapter->config = route->config;
  adapter->handler = route->handler;
  adapter->userdata = route->userdata;
  adapter->buffer_bytes = route->buffer_bytes > 0u
                              ? route->buffer_bytes
                              : VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
  upload_adapter =
      (vectis_upload_reader_adapter *)calloc(1u, sizeof(*upload_adapter));
  if (upload_adapter == NULL) {
    free(adapter);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate DSV upload reader adapter");
    return VECTIS_ERR_NOMEM;
  }
  upload_adapter->handler = vectis_dsv_route_dispatch;
  upload_adapter->userdata = adapter;
  upload_adapter->buffer_bytes = adapter->buffer_bytes;
  upload_adapter->free_userdata = free;
  vectis_upload_reader_route_config_init(&upload_route);
  upload_route.method = route->method;
  upload_route.methods = route->methods;
  upload_route.path = route->path;
  upload_route.path_kind = route->path_kind;
  upload_route.body = route->body;
  upload_route.buffer_bytes = adapter->buffer_bytes;
  upload_route.handler = vectis_dsv_route_dispatch;
  upload_route.userdata = adapter;
  status = vectis_register_upload_reader_adapter(app, &upload_route,
                                                 upload_adapter, error);
  if (status != VECTIS_OK) {
    free(upload_adapter);
    free(adapter);
  }
  return status;
}

static vectis_status vectis_json_route_alloc_input(vectis_request *request,
                                                   const lonejson_map *map,
                                                   size_t size, void **out,
                                                   vectis_error *error) {
  void *input;
  lonejson *runtime;
  vectis_status status;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "json input output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  if (map == NULL) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  input = calloc(1u, size);
  if (input == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate json route input");
    return VECTIS_ERR_NOMEM;
  }
  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    free(input);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  lonejson_init(runtime, map, input);
  lonejson_free(runtime);
  status = vectis_request_json_into(request, map, input, error);
  if (status != VECTIS_OK) {
    lonejson_cleanup(map, input);
    free(input);
    return status;
  }
  *out = input;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status vectis_json_route_dispatch(vectis_app *app,
                                                vectis_request *request,
                                                vectis_response *response,
                                                void *userdata,
                                                vectis_error *error) {
  vectis_json_route_adapter *adapter;
  void *input;
  void *output;
  lonejson *runtime;
  vectis_status status;

  adapter = (vectis_json_route_adapter *)userdata;
  if (adapter == NULL || adapter->handler == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "json route adapter is invalid");
    return VECTIS_ERR_INVALID;
  }

  input = NULL;
  output = NULL;
  status = vectis_json_route_alloc_input(request, adapter->input_map,
                                         adapter->input_size, &input, error);
  if (status != VECTIS_OK) {
    return status;
  }

  if (adapter->output_map != NULL) {
    output = calloc(1u, adapter->output_size);
    if (output == NULL) {
      if (adapter->input_map != NULL) {
        lonejson_cleanup(adapter->input_map, input);
      }
      free(input);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to allocate json route output");
      return VECTIS_ERR_NOMEM;
    }
    runtime = vectis_lonejson_new(error);
    if (runtime == NULL) {
      if (adapter->input_map != NULL) {
        lonejson_cleanup(adapter->input_map, input);
      }
      free(input);
      free(output);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    lonejson_init(runtime, adapter->output_map, output);
    lonejson_free(runtime);
  }

  status =
      adapter->handler(app, request, input, output, adapter->userdata, error);
  if (status == VECTIS_OK) {
    if (adapter->output_map != NULL) {
      status = vectis_response_json(response, 200, adapter->output_map, output,
                                    error);
    } else {
      status = vectis_response_status(response, 204, error);
    }
  }

  if (adapter->output_map != NULL) {
    lonejson_cleanup(adapter->output_map, output);
  }
  if (adapter->input_map != NULL) {
    lonejson_cleanup(adapter->input_map, input);
  }
  free(output);
  free(input);
  return status;
}

static vectis_status vectis_json_typed_route_dispatch(vectis_app *app,
                                                      vectis_request *request,
                                                      vectis_response *response,
                                                      void *userdata,
                                                      vectis_error *error) {
  vectis_json_typed_route_adapter *adapter;
  vectis_json_response json_response;
  void *input;
  vectis_status status;

  adapter = (vectis_json_typed_route_adapter *)userdata;
  if (adapter == NULL || adapter->handler == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "typed json route adapter is invalid");
    return VECTIS_ERR_INVALID;
  }
  input = NULL;
  status = vectis_json_route_alloc_input(request, adapter->input_map,
                                         adapter->input_size, &input, error);
  if (status != VECTIS_OK) {
    return status;
  }
  json_response.response = response;
  json_response.sent = 0;
  status = adapter->handler(app, request, input, &json_response,
                            adapter->userdata, error);
  if (status == VECTIS_OK && !json_response.sent) {
    status = vectis_response_status(response, 204, error);
  }
  if (adapter->input_map != NULL) {
    lonejson_cleanup(adapter->input_map, input);
  }
  free(input);
  return status;
}

vectis_status vectis_register_json_route(vectis_app *app,
                                         const vectis_json_route_config *route,
                                         vectis_error *error) {
  vectis_app_impl *impl;
  vectis_json_route_adapter *adapter;
  vectis_route_config raw_route;
  vectis_status status;

  if (app == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  if (route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json route is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_validate_methods(route->method, route->methods, error) !=
      VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (vectis_validate_route_path(route->path, route->path_kind, error) !=
      VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (route->handler == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "json route handler is required");
    return VECTIS_ERR_INVALID;
  }
  if (route->input_map != NULL &&
      (route->input_size == 0u || route->input_size > 10485760u)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "json route input_size is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (route->input_map != NULL &&
      route->input_size != route->input_map->struct_size) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "json route input_size does not match lonejson map");
    return VECTIS_ERR_INVALID;
  }
  if (route->output_map != NULL &&
      (route->output_size == 0u || route->output_size > 10485760u)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "json route output_size is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (route->output_map != NULL &&
      route->output_size != route->output_map->struct_size) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "json route output_size does not match lonejson map");
    return VECTIS_ERR_INVALID;
  }
  impl = (vectis_app_impl *)app->impl;
  if (vectis_validate_body_policy(&route->body,
                                  impl != NULL ? &impl->server : NULL,
                                  error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }

  adapter = (vectis_json_route_adapter *)calloc(1u, sizeof(*adapter));
  if (adapter == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate json route adapter");
    return VECTIS_ERR_NOMEM;
  }
  adapter->input_map = route->input_map;
  adapter->input_size = route->input_size;
  adapter->output_map = route->output_map;
  adapter->output_size = route->output_size;
  adapter->handler = route->handler;
  adapter->userdata = route->userdata;

  vectis_route_config_init(&raw_route);
  raw_route.method = route->method;
  raw_route.methods = vectis_normalize_methods(route->method, route->methods);
  raw_route.path = route->path;
  raw_route.path_kind = route->path_kind;
  raw_route.body = route->body;
  raw_route.handler = vectis_json_route_dispatch;
  raw_route.userdata = adapter;

  status = vectis_app_register_route_owned_userdata(app, &raw_route, 1, error);
  if (status != VECTIS_OK) {
    free(adapter);
  }
  return status;
}

vectis_status
vectis_register_json_typed_route(vectis_app *app,
                                 const vectis_json_typed_route_config *route,
                                 vectis_error *error) {
  vectis_app_impl *impl;
  vectis_json_typed_route_adapter *adapter;
  vectis_route_config raw_route;
  vectis_status status;

  if (app == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  if (route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "typed json route is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_validate_methods(route->method, route->methods, error) !=
      VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (vectis_validate_route_path(route->path, route->path_kind, error) !=
      VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (route->handler == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "typed json route handler is required");
    return VECTIS_ERR_INVALID;
  }
  if (route->input_map != NULL &&
      (route->input_size == 0u || route->input_size > 10485760u)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "typed json route input_size is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (route->input_map != NULL &&
      route->input_size != route->input_map->struct_size) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "typed json route input_size does not match lonejson map");
    return VECTIS_ERR_INVALID;
  }
  impl = (vectis_app_impl *)app->impl;
  if (vectis_validate_body_policy(&route->body,
                                  impl != NULL ? &impl->server : NULL,
                                  error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  adapter = (vectis_json_typed_route_adapter *)calloc(1u, sizeof(*adapter));
  if (adapter == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate typed json route adapter");
    return VECTIS_ERR_NOMEM;
  }
  adapter->input_map = route->input_map;
  adapter->input_size = route->input_size;
  adapter->handler = route->handler;
  adapter->userdata = route->userdata;

  vectis_route_config_init(&raw_route);
  raw_route.method = route->method;
  raw_route.methods = vectis_normalize_methods(route->method, route->methods);
  raw_route.path = route->path;
  raw_route.path_kind = route->path_kind;
  raw_route.body = route->body;
  raw_route.handler = vectis_json_typed_route_dispatch;
  raw_route.userdata = adapter;

  status = vectis_app_register_route_owned_userdata(app, &raw_route, 1, error);
  if (status != VECTIS_OK) {
    free(adapter);
  }
  return status;
}

vectis_status
vectis_register_prefixed_json_route(vectis_app *app, const char *prefix,
                                    const vectis_json_route_config *route,
                                    vectis_error *error) {
  vectis_json_route_config prefixed;
  char *path;
  vectis_status status;

  if (route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json route is required");
    return VECTIS_ERR_INVALID;
  }
  if (route->path_kind == VECTIS_ROUTE_PATH_REGEX) {
    path = vectis_join_regex_route_prefix(prefix, route->path, error);
  } else {
    path = vectis_join_route_prefix(prefix, route->path, error);
  }
  if (path == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  prefixed = *route;
  prefixed.path = path;
  prefixed.path_kind = vectis_infer_route_path_kind(path);
  if (route->path_kind == VECTIS_ROUTE_PATH_REGEX) {
    prefixed.path_kind = VECTIS_ROUTE_PATH_REGEX;
  }
  status = vectis_register_json_route(app, &prefixed, error);
  free(path);
  return status;
}

vectis_status vectis_register_prefixed_json_typed_route(
    vectis_app *app, const char *prefix,
    const vectis_json_typed_route_config *route, vectis_error *error) {
  vectis_json_typed_route_config prefixed;
  char *path;
  vectis_status status;

  if (route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "typed json route is required");
    return VECTIS_ERR_INVALID;
  }
  if (route->path_kind == VECTIS_ROUTE_PATH_REGEX) {
    path = vectis_join_regex_route_prefix(prefix, route->path, error);
  } else {
    path = vectis_join_route_prefix(prefix, route->path, error);
  }
  if (path == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  prefixed = *route;
  prefixed.path = path;
  prefixed.path_kind = vectis_infer_route_path_kind(path);
  if (route->path_kind == VECTIS_ROUTE_PATH_REGEX) {
    prefixed.path_kind = VECTIS_ROUTE_PATH_REGEX;
  }
  status = vectis_register_json_typed_route(app, &prefixed, error);
  free(path);
  return status;
}

vectis_status
vectis_register_prefixed_xml_route(vectis_app *app, const char *prefix,
                                   const vectis_xml_route_config *route,
                                   vectis_error *error) {
  vectis_xml_route_config prefixed;
  char *path;
  vectis_status status;

  if (route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML route is required");
    return VECTIS_ERR_INVALID;
  }
  if (route->path_kind == VECTIS_ROUTE_PATH_REGEX) {
    path = vectis_join_regex_route_prefix(prefix, route->path, error);
  } else {
    path = vectis_join_route_prefix(prefix, route->path, error);
  }
  if (path == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  prefixed = *route;
  prefixed.path = path;
  prefixed.path_kind = vectis_infer_route_path_kind(path);
  if (route->path_kind == VECTIS_ROUTE_PATH_REGEX) {
    prefixed.path_kind = VECTIS_ROUTE_PATH_REGEX;
  }
  status = vectis_register_xml_route(app, &prefixed, error);
  free(path);
  return status;
}

vectis_status
vectis_register_prefixed_dsv_route(vectis_app *app, const char *prefix,
                                   const vectis_dsv_route_config *route,
                                   vectis_error *error) {
  vectis_dsv_route_config prefixed;
  char *path;
  vectis_status status;

  if (route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV route is required");
    return VECTIS_ERR_INVALID;
  }
  if (route->path_kind == VECTIS_ROUTE_PATH_REGEX) {
    path = vectis_join_regex_route_prefix(prefix, route->path, error);
  } else {
    path = vectis_join_route_prefix(prefix, route->path, error);
  }
  if (path == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  prefixed = *route;
  prefixed.path = path;
  prefixed.path_kind = vectis_infer_route_path_kind(path);
  if (route->path_kind == VECTIS_ROUTE_PATH_REGEX) {
    prefixed.path_kind = VECTIS_ROUTE_PATH_REGEX;
  }
  status = vectis_register_dsv_route(app, &prefixed, error);
  free(path);
  return status;
}

vectis_status vectis_attach_openapi_doc(vectis_app *app,
                                        vectis_http_methods methods,
                                        const char *path,
                                        const vectis_openapi_route_doc *doc,
                                        vectis_error *error) {
  vectis_app_impl *impl;
  vectis_openapi_doc_entry *grown;
  vectis_openapi_route_doc doc_copy;
  char *path_copy;
  size_t next_capacity;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  if (methods == VECTIS_HTTP_METHODS_NONE ||
      (methods & ~VECTIS_HTTP_METHODS_ALL) != 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "OpenAPI methods are invalid");
    return VECTIS_ERR_INVALID;
  }
  if (path == NULL || path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "OpenAPI path is required");
    return VECTIS_ERR_INVALID;
  }
  if (doc == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OpenAPI route doc is required");
    return VECTIS_ERR_INVALID;
  }
  path_copy = vectis_strdup(path);
  if (path_copy == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy OpenAPI path");
    return VECTIS_ERR_NOMEM;
  }
  if (vectis_openapi_route_doc_deep_copy(&doc_copy, doc, error) != VECTIS_OK) {
    free(path_copy);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  impl = (vectis_app_impl *)app->impl;
  (void)pthread_mutex_lock(&impl->mutex);
  if (impl->openapi_doc_count == impl->openapi_doc_capacity) {
    next_capacity =
        impl->openapi_doc_capacity == 0u ? 4u : impl->openapi_doc_capacity * 2u;
    grown = (vectis_openapi_doc_entry *)realloc(impl->openapi_docs,
                                                next_capacity * sizeof(*grown));
    if (grown == NULL) {
      (void)pthread_mutex_unlock(&impl->mutex);
      free(path_copy);
      vectis_openapi_route_doc_deep_cleanup(&doc_copy);
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to grow OpenAPI docs");
      return VECTIS_ERR_NOMEM;
    }
    impl->openapi_docs = grown;
    impl->openapi_doc_capacity = next_capacity;
  }
  impl->openapi_docs[impl->openapi_doc_count].methods = methods;
  impl->openapi_docs[impl->openapi_doc_count].path = path_copy;
  impl->openapi_docs[impl->openapi_doc_count].doc = doc_copy;
  impl->openapi_doc_count++;
  (void)pthread_mutex_unlock(&impl->mutex);
  vectis_error_clear(error);
  return VECTIS_OK;
}

static const char *vectis_openapi_schema_name(vectis_openapi_schema schema) {
  if (schema.name != NULL && schema.name[0] != '\0') {
    return schema.name;
  }
  if (schema.map != NULL && schema.map->name != NULL &&
      schema.map->name[0] != '\0') {
    return schema.map->name;
  }
  return "Schema";
}

static vectis_status
vectis_append_lonejson_string(vectis_string_builder *builder, const char *value,
                              vectis_error *error) {
  vectis_lonejson_builder_sink sink;
  lonejson_error json_error;
  lonejson *runtime;
  size_t length;
  lonejson_status json_status;

  value = value != NULL ? value : "";
  length = strlen(value);
  sink.builder = builder;
  sink.error = error;
  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  json_status = lonejson_write_json_string_buffer_sink(
      runtime, value, length, vectis_lonejson_builder_sink_write, &sink,
      &json_error);
  lonejson_free(runtime);
  if (json_status != LONEJSON_STATUS_OK) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to serialize JSON string: %s",
                      json_error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
      error->dependency_code = (long)json_status;
    }
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static vectis_status vectis_openapi_append_path(vectis_string_builder *builder,
                                                const char *path,
                                                vectis_error *error) {
  const char *p;
  const char *start;

  p = path != NULL ? path : "/";
  while (*p != '\0') {
    if (*p == ':') {
      ++p;
      start = p;
      while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
             (*p >= '0' && *p <= '9') || *p == '_') {
        ++p;
      }
      if (vectis_string_builder_append(builder, "{", error) != VECTIS_OK ||
          vectis_string_builder_append_n(builder, start, (size_t)(p - start),
                                         error) != VECTIS_OK ||
          vectis_string_builder_append(builder, "}", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
      if (*p == '?') {
        ++p;
      }
    } else {
      if (vectis_string_builder_append_n(builder, p, 1u, error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
      ++p;
    }
  }
  return VECTIS_OK;
}

static const char *vectis_openapi_method_name(vectis_http_method method) {
  switch (method) {
  case VECTIS_HTTP_GET:
    return "get";
  case VECTIS_HTTP_POST:
    return "post";
  case VECTIS_HTTP_PUT:
    return "put";
  case VECTIS_HTTP_PATCH:
    return "patch";
  case VECTIS_HTTP_DELETE:
    return "delete";
  case VECTIS_HTTP_HEAD:
    return "head";
  case VECTIS_HTTP_OPTIONS:
    return "options";
  case VECTIS_HTTP_PROPFIND:
    return "propfind";
  case VECTIS_HTTP_MKCOL:
    return "mkcol";
  case VECTIS_HTTP_COPY:
    return "copy";
  case VECTIS_HTTP_MOVE:
    return "move";
  default:
    return "get";
  }
}

static const char *vectis_openapi_field_type(const lonejson_field *field) {
  switch (field->kind) {
  case LONEJSON_FIELD_KIND_I64:
  case LONEJSON_FIELD_KIND_U64:
  case LONEJSON_FIELD_KIND_I64_ARRAY:
  case LONEJSON_FIELD_KIND_U64_ARRAY:
    return "integer";
  case LONEJSON_FIELD_KIND_F64:
  case LONEJSON_FIELD_KIND_F64_ARRAY:
    return "number";
  case LONEJSON_FIELD_KIND_BOOL:
  case LONEJSON_FIELD_KIND_BOOL_ARRAY:
    return "boolean";
  case LONEJSON_FIELD_KIND_OBJECT:
  case LONEJSON_FIELD_KIND_OBJECT_ARRAY:
    return "object";
  default:
    return "string";
  }
}

static int vectis_openapi_field_is_array(const lonejson_field *field) {
  return field->kind == LONEJSON_FIELD_KIND_STRING_ARRAY ||
         field->kind == LONEJSON_FIELD_KIND_I64_ARRAY ||
         field->kind == LONEJSON_FIELD_KIND_U64_ARRAY ||
         field->kind == LONEJSON_FIELD_KIND_F64_ARRAY ||
         field->kind == LONEJSON_FIELD_KIND_BOOL_ARRAY ||
         field->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY;
}

static lonejson_status vectis_openapi_writer_ref(lonejson_writer *writer,
                                                 vectis_openapi_schema schema,
                                                 lonejson_error *error) {
  const char *prefix = "#/components/schemas/";
  const char *name;

  name = vectis_openapi_schema_name(schema);
  return lonejson_writer_begin_object(writer, error) == LONEJSON_STATUS_OK &&
                 lonejson_writer_key(writer, "$ref", 4u, error) ==
                     LONEJSON_STATUS_OK &&
                 lonejson_writer_string_begin(writer, error) ==
                     LONEJSON_STATUS_OK &&
                 lonejson_writer_string_chunk(writer, prefix, strlen(prefix),
                                              error) == LONEJSON_STATUS_OK &&
                 lonejson_writer_string_chunk(writer, name, strlen(name),
                                              error) == LONEJSON_STATUS_OK &&
                 lonejson_writer_string_end(writer, error) ==
                     LONEJSON_STATUS_OK &&
                 lonejson_writer_end_object(writer, error) == LONEJSON_STATUS_OK
             ? LONEJSON_STATUS_OK
             : error->code;
}

static lonejson_status
vectis_openapi_writer_field_schema(lonejson_writer *writer,
                                   const lonejson_field *field,
                                   lonejson_error *error) {
  const char *type;

  type = vectis_openapi_field_type(field);
  if (vectis_openapi_field_is_array(field)) {
    if (field->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY &&
        field->submap != NULL) {
      vectis_openapi_schema schema;

      schema.name = field->submap->name;
      schema.map = field->submap;
      if (lonejson_writer_begin_object(writer, error) != LONEJSON_STATUS_OK ||
          lonejson_writer_key(writer, "type", 4u, error) !=
              LONEJSON_STATUS_OK ||
          lonejson_writer_string(writer, "array", 5u, error) !=
              LONEJSON_STATUS_OK ||
          lonejson_writer_key(writer, "items", 5u, error) !=
              LONEJSON_STATUS_OK ||
          vectis_openapi_writer_ref(writer, schema, error) !=
              LONEJSON_STATUS_OK ||
          lonejson_writer_end_object(writer, error) != LONEJSON_STATUS_OK) {
        return error->code;
      }
      return LONEJSON_STATUS_OK;
    }
    if (lonejson_writer_begin_object(writer, error) != LONEJSON_STATUS_OK ||
        lonejson_writer_key(writer, "type", 4u, error) != LONEJSON_STATUS_OK ||
        lonejson_writer_string(writer, "array", 5u, error) !=
            LONEJSON_STATUS_OK ||
        lonejson_writer_key(writer, "items", 5u, error) != LONEJSON_STATUS_OK ||
        lonejson_writer_begin_object(writer, error) != LONEJSON_STATUS_OK ||
        lonejson_writer_key(writer, "type", 4u, error) != LONEJSON_STATUS_OK ||
        lonejson_writer_string(writer, type, strlen(type), error) !=
            LONEJSON_STATUS_OK ||
        lonejson_writer_end_object(writer, error) != LONEJSON_STATUS_OK ||
        lonejson_writer_end_object(writer, error) != LONEJSON_STATUS_OK) {
      return error->code;
    }
    return LONEJSON_STATUS_OK;
  }
  if (field->kind == LONEJSON_FIELD_KIND_OBJECT && field->submap != NULL) {
    vectis_openapi_schema schema;

    schema.name = field->submap->name;
    schema.map = field->submap;
    return vectis_openapi_writer_ref(writer, schema, error);
  }
  if (lonejson_writer_begin_object(writer, error) != LONEJSON_STATUS_OK ||
      lonejson_writer_key(writer, "type", 4u, error) != LONEJSON_STATUS_OK ||
      lonejson_writer_string(writer, type, strlen(type), error) !=
          LONEJSON_STATUS_OK ||
      lonejson_writer_end_object(writer, error) != LONEJSON_STATUS_OK) {
    return error->code;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
vectis_openapi_writer_schema(lonejson_writer *writer,
                             vectis_openapi_schema schema,
                             lonejson_error *error) {
  const lonejson_map *map;
  const lonejson_field *field;
  size_t i;
  size_t required_count;

  map = schema.map;
  if (map == NULL) {
    lonejson_error_init(error);
    error->code = LONEJSON_STATUS_INVALID_ARGUMENT;
    (void)snprintf(error->message, sizeof(error->message), "%s",
                   "OpenAPI schema map is required");
    return LONEJSON_STATUS_INVALID_ARGUMENT;
  }
  if (lonejson_writer_begin_object(writer, error) != LONEJSON_STATUS_OK ||
      lonejson_writer_key(writer, "type", 4u, error) != LONEJSON_STATUS_OK ||
      lonejson_writer_string(writer, "object", 6u, error) !=
          LONEJSON_STATUS_OK ||
      lonejson_writer_key(writer, "properties", 10u, error) !=
          LONEJSON_STATUS_OK ||
      lonejson_writer_begin_object(writer, error) != LONEJSON_STATUS_OK) {
    return error->code;
  }
  for (i = 0u; i < map->field_count; ++i) {
    field = &map->fields[i];
    if (lonejson_writer_key(writer, field->json_key, strlen(field->json_key),
                            error) != LONEJSON_STATUS_OK ||
        vectis_openapi_writer_field_schema(writer, field, error) !=
            LONEJSON_STATUS_OK) {
      return error->code;
    }
  }
  required_count = 0u;
  for (i = 0u; i < map->field_count; ++i) {
    if ((map->fields[i].flags & LONEJSON_FIELD_REQUIRED) != 0u) {
      required_count++;
    }
  }
  if (lonejson_writer_end_object(writer, error) != LONEJSON_STATUS_OK) {
    return error->code;
  }
  if (required_count > 0u) {
    if (lonejson_writer_key(writer, "required", 8u, error) !=
            LONEJSON_STATUS_OK ||
        lonejson_writer_begin_array(writer, error) != LONEJSON_STATUS_OK) {
      return error->code;
    }
    for (i = 0u; i < map->field_count; ++i) {
      if ((map->fields[i].flags & LONEJSON_FIELD_REQUIRED) != 0u) {
        if (lonejson_writer_string(writer, map->fields[i].json_key,
                                   strlen(map->fields[i].json_key),
                                   error) != LONEJSON_STATUS_OK) {
          return error->code;
        }
      }
    }
    if (lonejson_writer_end_array(writer, error) != LONEJSON_STATUS_OK) {
      return error->code;
    }
  }
  return lonejson_writer_end_object(writer, error);
}

static int vectis_openapi_schema_seen(const vectis_openapi_schema *schemas,
                                      size_t count,
                                      vectis_openapi_schema schema) {
  const char *name;
  size_t i;

  name = vectis_openapi_schema_name(schema);
  for (i = 0u; i < count; ++i) {
    if (strcmp(vectis_openapi_schema_name(schemas[i]), name) == 0) {
      return 1;
    }
  }
  return 0;
}

static vectis_status vectis_openapi_collect_schema_children(
    vectis_openapi_schema **schemas, size_t *count, size_t *capacity,
    const lonejson_map *map, vectis_error *error);

static vectis_status
vectis_openapi_collect_schema(vectis_openapi_schema **schemas, size_t *count,
                              size_t *capacity, vectis_openapi_schema schema,
                              vectis_error *error) {
  vectis_openapi_schema *grown;
  size_t next_capacity;

  if (schema.map == NULL ||
      vectis_openapi_schema_seen(*schemas, *count, schema)) {
    return VECTIS_OK;
  }
  if (*count == *capacity) {
    next_capacity = *capacity == 0u ? 8u : *capacity * 2u;
    grown = (vectis_openapi_schema *)realloc(*schemas,
                                             next_capacity * sizeof(*grown));
    if (grown == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to collect OpenAPI schemas");
      return VECTIS_ERR_NOMEM;
    }
    *schemas = grown;
    *capacity = next_capacity;
  }
  (*schemas)[*count] = schema;
  (*count)++;
  return vectis_openapi_collect_schema_children(schemas, count, capacity,
                                                schema.map, error);
}

static vectis_status vectis_openapi_collect_schema_children(
    vectis_openapi_schema **schemas, size_t *count, size_t *capacity,
    const lonejson_map *map, vectis_error *error) {
  vectis_openapi_schema child;
  const lonejson_field *field;
  size_t i;

  if (map == NULL) {
    return VECTIS_OK;
  }
  for (i = 0u; i < map->field_count; ++i) {
    field = &map->fields[i];
    if ((field->kind == LONEJSON_FIELD_KIND_OBJECT ||
         field->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY) &&
        field->submap != NULL) {
      child.name = field->submap->name;
      child.map = field->submap;
      if (vectis_openapi_collect_schema(schemas, count, capacity, child,
                                        error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
    }
  }
  return VECTIS_OK;
}

static vectis_status vectis_openapi_collect_doc_schemas(
    const vectis_openapi_route_doc *doc, vectis_openapi_schema **schemas,
    size_t *count, size_t *capacity, vectis_error *error) {
  size_t i;

  if (doc == NULL) {
    return VECTIS_OK;
  }
  if (vectis_openapi_collect_schema(schemas, count, capacity,
                                    doc->request_schema, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  for (i = 0u; i < doc->response_count; ++i) {
    if (vectis_openapi_collect_schema(schemas, count, capacity,
                                      doc->responses[i].schema,
                                      error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
  }
  return VECTIS_OK;
}

static void vectis_openapi_paths_cleanup(char **paths, size_t count) {
  size_t i;

  if (paths == NULL) {
    return;
  }
  for (i = 0u; i < count; ++i) {
    free(paths[i]);
  }
  free(paths);
}

static vectis_status vectis_openapi_collect_paths(vectis_app_impl *impl,
                                                  char ***paths_out,
                                                  vectis_error *error) {
  vectis_string_builder builder;
  char **paths;
  size_t i;

  if (paths_out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OpenAPI path output is required");
    return VECTIS_ERR_INVALID;
  }
  *paths_out = NULL;
  if (impl == NULL || impl->openapi_doc_count == 0u) {
    return VECTIS_OK;
  }
  paths = (char **)calloc(impl->openapi_doc_count, sizeof(*paths));
  if (paths == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to collect OpenAPI paths");
    return VECTIS_ERR_NOMEM;
  }
  memset(&builder, 0, sizeof(builder));
  for (i = 0u; i < impl->openapi_doc_count; ++i) {
    builder.size = 0u;
    if (builder.data != NULL) {
      builder.data[0] = '\0';
    }
    if (vectis_openapi_append_path(&builder, impl->openapi_docs[i].path,
                                   error) != VECTIS_OK) {
      vectis_string_builder_cleanup(&builder);
      vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    paths[i] = vectis_strdup(builder.data != NULL ? builder.data : "");
    if (paths[i] == NULL) {
      vectis_string_builder_cleanup(&builder);
      vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy OpenAPI path");
      return VECTIS_ERR_NOMEM;
    }
  }
  vectis_string_builder_cleanup(&builder);
  *paths_out = paths;
  return VECTIS_OK;
}

static int vectis_openapi_path_seen(const char *const *paths, size_t index) {
  size_t i;

  if (paths == NULL || paths[index] == NULL) {
    return 0;
  }
  for (i = 0u; i < index; ++i) {
    if (paths[i] != NULL && strcmp(paths[i], paths[index]) == 0) {
      return 1;
    }
  }
  return 0;
}

static vectis_status vectis_generate_openapi_json(
    vectis_app_impl *impl, const vectis_openapi_document *document,
    vectis_string_builder *builder, vectis_error *error) {
  vectis_lonejson_builder_sink sink;
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status json_status;
  lonejson *runtime;
  vectis_openapi_schema *schemas;
  char **paths;
  size_t schema_count;
  size_t schema_capacity;
  size_t i;
  size_t j;
  size_t k;
  vectis_http_method method;
  const vectis_openapi_route_doc *doc;
  char status_key[16];

  schemas = NULL;
  paths = NULL;
  schema_count = 0u;
  schema_capacity = 0u;
  for (i = 0u; i < impl->openapi_doc_count; ++i) {
    if (vectis_openapi_collect_doc_schemas(&impl->openapi_docs[i].doc, &schemas,
                                           &schema_count, &schema_capacity,
                                           error) != VECTIS_OK) {
      free(schemas);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
  }
  if (vectis_openapi_collect_paths(impl, &paths, error) != VECTIS_OK) {
    free(schemas);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  sink.builder = builder;
  sink.error = error;
  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
    free(schemas);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  json_status = lonejson_writer_init_sink(
      runtime, &writer, vectis_lonejson_builder_sink_write, &sink, &json_error);
  if (json_status != LONEJSON_STATUS_OK) {
    goto json_failed;
  }
  if (lonejson_writer_begin_object(&writer, &json_error) !=
          LONEJSON_STATUS_OK ||
      lonejson_writer_key(&writer, "openapi", 7u, &json_error) !=
          LONEJSON_STATUS_OK ||
      lonejson_writer_string(&writer, "3.1.0", 5u, &json_error) !=
          LONEJSON_STATUS_OK ||
      lonejson_writer_key(&writer, "info", 4u, &json_error) !=
          LONEJSON_STATUS_OK ||
      lonejson_writer_begin_object(&writer, &json_error) !=
          LONEJSON_STATUS_OK ||
      lonejson_writer_key(&writer, "title", 5u, &json_error) !=
          LONEJSON_STATUS_OK ||
      lonejson_writer_string(
          &writer, document->title != NULL ? document->title : "",
          document->title != NULL ? strlen(document->title) : 0u,
          &json_error) != LONEJSON_STATUS_OK ||
      lonejson_writer_key(&writer, "version", 7u, &json_error) !=
          LONEJSON_STATUS_OK ||
      lonejson_writer_string(
          &writer, document->version != NULL ? document->version : "",
          document->version != NULL ? strlen(document->version) : 0u,
          &json_error) != LONEJSON_STATUS_OK ||
      lonejson_writer_end_object(&writer, &json_error) != LONEJSON_STATUS_OK ||
      lonejson_writer_key(&writer, "paths", 5u, &json_error) !=
          LONEJSON_STATUS_OK ||
      lonejson_writer_begin_object(&writer, &json_error) !=
          LONEJSON_STATUS_OK) {
    json_status = json_error.code;
    goto json_cleanup_failed;
  }
  for (i = 0u; i < impl->openapi_doc_count; ++i) {
    if (vectis_openapi_path_seen((const char *const *)paths, i)) {
      continue;
    }
    if (lonejson_writer_key(&writer, paths[i] != NULL ? paths[i] : "",
                            paths[i] != NULL ? strlen(paths[i]) : 0u,
                            &json_error) != LONEJSON_STATUS_OK ||
        lonejson_writer_begin_object(&writer, &json_error) !=
            LONEJSON_STATUS_OK) {
      json_status = json_error.code;
      goto json_cleanup_failed;
    }
    for (k = 0u; k < impl->openapi_doc_count; ++k) {
      if (paths[k] == NULL || strcmp(paths[k], paths[i]) != 0) {
        continue;
      }
      doc = &impl->openapi_docs[k].doc;
      for (method = VECTIS_HTTP_GET; method <= VECTIS_HTTP_MOVE; ++method) {
        if ((impl->openapi_docs[k].methods & VECTIS_HTTP_METHOD_MASK(method)) !=
            0u) {
          if (lonejson_writer_key(&writer, vectis_openapi_method_name(method),
                                  strlen(vectis_openapi_method_name(method)),
                                  &json_error) != LONEJSON_STATUS_OK ||
              lonejson_writer_begin_object(&writer, &json_error) !=
                  LONEJSON_STATUS_OK) {
            json_status = json_error.code;
            goto json_cleanup_failed;
          }
          if (doc->operation_id != NULL) {
            if (lonejson_writer_key(&writer, "operationId", 11u, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_string(&writer, doc->operation_id,
                                       strlen(doc->operation_id),
                                       &json_error) != LONEJSON_STATUS_OK) {
              json_status = json_error.code;
              goto json_cleanup_failed;
            }
          }
          if (doc->summary != NULL) {
            if (lonejson_writer_key(&writer, "summary", 7u, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_string(&writer, doc->summary,
                                       strlen(doc->summary),
                                       &json_error) != LONEJSON_STATUS_OK) {
              json_status = json_error.code;
              goto json_cleanup_failed;
            }
          }
          if (doc->tag_count > 0u) {
            if (lonejson_writer_key(&writer, "tags", 4u, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_begin_array(&writer, &json_error) !=
                    LONEJSON_STATUS_OK) {
              json_status = json_error.code;
              goto json_cleanup_failed;
            }
            for (j = 0u; j < doc->tag_count; ++j) {
              if (lonejson_writer_string(
                      &writer, doc->tags[j] != NULL ? doc->tags[j] : "",
                      doc->tags[j] != NULL ? strlen(doc->tags[j]) : 0u,
                      &json_error) != LONEJSON_STATUS_OK) {
                json_status = json_error.code;
                goto json_cleanup_failed;
              }
            }
            if (lonejson_writer_end_array(&writer, &json_error) !=
                LONEJSON_STATUS_OK) {
              json_status = json_error.code;
              goto json_cleanup_failed;
            }
          }
          if (doc->request_schema.map != NULL) {
            if (lonejson_writer_key(&writer, "requestBody", 11u, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_begin_object(&writer, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_key(&writer, "content", 7u, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_begin_object(&writer, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_key(&writer, "application/json", 16u,
                                    &json_error) != LONEJSON_STATUS_OK ||
                lonejson_writer_begin_object(&writer, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_key(&writer, "schema", 6u, &json_error) !=
                    LONEJSON_STATUS_OK ||
                vectis_openapi_writer_ref(&writer, doc->request_schema,
                                          &json_error) != LONEJSON_STATUS_OK ||
                lonejson_writer_end_object(&writer, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_end_object(&writer, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_key(&writer, "required", 8u, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_bool(&writer, 1, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_end_object(&writer, &json_error) !=
                    LONEJSON_STATUS_OK) {
              json_status = json_error.code;
              goto json_cleanup_failed;
            }
          }
          if (lonejson_writer_key(&writer, "responses", 9u, &json_error) !=
                  LONEJSON_STATUS_OK ||
              lonejson_writer_begin_object(&writer, &json_error) !=
                  LONEJSON_STATUS_OK) {
            json_status = json_error.code;
            goto json_cleanup_failed;
          }
          for (j = 0u; j < doc->response_count; ++j) {
            (void)snprintf(status_key, sizeof(status_key), "%d",
                           doc->responses[j].status_code);
            if (lonejson_writer_key(&writer, status_key, strlen(status_key),
                                    &json_error) != LONEJSON_STATUS_OK ||
                lonejson_writer_begin_object(&writer, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_key(&writer, "description", 11u, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_string(
                    &writer,
                    doc->responses[j].description != NULL
                        ? doc->responses[j].description
                        : "",
                    doc->responses[j].description != NULL
                        ? strlen(doc->responses[j].description)
                        : 0u,
                    &json_error) != LONEJSON_STATUS_OK ||
                lonejson_writer_key(&writer, "content", 7u, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_begin_object(&writer, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_key(&writer, "application/json", 16u,
                                    &json_error) != LONEJSON_STATUS_OK ||
                lonejson_writer_begin_object(&writer, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_key(&writer, "schema", 6u, &json_error) !=
                    LONEJSON_STATUS_OK ||
                vectis_openapi_writer_ref(&writer, doc->responses[j].schema,
                                          &json_error) != LONEJSON_STATUS_OK ||
                lonejson_writer_end_object(&writer, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_end_object(&writer, &json_error) !=
                    LONEJSON_STATUS_OK ||
                lonejson_writer_end_object(&writer, &json_error) !=
                    LONEJSON_STATUS_OK) {
              json_status = json_error.code;
              goto json_cleanup_failed;
            }
          }
          if (lonejson_writer_end_object(&writer, &json_error) !=
                  LONEJSON_STATUS_OK ||
              lonejson_writer_end_object(&writer, &json_error) !=
                  LONEJSON_STATUS_OK) {
            json_status = json_error.code;
            goto json_cleanup_failed;
          }
        }
      }
    }
    if (lonejson_writer_end_object(&writer, &json_error) !=
        LONEJSON_STATUS_OK) {
      json_status = json_error.code;
      goto json_cleanup_failed;
    }
  }
  if (lonejson_writer_end_object(&writer, &json_error) != LONEJSON_STATUS_OK ||
      lonejson_writer_key(&writer, "components", 10u, &json_error) !=
          LONEJSON_STATUS_OK ||
      lonejson_writer_begin_object(&writer, &json_error) !=
          LONEJSON_STATUS_OK ||
      lonejson_writer_key(&writer, "schemas", 7u, &json_error) !=
          LONEJSON_STATUS_OK ||
      lonejson_writer_begin_object(&writer, &json_error) !=
          LONEJSON_STATUS_OK) {
    json_status = json_error.code;
    goto json_cleanup_failed;
  }
  for (i = 0u; i < schema_count; ++i) {
    if (lonejson_writer_key(&writer, vectis_openapi_schema_name(schemas[i]),
                            strlen(vectis_openapi_schema_name(schemas[i])),
                            &json_error) != LONEJSON_STATUS_OK ||
        vectis_openapi_writer_schema(&writer, schemas[i], &json_error) !=
            LONEJSON_STATUS_OK) {
      json_status = json_error.code;
      goto json_cleanup_failed;
    }
  }
  if (lonejson_writer_end_object(&writer, &json_error) != LONEJSON_STATUS_OK ||
      lonejson_writer_end_object(&writer, &json_error) != LONEJSON_STATUS_OK ||
      lonejson_writer_end_object(&writer, &json_error) != LONEJSON_STATUS_OK) {
    json_status = json_error.code;
    goto json_cleanup_failed;
  }
  json_status = lonejson_writer_finish(&writer, &json_error);
  lonejson_writer_cleanup(&writer);
  lonejson_free(runtime);
  runtime = NULL;
  vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
  free(schemas);
  if (json_status == LONEJSON_STATUS_OK) {
    return VECTIS_OK;
  }
  goto json_failed;

json_cleanup_failed:
  lonejson_writer_cleanup(&writer);
json_failed:
  if (runtime != NULL) {
    lonejson_free(runtime);
  }
  vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
  free(schemas);
  vectis_set_errorf(
      error, VECTIS_ERR_INVALID,
      "failed to serialize OpenAPI document through lonejson writer: %s",
      json_error.message);
  if (error != NULL) {
    error->source = VECTIS_ERROR_SOURCE_LONEJSON;
    error->dependency_code = (long)json_status;
  }
  return VECTIS_ERR_INVALID;
}

static vectis_status vectis_generate_openapi_yaml(
    vectis_app_impl *impl, const vectis_openapi_document *document,
    vectis_string_builder *builder, vectis_error *error) {
  vectis_openapi_schema *schemas;
  char **paths;
  size_t schema_count;
  size_t schema_capacity;
  size_t i;
  size_t j;
  size_t k;
  vectis_http_method method;
  const vectis_openapi_route_doc *doc;
  const lonejson_map *map;
  const lonejson_field *field;

  schemas = NULL;
  paths = NULL;
  schema_count = 0u;
  schema_capacity = 0u;
  for (i = 0u; i < impl->openapi_doc_count; ++i) {
    if (vectis_openapi_collect_doc_schemas(&impl->openapi_docs[i].doc, &schemas,
                                           &schema_count, &schema_capacity,
                                           error) != VECTIS_OK) {
      free(schemas);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
  }
  if (vectis_openapi_collect_paths(impl, &paths, error) != VECTIS_OK) {
    free(schemas);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  if (vectis_string_builder_append(
          builder, "openapi: 3.1.0\ninfo:\n  title: ", error) != VECTIS_OK ||
      vectis_append_lonejson_string(builder, document->title, error) !=
          VECTIS_OK ||
      vectis_string_builder_append(builder, "\n  version: ", error) !=
          VECTIS_OK ||
      vectis_append_lonejson_string(builder, document->version, error) !=
          VECTIS_OK ||
      vectis_string_builder_append(builder, "\npaths:\n", error) != VECTIS_OK) {
    vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
    free(schemas);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  for (i = 0u; i < impl->openapi_doc_count; ++i) {
    if (vectis_openapi_path_seen((const char *const *)paths, i)) {
      continue;
    }
    if (vectis_string_builder_append(builder, "  ", error) != VECTIS_OK ||
        vectis_append_lonejson_string(builder, paths[i], error) != VECTIS_OK ||
        vectis_string_builder_append(builder, ":\n", error) != VECTIS_OK) {
      vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
      free(schemas);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    for (k = 0u; k < impl->openapi_doc_count; ++k) {
      if (paths[k] == NULL || strcmp(paths[k], paths[i]) != 0) {
        continue;
      }
      doc = &impl->openapi_docs[k].doc;
      for (method = VECTIS_HTTP_GET; method <= VECTIS_HTTP_MOVE; ++method) {
        if ((impl->openapi_docs[k].methods & VECTIS_HTTP_METHOD_MASK(method)) !=
            0u) {
          if (vectis_string_builder_appendf(
                  builder, error, "    %s:\n",
                  vectis_openapi_method_name(method)) != VECTIS_OK) {
            vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
            free(schemas);
            return error != NULL ? error->code : VECTIS_ERR_NOMEM;
          }
          if (doc->operation_id != NULL &&
              (vectis_string_builder_append(
                   builder, "      operationId: ", error) != VECTIS_OK ||
               vectis_append_lonejson_string(builder, doc->operation_id,
                                             error) != VECTIS_OK ||
               vectis_string_builder_append(builder, "\n", error) !=
                   VECTIS_OK)) {
            vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
            free(schemas);
            return error != NULL ? error->code : VECTIS_ERR_NOMEM;
          }
          if (doc->summary != NULL &&
              (vectis_string_builder_append(
                   builder, "      summary: ", error) != VECTIS_OK ||
               vectis_append_lonejson_string(builder, doc->summary, error) !=
                   VECTIS_OK ||
               vectis_string_builder_append(builder, "\n", error) !=
                   VECTIS_OK)) {
            vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
            free(schemas);
            return error != NULL ? error->code : VECTIS_ERR_NOMEM;
          }
          if (doc->tag_count > 0u) {
            if (vectis_string_builder_append(builder, "      tags:\n", error) !=
                VECTIS_OK) {
              vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
              free(schemas);
              return error != NULL ? error->code : VECTIS_ERR_NOMEM;
            }
            for (j = 0u; j < doc->tag_count; ++j) {
              if (vectis_string_builder_append(builder, "        - ", error) !=
                      VECTIS_OK ||
                  vectis_append_lonejson_string(builder, doc->tags[j], error) !=
                      VECTIS_OK ||
                  vectis_string_builder_append(builder, "\n", error) !=
                      VECTIS_OK) {
                vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
                free(schemas);
                return error != NULL ? error->code : VECTIS_ERR_NOMEM;
              }
            }
          }
          if (doc->request_schema.map != NULL &&
              (vectis_string_builder_append(
                   builder,
                   "      requestBody:\n        required: true\n        "
                   "content:\n          application/json:\n            "
                   "schema:\n              $ref: \"#/components/schemas/",
                   error) != VECTIS_OK ||
               vectis_string_builder_append(
                   builder, vectis_openapi_schema_name(doc->request_schema),
                   error) != VECTIS_OK ||
               vectis_string_builder_append(builder, "\"\n", error) !=
                   VECTIS_OK)) {
            vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
            free(schemas);
            return error != NULL ? error->code : VECTIS_ERR_NOMEM;
          }
          if (vectis_string_builder_append(builder, "      responses:\n",
                                           error) != VECTIS_OK) {
            vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
            free(schemas);
            return error != NULL ? error->code : VECTIS_ERR_NOMEM;
          }
          for (j = 0u; j < doc->response_count; ++j) {
            if (vectis_string_builder_appendf(
                    builder, error, "        \"%d\":\n          description: ",
                    doc->responses[j].status_code) != VECTIS_OK ||
                vectis_append_lonejson_string(
                    builder,
                    doc->responses[j].description != NULL
                        ? doc->responses[j].description
                        : "",
                    error) != VECTIS_OK ||
                vectis_string_builder_append(
                    builder,
                    "\n          content:\n            application/json:\n     "
                    "         schema:\n                $ref: "
                    "\"#/components/schemas/",
                    error) != VECTIS_OK ||
                vectis_string_builder_append(
                    builder,
                    vectis_openapi_schema_name(doc->responses[j].schema),
                    error) != VECTIS_OK ||
                vectis_string_builder_append(builder, "\"\n", error) !=
                    VECTIS_OK) {
              vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
              free(schemas);
              return error != NULL ? error->code : VECTIS_ERR_NOMEM;
            }
          }
        }
      }
    }
  }
  if (vectis_string_builder_append(builder, "components:\n  schemas:\n",
                                   error) != VECTIS_OK) {
    vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
    free(schemas);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  for (i = 0u; i < schema_count; ++i) {
    map = schemas[i].map;
    if (vectis_string_builder_appendf(
            builder, error, "    %s:\n      type: object\n      properties:\n",
            vectis_openapi_schema_name(schemas[i])) != VECTIS_OK) {
      vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
      free(schemas);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    for (j = 0u; j < map->field_count; ++j) {
      field = &map->fields[j];
      if (field->kind == LONEJSON_FIELD_KIND_OBJECT && field->submap != NULL) {
        if (vectis_string_builder_appendf(
                builder, error,
                "        %s:\n          $ref: \"#/components/schemas/%s\"\n",
                field->json_key, field->submap->name) != VECTIS_OK) {
          vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
          free(schemas);
          return error != NULL ? error->code : VECTIS_ERR_NOMEM;
        }
        continue;
      }
      if (vectis_string_builder_appendf(
              builder, error, "        %s:\n          type: %s\n",
              field->json_key,
              vectis_openapi_field_is_array(field)
                  ? "array"
                  : vectis_openapi_field_type(field)) != VECTIS_OK) {
        vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
        free(schemas);
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
      if (field->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY &&
          field->submap != NULL) {
        if (vectis_string_builder_appendf(builder, error,
                                          "          items:\n            $ref: "
                                          "\"#/components/schemas/%s\"\n",
                                          field->submap->name) != VECTIS_OK) {
          vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
          free(schemas);
          return error != NULL ? error->code : VECTIS_ERR_NOMEM;
        }
      } else if (vectis_openapi_field_is_array(field) &&
                 vectis_string_builder_appendf(
                     builder, error, "          items:\n            type: %s\n",
                     vectis_openapi_field_type(field)) != VECTIS_OK) {
        vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
        free(schemas);
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
    }
    if (vectis_string_builder_append(builder, "      required:\n", error) !=
        VECTIS_OK) {
      vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
      free(schemas);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    for (j = 0u; j < map->field_count; ++j) {
      if ((map->fields[j].flags & LONEJSON_FIELD_REQUIRED) != 0u &&
          vectis_string_builder_appendf(builder, error, "        - %s\n",
                                        map->fields[j].json_key) != VECTIS_OK) {
        vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
        free(schemas);
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
    }
  }
  vectis_openapi_paths_cleanup(paths, impl->openapi_doc_count);
  free(schemas);
  return VECTIS_OK;
}

vectis_status vectis_generate_openapi(vectis_app *app,
                                      const vectis_openapi_document *document,
                                      vectis_openapi_format format,
                                      vectis_mutable_bytes *out,
                                      vectis_error *error) {
  vectis_openapi_document default_document;
  vectis_string_builder builder;
  lonejson_json_value json_value;
  lonejson_error json_error;
  lonejson_status json_status;
  vectis_status status;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "OpenAPI output is required");
    return VECTIS_ERR_INVALID;
  }
  if (format != VECTIS_OPENAPI_JSON && format != VECTIS_OPENAPI_YAML) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "OpenAPI format is invalid");
    return VECTIS_ERR_INVALID;
  }
  out->data = NULL;
  out->size = 0u;
  if (document == NULL) {
    vectis_openapi_document_init(&default_document);
    document = &default_document;
  }
  memset(&builder, 0, sizeof(builder));
  if (format == VECTIS_OPENAPI_JSON) {
    status = vectis_generate_openapi_json((vectis_app_impl *)app->impl,
                                          document, &builder, error);
  } else {
    status = vectis_generate_openapi_yaml((vectis_app_impl *)app->impl,
                                          document, &builder, error);
  }
  if (status != VECTIS_OK) {
    vectis_string_builder_cleanup(&builder);
    return status;
  }
  if (format == VECTIS_OPENAPI_JSON) {
    lonejson *runtime;

    runtime = vectis_lonejson_new(error);
    if (runtime == NULL) {
      vectis_string_builder_cleanup(&builder);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    lonejson_json_value_init(runtime, &json_value);
    json_status = lonejson_json_value_set_buffer(&json_value, builder.data,
                                                 builder.size, &json_error);
    if (json_status != LONEJSON_STATUS_OK) {
      lonejson_json_value_cleanup(&json_value);
      lonejson_free(runtime);
      vectis_string_builder_cleanup(&builder);
      vectis_set_errorf(error, VECTIS_ERR_INVALID,
                        "failed to validate generated OpenAPI JSON: %s",
                        json_error.message);
      if (error != NULL) {
        error->source = VECTIS_ERROR_SOURCE_LONEJSON;
        error->dependency_code = (long)json_status;
      }
      return VECTIS_ERR_INVALID;
    }
    lonejson_json_value_cleanup(&json_value);
    lonejson_free(runtime);
  }
  out->data = builder.data;
  out->size = builder.size;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static size_t vectis_app_route_count_impl(const vectis_app *app) {
  vectis_app_impl *impl;
  size_t count;

  if (app == NULL || app->impl == NULL) {
    return 0u;
  }
  impl = (vectis_app_impl *)app->impl;
  (void)pthread_mutex_lock(&impl->mutex);
  count = impl->route_count;
  (void)pthread_mutex_unlock(&impl->mutex);
  return count;
}

static size_t vectis_app_body_disk_offload_bytes(vectis_app_impl *impl,
                                                 int *configured) {
  size_t min_bytes;
  size_t limit;
  size_t i;

  if (configured != NULL) {
    *configured = 0;
  }
  if (impl == NULL) {
    return 0u;
  }
  min_bytes = 0u;
  (void)pthread_mutex_lock(&impl->mutex);
  for (i = 0u; i < impl->route_count; ++i) {
    if (impl->routes[i].body.mode != VECTIS_BODY_STREAMING_UPLOAD) {
      continue;
    }
    if (configured != NULL) {
      *configured = 1;
    }
    if (!impl->routes[i].body.disk_spool_disabled) {
      limit = impl->routes[i].body.memory_buffer_limit_bytes;
      if (limit == 0u) {
        limit = VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
      }
      if (limit > 0u && (min_bytes == 0u || limit < min_bytes)) {
        min_bytes = limit;
      }
    }
  }
  (void)pthread_mutex_unlock(&impl->mutex);
  return min_bytes;
}

static size_t vectis_app_max_request_body_bytes(vectis_app_impl *impl) {
  size_t max_bytes;
  size_t i;

  if (impl == NULL) {
    return VECTIS_SERVER_DEFAULT_MAX_REQUEST_BODY_BYTES;
  }
  if (impl->server.max_request_body_bytes > 0u) {
    return impl->server.max_request_body_bytes;
  }
  max_bytes = VECTIS_SERVER_DEFAULT_MAX_REQUEST_BODY_BYTES;
  (void)pthread_mutex_lock(&impl->mutex);
  for (i = 0u; i < impl->route_count; ++i) {
    if (impl->routes[i].body.mode != VECTIS_BODY_NONE &&
        impl->routes[i].body.max_bytes > max_bytes) {
      max_bytes = impl->routes[i].body.max_bytes;
    }
  }
  (void)pthread_mutex_unlock(&impl->mutex);
  return max_bytes;
}

size_t vectis_internal_max_request_body_bytes(vectis_app *app) {
  if (app == NULL || app->impl == NULL) {
    return VECTIS_SERVER_DEFAULT_MAX_REQUEST_BODY_BYTES;
  }
  return vectis_app_max_request_body_bytes((vectis_app_impl *)app->impl);
}

size_t vectis_route_count(const vectis_app *app) {
  if (app == NULL || app->impl == NULL) {
    return 0u;
  }
  return vectis_app_route_count_impl(app);
}

vectis_status vectis_internal_invoke_route(vectis_app *app, size_t index,
                                           vectis_request *request,
                                           vectis_response *response,
                                           vectis_error *error) {
  vectis_app_impl *impl;
  vectis_route_handler_fn handler;
  void *userdata;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request is required");
    return VECTIS_ERR_INVALID;
  }
  if (response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response is required");
    return VECTIS_ERR_INVALID;
  }

  impl = (vectis_app_impl *)app->impl;
  (void)pthread_mutex_lock(&impl->mutex);
  if (index >= impl->route_count) {
    (void)pthread_mutex_unlock(&impl->mutex);
    vectis_set_error(error, VECTIS_ERR_INVALID, "route index is invalid");
    return VECTIS_ERR_INVALID;
  }
  handler = impl->routes[index].handler;
  userdata = impl->routes[index].userdata;
  (void)pthread_mutex_unlock(&impl->mutex);

  if (handler == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "route handler is invalid");
    return VECTIS_ERR_INVALID;
  }
  return handler(app, request, response, userdata, error);
}

static int vectis_route_method_matches(const vectis_route_entry *route,
                                       vectis_http_method method) {
  vectis_http_methods mask;

  mask = vectis_method_mask(method);
  return mask != VECTIS_HTTP_METHODS_NONE && (route->methods & mask) != 0u;
}

static int vectis_path_segment_is_safe(const char *value, size_t len) {
  if (value == NULL || len == 0u) {
    return 0;
  }
  if ((len == 1u && value[0] == '.') ||
      (len == 2u && value[0] == '.' && value[1] == '.')) {
    return 0;
  }
  return 1;
}

static vectis_status
vectis_add_path_param_segment(vectis_request *request, const char *name,
                              size_t name_len, const char *value,
                              size_t value_len, vectis_error *error) {
  char *name_copy;
  char *value_copy;
  vectis_status status;

  name_copy = vectis_strndup(name, name_len);
  value_copy = vectis_strndup(value, value_len);
  if (name_copy == NULL || value_copy == NULL) {
    free(name_copy);
    free(value_copy);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy path parameter");
    return VECTIS_ERR_NOMEM;
  }
  status = vectis_internal_request_add_path_param(request, name_copy,
                                                  value_copy, error);
  free(name_copy);
  free(value_copy);
  return status;
}

static int vectis_match_param_route_segments(const char *route,
                                             const char *path,
                                             vectis_request *request,
                                             vectis_error *error) {
  const char *route_end;
  const char *path_end;
  const char *next_route;
  const char *next_path;
  const char *name_start;
  size_t route_len;
  size_t path_len;
  size_t name_len;
  size_t saved_count;
  int optional;

  if (*route == '\0') {
    return *path == '\0';
  }
  if (*path == '\0') {
    if (*route == ':' && strchr(route, '/') == NULL) {
      route_end = route + strlen(route);
      return route_end > route && route_end[-1] == '?';
    }
    return 0;
  }

  route_end = strchr(route, '/');
  if (route_end == NULL) {
    route_end = route + strlen(route);
  }
  path_end = strchr(path, '/');
  if (path_end == NULL) {
    path_end = path + strlen(path);
  }
  next_route = *route_end == '/' ? route_end + 1 : route_end;
  next_path = *path_end == '/' ? path_end + 1 : path_end;
  route_len = (size_t)(route_end - route);
  path_len = (size_t)(path_end - path);

  if (route_len > 0u && route[0] == ':') {
    optional = route_end > route && route_end[-1] == '?';
    name_start = route + 1;
    name_len = optional ? (size_t)(route_end - route - 2)
                        : (size_t)(route_end - route - 1);
    saved_count = request->path_param_count;

    if (optional &&
        vectis_match_param_route_segments(next_route, path, request, error)) {
      return 1;
    }
    if (!vectis_path_segment_is_safe(path, path_len)) {
      vectis_kv_truncate(&request->path_params, &request->path_param_count,
                         saved_count);
      return 0;
    }
    if (vectis_add_path_param_segment(request, name_start, name_len, path,
                                      path_len, error) != VECTIS_OK) {
      vectis_kv_truncate(&request->path_params, &request->path_param_count,
                         saved_count);
      return 0;
    }
    if (vectis_match_param_route_segments(next_route, next_path, request,
                                          error)) {
      return 1;
    }
    vectis_kv_truncate(&request->path_params, &request->path_param_count,
                       saved_count);
    return 0;
  }

  if (route_len != path_len || memcmp(route, path, route_len) != 0) {
    return 0;
  }
  return vectis_match_param_route_segments(next_route, next_path, request,
                                           error);
}

static int vectis_route_path_matches(const vectis_route_entry *route,
                                     const char *path, vectis_request *request,
                                     vectis_error *error) {
  regex_t compiled;
  int regex_rc;

  if (route->path_kind == VECTIS_ROUTE_PATH_LITERAL) {
    return strcmp(route->path, path) == 0;
  }
  if (route->path_kind == VECTIS_ROUTE_PATH_PARAMS) {
    return vectis_match_param_route_segments(route->path + 1, path + 1, request,
                                             error);
  }
  if (route->path_kind == VECTIS_ROUTE_PATH_REGEX) {
    regex_rc = regcomp(&compiled, route->path, REG_EXTENDED | REG_NOSUB);
    if (regex_rc != 0) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "registered regex route is invalid");
      return 0;
    }
    regex_rc = regexec(&compiled, path, 0u, NULL, 0);
    regfree(&compiled);
    return regex_rc == 0;
  }
  return 0;
}

vectis_status
vectis_internal_dispatch_route(vectis_app *app, vectis_http_method method,
                               const char *path, vectis_request *request,
                               vectis_response *response, vectis_error *error) {
  vectis_app_impl *impl;
  vectis_route_handler_fn handler;
  void *userdata;
  size_t i;
  size_t saved_count;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request is required");
    return VECTIS_ERR_INVALID;
  }
  if (response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_validate_request_path(path, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (vectis_method_mask(method) == VECTIS_HTTP_METHODS_NONE) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP method is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_internal_request_set_path(request, path, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  vectis_internal_request_set_method(request, method);

  impl = (vectis_app_impl *)app->impl;
  handler = NULL;
  userdata = NULL;
  saved_count = request->path_param_count;
  vectis_error_clear(error);

  (void)pthread_mutex_lock(&impl->mutex);
  for (i = 0u; i < impl->route_count; ++i) {
    vectis_kv_truncate(&request->path_params, &request->path_param_count,
                       saved_count);
    if (impl->routes[i].kind != VECTIS_ROUTE_ENTRY_HANDLER) {
      continue;
    }
    if (!vectis_route_method_matches(&impl->routes[i], method)) {
      continue;
    }
    if (vectis_route_path_matches(&impl->routes[i], path, request, error)) {
      handler = impl->routes[i].handler;
      userdata = impl->routes[i].userdata;
      break;
    }
    if (error != NULL && error->code == VECTIS_ERR_NOMEM) {
      vectis_kv_truncate(&request->path_params, &request->path_param_count,
                         saved_count);
      (void)pthread_mutex_unlock(&impl->mutex);
      return VECTIS_ERR_NOMEM;
    }
  }
  if (handler == NULL) {
    vectis_kv_truncate(&request->path_params, &request->path_param_count,
                       saved_count);
    (void)pthread_mutex_unlock(&impl->mutex);
    vectis_set_error(error, VECTIS_ERR_STATE, "no route matched request");
    return VECTIS_ERR_STATE;
  }
  (void)pthread_mutex_unlock(&impl->mutex);

  return handler(app, request, response, userdata, error);
}

vectis_status vectis_internal_route_body_policy(vectis_app *app,
                                                vectis_http_method method,
                                                const char *path,
                                                vectis_body_policy *policy,
                                                vectis_error *error) {
  vectis_app_impl *impl;
  vectis_request scratch;
  vectis_status status;
  size_t i;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  if (policy == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "body policy output is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_validate_request_path(path, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (vectis_method_mask(method) == VECTIS_HTTP_METHODS_NONE) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP method is invalid");
    return VECTIS_ERR_INVALID;
  }

  impl = (vectis_app_impl *)app->impl;
  status = VECTIS_ERR_STATE;
  vectis_internal_request_init(&scratch);
  vectis_error_clear(error);

  (void)pthread_mutex_lock(&impl->mutex);
  for (i = 0u; i < impl->route_count; ++i) {
    vectis_kv_truncate(&scratch.path_params, &scratch.path_param_count, 0u);
    if (!vectis_route_method_matches(&impl->routes[i], method)) {
      continue;
    }
    if (vectis_route_path_matches(&impl->routes[i], path, &scratch, error)) {
      *policy = impl->routes[i].body;
      status = VECTIS_OK;
      break;
    }
    if (error != NULL && error->code == VECTIS_ERR_NOMEM) {
      status = VECTIS_ERR_NOMEM;
      break;
    }
    if (error != NULL && error->code == VECTIS_ERR_INVALID) {
      status = VECTIS_ERR_INVALID;
      break;
    }
  }
  (void)pthread_mutex_unlock(&impl->mutex);

  vectis_internal_request_cleanup(&scratch);
  if (status == VECTIS_ERR_STATE) {
    vectis_set_error(error, VECTIS_ERR_STATE, "no route matched request");
  }
  return status;
}

vectis_status
vectis_internal_upload_stream_open(vectis_app *app, vectis_http_method method,
                                   const char *path, vectis_request *request,
                                   vectis_upload_stream_runtime *stream,
                                   vectis_error *error) {
  vectis_app_impl *impl;
  vectis_upload_open_fn open;
  void *userdata;
  size_t i;
  size_t saved_count;
  vectis_status status;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request is required");
    return VECTIS_ERR_INVALID;
  }
  if (stream == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "upload stream runtime is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_validate_request_path(path, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (vectis_method_mask(method) == VECTIS_HTTP_METHODS_NONE) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP method is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_internal_request_set_path(request, path, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  vectis_internal_request_set_method(request, method);

  memset(stream, 0, sizeof(*stream));
  impl = (vectis_app_impl *)app->impl;
  open = NULL;
  userdata = NULL;
  saved_count = request->path_param_count;
  status = VECTIS_ERR_STATE;
  vectis_error_clear(error);

  (void)pthread_mutex_lock(&impl->mutex);
  for (i = 0u; i < impl->route_count; ++i) {
    vectis_kv_truncate(&request->path_params, &request->path_param_count,
                       saved_count);
    if (impl->routes[i].kind != VECTIS_ROUTE_ENTRY_UPLOAD_STREAM) {
      continue;
    }
    if (!vectis_route_method_matches(&impl->routes[i], method)) {
      continue;
    }
    if (vectis_route_path_matches(&impl->routes[i], path, request, error)) {
      stream->policy = impl->routes[i].body;
      stream->write = impl->routes[i].upload_write;
      stream->finish = impl->routes[i].upload_finish;
      stream->close = impl->routes[i].upload_close;
      stream->userdata = impl->routes[i].userdata;
      open = impl->routes[i].upload_open;
      userdata = impl->routes[i].userdata;
      status = VECTIS_OK;
      break;
    }
    if (error != NULL && error->code == VECTIS_ERR_NOMEM) {
      status = VECTIS_ERR_NOMEM;
      break;
    }
    if (error != NULL && error->code == VECTIS_ERR_INVALID) {
      status = VECTIS_ERR_INVALID;
      break;
    }
  }
  if (status != VECTIS_OK) {
    vectis_kv_truncate(&request->path_params, &request->path_param_count,
                       saved_count);
  }
  (void)pthread_mutex_unlock(&impl->mutex);
  if (status != VECTIS_OK) {
    if (status == VECTIS_ERR_STATE) {
      vectis_set_error(error, VECTIS_ERR_STATE,
                       "no streaming upload route matched request");
    }
    return status;
  }
  if (stream->write == NULL || stream->finish == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "streaming upload route is invalid");
    return VECTIS_ERR_INVALID;
  }
  vectis_internal_request_set_streaming_upload(request, &stream->policy);
  if (open != NULL) {
    status = open(app, request, userdata, &stream->state, error);
    if (status != VECTIS_OK) {
      return status;
    }
  }
  stream->opened = 1;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status
vectis_internal_upload_stream_write(vectis_app *app, vectis_request *request,
                                    vectis_upload_stream_runtime *stream,
                                    const void *data, size_t size,
                                    vectis_error *error) {
  if (stream == NULL || !stream->opened || stream->write == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "streaming upload is not open");
    return VECTIS_ERR_INVALID;
  }
  if (data == NULL && size > 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "streaming upload chunk is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (size == 0u) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  return stream->write(app, request, data, size, stream->state,
                       stream->userdata, error);
}

vectis_status vectis_internal_upload_stream_finish(
    vectis_app *app, vectis_request *request, vectis_response *response,
    vectis_upload_stream_runtime *stream, vectis_error *error) {
  if (stream == NULL || !stream->opened || stream->finish == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "streaming upload is not open");
    return VECTIS_ERR_INVALID;
  }
  return stream->finish(app, request, response, stream->state, stream->userdata,
                        error);
}

void vectis_internal_upload_stream_close(vectis_app *app,
                                         vectis_request *request,
                                         vectis_upload_stream_runtime *stream) {
  if (stream == NULL || !stream->opened) {
    return;
  }
  if (stream->close != NULL) {
    stream->close(app, request, stream->state, stream->userdata);
  }
  memset(stream, 0, sizeof(*stream));
}

static pslog_logger *vectis_app_logger_impl(vectis_app *app) {
  vectis_app_impl *impl;

  if (app == NULL || app->impl == NULL) {
    return NULL;
  }
  impl = (vectis_app_impl *)app->impl;
  return impl->logger;
}

pslog_logger *vectis_logger(vectis_app *app) {
  if (app == NULL || app->impl == NULL) {
    return NULL;
  }
  return vectis_app_logger_impl(app);
}

struct lc_client *vectis_lockd_client(vectis_app *app) {
  vectis_app_impl *impl;
  vectis_error error;

  if (app == NULL || app->impl == NULL) {
    return NULL;
  }
  impl = (vectis_app_impl *)app->impl;
  if (!vectis_lockd_is_configured(impl)) {
    return NULL;
  }
  (void)pthread_mutex_lock(&impl->mutex);
  if (impl->lockd_client != NULL && impl->lockd_client_pid != getpid()) {
    impl->lockd_client = NULL;
    impl->lockd_client_pid = 0;
  }
  if (impl->lockd_client == NULL) {
    vectis_error_clear(&error);
    if (vectis_open_lockd_client(impl, &error) != VECTIS_OK) {
      if (impl->logger != NULL) {
        impl->logger->errorf(impl->logger, "vectis.lockd.open_failed",
                             "error=%s detail=%s", error.message, error.detail);
      }
    }
  }
  (void)pthread_mutex_unlock(&impl->mutex);
  return impl->lockd_client;
}

vectis_status vectis_consumer_service_new(
    vectis_app *app, const struct lc_consumer_service_config *config,
    vectis_consumer_service **out, vectis_error *error) {
  vectis_app_impl *impl;
  vectis_consumer_service *service;
  vectis_consumer_service_impl *service_impl;
  lc_error lcerr;
  int rc;
  vectis_status status;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "consumer service output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  if (config == NULL || config->consumers == NULL ||
      config->consumer_count == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "consumer service config requires at least one consumer");
    return VECTIS_ERR_INVALID;
  }

  impl = (vectis_app_impl *)app->impl;
  if (!vectis_lockd_is_configured(impl)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "consumer service requires configured lockd transport");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_validate_lockd_startable(impl, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_open_lockd_client(impl, error);
  if (status != VECTIS_OK) {
    return status;
  }

  service = (vectis_consumer_service *)calloc(1u, sizeof(*service));
  service_impl =
      (vectis_consumer_service_impl *)calloc(1u, sizeof(*service_impl));
  if (service == NULL || service_impl == NULL) {
    free(service);
    free(service_impl);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate consumer service");
    return VECTIS_ERR_NOMEM;
  }

  lc_error_init(&lcerr);
  rc = lc_client_new_consumer_service(impl->lockd_client, config,
                                      &service_impl->service, &lcerr);
  if (rc != LC_OK) {
    free(service_impl);
    free(service);
    status = vectis_set_lockdc_error(error, rc, &lcerr,
                                     "failed to create lockd consumer service");
    lc_error_cleanup(&lcerr);
    return status;
  }
  lc_error_cleanup(&lcerr);
  service->raw = vectis_consumer_service_raw;
  service->run = vectis_consumer_service_run;
  service->start = vectis_consumer_service_start;
  service->stop = vectis_consumer_service_stop;
  service->wait = vectis_consumer_service_wait;
  service->run_until = vectis_consumer_service_run_until;
  service->close = vectis_consumer_service_destroy;
  service->impl = service_impl;
  *out = service;
  vectis_error_clear(error);
  return VECTIS_OK;
}

void vectis_consumer_service_receiver_config_init(
    vectis_consumer_service_receiver_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->visibility_timeout_seconds = 30L;
  config->wait_seconds = 1L;
  config->worker_count = 1u;
}

void vectis_webdav_marker_receiver_config_init(
    vectis_webdav_marker_receiver_config *config) {
  vectis_webdav_config storage;

  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  vectis_webdav_config_init(&storage);
  config->site_id = "consumer";
  config->processing_path = "/consumer-processing.txt";
  config->done_path = "/consumer-done.txt";
  config->processing_body = "processing\n";
  config->done_body = "handled\n";
  config->max_file_bytes = storage.max_file_bytes;
  config->max_total_bytes = storage.max_total_bytes;
  config->max_resources = storage.max_resources;
}

static vectis_status vectis_register_consumer_receiver_impl(
    vectis_app_impl *impl, const vectis_consumer_receiver_adapter *adapter,
    vectis_error *error) {
  vectis_consumer_receiver_entry *entry;

  if (impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  if (adapter == NULL || adapter->kind == NULL || adapter->kind[0] == '\0' ||
      adapter->create == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "consumer receiver adapter requires kind and create");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_find_consumer_receiver(impl, adapter->kind) != NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "consumer receiver adapter is already registered");
    return VECTIS_ERR_INVALID;
  }
  entry = (vectis_consumer_receiver_entry *)calloc(1u, sizeof(*entry));
  if (entry == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate consumer receiver adapter");
    return VECTIS_ERR_NOMEM;
  }
  entry->kind = vectis_strdup(adapter->kind);
  if (entry->kind == NULL) {
    free(entry);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy consumer receiver adapter kind");
    return VECTIS_ERR_NOMEM;
  }
  entry->adapter = *adapter;
  entry->adapter.kind = entry->kind;
  entry->next = impl->consumer_receivers;
  impl->consumer_receivers = entry;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_register_consumer_receiver(
    vectis_app *app, const vectis_consumer_receiver_adapter *adapter,
    vectis_error *error) {
  vectis_app_impl *impl;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  impl = (vectis_app_impl *)app->impl;
  return vectis_register_consumer_receiver_impl(impl, adapter, error);
}

vectis_status vectis_consumer_service_new_receiver(
    vectis_app *app, const vectis_consumer_service_receiver_config *config,
    vectis_consumer_service **out, vectis_error *error) {
  vectis_app_impl *impl;
  vectis_consumer_receiver_entry *entry;
  vectis_consumer_receiver_runtime *runtime;
  lc_consumer_config consumer;
  lc_consumer_service_config service_config;
  vectis_consumer_service_impl *service_impl;
  vectis_status status;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "consumer service output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  if (config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "consumer service receiver config is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->queue == NULL || config->queue[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "consumer service queue is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->receiver_kind == NULL || config->receiver_kind[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "consumer service receiver_kind is required");
    return VECTIS_ERR_INVALID;
  }

  impl = (vectis_app_impl *)app->impl;
  entry = vectis_find_consumer_receiver(impl, config->receiver_kind);
  if (entry == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "consumer service receiver_kind is not registered");
    return VECTIS_ERR_INVALID;
  }

  runtime = (vectis_consumer_receiver_runtime *)calloc(1u, sizeof(*runtime));
  if (runtime == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate consumer receiver runtime");
    return VECTIS_ERR_NOMEM;
  }
  status = entry->adapter.create(entry->adapter.context, config->receiver_config,
                                 &runtime->receiver, error);
  if (status != VECTIS_OK) {
    free(runtime);
    return status;
  }
  if (runtime->receiver.handle == NULL) {
    vectis_consumer_receiver_runtime_cleanup(runtime);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "consumer receiver adapter returned no handler");
    return VECTIS_ERR_INVALID;
  }

  lc_consumer_config_init(&consumer);
  consumer.name = config->name;
  consumer.request.namespace_name = config->namespace_name;
  consumer.request.queue = config->queue;
  consumer.request.owner = config->owner;
  consumer.request.visibility_timeout_seconds =
      config->visibility_timeout_seconds;
  consumer.request.wait_seconds = config->wait_seconds;
  consumer.worker_count = config->worker_count;
  consumer.with_state = config->with_state;
  consumer.handle = vectis_consumer_receiver_bridge;
  consumer.context = runtime;
  lc_consumer_service_config_init(&service_config);
  service_config.consumers = &consumer;
  service_config.consumer_count = 1u;

  status = vectis_consumer_service_new(app, &service_config, out, error);
  if (status != VECTIS_OK) {
    vectis_consumer_receiver_runtime_cleanup(runtime);
    return status;
  }
  service_impl = (vectis_consumer_service_impl *)(*out)->impl;
  runtime->next = service_impl->receivers;
  service_impl->receivers = runtime;
  vectis_error_clear(error);
  return VECTIS_OK;
}

struct lc_consumer_service *
vectis_consumer_service_raw(vectis_consumer_service *service) {
  vectis_consumer_service_impl *impl;

  if (service == NULL) {
    return NULL;
  }
  impl = (vectis_consumer_service_impl *)service->impl;
  return impl != NULL ? impl->service : NULL;
}

vectis_status vectis_consumer_service_run(vectis_consumer_service *service,
                                          vectis_error *error) {
  vectis_consumer_service_impl *impl;
  lc_error lcerr;
  int rc;

  impl = service != NULL ? (vectis_consumer_service_impl *)service->impl : NULL;
  if (impl == NULL || impl->service == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "consumer service is required");
    return VECTIS_ERR_INVALID;
  }
  lc_error_init(&lcerr);
  rc = impl->service->run(impl->service, &lcerr);
  if (rc != LC_OK) {
    (void)vectis_set_lockdc_error(error, rc, &lcerr,
                                  "lockd consumer service run failed");
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_consumer_service_start(vectis_consumer_service *service,
                                            vectis_error *error) {
  vectis_consumer_service_impl *impl;
  lc_error lcerr;
  int rc;

  impl = service != NULL ? (vectis_consumer_service_impl *)service->impl : NULL;
  if (impl == NULL || impl->service == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "consumer service is required");
    return VECTIS_ERR_INVALID;
  }
  lc_error_init(&lcerr);
  rc = impl->service->start(impl->service, &lcerr);
  if (rc != LC_OK) {
    (void)vectis_set_lockdc_error(error, rc, &lcerr,
                                  "lockd consumer service start failed");
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_consumer_service_stop(vectis_consumer_service *service,
                                           vectis_error *error) {
  vectis_consumer_service_impl *impl;
  int rc;

  impl = service != NULL ? (vectis_consumer_service_impl *)service->impl : NULL;
  if (impl == NULL || impl->service == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "consumer service is required");
    return VECTIS_ERR_INVALID;
  }
  rc = impl->service->stop(impl->service);
  if (rc != LC_OK) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "lockd consumer service stop failed");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LOCKDC;
      error->dependency_code = (long)rc;
    }
    return VECTIS_ERR_STATE;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_consumer_service_wait(vectis_consumer_service *service,
                                           vectis_error *error) {
  vectis_consumer_service_impl *impl;
  lc_error lcerr;
  int rc;

  impl = service != NULL ? (vectis_consumer_service_impl *)service->impl : NULL;
  if (impl == NULL || impl->service == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "consumer service is required");
    return VECTIS_ERR_INVALID;
  }
  lc_error_init(&lcerr);
  rc = impl->service->wait(impl->service, &lcerr);
  if (rc != LC_OK) {
    (void)vectis_set_lockdc_error(error, rc, &lcerr,
                                  "lockd consumer service wait failed");
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

static long vectis_now_ms(void) {
  struct timeval tv;

  if (gettimeofday(&tv, NULL) != 0) {
    return 0L;
  }
  return (long)(tv.tv_sec * 1000L) + (long)(tv.tv_usec / 1000L);
}

vectis_status
vectis_consumer_service_run_until(vectis_consumer_service *service,
                                  const volatile int *done, long timeout_ms,
                                  vectis_error *error) {
  vectis_consumer_service_impl *impl;
  long deadline;
  struct timespec pause_time;
  vectis_status status;

  impl = service != NULL ? (vectis_consumer_service_impl *)service->impl : NULL;
  if (impl == NULL || impl->service == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "consumer service is required");
    return VECTIS_ERR_INVALID;
  }
  if (done == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "done flag is required");
    return VECTIS_ERR_INVALID;
  }
  if (timeout_ms <= 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "timeout_ms must be greater than zero");
    return VECTIS_ERR_INVALID;
  }

  status = vectis_consumer_service_start(service, error);
  if (status != VECTIS_OK) {
    return status;
  }
  deadline = vectis_now_ms() + timeout_ms;
  pause_time.tv_sec = 0;
  pause_time.tv_nsec = 10000000L;
  while (!*done && vectis_now_ms() < deadline) {
    (void)nanosleep(&pause_time, NULL);
  }
  if (!*done) {
    (void)vectis_consumer_service_stop(service, error);
    (void)vectis_consumer_service_wait(service, error);
    vectis_set_error(error, VECTIS_ERR_TIMEOUT, "consumer service timed out");
    return VECTIS_ERR_TIMEOUT;
  }
  status = vectis_consumer_service_stop(service, error);
  if (status != VECTIS_OK) {
    (void)vectis_consumer_service_wait(service, error);
    return status;
  }
  return vectis_consumer_service_wait(service, error);
}

void vectis_consumer_service_destroy(vectis_consumer_service *service) {
  vectis_consumer_service_impl *impl;

  if (service == NULL) {
    return;
  }
  impl = (vectis_consumer_service_impl *)service->impl;
  if (impl != NULL && impl->service != NULL) {
    impl->service->close(impl->service);
  }
  if (impl != NULL) {
    vectis_consumer_receiver_runtime_cleanup(impl->receivers);
  }
  free(impl);
  free(service);
}

vectis_status vectis_json_validate_cstr(const char *json, vectis_error *error) {
  lonejson_error json_error;
  lonejson_status status;
  lonejson *runtime;

  if (json == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json is required");
    return VECTIS_ERR_INVALID;
  }

  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  status = lonejson_validate_cstr(runtime, json, &json_error);
  lonejson_free(runtime);
  if (status != LONEJSON_STATUS_OK) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "invalid json at line %lu column %lu: %s",
                      (unsigned long)json_error.line,
                      (unsigned long)json_error.column, json_error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
      error->dependency_code = (long)status;
    }
    return VECTIS_ERR_INVALID;
  }

  vectis_error_clear(error);
  return VECTIS_OK;
}

void vectis_dsv_config_init(vectis_dsv_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->delimiter = ',';
  config->quote = '"';
  config->escape = '"';
  config->max_field_bytes = 1048576u;
}

vectis_dsv_config vectis_dsv_csv(void) {
  vectis_dsv_config config;

  vectis_dsv_config_init(&config);
  return config;
}

vectis_dsv_config vectis_dsv_tsv(void) {
  vectis_dsv_config config;

  vectis_dsv_config_init(&config);
  config.delimiter = '\t';
  return config;
}

vectis_dsv_config vectis_dsv_csv_rows(void) {
  vectis_dsv_config config;

  config = vectis_dsv_csv();
  config.header_disabled = 1;
  return config;
}

vectis_dsv_config vectis_dsv_tsv_rows(void) {
  vectis_dsv_config config;

  config = vectis_dsv_tsv();
  config.header_disabled = 1;
  return config;
}

static void vectis_dsv_fields_cleanup(vectis_dsv_fields *fields) {
  size_t i;

  if (fields == NULL) {
    return;
  }
  for (i = 0u; i < fields->count; ++i) {
    free(fields->items[i]);
  }
  free(fields->items);
  free(fields->sizes);
  fields->items = NULL;
  fields->sizes = NULL;
  fields->count = 0u;
  fields->capacity = 0u;
}

static vectis_status vectis_dsv_fields_push(vectis_dsv_fields *fields,
                                            const char *data, size_t size,
                                            vectis_error *error) {
  char **next_items;
  size_t *next_sizes;
  size_t capacity;
  char *copy;

  if (fields->count == fields->capacity) {
    capacity = fields->capacity == 0u ? 8u : fields->capacity * 2u;
    next_items =
        (char **)realloc(fields->items, capacity * sizeof(fields->items[0]));
    if (next_items == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to grow DSV field list");
      return VECTIS_ERR_NOMEM;
    }
    fields->items = next_items;
    next_sizes =
        (size_t *)realloc(fields->sizes, capacity * sizeof(fields->sizes[0]));
    if (next_sizes == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to grow DSV field sizes");
      return VECTIS_ERR_NOMEM;
    }
    fields->sizes = next_sizes;
    fields->capacity = capacity;
  }
  copy = (char *)malloc(size + 1u);
  if (copy == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate DSV field");
    return VECTIS_ERR_NOMEM;
  }
  if (size > 0u) {
    memcpy(copy, data, size);
  }
  copy[size] = '\0';
  fields->items[fields->count] = copy;
  fields->sizes[fields->count] = size;
  fields->count++;
  return VECTIS_OK;
}

static void vectis_dsv_fields_reset(vectis_dsv_fields *fields) {
  size_t i;

  if (fields == NULL) {
    return;
  }
  for (i = 0u; i < fields->count; ++i) {
    free(fields->items[i]);
    fields->items[i] = NULL;
    fields->sizes[i] = 0u;
  }
  fields->count = 0u;
}

static vectis_status vectis_dsv_parser_next_char(vectis_dsv_parser *parser,
                                                 int *out,
                                                 vectis_error *error) {
  lc_error lcerr;
  size_t nread;

  if (parser->offset >= parser->size) {
    if (parser->eof) {
      *out = EOF;
      return VECTIS_OK;
    }
    lc_error_init(&lcerr);
    nread = parser->source->read(parser->source, parser->buffer,
                                 sizeof(parser->buffer), &lcerr);
    if (nread == 0u) {
      if (lcerr.code != 0) {
        (void)vectis_source_error(error, lcerr.code, &lcerr,
                                  "failed to read DSV source");
        lc_error_cleanup(&lcerr);
        return error != NULL ? error->code : VECTIS_ERR_STATE;
      }
      parser->eof = 1;
      *out = EOF;
      lc_error_cleanup(&lcerr);
      return VECTIS_OK;
    }
    parser->offset = 0u;
    parser->size = nread;
    lc_error_cleanup(&lcerr);
  }
  *out = (unsigned char)parser->buffer[parser->offset++];
  return VECTIS_OK;
}

static vectis_status vectis_dsv_push_current_field(vectis_dsv_parser *parser,
                                                   vectis_error *error) {
  vectis_status status;

  status = vectis_dsv_fields_push(&parser->fields, parser->field.data,
                                  parser->field.size, error);
  parser->field.size = 0u;
  if (parser->field.data != NULL) {
    parser->field.data[0] = '\0';
  }
  return status;
}

static vectis_status vectis_dsv_read_record(vectis_dsv_parser *parser,
                                            int *has_record,
                                            vectis_error *error) {
  int c = 0;
  char ch;
  int in_quotes;
  int after_quote;
  int at_field_start;
  vectis_status status;

  *has_record = 0;
  vectis_dsv_fields_reset(&parser->fields);
  parser->field.size = 0u;
  if (parser->field.data != NULL) {
    parser->field.data[0] = '\0';
  }
  parser->first_field_quoted = 0;
  in_quotes = 0;
  after_quote = 0;
  at_field_start = 1;

  for (;;) {
    status = vectis_dsv_parser_next_char(parser, &c, error);
    if (status != VECTIS_OK) {
      return status;
    }
    if (c == EOF) {
      if (in_quotes) {
        vectis_set_error(error, VECTIS_ERR_INVALID,
                         "unterminated quoted DSV field");
        return VECTIS_ERR_INVALID;
      }
      if (parser->field.size > 0u || parser->fields.count > 0u) {
        if (vectis_dsv_push_current_field(parser, error) != VECTIS_OK) {
          return error != NULL ? error->code : VECTIS_ERR_NOMEM;
        }
        *has_record = 1;
      }
      return VECTIS_OK;
    }
    if (parser->skip_next_lf) {
      parser->skip_next_lf = 0;
      if (c == '\n') {
        continue;
      }
    }
    if (after_quote) {
      if (c == parser->config.quote &&
          parser->config.escape == parser->config.quote) {
        if (vectis_string_builder_append_n(&parser->field, "\"", 1u, error) !=
            VECTIS_OK) {
          return error != NULL ? error->code : VECTIS_ERR_NOMEM;
        }
        after_quote = 0;
        in_quotes = 1;
        at_field_start = 0;
        continue;
      }
      in_quotes = 0;
      after_quote = 0;
    }
    if (in_quotes) {
      if (c == parser->config.quote) {
        after_quote = 1;
      } else {
        ch = (char)c;
        if (parser->config.max_field_bytes > 0u &&
            parser->field.size + 1u > parser->config.max_field_bytes) {
          vectis_set_error(error, VECTIS_ERR_INVALID,
                           "DSV field exceeds max_field_bytes");
          return VECTIS_ERR_INVALID;
        }
        if (vectis_string_builder_append_n(&parser->field, &ch, 1u, error) !=
            VECTIS_OK) {
          return error != NULL ? error->code : VECTIS_ERR_NOMEM;
        }
      }
      continue;
    }
    if (c == parser->config.quote && at_field_start) {
      if (parser->fields.count == 0u && parser->field.size == 0u) {
        parser->first_field_quoted = 1;
      }
      in_quotes = 1;
      at_field_start = 0;
      continue;
    }
    if (c == parser->config.delimiter) {
      if (vectis_dsv_push_current_field(parser, error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
      at_field_start = 1;
      continue;
    }
    if (c == '\r' || c == '\n') {
      if (c == '\r') {
        parser->skip_next_lf = !parser->config.trim_cr_disabled;
      }
      if (vectis_dsv_push_current_field(parser, error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
      parser->physical_row++;
      *has_record = 1;
      return VECTIS_OK;
    }
    if (parser->config.max_field_bytes > 0u &&
        parser->field.size + 1u > parser->config.max_field_bytes) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "DSV field exceeds max_field_bytes");
      return VECTIS_ERR_INVALID;
    }
    ch = (char)c;
    if (vectis_string_builder_append_n(&parser->field, &ch, 1u, error) !=
        VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    at_field_start = 0;
  }
}

static int vectis_dsv_record_is_comment(const vectis_dsv_parser *parser) {
  const char *value;
  const char *prefix;
  size_t prefix_size;

  if (parser == NULL || parser->config.comment_prefix == NULL ||
      parser->config.comment_prefix[0] == '\0' || parser->fields.count == 0u ||
      parser->first_field_quoted) {
    return 0;
  }
  value = parser->fields.items[0];
  if (value == NULL) {
    return 0;
  }
  if (!parser->config.indented_comments_disabled) {
    while (*value == ' ' || *value == '\t') {
      value++;
    }
  }
  prefix = parser->config.comment_prefix;
  prefix_size = strlen(prefix);
  return strncmp(value, prefix, prefix_size) == 0;
}

static vectis_status vectis_dsv_read_data_record(vectis_dsv_parser *parser,
                                                 int *has_record,
                                                 vectis_error *error) {
  vectis_status status;

  for (;;) {
    status = vectis_dsv_read_record(parser, has_record, error);
    if (status != VECTIS_OK || !*has_record) {
      return status;
    }
    if (!vectis_dsv_record_is_comment(parser)) {
      return VECTIS_OK;
    }
  }
}

static const lonejson_field *vectis_dsv_find_field(const lonejson_map *map,
                                                   const char *name) {
  size_t i;

  if (map == NULL || name == NULL) {
    return NULL;
  }
  for (i = 0u; i < map->field_count; ++i) {
    if (strcmp(map->fields[i].json_key, name) == 0) {
      return &map->fields[i];
    }
  }
  return NULL;
}

static vectis_status vectis_dsv_append_string_row_json(
    vectis_string_builder *builder, const char *const *columns,
    size_t column_count, const vectis_dsv_fields *fields, vectis_error *error) {
  vectis_lonejson_builder_sink sink;
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status json_status;
  lonejson *runtime;
  size_t i;
  const char *value;

  sink.builder = builder;
  sink.error = error;
  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  json_status = lonejson_writer_init_sink(
      runtime, &writer, vectis_lonejson_builder_sink_write, &sink, &json_error);
  if (json_status != LONEJSON_STATUS_OK) {
    goto json_failed;
  }
  json_status = lonejson_writer_begin_object(&writer, &json_error);
  if (json_status != LONEJSON_STATUS_OK) {
    goto json_cleanup_failed;
  }
  for (i = 0u; i < column_count; ++i) {
    value = i < fields->count ? fields->items[i] : "";
    json_status = lonejson_writer_key(&writer, columns[i], strlen(columns[i]),
                                      &json_error);
    if (json_status != LONEJSON_STATUS_OK) {
      goto json_cleanup_failed;
    }
    json_status = lonejson_writer_string(
        &writer, value, i < fields->count ? fields->sizes[i] : 0u, &json_error);
    if (json_status != LONEJSON_STATUS_OK) {
      goto json_cleanup_failed;
    }
  }
  json_status = lonejson_writer_end_object(&writer, &json_error);
  if (json_status != LONEJSON_STATUS_OK) {
    goto json_cleanup_failed;
  }
  json_status = lonejson_writer_finish(&writer, &json_error);
  lonejson_writer_cleanup(&writer);
  if (json_status == LONEJSON_STATUS_OK) {
    lonejson_free(runtime);
    return VECTIS_OK;
  }
  goto json_failed;

json_cleanup_failed:
  lonejson_writer_cleanup(&writer);
json_failed:
  lonejson_free(runtime);
  vectis_set_errorf(
      error, VECTIS_ERR_INVALID,
      "failed to serialize DSV string row through lonejson writer: %s",
      json_error.message);
  if (error != NULL) {
    error->source = VECTIS_ERROR_SOURCE_LONEJSON;
    error->dependency_code = (long)json_status;
  }
  return VECTIS_ERR_INVALID;
}

static vectis_status
vectis_dsv_effective_config(const vectis_dsv_config *config,
                            vectis_dsv_config *out, vectis_error *error) {
  vectis_dsv_config defaults;

  vectis_dsv_config_init(&defaults);
  *out = config != NULL ? *config : defaults;
  if (out->delimiter == 0 || out->delimiter == '\r' || out->delimiter == '\n') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV delimiter is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (out->quote == 0) {
    out->quote = '"';
  }
  if (out->escape == 0) {
    out->escape = out->quote;
  }
  if (out->max_field_bytes == 0u) {
    out->max_field_bytes = defaults.max_field_bytes;
  }
  return VECTIS_OK;
}

static vectis_status vectis_dsv_open_source(const vectis_source *source,
                                            lc_source **out, int *owned,
                                            vectis_error *error) {
  lc_error lcerr;
  int rc;

  if (out == NULL || owned == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV source output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  *owned = 0;
  if (source == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV source is required");
    return VECTIS_ERR_INVALID;
  }
  if (source->source != NULL) {
    *out = source->source;
    return VECTIS_OK;
  }
  lc_error_init(&lcerr);
  if (source->path != NULL) {
    rc = lc_source_from_file(source->path, out, &lcerr);
  } else if (source->memory != NULL || source->memory_size > 0u) {
    rc =
        lc_source_from_memory(source->memory, source->memory_size, out, &lcerr);
  } else {
    lc_error_cleanup(&lcerr);
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV source is empty");
    return VECTIS_ERR_INVALID;
  }
  if (rc != LC_OK) {
    (void)vectis_source_error(error, rc, &lcerr, "failed to open DSV source");
    lc_error_cleanup(&lcerr);
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  *owned = 1;
  return VECTIS_OK;
}

static vectis_status vectis_dsv_columns_from_map(const lonejson_map *map,
                                                 const char ***out,
                                                 size_t *out_count,
                                                 vectis_error *error) {
  const char **columns;
  size_t i;

  if (out == NULL || out_count == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV column output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  *out_count = 0u;
  if (map == NULL || map->field_count == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV lonejson map has no fields");
    return VECTIS_ERR_INVALID;
  }
  columns = (const char **)calloc(map->field_count, sizeof(columns[0]));
  if (columns == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate DSV map columns");
    return VECTIS_ERR_NOMEM;
  }
  for (i = 0u; i < map->field_count; ++i) {
    columns[i] = map->fields[i].json_key;
  }
  *out = columns;
  *out_count = map->field_count;
  return VECTIS_OK;
}

static int vectis_dsv_column_index(const char *const *columns,
                                   size_t column_count, const char *name,
                                   size_t *out) {
  size_t i;

  if (columns == NULL || name == NULL || out == NULL) {
    return 0;
  }
  for (i = 0u; i < column_count; ++i) {
    if (columns[i] != NULL && strcmp(columns[i], name) == 0) {
      *out = i;
      return 1;
    }
  }
  return 0;
}

static vectis_status
vectis_dsv_set_lonejson_string_field(const lonejson_field *field, void *value,
                                     const char *text, size_t text_size,
                                     vectis_error *error) {
  char *dst;
  char **dyn;
  size_t copy_size;

  if (field->storage == LONEJSON_STORAGE_DYNAMIC) {
    dyn = (char **)((unsigned char *)value + field->struct_offset);
    *dyn = (char *)malloc(text_size + 1u);
    if (*dyn == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to allocate DSV lonejson string field");
      return VECTIS_ERR_NOMEM;
    }
    memcpy(*dyn, text, text_size);
    (*dyn)[text_size] = '\0';
    return VECTIS_OK;
  }
  if (field->fixed_capacity == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV lonejson string field has no capacity");
    return VECTIS_ERR_INVALID;
  }
  dst = (char *)((unsigned char *)value + field->struct_offset);
  if (text_size >= field->fixed_capacity) {
    if (field->overflow_policy == LONEJSON_OVERFLOW_FAIL) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "DSV lonejson string field overflow");
      return VECTIS_ERR_INVALID;
    }
    copy_size = field->fixed_capacity - 1u;
  } else {
    copy_size = text_size;
  }
  memcpy(dst, text, copy_size);
  dst[copy_size] = '\0';
  return VECTIS_OK;
}

static int vectis_parse_lonejson_u64_text(const char *text,
                                          lonejson_uint64 *value) {
  lonejson_uint64 limit;
  lonejson_uint64 parsed;

  while (isspace((unsigned char)*text)) {
    text++;
  }
  if (*text == '+') {
    text++;
  } else if (*text == '-') {
    return 0;
  }
  if (!isdigit((unsigned char)*text)) {
    return 0;
  }
  limit = ~(lonejson_uint64)0u;
  parsed = 0u;
  do {
    lonejson_uint64 digit;

    digit = (lonejson_uint64)(*text - '0');
    if (parsed > (limit - digit) / 10u) {
      return 0;
    }
    parsed = (lonejson_uint64)(parsed * 10u + digit);
    text++;
  } while (isdigit((unsigned char)*text));
  if (*text != '\0') {
    return 0;
  }
  *value = parsed;
  return 1;
}

static int vectis_parse_lonejson_i64_text(const char *text,
                                          lonejson_int64 *value) {
  lonejson_uint64 signed_max;
  lonejson_uint64 limit;
  lonejson_uint64 parsed;
  int negative;

  while (isspace((unsigned char)*text)) {
    text++;
  }
  negative = 0;
  if (*text == '-') {
    negative = 1;
    text++;
  } else if (*text == '+') {
    text++;
  }
  if (!isdigit((unsigned char)*text)) {
    return 0;
  }
  signed_max = (~(lonejson_uint64)0u) >> 1u;
  limit = negative ? signed_max + 1u : signed_max;
  parsed = 0u;
  do {
    lonejson_uint64 digit;

    digit = (lonejson_uint64)(*text - '0');
    if (parsed > (limit - digit) / 10u) {
      return 0;
    }
    parsed = (lonejson_uint64)(parsed * 10u + digit);
    text++;
  } while (isdigit((unsigned char)*text));
  if (*text != '\0') {
    return 0;
  }
  if (negative) {
    if (parsed == signed_max + 1u) {
      *value = (lonejson_int64)(-((lonejson_int64)signed_max) - 1);
    } else {
      *value = (lonejson_int64)(-((lonejson_int64)parsed));
    }
  } else {
    *value = (lonejson_int64)parsed;
  }
  return 1;
}

static vectis_status
vectis_dsv_set_lonejson_scalar_field(const lonejson_field *field, void *value,
                                     const char *text, size_t text_size,
                                     vectis_error *error) {
  char buffer[128];
  char *end;
  lonejson_int64 i64_value;
  lonejson_uint64 u64_value;

  if (field->flags & LONEJSON_FIELD_HAS_PRESENCE) {
    *(int *)((unsigned char *)value + field->presence_offset) = 1;
  }
  if (text_size >= sizeof(buffer)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV lonejson scalar field is too large");
    return VECTIS_ERR_INVALID;
  }
  memcpy(buffer, text, text_size);
  buffer[text_size] = '\0';
  errno = 0;
  switch (field->kind) {
  case LONEJSON_FIELD_KIND_STRING:
    return vectis_dsv_set_lonejson_string_field(field, value, text, text_size,
                                                error);
  case LONEJSON_FIELD_KIND_I64:
    if (!vectis_parse_lonejson_i64_text(buffer, &i64_value)) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "DSV lonejson scalar field is invalid");
      return VECTIS_ERR_INVALID;
    }
    *(lonejson_int64 *)((unsigned char *)value + field->struct_offset) =
        i64_value;
    return VECTIS_OK;
  case LONEJSON_FIELD_KIND_U64:
    if (!vectis_parse_lonejson_u64_text(buffer, &u64_value)) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "DSV lonejson scalar field is invalid");
      return VECTIS_ERR_INVALID;
    }
    *(lonejson_uint64 *)((unsigned char *)value + field->struct_offset) =
        u64_value;
    return VECTIS_OK;
  case LONEJSON_FIELD_KIND_F64:
    *(double *)((unsigned char *)value + field->struct_offset) =
        strtod(buffer, &end);
    break;
  case LONEJSON_FIELD_KIND_BOOL:
    if (strcmp(buffer, "true") == 0 || strcmp(buffer, "1") == 0) {
      *(int *)((unsigned char *)value + field->struct_offset) = 1;
      return VECTIS_OK;
    }
    if (strcmp(buffer, "false") == 0 || strcmp(buffer, "0") == 0) {
      *(int *)((unsigned char *)value + field->struct_offset) = 0;
      return VECTIS_OK;
    }
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV lonejson boolean field is invalid");
    return VECTIS_ERR_INVALID;
  default:
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV lonejson field kind requires JSON input and is not "
                     "supported by direct DSV mapping");
    return VECTIS_ERR_INVALID;
  }
  if (errno != 0 || end == buffer || *end != '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV lonejson scalar field is invalid");
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static vectis_status vectis_dsv_fields_to_lonejson_value(
    const lonejson_map *map, const char *const *columns, size_t column_count,
    const vectis_dsv_fields *fields, void *value, vectis_error *error) {
  const lonejson_field *field;
  const char *text;
  size_t text_size;
  size_t column_index;
  size_t i;
  int present;

  if (map == NULL || columns == NULL || fields == NULL || value == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV lonejson mapping input is required");
    return VECTIS_ERR_INVALID;
  }
  for (i = 0u; i < map->field_count; ++i) {
    field = &map->fields[i];
    present = vectis_dsv_column_index(columns, column_count, field->json_key,
                                      &column_index) &&
              column_index < fields->count;
    if (!present) {
      if ((field->flags & LONEJSON_FIELD_REQUIRED) != 0u) {
        vectis_set_errorf(error, VECTIS_ERR_INVALID,
                          "DSV required column '%s' is missing",
                          field->json_key);
        return VECTIS_ERR_INVALID;
      }
      continue;
    }
    text = fields->items[column_index];
    text_size = fields->sizes[column_index];
    if (text_size == 0u && field->kind != LONEJSON_FIELD_KIND_STRING) {
      if ((field->flags & LONEJSON_FIELD_REQUIRED) != 0u) {
        vectis_set_errorf(error, VECTIS_ERR_INVALID,
                          "DSV required column '%s' is empty", field->json_key);
        return VECTIS_ERR_INVALID;
      }
      continue;
    }
    if (vectis_dsv_set_lonejson_scalar_field(field, value, text, text_size,
                                             error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_INVALID;
    }
  }
  return VECTIS_OK;
}

static vectis_status vectis_dsv_append_mapped_row_json(
    vectis_string_builder *builder, const lonejson_map *map,
    const char *const *columns, size_t column_count,
    const vectis_dsv_fields *fields, vectis_error *error) {
  lonejson_error json_error;
  lonejson *runtime;
  char *json;
  size_t json_size;
  vectis_status status;
  void *value;

  if (builder == NULL || map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV lonejson output map is required");
    return VECTIS_ERR_INVALID;
  }
  value = calloc(1u, map->struct_size);
  if (value == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate DSV row struct");
    return VECTIS_ERR_NOMEM;
  }
  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    free(value);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  lonejson_init(runtime, map, value);
  status = vectis_dsv_fields_to_lonejson_value(map, columns, column_count,
                                               fields, value, error);
  if (status != VECTIS_OK) {
    lonejson_cleanup(map, value);
    lonejson_free(runtime);
    free(value);
    return status;
  }
  json = lonejson_serialize_alloc(runtime, map, value, &json_size, &json_error);
  lonejson_cleanup(map, value);
  lonejson_free(runtime);
  free(value);
  if (json == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to serialize DSV row through lonejson: %s",
                      json_error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
    }
    return VECTIS_ERR_INVALID;
  }
  status = vectis_string_builder_append_n(builder, json, json_size, error);
  free(json);
  return status;
}

vectis_status vectis_dsv_parse_lonejson(struct lc_source *source,
                                        const lonejson_map *map,
                                        const vectis_dsv_config *config,
                                        vectis_dsv_lonejson_row_fn row,
                                        void *userdata, vectis_error *error) {
  vectis_dsv_parser parser;
  vectis_dsv_config effective;
  vectis_dsv_fields headers;
  const char *const *columns;
  const char **header_columns;
  size_t column_count;
  size_t data_row;
  int has_record;
  void *value;
  lonejson *runtime;
  vectis_status status;

  if (source == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV source is required");
    return VECTIS_ERR_INVALID;
  }
  if (map == NULL || row == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV map and row callback are required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_dsv_effective_config(config, &effective, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  memset(&parser, 0, sizeof(parser));
  memset(&headers, 0, sizeof(headers));
  parser.config = effective;
  parser.source = source;
  columns = effective.columns;
  header_columns = NULL;
  column_count = effective.column_count;
  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    vectis_dsv_fields_cleanup(&parser.fields);
    vectis_string_builder_cleanup(&parser.field);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  if (!effective.header_disabled) {
    status = vectis_dsv_read_data_record(&parser, &has_record, error);
    if (status != VECTIS_OK) {
      vectis_dsv_fields_cleanup(&parser.fields);
      vectis_string_builder_cleanup(&parser.field);
      lonejson_free(runtime);
      return status;
    }
    if (!has_record) {
      vectis_dsv_fields_cleanup(&parser.fields);
      vectis_string_builder_cleanup(&parser.field);
      lonejson_free(runtime);
      vectis_error_clear(error);
      return VECTIS_OK;
    }
    headers = parser.fields;
    memset(&parser.fields, 0, sizeof(parser.fields));
    if (columns == NULL) {
      if (headers.count == 0u) {
        vectis_dsv_fields_cleanup(&headers);
        vectis_dsv_fields_cleanup(&parser.fields);
        vectis_string_builder_cleanup(&parser.field);
        lonejson_free(runtime);
        vectis_set_error(error, VECTIS_ERR_INVALID, "DSV header row is empty");
        return VECTIS_ERR_INVALID;
      }
      header_columns =
          (const char **)calloc(headers.count, sizeof(header_columns[0]));
      if (header_columns == NULL) {
        vectis_dsv_fields_cleanup(&headers);
        vectis_dsv_fields_cleanup(&parser.fields);
        vectis_string_builder_cleanup(&parser.field);
        lonejson_free(runtime);
        vectis_set_error(error, VECTIS_ERR_NOMEM,
                         "failed to allocate DSV header columns");
        return VECTIS_ERR_NOMEM;
      }
      for (column_count = 0u; column_count < headers.count; ++column_count) {
        header_columns[column_count] = headers.items[column_count];
      }
      columns = header_columns;
      column_count = headers.count;
    }
  } else if (columns == NULL) {
    status =
        vectis_dsv_columns_from_map(map, &header_columns, &column_count, error);
    if (status != VECTIS_OK) {
      vectis_dsv_fields_cleanup(&headers);
      vectis_dsv_fields_cleanup(&parser.fields);
      vectis_string_builder_cleanup(&parser.field);
      lonejson_free(runtime);
      return status;
    }
    columns = header_columns;
  }
  if (columns == NULL || column_count == 0u) {
    vectis_dsv_fields_cleanup(&headers);
    vectis_dsv_fields_cleanup(&parser.fields);
    vectis_string_builder_cleanup(&parser.field);
    lonejson_free(runtime);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV columns are required when no header row is used");
    return VECTIS_ERR_INVALID;
  }
  data_row = 0u;
  for (;;) {
    status = vectis_dsv_read_data_record(&parser, &has_record, error);
    if (status != VECTIS_OK) {
      break;
    }
    if (!has_record) {
      break;
    }
    if (!effective.strict_row_width_disabled &&
        parser.fields.count != column_count) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "DSV row width does not match columns");
      status = VECTIS_ERR_INVALID;
      break;
    }
    value = calloc(1u, map->struct_size);
    if (value == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to allocate DSV row struct");
      status = VECTIS_ERR_NOMEM;
      break;
    }
    lonejson_init(runtime, map, value);
    status = vectis_dsv_fields_to_lonejson_value(map, columns, column_count,
                                                 &parser.fields, value, error);
    if (status != VECTIS_OK) {
      lonejson_cleanup(map, value);
      free(value);
      break;
    }
    status = row(userdata, data_row + 1u, value, error);
    lonejson_cleanup(map, value);
    free(value);
    if (status != VECTIS_OK) {
      break;
    }
    data_row++;
  }
  vectis_dsv_fields_cleanup(&headers);
  vectis_dsv_fields_cleanup(&parser.fields);
  vectis_string_builder_cleanup(&parser.field);
  free(header_columns);
  lonejson_free(runtime);
  if (status == VECTIS_OK) {
    vectis_error_clear(error);
  }
  return status;
}

vectis_status vectis_dsv_parse_lonejson_source(const vectis_source *source,
                                               const lonejson_map *map,
                                               const vectis_dsv_config *config,
                                               vectis_dsv_lonejson_row_fn row,
                                               void *userdata,
                                               vectis_error *error) {
  lc_source *reader;
  int owned;
  vectis_status status;

  status = vectis_dsv_open_source(source, &reader, &owned, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_dsv_parse_lonejson(reader, map, config, row, userdata, error);
  if (owned) {
    lc_source_close(reader);
  }
  return status;
}

static void vectis_dsv_rows_cleanup_value(vectis_dsv_rows *rows) {
  if (rows == NULL || rows->value == NULL) {
    return;
  }
  lonejson_cleanup(rows->map, rows->value);
  free(rows->value);
  rows->value = NULL;
}

static void vectis_dsv_rows_cleanup(vectis_dsv_rows *rows) {
  if (rows == NULL) {
    return;
  }
  vectis_dsv_rows_cleanup_value(rows);
  vectis_dsv_fields_cleanup(&rows->headers);
  vectis_dsv_fields_cleanup(&rows->parser.fields);
  vectis_string_builder_cleanup(&rows->parser.field);
  free(rows->header_columns);
  if (rows->runtime != NULL) {
    lonejson_free(rows->runtime);
  }
  memset(rows, 0, sizeof(*rows));
}

static vectis_status
vectis_dsv_rows_open(vectis_dsv_rows *rows, struct lc_source *source,
                     const lonejson_map *map, size_t row_size,
                     const vectis_dsv_config *config, vectis_error *error) {
  vectis_dsv_config effective;
  int has_record;
  size_t i;
  vectis_status status;

  if (rows == NULL || source == NULL || map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV route rows require source and map");
    return VECTIS_ERR_INVALID;
  }
  if (row_size == 0u || row_size != map->struct_size) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV route row_size does not match lonejson map");
    return VECTIS_ERR_INVALID;
  }
  memset(rows, 0, sizeof(*rows));
  if (vectis_dsv_effective_config(config, &effective, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  rows->map = map;
  rows->row_size = row_size;
  rows->parser.config = effective;
  rows->parser.source = source;
  rows->columns = effective.columns;
  rows->column_count = effective.column_count;
  rows->runtime = vectis_lonejson_new(error);
  if (rows->runtime == NULL) {
    vectis_dsv_rows_cleanup(rows);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }

  if (!effective.header_disabled) {
    status = vectis_dsv_read_data_record(&rows->parser, &has_record, error);
    if (status != VECTIS_OK) {
      vectis_dsv_rows_cleanup(rows);
      return status;
    }
    if (!has_record) {
      rows->done = 1;
      vectis_error_clear(error);
      return VECTIS_OK;
    }
    rows->headers = rows->parser.fields;
    memset(&rows->parser.fields, 0, sizeof(rows->parser.fields));
    if (rows->columns == NULL) {
      if (rows->headers.count == 0u) {
        vectis_set_error(error, VECTIS_ERR_INVALID, "DSV header row is empty");
        vectis_dsv_rows_cleanup(rows);
        return VECTIS_ERR_INVALID;
      }
      rows->header_columns =
          (const char **)calloc(rows->headers.count, sizeof(rows->columns[0]));
      if (rows->header_columns == NULL) {
        vectis_set_error(error, VECTIS_ERR_NOMEM,
                         "failed to allocate DSV route header columns");
        vectis_dsv_rows_cleanup(rows);
        return VECTIS_ERR_NOMEM;
      }
      for (i = 0u; i < rows->headers.count; ++i) {
        rows->header_columns[i] = rows->headers.items[i];
      }
      rows->columns = rows->header_columns;
      rows->column_count = rows->headers.count;
    }
  } else if (rows->columns == NULL) {
    status = vectis_dsv_columns_from_map(map, &rows->header_columns,
                                         &rows->column_count, error);
    if (status != VECTIS_OK) {
      vectis_dsv_rows_cleanup(rows);
      return status;
    }
    rows->columns = rows->header_columns;
  }
  if (rows->columns == NULL || rows->column_count == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV columns are required when no header row is used");
    vectis_dsv_rows_cleanup(rows);
    return VECTIS_ERR_INVALID;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_dsv_rows_next(vectis_dsv_rows *rows, int *has_row,
                                   size_t *row_number, const void **row,
                                   vectis_error *error) {
  int has_record;
  vectis_status status;

  if (has_row != NULL) {
    *has_row = 0;
  }
  if (row_number != NULL) {
    *row_number = 0u;
  }
  if (row != NULL) {
    *row = NULL;
  }
  if (rows == NULL || has_row == NULL || row_number == NULL || row == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV row iterator output is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_dsv_rows_cleanup_value(rows);
  if (rows->done) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  status = vectis_dsv_read_data_record(&rows->parser, &has_record, error);
  if (status != VECTIS_OK) {
    rows->done = 1;
    return status;
  }
  if (!has_record) {
    rows->done = 1;
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  if (!rows->parser.config.strict_row_width_disabled &&
      rows->parser.fields.count != rows->column_count) {
    rows->done = 1;
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV row width does not match columns");
    return VECTIS_ERR_INVALID;
  }
  rows->value = calloc(1u, rows->row_size);
  if (rows->value == NULL) {
    rows->done = 1;
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate DSV route row");
    return VECTIS_ERR_NOMEM;
  }
  lonejson_init(rows->runtime, rows->map, rows->value);
  status = vectis_dsv_fields_to_lonejson_value(
      rows->map, rows->columns, rows->column_count, &rows->parser.fields,
      rows->value, error);
  if (status != VECTIS_OK) {
    vectis_dsv_rows_cleanup_value(rows);
    rows->done = 1;
    return status;
  }
  rows->data_row++;
  *has_row = 1;
  *row_number = rows->data_row;
  *row = rows->value;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status
vectis_dsv_to_json_array_impl(struct lc_source *source, const lonejson_map *map,
                              const vectis_dsv_config *config,
                              vectis_mutable_bytes *out, int all_strings,
                              vectis_error *error) {
  vectis_dsv_parser parser;
  vectis_dsv_config effective;
  vectis_dsv_fields headers;
  const char *const *columns;
  const char **header_columns;
  size_t column_count;
  int has_record;
  int first;
  vectis_status status;
  vectis_string_builder json;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV JSON output is required");
    return VECTIS_ERR_INVALID;
  }
  out->data = NULL;
  out->size = 0u;
  if (source == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV source is required");
    return VECTIS_ERR_INVALID;
  }
  if (!all_strings && map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_dsv_effective_config(config, &effective, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  memset(&parser, 0, sizeof(parser));
  memset(&headers, 0, sizeof(headers));
  memset(&json, 0, sizeof(json));
  parser.config = effective;
  parser.source = source;
  columns = effective.columns;
  header_columns = NULL;
  column_count = effective.column_count;
  if (!effective.header_disabled) {
    status = vectis_dsv_read_data_record(&parser, &has_record, error);
    if (status != VECTIS_OK) {
      goto cleanup;
    }
    if (has_record) {
      headers = parser.fields;
      memset(&parser.fields, 0, sizeof(parser.fields));
      if (columns == NULL) {
        if (headers.count == 0u) {
          vectis_set_error(error, VECTIS_ERR_INVALID,
                           "DSV header row is empty");
          status = VECTIS_ERR_INVALID;
          goto cleanup;
        }
        header_columns =
            (const char **)calloc(headers.count, sizeof(header_columns[0]));
        if (header_columns == NULL) {
          vectis_set_error(error, VECTIS_ERR_NOMEM,
                           "failed to allocate DSV header columns");
          status = VECTIS_ERR_NOMEM;
          goto cleanup;
        }
        for (column_count = 0u; column_count < headers.count; ++column_count) {
          header_columns[column_count] = headers.items[column_count];
        }
        columns = header_columns;
        column_count = headers.count;
      }
    }
  } else if (columns == NULL && !all_strings) {
    status =
        vectis_dsv_columns_from_map(map, &header_columns, &column_count, error);
    if (status != VECTIS_OK) {
      goto cleanup;
    }
    columns = header_columns;
  }
  if (columns == NULL || column_count == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV columns are required when no header row is used");
    status = VECTIS_ERR_INVALID;
    goto cleanup;
  }
  status = vectis_string_builder_append(&json, "[", error);
  first = 1;
  while (status == VECTIS_OK) {
    status = vectis_dsv_read_data_record(&parser, &has_record, error);
    if (status != VECTIS_OK || !has_record) {
      break;
    }
    if (!effective.strict_row_width_disabled &&
        parser.fields.count != column_count) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "DSV row width does not match columns");
      status = VECTIS_ERR_INVALID;
      break;
    }
    if (!first &&
        vectis_string_builder_append(&json, ",", error) != VECTIS_OK) {
      status = error != NULL ? error->code : VECTIS_ERR_NOMEM;
      break;
    }
    first = 0;
    if (all_strings) {
      status = vectis_dsv_append_string_row_json(&json, columns, column_count,
                                                 &parser.fields, error);
    } else {
      status = vectis_dsv_append_mapped_row_json(
          &json, map, columns, column_count, &parser.fields, error);
    }
  }
  if (status == VECTIS_OK &&
      vectis_string_builder_append(&json, "]", error) == VECTIS_OK) {
    out->data = json.data;
    out->size = json.size;
    json.data = NULL;
    json.size = 0u;
    vectis_error_clear(error);
  } else if (status == VECTIS_OK) {
    status = error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }

cleanup:
  vectis_dsv_fields_cleanup(&headers);
  vectis_dsv_fields_cleanup(&parser.fields);
  vectis_string_builder_cleanup(&parser.field);
  vectis_string_builder_cleanup(&json);
  free(header_columns);
  return status;
}

static void vectis_spill_writer_cleanup(vectis_spill_writer *writer) {
  if (writer == NULL) {
    return;
  }
  if (writer->fp != NULL) {
    (void)fclose(writer->fp);
  }
  if (writer->path != NULL) {
    (void)unlink(writer->path);
  }
  free(writer->path);
  free(writer->memory);
  memset(writer, 0, sizeof(*writer));
}

static vectis_status
vectis_spill_writer_open_file(vectis_spill_writer *writer,
                              const vectis_body_spill_config *config,
                              vectis_error *error) {
  vectis_body_materialize_config materialize;
  char tmp_template[PATH_MAX];
  int fd;

  vectis_body_materialize_config_init(&materialize);
  if (config != NULL) {
    materialize.directory = config->directory;
    materialize.prefix = config->prefix;
  }
  if (!vectis_tmp_template(tmp_template, sizeof(tmp_template), &materialize)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "spill output path is too long");
    return VECTIS_ERR_INVALID;
  }
  fd = mkstemp(tmp_template);
  if (fd < 0) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to create spill output file");
    return VECTIS_ERR_STATE;
  }
  writer->path = vectis_strdup(tmp_template);
  if (writer->path == NULL) {
    (void)close(fd);
    (void)unlink(tmp_template);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy spill output path");
    return VECTIS_ERR_NOMEM;
  }
  writer->fp = fdopen(fd, "wb");
  if (writer->fp == NULL) {
    (void)close(fd);
    (void)unlink(writer->path);
    free(writer->path);
    writer->path = NULL;
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to open spill output file");
    return VECTIS_ERR_STATE;
  }
  if (writer->memory_size > 0u &&
      fwrite(writer->memory, 1u, writer->memory_size, writer->fp) !=
          writer->memory_size) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to write spill output file");
    return VECTIS_ERR_STATE;
  }
  free(writer->memory);
  writer->memory = NULL;
  writer->memory_size = 0u;
  writer->memory_capacity = 0u;
  writer->spooled = 1;
  return VECTIS_OK;
}

static vectis_status
vectis_spill_writer_init(vectis_spill_writer *writer,
                         const vectis_body_spill_config *config) {
  memset(writer, 0, sizeof(*writer));
  writer->memory_limit = config != NULL ? config->memory_limit_bytes : 0u;
  if (writer->memory_limit == 0u) {
    writer->memory_limit = VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
  }
  return VECTIS_OK;
}

static vectis_status
vectis_spill_writer_write(vectis_spill_writer *writer, const void *data,
                          size_t size, const vectis_body_spill_config *config,
                          vectis_error *error) {
  void *next;

  if (size == 0u) {
    return VECTIS_OK;
  }
  if (writer == NULL || data == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "spill writer data is required");
    return VECTIS_ERR_INVALID;
  }
  if ((size_t)-1 - writer->total < size) {
    vectis_set_error(error, VECTIS_ERR_STATE, "spill output size overflow");
    return VECTIS_ERR_STATE;
  }
  if (!writer->spooled && writer->total + size > writer->memory_limit) {
    if (vectis_spill_writer_open_file(writer, config, error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
  }
  if (writer->spooled) {
    if (fwrite(data, 1u, size, writer->fp) != size) {
      vectis_set_error(error, VECTIS_ERR_STATE,
                       "failed to write spill output file");
      return VECTIS_ERR_STATE;
    }
  } else {
    if (writer->memory_size + size > writer->memory_capacity) {
      writer->memory_capacity =
          writer->memory_capacity == 0u ? size : writer->memory_capacity;
      while (writer->memory_capacity < writer->memory_size + size) {
        if (writer->memory_capacity > ((size_t)-1 / 2u)) {
          writer->memory_capacity = writer->memory_size + size;
          break;
        }
        writer->memory_capacity *= 2u;
      }
      next = realloc(writer->memory, writer->memory_capacity);
      if (next == NULL) {
        vectis_set_error(error, VECTIS_ERR_NOMEM,
                         "failed to allocate spill output memory");
        return VECTIS_ERR_NOMEM;
      }
      writer->memory = (unsigned char *)next;
    }
    memcpy(writer->memory + writer->memory_size, data, size);
    writer->memory_size += size;
  }
  writer->total += size;
  return VECTIS_OK;
}

typedef struct vectis_lonejson_spill_sink {
  vectis_spill_writer *writer;
  const vectis_body_spill_config *spill;
  vectis_error *error;
} vectis_lonejson_spill_sink;

static lonejson_status
vectis_lonejson_spill_sink_write(void *user, const void *data, size_t size,
                                 lonejson_error *json_error) {
  vectis_lonejson_spill_sink *sink;
  const char *message;
  vectis_status status;

  sink = (vectis_lonejson_spill_sink *)user;
  if (sink == NULL || sink->writer == NULL) {
    if (json_error != NULL) {
      lonejson_error_init(json_error);
      json_error->code = LONEJSON_STATUS_CALLBACK_FAILED;
      memcpy(json_error->message, "spill JSON sink is required",
             sizeof("spill JSON sink is required"));
    }
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  status = vectis_spill_writer_write(sink->writer, data, size, sink->spill,
                                     sink->error);
  if (status != VECTIS_OK) {
    if (json_error != NULL) {
      lonejson_error_init(json_error);
      json_error->code = LONEJSON_STATUS_CALLBACK_FAILED;
      message = sink->error != NULL && sink->error->message[0] != '\0'
                    ? sink->error->message
                    : "spill JSON sink write failed";
      strncpy(json_error->message, message, sizeof(json_error->message) - 1u);
      json_error->message[sizeof(json_error->message) - 1u] = '\0';
    }
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static vectis_status
vectis_spill_writer_write_cstr(vectis_spill_writer *writer, const char *value,
                               const vectis_body_spill_config *config,
                               vectis_error *error) {
  return vectis_spill_writer_write(
      writer, value, value != NULL ? strlen(value) : 0u, config, error);
}

static vectis_status vectis_spill_writer_finish(vectis_spill_writer *writer,
                                                vectis_body_spill_result *out,
                                                vectis_error *error) {
  if (writer == NULL || out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "spill writer output is required");
    return VECTIS_ERR_INVALID;
  }
  if (writer->fp != NULL && fclose(writer->fp) != 0) {
    writer->fp = NULL;
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to close spill output file");
    return VECTIS_ERR_STATE;
  }
  writer->fp = NULL;
  out->memory.data = NULL;
  out->memory.size = 0u;
  out->path = NULL;
  out->size = writer->total;
  out->spooled_to_disk = writer->spooled;
  if (writer->spooled) {
    out->path = writer->path;
    writer->path = NULL;
  } else {
    out->memory.data = writer->memory;
    out->memory.size = writer->memory_size;
    writer->memory = NULL;
    writer->memory_size = 0u;
    writer->memory_capacity = 0u;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status vectis_dsv_write_mapped_row_json_spill(
    vectis_spill_writer *writer, const lonejson_map *map,
    const char *const *columns, size_t column_count,
    const vectis_dsv_fields *fields, const vectis_body_spill_config *spill,
    vectis_error *error) {
  vectis_lonejson_spill_sink sink;
  lonejson_error json_error;
  lonejson_status json_status;
  lonejson *runtime;
  vectis_status status;
  void *value;

  if (writer == NULL || map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV lonejson spill output map is required");
    return VECTIS_ERR_INVALID;
  }
  value = calloc(1u, map->struct_size);
  if (value == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate DSV row struct");
    return VECTIS_ERR_NOMEM;
  }
  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    free(value);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  lonejson_init(runtime, map, value);
  status = vectis_dsv_fields_to_lonejson_value(map, columns, column_count,
                                               fields, value, error);
  if (status != VECTIS_OK) {
    lonejson_cleanup(map, value);
    lonejson_free(runtime);
    free(value);
    return status;
  }
  sink.writer = writer;
  sink.spill = spill;
  sink.error = error;
  json_status = lonejson_serialize_sink(runtime, map, value,
                                        vectis_lonejson_spill_sink_write, &sink,
                                        &json_error);
  lonejson_cleanup(map, value);
  lonejson_free(runtime);
  free(value);
  if (json_status != LONEJSON_STATUS_OK) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to serialize DSV row through lonejson: %s",
                      json_error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
      error->dependency_code = (long)json_status;
    }
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static vectis_status vectis_dsv_write_string_row_json_spill(
    vectis_spill_writer *writer, const char *const *columns,
    size_t column_count, const vectis_dsv_fields *fields,
    const vectis_body_spill_config *spill, vectis_error *error) {
  vectis_lonejson_spill_sink sink;
  lonejson_writer json_writer;
  lonejson_error json_error;
  lonejson_status json_status;
  lonejson *runtime;
  size_t i;
  const char *value;
  size_t value_size;

  sink.writer = writer;
  sink.spill = spill;
  sink.error = error;
  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  json_status = lonejson_writer_init_sink(runtime, &json_writer,
                                          vectis_lonejson_spill_sink_write,
                                          &sink, &json_error);
  if (json_status != LONEJSON_STATUS_OK) {
    goto json_failed;
  }
  json_status = lonejson_writer_begin_object(&json_writer, &json_error);
  if (json_status != LONEJSON_STATUS_OK) {
    goto json_cleanup_failed;
  }
  for (i = 0u; i < column_count; ++i) {
    value = i < fields->count ? fields->items[i] : "";
    value_size = i < fields->count ? fields->sizes[i] : 0u;
    json_status = lonejson_writer_key(&json_writer, columns[i],
                                      strlen(columns[i]), &json_error);
    if (json_status != LONEJSON_STATUS_OK) {
      goto json_cleanup_failed;
    }
    json_status =
        lonejson_writer_string(&json_writer, value, value_size, &json_error);
    if (json_status != LONEJSON_STATUS_OK) {
      goto json_cleanup_failed;
    }
  }
  json_status = lonejson_writer_end_object(&json_writer, &json_error);
  if (json_status != LONEJSON_STATUS_OK) {
    goto json_cleanup_failed;
  }
  json_status = lonejson_writer_finish(&json_writer, &json_error);
  lonejson_writer_cleanup(&json_writer);
  if (json_status == LONEJSON_STATUS_OK) {
    lonejson_free(runtime);
    return VECTIS_OK;
  }
  goto json_failed;

json_cleanup_failed:
  lonejson_writer_cleanup(&json_writer);
json_failed:
  lonejson_free(runtime);
  vectis_set_errorf(
      error, VECTIS_ERR_INVALID,
      "failed to serialize DSV string row spill through lonejson writer: %s",
      json_error.message);
  if (error != NULL) {
    error->source = VECTIS_ERROR_SOURCE_LONEJSON;
    error->dependency_code = (long)json_status;
  }
  return VECTIS_ERR_INVALID;
}

static vectis_status vectis_dsv_to_json_array_spill_impl(
    struct lc_source *source, const lonejson_map *map,
    const vectis_dsv_config *config, const vectis_body_spill_config *spill,
    vectis_body_spill_result *out, int all_strings, vectis_error *error) {
  vectis_dsv_parser parser;
  vectis_dsv_config effective;
  vectis_dsv_fields headers;
  const char *const *columns;
  const char **header_columns;
  size_t column_count;
  int has_record;
  int first;
  vectis_status status;
  vectis_spill_writer writer;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV spill output is required");
    return VECTIS_ERR_INVALID;
  }
  out->memory.data = NULL;
  out->memory.size = 0u;
  out->path = NULL;
  out->size = 0u;
  out->spooled_to_disk = 0;
  if (source == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV source is required");
    return VECTIS_ERR_INVALID;
  }
  if (!all_strings && map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_dsv_effective_config(config, &effective, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  memset(&parser, 0, sizeof(parser));
  memset(&headers, 0, sizeof(headers));
  vectis_spill_writer_init(&writer, spill);
  parser.config = effective;
  parser.source = source;
  columns = effective.columns;
  header_columns = NULL;
  column_count = effective.column_count;
  status = VECTIS_OK;
  if (!effective.header_disabled) {
    status = vectis_dsv_read_data_record(&parser, &has_record, error);
    if (status != VECTIS_OK) {
      goto cleanup;
    }
    if (has_record) {
      headers = parser.fields;
      memset(&parser.fields, 0, sizeof(parser.fields));
      if (columns == NULL) {
        if (headers.count == 0u) {
          vectis_set_error(error, VECTIS_ERR_INVALID,
                           "DSV header row is empty");
          status = VECTIS_ERR_INVALID;
          goto cleanup;
        }
        header_columns =
            (const char **)calloc(headers.count, sizeof(header_columns[0]));
        if (header_columns == NULL) {
          vectis_set_error(error, VECTIS_ERR_NOMEM,
                           "failed to allocate DSV header columns");
          status = VECTIS_ERR_NOMEM;
          goto cleanup;
        }
        for (column_count = 0u; column_count < headers.count; ++column_count) {
          header_columns[column_count] = headers.items[column_count];
        }
        columns = header_columns;
        column_count = headers.count;
      }
    }
  } else if (columns == NULL && !all_strings) {
    status =
        vectis_dsv_columns_from_map(map, &header_columns, &column_count, error);
    if (status != VECTIS_OK) {
      goto cleanup;
    }
    columns = header_columns;
  }
  if (columns == NULL || column_count == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV columns are required when no header row is used");
    status = VECTIS_ERR_INVALID;
    goto cleanup;
  }
  status = vectis_spill_writer_write_cstr(&writer, "[", spill, error);
  first = 1;
  while (status == VECTIS_OK) {
    status = vectis_dsv_read_data_record(&parser, &has_record, error);
    if (status != VECTIS_OK || !has_record) {
      break;
    }
    if (!effective.strict_row_width_disabled &&
        parser.fields.count != column_count) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "DSV row width does not match columns");
      status = VECTIS_ERR_INVALID;
      break;
    }
    if (!first && vectis_spill_writer_write_cstr(&writer, ",", spill, error) !=
                      VECTIS_OK) {
      status = error != NULL ? error->code : VECTIS_ERR_STATE;
      break;
    }
    first = 0;
    if (all_strings) {
      status = vectis_dsv_write_string_row_json_spill(
          &writer, columns, column_count, &parser.fields, spill, error);
    } else {
      status = vectis_dsv_write_mapped_row_json_spill(
          &writer, map, columns, column_count, &parser.fields, spill, error);
    }
  }
  if (status == VECTIS_OK &&
      vectis_spill_writer_write_cstr(&writer, "]", spill, error) != VECTIS_OK) {
    status = error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  if (status == VECTIS_OK) {
    status = vectis_spill_writer_finish(&writer, out, error);
  }

cleanup:
  if (status != VECTIS_OK) {
    vectis_body_spill_result_cleanup(out);
  }
  vectis_spill_writer_cleanup(&writer);
  vectis_dsv_fields_cleanup(&headers);
  vectis_dsv_fields_cleanup(&parser.fields);
  vectis_string_builder_cleanup(&parser.field);
  free(header_columns);
  return status;
}

vectis_status vectis_dsv_to_json_array(struct lc_source *source,
                                       const vectis_dsv_config *config,
                                       vectis_mutable_bytes *out,
                                       vectis_error *error) {
  return vectis_dsv_to_json_array_impl(source, NULL, config, out, 1, error);
}

vectis_status vectis_dsv_source_to_json_array(const vectis_source *source,
                                              const vectis_dsv_config *config,
                                              vectis_mutable_bytes *out,
                                              vectis_error *error) {
  lc_source *reader;
  int owned;
  vectis_status status;

  status = vectis_dsv_open_source(source, &reader, &owned, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_dsv_to_json_array(reader, config, out, error);
  if (owned) {
    lc_source_close(reader);
  }
  return status;
}

vectis_status vectis_dsv_to_lonejson_array(struct lc_source *source,
                                           const lonejson_map *map,
                                           const vectis_dsv_config *config,
                                           vectis_mutable_bytes *out,
                                           vectis_error *error) {
  return vectis_dsv_to_json_array_impl(source, map, config, out, 0, error);
}

vectis_status vectis_dsv_source_to_lonejson_array(
    const vectis_source *source, const lonejson_map *map,
    const vectis_dsv_config *config, vectis_mutable_bytes *out,
    vectis_error *error) {
  lc_source *reader;
  int owned;
  vectis_status status;

  status = vectis_dsv_open_source(source, &reader, &owned, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_dsv_to_lonejson_array(reader, map, config, out, error);
  if (owned) {
    lc_source_close(reader);
  }
  return status;
}

vectis_status vectis_dsv_to_json_array_spill(
    struct lc_source *source, const vectis_dsv_config *config,
    const vectis_body_spill_config *spill, vectis_body_spill_result *out,
    vectis_error *error) {
  return vectis_dsv_to_json_array_spill_impl(source, NULL, config, spill, out,
                                             1, error);
}

vectis_status vectis_dsv_source_to_json_array_spill(
    const vectis_source *source, const vectis_dsv_config *config,
    const vectis_body_spill_config *spill, vectis_body_spill_result *out,
    vectis_error *error) {
  lc_source *reader;
  int owned;
  vectis_status status;

  status = vectis_dsv_open_source(source, &reader, &owned, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_dsv_to_json_array_spill(reader, config, spill, out, error);
  if (owned) {
    lc_source_close(reader);
  }
  return status;
}

vectis_status vectis_dsv_to_lonejson_array_spill(
    struct lc_source *source, const lonejson_map *map,
    const vectis_dsv_config *config, const vectis_body_spill_config *spill,
    vectis_body_spill_result *out, vectis_error *error) {
  return vectis_dsv_to_json_array_spill_impl(source, map, config, spill, out, 0,
                                             error);
}

vectis_status vectis_dsv_source_to_lonejson_array_spill(
    const vectis_source *source, const lonejson_map *map,
    const vectis_dsv_config *config, const vectis_body_spill_config *spill,
    vectis_body_spill_result *out, vectis_error *error) {
  lc_source *reader;
  int owned;
  vectis_status status;

  status = vectis_dsv_open_source(source, &reader, &owned, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_dsv_to_lonejson_array_spill(reader, map, config, spill, out,
                                              error);
  if (owned) {
    lc_source_close(reader);
  }
  return status;
}

static vectis_status vectis_dsv_sink_write(lc_sink *sink, const void *data,
                                           size_t size, vectis_error *error) {
  lc_error lcerr;
  int rc;

  if (size == 0u) {
    return VECTIS_OK;
  }
  if (sink == NULL || sink->write == NULL || data == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV output sink is required");
    return VECTIS_ERR_INVALID;
  }
  lc_error_init(&lcerr);
  rc = sink->write(sink, data, size, &lcerr);
  if (!rc) {
    (void)vectis_source_error(
        error, lcerr.code != LC_OK ? lcerr.code : LC_ERR_TRANSPORT, &lcerr,
        "failed to write DSV output");
    lc_error_cleanup(&lcerr);
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  return VECTIS_OK;
}

static vectis_status vectis_dsv_sink_write_cstr(lc_sink *sink,
                                                const char *value,
                                                vectis_error *error) {
  return vectis_dsv_sink_write(sink, value, value != NULL ? strlen(value) : 0u,
                               error);
}

static int vectis_dsv_field_is_writable_scalar(const lonejson_field *field) {
  if (field == NULL) {
    return 0;
  }
  switch (field->kind) {
  case LONEJSON_FIELD_KIND_STRING:
  case LONEJSON_FIELD_KIND_I64:
  case LONEJSON_FIELD_KIND_U64:
  case LONEJSON_FIELD_KIND_F64:
  case LONEJSON_FIELD_KIND_BOOL:
    return 1;
  default:
    return 0;
  }
}

static vectis_status vectis_dsv_get_columns(const lonejson_map *map,
                                            const vectis_dsv_config *config,
                                            const char *const **out_columns,
                                            const char ***out_owned_columns,
                                            size_t *out_count,
                                            vectis_error *error) {
  const char **owned;
  vectis_status status;

  if (out_columns == NULL || out_owned_columns == NULL || out_count == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV column output is required");
    return VECTIS_ERR_INVALID;
  }
  *out_columns = NULL;
  *out_owned_columns = NULL;
  *out_count = 0u;
  if (config != NULL && config->columns != NULL) {
    if (config->column_count == 0u) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "DSV column count is required");
      return VECTIS_ERR_INVALID;
    }
    *out_columns = config->columns;
    *out_count = config->column_count;
    return VECTIS_OK;
  }
  status = vectis_dsv_columns_from_map(map, &owned, out_count, error);
  if (status != VECTIS_OK) {
    return status;
  }
  *out_columns = owned;
  *out_owned_columns = owned;
  return VECTIS_OK;
}

static vectis_status
vectis_dsv_validate_output_fields(const lonejson_map *map,
                                  const char *const *columns,
                                  size_t column_count, vectis_error *error) {
  size_t i;

  if (map == NULL || map->struct_size == 0u || columns == NULL ||
      column_count == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV map and columns are required");
    return VECTIS_ERR_INVALID;
  }
  for (i = 0u; i < column_count; ++i) {
    const lonejson_field *field;

    field = vectis_dsv_find_field(map, columns[i]);
    if (field == NULL) {
      vectis_set_errorf(error, VECTIS_ERR_INVALID,
                        "DSV output column has no mapped field: %s",
                        columns[i] != NULL ? columns[i] : "(null)");
      return VECTIS_ERR_INVALID;
    }
    if (!vectis_dsv_field_is_writable_scalar(field)) {
      vectis_set_errorf(error, VECTIS_ERR_INVALID,
                        "DSV output column is not a scalar field: %s",
                        columns[i] != NULL ? columns[i] : "(null)");
      return VECTIS_ERR_INVALID;
    }
  }
  return VECTIS_OK;
}

static int vectis_dsv_needs_quote(const char *value, size_t size,
                                  const vectis_dsv_config *config,
                                  int first_field) {
  size_t i;

  if (value == NULL || size == 0u) {
    return 0;
  }
  if (first_field && config->comment_prefix != NULL &&
      config->comment_prefix[0] != '\0' &&
      strncmp(value, config->comment_prefix, strlen(config->comment_prefix)) ==
          0) {
    return 1;
  }
  if (first_field && !config->indented_comments_disabled &&
      config->comment_prefix != NULL && config->comment_prefix[0] != '\0') {
    i = 0u;
    while (i < size && (value[i] == ' ' || value[i] == '\t')) {
      i++;
    }
    if (strncmp(value + i, config->comment_prefix,
                strlen(config->comment_prefix)) == 0) {
      return 1;
    }
  }
  for (i = 0u; i < size; ++i) {
    if ((unsigned char)value[i] == (unsigned char)config->delimiter ||
        (unsigned char)value[i] == (unsigned char)config->quote ||
        value[i] == '\r' || value[i] == '\n') {
      return 1;
    }
  }
  return 0;
}

static vectis_status
vectis_dsv_write_delimited_field(lc_sink *sink, const char *value, size_t size,
                                 const vectis_dsv_config *config,
                                 int first_field, vectis_error *error) {
  char quote;
  char escape;
  size_t i;

  if (value == NULL) {
    return VECTIS_OK;
  }
  if (!vectis_dsv_needs_quote(value, size, config, first_field)) {
    return vectis_dsv_sink_write(sink, value, size, error);
  }
  quote = (char)config->quote;
  escape = (char)config->escape;
  if (vectis_dsv_sink_write(sink, &quote, 1u, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  for (i = 0u; i < size; ++i) {
    if (value[i] == quote &&
        vectis_dsv_sink_write(sink, &escape, 1u, error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
    if (vectis_dsv_sink_write(sink, value + i, 1u, error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
  }
  return vectis_dsv_sink_write(sink, &quote, 1u, error);
}

static const char *vectis_dsv_string_field_value(const lonejson_field *field,
                                                 const void *row) {
  const char *base;

  base = (const char *)row + field->struct_offset;
  if (field->storage == LONEJSON_STORAGE_DYNAMIC) {
    const char *const *ptr;

    ptr = (const char *const *)base;
    return *ptr != NULL ? *ptr : "";
  }
  return base;
}

static vectis_status
vectis_dsv_write_field_value(lc_sink *sink, const lonejson_field *field,
                             const void *row, const vectis_dsv_config *config,
                             int first_field, vectis_error *error) {
  const char *base;
  const char *text;
  char number[64];
  int written;

  base = (const char *)row + field->struct_offset;
  switch (field->kind) {
  case LONEJSON_FIELD_KIND_STRING:
    text = vectis_dsv_string_field_value(field, row);
    return vectis_dsv_write_delimited_field(sink, text, strlen(text), config,
                                            first_field, error);
  case LONEJSON_FIELD_KIND_I64:
    written = snprintf(number, sizeof(number), "%lld",
                       (long long)*(const lonejson_int64 *)base);
    break;
  case LONEJSON_FIELD_KIND_U64:
    written = snprintf(number, sizeof(number), "%llu",
                       (unsigned long long)*(const lonejson_uint64 *)base);
    break;
  case LONEJSON_FIELD_KIND_F64:
    written = snprintf(number, sizeof(number), "%.17g", *(const double *)base);
    break;
  case LONEJSON_FIELD_KIND_BOOL:
    text = *(const int *)base ? "true" : "false";
    return vectis_dsv_sink_write_cstr(sink, text, error);
  default:
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV output field is not scalar");
    return VECTIS_ERR_INVALID;
  }
  if (written < 0 || (size_t)written >= sizeof(number)) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to format DSV scalar field");
    return VECTIS_ERR_STATE;
  }
  return vectis_dsv_sink_write(sink, number, (size_t)written, error);
}

vectis_status vectis_dsv_write_lonejson_rows(struct lc_sink *sink,
                                             const lonejson_map *map,
                                             const vectis_dsv_config *config,
                                             const void *rows, size_t row_count,
                                             size_t row_stride,
                                             vectis_error *error) {
  vectis_dsv_config effective;
  const char *const *columns;
  const char **owned_columns;
  size_t column_count;
  size_t row_index;
  size_t column_index;
  vectis_status status;
  char delimiter;

  if (sink == NULL || (rows == NULL && row_count > 0u) || map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV sink, map, and rows are required");
    return VECTIS_ERR_INVALID;
  }
  if (row_stride == 0u) {
    row_stride = map->struct_size;
  }
  if (row_stride < map->struct_size) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "DSV row stride is smaller than map struct size");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_dsv_effective_config(config, &effective, error);
  if (status != VECTIS_OK) {
    return status;
  }
  columns = NULL;
  owned_columns = NULL;
  column_count = 0u;
  status = vectis_dsv_get_columns(map, &effective, &columns, &owned_columns,
                                  &column_count, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_dsv_validate_output_fields(map, columns, column_count, error);
  if (status != VECTIS_OK) {
    free(owned_columns);
    return status;
  }
  delimiter = (char)effective.delimiter;
  if (!effective.header_disabled) {
    for (column_index = 0u; column_index < column_count; ++column_index) {
      if (column_index > 0u &&
          vectis_dsv_sink_write(sink, &delimiter, 1u, error) != VECTIS_OK) {
        free(owned_columns);
        return error != NULL ? error->code : VECTIS_ERR_STATE;
      }
      status = vectis_dsv_write_delimited_field(
          sink, columns[column_index], strlen(columns[column_index]),
          &effective, column_index == 0u, error);
      if (status != VECTIS_OK) {
        free(owned_columns);
        return status;
      }
    }
    if (vectis_dsv_sink_write_cstr(sink, "\n", error) != VECTIS_OK) {
      free(owned_columns);
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
  }
  for (row_index = 0u; row_index < row_count; ++row_index) {
    const char *row;

    row = (const char *)rows + row_index * row_stride;
    for (column_index = 0u; column_index < column_count; ++column_index) {
      const lonejson_field *field;

      if (column_index > 0u &&
          vectis_dsv_sink_write(sink, &delimiter, 1u, error) != VECTIS_OK) {
        free(owned_columns);
        return error != NULL ? error->code : VECTIS_ERR_STATE;
      }
      field = vectis_dsv_find_field(map, columns[column_index]);
      status = vectis_dsv_write_field_value(sink, field, row, &effective,
                                            column_index == 0u, error);
      if (status != VECTIS_OK) {
        free(owned_columns);
        return status;
      }
    }
    if (vectis_dsv_sink_write_cstr(sink, "\n", error) != VECTIS_OK) {
      free(owned_columns);
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
  }
  free(owned_columns);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_dsv_lonejson_rows_to_bytes(
    const lonejson_map *map, const vectis_dsv_config *config, const void *rows,
    size_t row_count, size_t row_stride, vectis_mutable_bytes *out,
    vectis_error *error) {
  lc_sink *sink;
  lc_error lcerr;
  const void *bytes;
  size_t size;
  vectis_status status;
  int rc;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV byte output is required");
    return VECTIS_ERR_INVALID;
  }
  out->data = NULL;
  out->size = 0u;
  lc_error_init(&lcerr);
  sink = NULL;
  rc = lc_sink_to_memory(&sink, &lcerr);
  if (rc != LC_OK) {
    (void)vectis_source_error(error, rc, &lcerr,
                              "failed to create DSV memory sink");
    lc_error_cleanup(&lcerr);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  lc_error_cleanup(&lcerr);
  status = vectis_dsv_write_lonejson_rows(sink, map, config, rows, row_count,
                                          row_stride, error);
  if (status != VECTIS_OK) {
    lc_sink_close(sink);
    return status;
  }
  lc_error_init(&lcerr);
  rc = lc_sink_memory_bytes(sink, &bytes, &size, &lcerr);
  if (rc != LC_OK) {
    (void)vectis_source_error(error, rc, &lcerr,
                              "failed to read DSV memory sink");
    lc_sink_close(sink);
    lc_error_cleanup(&lcerr);
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  out->data = malloc(size + 1u);
  if (out->data == NULL) {
    lc_sink_close(sink);
    lc_error_cleanup(&lcerr);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate DSV bytes");
    return VECTIS_ERR_NOMEM;
  }
  memcpy(out->data, bytes, size);
  ((char *)out->data)[size] = '\0';
  out->size = size;
  lc_sink_close(sink);
  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

typedef struct vectis_lonejson_lc_reader {
  lc_source *source;
  int last_error;
} vectis_lonejson_lc_reader;

typedef struct vectis_json_array_callback_state {
  vectis_json_array_item_fn callback;
  void *userdata;
  vectis_error *error;
  size_t index;
  vectis_status status;
} vectis_json_array_callback_state;

typedef struct vectis_lonejson_lc_sink {
  lc_sink *sink;
  int last_error;
} vectis_lonejson_lc_sink;

static lonejson_read_result
vectis_lonejson_lc_read(void *user, unsigned char *buffer, size_t capacity) {
  vectis_lonejson_lc_reader *reader;
  lonejson_read_result result;
  lc_error lcerr;
  size_t nread;

  result = lonejson_default_read_result();
  reader = (vectis_lonejson_lc_reader *)user;
  if (reader == NULL || reader->source == NULL || buffer == NULL) {
    result.error_code = EINVAL;
    return result;
  }
  if (capacity == 0u) {
    return result;
  }
  lc_error_init(&lcerr);
  nread = reader->source->read(reader->source, buffer, capacity, &lcerr);
  if (nread == 0u) {
    if (lcerr.code != 0) {
      reader->last_error = lcerr.code;
      result.error_code = EIO;
    } else {
      result.eof = 1;
    }
  } else {
    result.bytes_read = nread;
  }
  lc_error_cleanup(&lcerr);
  return result;
}

static lonejson_status
vectis_lonejson_lc_sink_write(void *user, const void *data, size_t size,
                              lonejson_error *json_error) {
  vectis_lonejson_lc_sink *writer;
  lc_error lcerr;
  int rc;

  (void)json_error;
  writer = (vectis_lonejson_lc_sink *)user;
  if (writer == NULL || writer->sink == NULL || writer->sink->write == NULL ||
      (data == NULL && size > 0u)) {
    if (json_error != NULL) {
      lonejson_error_init(json_error);
      json_error->code = LONEJSON_STATUS_CALLBACK_FAILED;
      (void)snprintf(json_error->message, sizeof(json_error->message), "%s",
                     "JSON rewrite sink is required");
    }
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (size == 0u) {
    return LONEJSON_STATUS_OK;
  }
  lc_error_init(&lcerr);
  rc = writer->sink->write(writer->sink, data, size, &lcerr);
  if (!rc) {
    writer->last_error = lcerr.code != LC_OK ? lcerr.code : LC_ERR_TRANSPORT;
    if (json_error != NULL) {
      lonejson_error_init(json_error);
      json_error->code = LONEJSON_STATUS_CALLBACK_FAILED;
      json_error->system_errno = writer->last_error;
      (void)snprintf(json_error->message, sizeof(json_error->message), "%s",
                     lcerr.message != NULL ? lcerr.message
                                           : "JSON rewrite sink write failed");
    }
    lc_error_cleanup(&lcerr);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  lc_error_cleanup(&lcerr);
  return LONEJSON_STATUS_OK;
}

static vectis_status vectis_set_lonejson_error(vectis_error *error,
                                               lonejson_status status,
                                               const lonejson_error *json_error,
                                               const char *context) {
  const char *message;

  message = json_error != NULL && json_error->message[0] != '\0'
                ? json_error->message
                : lonejson_status_string(status);
  vectis_set_errorf(error, VECTIS_ERR_INVALID, "%s: %s", context, message);
  if (error != NULL) {
    error->source = VECTIS_ERROR_SOURCE_LONEJSON;
    error->dependency_code = (long)status;
  }
  return VECTIS_ERR_INVALID;
}

vectis_status vectis_json_array_each_source(const vectis_source *source,
                                            const char *array_path,
                                            const lonejson_map *map, void *item,
                                            vectis_json_array_item_fn callback,
                                            void *userdata,
                                            vectis_error *error) {
  lc_source *reader_source;
  vectis_lonejson_lc_reader reader;
  lonejson_array_stream *stream;
  lonejson_array_stream_result result;
  lonejson_error json_error;
  lonejson *runtime;
  vectis_status callback_status;
  int owned;
  size_t index;

  if (map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  if (item == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "JSON array item storage is required");
    return VECTIS_ERR_INVALID;
  }
  if (callback == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "JSON array callback is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_dsv_open_source(source, &reader_source, &owned, error) !=
      VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  reader.source = reader_source;
  reader.last_error = 0;
  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    if (owned) {
      lc_source_close(reader_source);
    }
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  stream = lonejson_array_stream_open_reader(
      runtime, array_path, vectis_lonejson_lc_read, &reader, &json_error);
  if (stream == NULL) {
    lonejson_free(runtime);
    if (owned) {
      lc_source_close(reader_source);
    }
    return vectis_set_lonejson_error(error, LONEJSON_STATUS_INVALID_JSON,
                                     &json_error,
                                     "failed to open JSON array stream");
  }
  index = 0u;
  for (;;) {
    lonejson_init(runtime, map, item);
    result = lonejson_array_stream_next(stream, map, item, &json_error);
    if (result == LONEJSON_ARRAY_STREAM_ITEM) {
      callback_status = callback(userdata, index, item, error);
      lonejson_cleanup(map, item);
      if (callback_status != VECTIS_OK) {
        lonejson_array_stream_close(stream);
        lonejson_free(runtime);
        if (owned) {
          lc_source_close(reader_source);
        }
        return callback_status;
      }
      ++index;
      continue;
    }
    lonejson_cleanup(map, item);
    break;
  }
  lonejson_array_stream_close(stream);
  lonejson_free(runtime);
  if (owned) {
    lc_source_close(reader_source);
  }
  if (result != LONEJSON_ARRAY_STREAM_EOF) {
    return vectis_set_lonejson_error(error, LONEJSON_STATUS_INVALID_JSON,
                                     &json_error,
                                     "failed to stream JSON array");
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_json_array_rewrite_source(
    const vectis_source *source, const char *selector, struct lc_sink *sink,
    const lonejson_array_rewrite_options *options, vectis_error *error) {
  lc_source *reader_source;
  vectis_lonejson_lc_reader reader;
  vectis_lonejson_lc_sink writer;
  lonejson_error json_error;
  lonejson_status json_status;
  lonejson *runtime;
  int owned;

  if (sink == NULL || sink->write == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "JSON rewrite output sink is required");
    return VECTIS_ERR_INVALID;
  }
  if (options == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "JSON rewrite options are required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_dsv_open_source(source, &reader_source, &owned, error) !=
      VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  reader.source = reader_source;
  reader.last_error = 0;
  writer.sink = sink;
  writer.last_error = 0;
  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    if (owned) {
      lc_source_close(reader_source);
    }
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  json_status = lonejson_array_rewrite_reader(
      runtime, selector, vectis_lonejson_lc_read, &reader,
      vectis_lonejson_lc_sink_write, &writer, options, &json_error);
  lonejson_free(runtime);
  if (owned) {
    lc_source_close(reader_source);
  }
  if (json_status != LONEJSON_STATUS_OK) {
    return vectis_set_lonejson_error(error, json_status, &json_error,
                                     "failed to rewrite JSON array");
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

void vectis_xml_config_init(vectis_xml_config *config) {
  if (config == NULL) {
    return;
  }
  config->root_element = NULL;
  config->text_key = "text";
  config->attribute_prefix = "";
  config->trim_text = 0;
  config->max_depth = 64u;
  config->max_text_bytes = 67108864u;
}

vectis_xml_config vectis_xml_default(void) {
  vectis_xml_config config;

  vectis_xml_config_init(&config);
  return config;
}

static int vectis_xml_lc_read(void *ctx, char *buffer, int len) {
  vectis_xml_lc_reader *reader;
  size_t nread;

  reader = (vectis_xml_lc_reader *)ctx;
  if (reader == NULL || reader->source == NULL || buffer == NULL || len < 0) {
    return -1;
  }
  lc_error_cleanup(&reader->error);
  lc_error_init(&reader->error);
  nread =
      reader->source->read(reader->source, buffer, (size_t)len, &reader->error);
  if (nread == 0u && reader->error.code != 0) {
    reader->has_error = 1;
    reader->status = VECTIS_ERR_STATE;
    return -1;
  }
  return (int)nread;
}

static vectis_status
vectis_xml_effective_config(const vectis_xml_config *config,
                            vectis_xml_config *out, vectis_error *error) {
  vectis_xml_config defaults;

  vectis_xml_config_init(&defaults);
  *out = config != NULL ? *config : defaults;
  if (out->max_depth == 0u) {
    out->max_depth = defaults.max_depth;
  }
  if (out->max_text_bytes == 0u) {
    out->max_text_bytes = defaults.max_text_bytes;
  }
  if (out->text_key == NULL) {
    out->text_key = defaults.text_key;
  }
  if (out->attribute_prefix == NULL) {
    out->attribute_prefix = defaults.attribute_prefix;
  }
  if (out->root_element != NULL && out->root_element[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "XML root_element must not be empty");
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static const char *vectis_xml_reader_local_name(xmlTextReaderPtr reader) {
  const xmlChar *name;

  name = xmlTextReaderConstLocalName(reader);
  if (name == NULL) {
    name = xmlTextReaderConstName(reader);
  }
  return (const char *)name;
}

static int vectis_xml_name_matches(const char *actual, const char *expected) {
  return actual != NULL && expected != NULL && strcmp(actual, expected) == 0;
}

static const lonejson_field *vectis_xml_find_field(const lonejson_map *map,
                                                   const char *name) {
  return vectis_dsv_find_field(map, name);
}

static const lonejson_field *vectis_xml_find_attribute_field(
    const lonejson_map *map, const vectis_xml_config *config, const char *name,
    vectis_string_builder *key, vectis_error *error) {
  const lonejson_field *field;

  field = vectis_xml_find_field(map, name);
  if (field != NULL || config->attribute_prefix[0] == '\0') {
    return field;
  }
  key->size = 0u;
  if (key->data != NULL) {
    key->data[0] = '\0';
  }
  if (vectis_string_builder_append(key, config->attribute_prefix, error) !=
          VECTIS_OK ||
      vectis_string_builder_append(key, name, error) != VECTIS_OK) {
    return NULL;
  }
  return vectis_xml_find_field(map, key->data);
}

static int vectis_xml_field_is_array(const lonejson_field *field) {
  return field != NULL && (field->kind == LONEJSON_FIELD_KIND_STRING_ARRAY ||
                           field->kind == LONEJSON_FIELD_KIND_I64_ARRAY ||
                           field->kind == LONEJSON_FIELD_KIND_U64_ARRAY ||
                           field->kind == LONEJSON_FIELD_KIND_F64_ARRAY ||
                           field->kind == LONEJSON_FIELD_KIND_BOOL_ARRAY ||
                           field->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY);
}

static void vectis_xml_trim_span(const char **data, size_t *size) {
  const char *start;
  const char *end;

  start = *data;
  end = start + *size;
  while (start < end && isspace((unsigned char)*start)) {
    start++;
  }
  while (end > start && isspace((unsigned char)*(end - 1))) {
    end--;
  }
  *data = start;
  *size = (size_t)(end - start);
}

static int vectis_xml_span_has_nonspace(const char *data, size_t size) {
  size_t i;

  for (i = 0u; i < size; ++i) {
    if (!isspace((unsigned char)data[i])) {
      return 1;
    }
  }
  return 0;
}

static int vectis_xml_field_is_direct_text_stream(const lonejson_field *field) {
  return field != NULL && field->kind == LONEJSON_FIELD_KIND_STRING_STREAM;
}

typedef union vectis_lonejson_owned_align {
  void *ptr;
  lonejson_uint64 u64;
  double f64;
  long double ld;
} vectis_lonejson_owned_align;

typedef union vectis_lonejson_owned_header {
  struct {
    lonejson_allocator allocator;
    size_t size;
    const void *budget_tag;
    size_t budget_bytes;
  } meta;
  vectis_lonejson_owned_align align;
} vectis_lonejson_owned_header;

static void *vectis_lonejson_owned_malloc(size_t size) {
  lonejson_allocator allocator;
  vectis_lonejson_owned_header *header;
  void *raw;

  allocator = lonejson_default_allocator();
  raw = allocator.malloc_fn(allocator.ctx, sizeof(*header) + size);
  if (raw == NULL) {
    return NULL;
  }
  header = (vectis_lonejson_owned_header *)raw;
  header->meta.allocator = allocator;
  header->meta.size = size;
  header->meta.budget_tag = NULL;
  header->meta.budget_bytes = 0u;
  return (void *)(header + 1u);
}

static void *vectis_lonejson_owned_realloc(void *ptr, size_t size) {
  vectis_lonejson_owned_header *header;
  vectis_lonejson_owned_header *next;
  lonejson_allocator allocator;

  if (ptr == NULL) {
    return vectis_lonejson_owned_malloc(size);
  }
  header = ((vectis_lonejson_owned_header *)ptr) - 1u;
  allocator = header->meta.allocator;
  next = (vectis_lonejson_owned_header *)allocator.realloc_fn(
      allocator.ctx, header, sizeof(*header) + size);
  if (next == NULL) {
    return NULL;
  }
  next->meta.allocator = allocator;
  next->meta.size = size;
  return (void *)(next + 1u);
}

static vectis_status
vectis_xml_set_lonejson_string_field(const lonejson_field *field, void *value,
                                     const char *text, size_t text_size,
                                     vectis_error *error) {
  char *dst;
  char **dyn;
  size_t copy_size;

  if (field->storage == LONEJSON_STORAGE_DYNAMIC) {
    dyn = (char **)((unsigned char *)value + field->struct_offset);
    *dyn = (char *)vectis_lonejson_owned_malloc(text_size + 1u);
    if (*dyn == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to allocate XML lonejson string field");
      return VECTIS_ERR_NOMEM;
    }
    memcpy(*dyn, text, text_size);
    (*dyn)[text_size] = '\0';
    return VECTIS_OK;
  }
  if (field->fixed_capacity == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "XML lonejson string field has no capacity");
    return VECTIS_ERR_INVALID;
  }
  dst = (char *)((unsigned char *)value + field->struct_offset);
  if (text_size >= field->fixed_capacity) {
    if (field->overflow_policy == LONEJSON_OVERFLOW_FAIL) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "XML lonejson string field overflow");
      return VECTIS_ERR_INVALID;
    }
    copy_size = field->fixed_capacity - 1u;
  } else {
    copy_size = text_size;
  }
  memcpy(dst, text, copy_size);
  dst[copy_size] = '\0';
  return VECTIS_OK;
}

static vectis_status vectis_xml_set_lonejson_scalar_field(
    const lonejson_field *field, void *value, const char *data, size_t size,
    const vectis_xml_config *config, vectis_error *error) {
  const char *text;
  size_t text_size;
  char buffer[128];
  char *end;
  lonejson_int64 i64_value;
  lonejson_uint64 u64_value;

  text = data;
  text_size = size;
  if (config->trim_text) {
    vectis_xml_trim_span(&text, &text_size);
  }
  if (field->flags & LONEJSON_FIELD_HAS_PRESENCE) {
    *(int *)((unsigned char *)value + field->presence_offset) = 1;
  }
  if (field->kind == LONEJSON_FIELD_KIND_STRING) {
    return vectis_xml_set_lonejson_string_field(field, value, text, text_size,
                                                error);
  }
  if (text_size == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "XML scalar field must not be empty");
    return VECTIS_ERR_INVALID;
  }
  if (text_size >= sizeof(buffer)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "XML scalar field is too large");
    return VECTIS_ERR_INVALID;
  }
  memcpy(buffer, text, text_size);
  buffer[text_size] = '\0';
  errno = 0;
  switch (field->kind) {
  case LONEJSON_FIELD_KIND_I64:
    if (!vectis_parse_lonejson_i64_text(buffer, &i64_value)) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "XML scalar field is invalid");
      return VECTIS_ERR_INVALID;
    }
    *(lonejson_int64 *)((unsigned char *)value + field->struct_offset) =
        i64_value;
    return VECTIS_OK;
  case LONEJSON_FIELD_KIND_U64:
    if (!vectis_parse_lonejson_u64_text(buffer, &u64_value)) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "XML scalar field is invalid");
      return VECTIS_ERR_INVALID;
    }
    *(lonejson_uint64 *)((unsigned char *)value + field->struct_offset) =
        u64_value;
    return VECTIS_OK;
  case LONEJSON_FIELD_KIND_F64:
    *(double *)((unsigned char *)value + field->struct_offset) =
        strtod(buffer, &end);
    break;
  case LONEJSON_FIELD_KIND_BOOL:
    if ((text_size == 4u && strncasecmp(text, "true", 4u) == 0) ||
        (text_size == 1u && text[0] == '1')) {
      *(int *)((unsigned char *)value + field->struct_offset) = 1;
      return VECTIS_OK;
    } else if ((text_size == 5u && strncasecmp(text, "false", 5u) == 0) ||
               (text_size == 1u && text[0] == '0')) {
      *(int *)((unsigned char *)value + field->struct_offset) = 0;
      return VECTIS_OK;
    }
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "XML boolean value must be true, false, 1, or 0");
    return VECTIS_ERR_INVALID;
  default:
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "XML lonejson field kind is not supported by direct XML "
                     "mapping");
    return VECTIS_ERR_INVALID;
  }
  if (errno != 0 || end == buffer || *end != '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML scalar field is invalid");
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static vectis_status vectis_xml_lonejson_error(vectis_error *error,
                                               lonejson_status status,
                                               const lonejson_error *json_error,
                                               const char *message) {
  if (json_error != NULL && json_error->message[0] != '\0') {
    return vectis_set_lonejson_error(error, status, json_error, message);
  }
  vectis_set_error(error, VECTIS_ERR_INVALID, message);
  if (error != NULL) {
    error->source = VECTIS_ERROR_SOURCE_LONEJSON;
    error->dependency_code = (long)status;
  }
  return VECTIS_ERR_INVALID;
}

static vectis_status vectis_xml_set_spooled_field(const lonejson_field *field,
                                                  void *value, const char *data,
                                                  size_t size,
                                                  vectis_error *error) {
  lonejson_error json_error;
  lonejson_status json_status;
  lonejson_spooled *spooled;

  spooled = (lonejson_spooled *)((unsigned char *)value + field->struct_offset);
  json_status = lonejson_spooled_append(spooled, data, size, &json_error);
  if (json_status != LONEJSON_STATUS_OK) {
    return vectis_xml_lonejson_error(error, json_status, &json_error,
                                     "failed to append XML spooled field");
  }
  return VECTIS_OK;
}

static vectis_status vectis_xml_reserve_array(void *array_value,
                                              size_t elem_size,
                                              vectis_error *error) {
  lonejson_string_array *array;
  size_t new_capacity;
  void *items;

  array = (lonejson_string_array *)array_value;
  if (array->count < array->capacity) {
    return VECTIS_OK;
  }
  if (array->flags & LONEJSON_ARRAY_FIXED_CAPACITY) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "XML lonejson array field overflow");
    return VECTIS_ERR_INVALID;
  }
  new_capacity = array->capacity == 0u ? 4u : array->capacity * 2u;
  if (new_capacity < array->capacity ||
      (elem_size != 0u && new_capacity > SIZE_MAX / elem_size)) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "XML array size overflow");
    return VECTIS_ERR_NOMEM;
  }
  items = vectis_lonejson_owned_realloc(array->items, new_capacity * elem_size);
  if (items == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate XML lonejson array field");
    return VECTIS_ERR_NOMEM;
  }
  array->items = (char **)items;
  array->capacity = new_capacity;
  array->flags |= LONEJSON_ARRAY_OWNS_ITEMS;
  return VECTIS_OK;
}

static vectis_status vectis_xml_set_array_scalar_field(
    const lonejson_field *field, void *array_value, const char *data,
    size_t size, const vectis_xml_config *config, vectis_error *error) {
  lonejson_field item_field;
  lonejson_string_array *strings;
  lonejson_i64_array *i64s;
  lonejson_u64_array *u64s;
  lonejson_f64_array *f64s;
  lonejson_bool_array *bools;

  item_field = *field;
  item_field.struct_offset = 0u;
  item_field.flags = 0u;
  item_field.fixed_capacity = 0u;
  item_field.submap = NULL;
  item_field.storage = LONEJSON_STORAGE_FIXED;
  switch (field->kind) {
  case LONEJSON_FIELD_KIND_STRING_ARRAY:
    strings = (lonejson_string_array *)array_value;
    if (vectis_xml_reserve_array(strings, sizeof(strings->items[0]), error) !=
        VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    item_field.kind = LONEJSON_FIELD_KIND_STRING;
    item_field.storage = LONEJSON_STORAGE_DYNAMIC;
    return vectis_xml_set_lonejson_scalar_field(
        &item_field, &strings->items[strings->count++], data, size, config,
        error);
  case LONEJSON_FIELD_KIND_I64_ARRAY:
    i64s = (lonejson_i64_array *)array_value;
    if (vectis_xml_reserve_array(i64s, sizeof(i64s->items[0]), error) !=
        VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    item_field.kind = LONEJSON_FIELD_KIND_I64;
    return vectis_xml_set_lonejson_scalar_field(
        &item_field, &i64s->items[i64s->count++], data, size, config, error);
  case LONEJSON_FIELD_KIND_U64_ARRAY:
    u64s = (lonejson_u64_array *)array_value;
    if (vectis_xml_reserve_array(u64s, sizeof(u64s->items[0]), error) !=
        VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    item_field.kind = LONEJSON_FIELD_KIND_U64;
    return vectis_xml_set_lonejson_scalar_field(
        &item_field, &u64s->items[u64s->count++], data, size, config, error);
  case LONEJSON_FIELD_KIND_F64_ARRAY:
    f64s = (lonejson_f64_array *)array_value;
    if (vectis_xml_reserve_array(f64s, sizeof(f64s->items[0]), error) !=
        VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    item_field.kind = LONEJSON_FIELD_KIND_F64;
    return vectis_xml_set_lonejson_scalar_field(
        &item_field, &f64s->items[f64s->count++], data, size, config, error);
  case LONEJSON_FIELD_KIND_BOOL_ARRAY:
    bools = (lonejson_bool_array *)array_value;
    if (vectis_xml_reserve_array(bools, sizeof(bools->items[0]), error) !=
        VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    item_field.kind = LONEJSON_FIELD_KIND_BOOL;
    return vectis_xml_set_lonejson_scalar_field(
        &item_field, &bools->items[bools->count++], data, size, config, error);
  default:
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "XML scalar array field kind is unsupported");
    return VECTIS_ERR_INVALID;
  }
}

static vectis_status vectis_xml_append_object_array_item(
    lonejson *runtime, const lonejson_field *field, void *array_value,
    void **out, vectis_error *error) {
  lonejson_object_array *array;
  unsigned char *items;
  size_t new_capacity;
  size_t elem_size;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "XML object array output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  if (field->submap == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "XML object array field requires a submap");
    return VECTIS_ERR_INVALID;
  }
  array = (lonejson_object_array *)array_value;
  elem_size =
      field->elem_size != 0u ? field->elem_size : field->submap->struct_size;
  if (elem_size == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "XML object array field has no element size");
    return VECTIS_ERR_INVALID;
  }
  array->elem_size = elem_size;
  if (array->count >= array->capacity) {
    if (array->flags & LONEJSON_ARRAY_FIXED_CAPACITY) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "XML lonejson object array field overflow");
      return VECTIS_ERR_INVALID;
    }
    new_capacity = array->capacity == 0u ? 4u : array->capacity * 2u;
    if (new_capacity < array->capacity || new_capacity > SIZE_MAX / elem_size) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "XML object array size overflow");
      return VECTIS_ERR_NOMEM;
    }
    items = (unsigned char *)vectis_lonejson_owned_realloc(
        array->items, new_capacity * elem_size);
    if (items == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to allocate XML lonejson object array field");
      return VECTIS_ERR_NOMEM;
    }
    array->items = items;
    array->capacity = new_capacity;
    array->flags |= LONEJSON_ARRAY_OWNS_ITEMS;
  }
  items = (unsigned char *)array->items;
  *out = items + array->count * elem_size;
  memset(*out, 0, elem_size);
  lonejson_init(runtime, field->submap, *out);
  array->count++;
  return VECTIS_OK;
}

static vectis_status vectis_xml_skip_element(xmlTextReaderPtr reader,
                                             vectis_error *error) {
  int start_depth;
  int rc;
  int type;

  if (xmlTextReaderIsEmptyElement(reader)) {
    return VECTIS_OK;
  }
  start_depth = xmlTextReaderDepth(reader);
  for (;;) {
    rc = xmlTextReaderRead(reader);
    if (rc != 1) {
      if (rc == 0) {
        vectis_set_error(error, VECTIS_ERR_INVALID,
                         "unexpected end of XML while skipping element");
      } else {
        vectis_set_error(error, VECTIS_ERR_INVALID,
                         "libxml2 failed while skipping XML element");
      }
      return VECTIS_ERR_INVALID;
    }
    type = xmlTextReaderNodeType(reader);
    if (type == XML_READER_TYPE_END_ELEMENT &&
        xmlTextReaderDepth(reader) == start_depth) {
      return VECTIS_OK;
    }
  }
}

static vectis_status
vectis_xml_stream_object(xmlTextReaderPtr reader, const lonejson_map *map,
                         const vectis_xml_config *config, lonejson *runtime,
                         void *out, size_t depth, vectis_error *error);

static vectis_status vectis_xml_finish_array(vectis_xml_field_state *states,
                                             size_t *open_index) {
  if (*open_index == SIZE_MAX) {
    return VECTIS_OK;
  }
  states[*open_index].array_open = 0;
  states[*open_index].array_closed = 1;
  *open_index = SIZE_MAX;
  return VECTIS_OK;
}

static vectis_status vectis_xml_begin_field(vectis_xml_field_state *states,
                                            size_t *open_index, size_t index,
                                            const lonejson_field *field,
                                            vectis_error *error) {
  vectis_status status;

  if (!states[index].array && states[index].count > 0u) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "duplicate XML value for field '%s'", field->json_key);
    return VECTIS_ERR_INVALID;
  }
  if (states[index].array && states[index].array_closed) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "non-contiguous XML array field '%s'", field->json_key);
    return VECTIS_ERR_INVALID;
  }
  if (states[index].array && *open_index != SIZE_MAX && *open_index == index) {
    states[index].count++;
    return VECTIS_OK;
  }
  status = vectis_xml_finish_array(states, open_index);
  if (status != VECTIS_OK) {
    return status;
  }
  if (states[index].array) {
    states[index].array_open = 1;
    *open_index = index;
  }
  states[index].count++;
  return VECTIS_OK;
}

static vectis_status vectis_xml_stream_scalar_content(
    xmlTextReaderPtr reader, const lonejson_field *field,
    const vectis_xml_config *config, void *dst, vectis_error *error) {
  vectis_string_builder text;
  const xmlChar *xml_value;
  const char *chunk;
  size_t chunk_size;
  int start_depth;
  int type;
  int rc;
  vectis_status status;

  text.data = NULL;
  text.size = 0u;
  text.capacity = 0u;
  status = VECTIS_OK;
  if (vectis_xml_field_is_direct_text_stream(field) && config->trim_text) {
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "XML spooled lonejson fields require trim_text=0 for true streaming");
    return VECTIS_ERR_INVALID;
  }
  if (xmlTextReaderIsEmptyElement(reader)) {
    status = vectis_xml_field_is_direct_text_stream(field)
                 ? vectis_xml_set_spooled_field(field, dst, "", 0u, error)
                 : vectis_xml_set_lonejson_scalar_field(field, dst, "", 0u,
                                                        config, error);
    vectis_string_builder_cleanup(&text);
    return status;
  }
  start_depth = xmlTextReaderDepth(reader);
  for (;;) {
    rc = xmlTextReaderRead(reader);
    if (rc != 1) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       rc == 0 ? "unexpected end of XML while reading scalar"
                               : "libxml2 failed while reading XML scalar");
      status = VECTIS_ERR_INVALID;
      break;
    }
    type = xmlTextReaderNodeType(reader);
    if (type == XML_READER_TYPE_END_ELEMENT &&
        xmlTextReaderDepth(reader) == start_depth) {
      break;
    }
    if (type == XML_READER_TYPE_ELEMENT) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "XML scalar field contains nested elements");
      status = VECTIS_ERR_INVALID;
      break;
    }
    if (type == XML_READER_TYPE_TEXT || type == XML_READER_TYPE_CDATA ||
        type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE ||
        type == XML_READER_TYPE_WHITESPACE) {
      xml_value = xmlTextReaderConstValue(reader);
      if (xml_value != NULL) {
        chunk = (const char *)xml_value;
        chunk_size = strlen(chunk);
        if (config->max_text_bytes > 0u &&
            text.size + chunk_size > config->max_text_bytes) {
          vectis_set_error(error, VECTIS_ERR_INVALID,
                           "XML text exceeds max_text_bytes");
          status = VECTIS_ERR_INVALID;
          break;
        }
        if (vectis_xml_field_is_direct_text_stream(field)) {
          text.size += chunk_size;
          status = vectis_xml_set_spooled_field(field, dst, chunk, chunk_size,
                                                error);
          if (status != VECTIS_OK) {
            break;
          }
        } else {
          if (vectis_string_builder_append_n(&text, chunk, chunk_size, error) !=
              VECTIS_OK) {
            status = error != NULL ? error->code : VECTIS_ERR_NOMEM;
            break;
          }
        }
      }
    }
  }
  if (!vectis_xml_field_is_direct_text_stream(field) && status == VECTIS_OK) {
    status = vectis_xml_set_lonejson_scalar_field(
        field, dst, text.data != NULL ? text.data : "", text.size, config,
        error);
  }
  vectis_string_builder_cleanup(&text);
  return status;
}

static vectis_status vectis_xml_stream_array_scalar_content(
    xmlTextReaderPtr reader, const lonejson_field *field,
    const vectis_xml_config *config, void *array_value, vectis_error *error) {
  vectis_string_builder text;
  const xmlChar *xml_value;
  const char *chunk;
  size_t chunk_size;
  int start_depth;
  int type;
  int rc;
  vectis_status status;

  text.data = NULL;
  text.size = 0u;
  text.capacity = 0u;
  status = VECTIS_OK;
  if (xmlTextReaderIsEmptyElement(reader)) {
    status = vectis_xml_set_array_scalar_field(field, array_value, "", 0u,
                                               config, error);
    vectis_string_builder_cleanup(&text);
    return status;
  }
  start_depth = xmlTextReaderDepth(reader);
  for (;;) {
    rc = xmlTextReaderRead(reader);
    if (rc != 1) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       rc == 0 ? "unexpected end of XML while reading scalar"
                               : "libxml2 failed while reading XML scalar");
      status = VECTIS_ERR_INVALID;
      break;
    }
    type = xmlTextReaderNodeType(reader);
    if (type == XML_READER_TYPE_END_ELEMENT &&
        xmlTextReaderDepth(reader) == start_depth) {
      break;
    }
    if (type == XML_READER_TYPE_ELEMENT) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "XML scalar field contains nested elements");
      status = VECTIS_ERR_INVALID;
      break;
    }
    if (type == XML_READER_TYPE_TEXT || type == XML_READER_TYPE_CDATA ||
        type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE ||
        type == XML_READER_TYPE_WHITESPACE) {
      xml_value = xmlTextReaderConstValue(reader);
      if (xml_value != NULL) {
        chunk = (const char *)xml_value;
        chunk_size = strlen(chunk);
        if (config->max_text_bytes > 0u &&
            text.size + chunk_size > config->max_text_bytes) {
          vectis_set_error(error, VECTIS_ERR_INVALID,
                           "XML text exceeds max_text_bytes");
          status = VECTIS_ERR_INVALID;
          break;
        }
        if (vectis_string_builder_append_n(&text, chunk, chunk_size, error) !=
            VECTIS_OK) {
          status = error != NULL ? error->code : VECTIS_ERR_NOMEM;
          break;
        }
      }
    }
  }
  if (status == VECTIS_OK) {
    status = vectis_xml_set_array_scalar_field(
        field, array_value, text.data != NULL ? text.data : "", text.size,
        config, error);
  }
  vectis_string_builder_cleanup(&text);
  return status;
}

static vectis_status vectis_xml_stream_field_value(
    xmlTextReaderPtr reader, const lonejson_field *field,
    const vectis_xml_config *config, lonejson *runtime, void *out, size_t depth,
    vectis_error *error) {
  void *field_value;
  void *item;

  field_value = (unsigned char *)out + field->struct_offset;
  if (field->kind == LONEJSON_FIELD_KIND_OBJECT) {
    if (field->submap == NULL) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "XML object field requires a submap");
      return VECTIS_ERR_INVALID;
    }
    return vectis_xml_stream_object(reader, field->submap, config, runtime,
                                    field_value, depth, error);
  }
  if (field->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY) {
    if (vectis_xml_append_object_array_item(runtime, field, field_value, &item,
                                            error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_INVALID;
    }
    return vectis_xml_stream_object(reader, field->submap, config, runtime,
                                    item, depth, error);
  }
  if (vectis_xml_field_is_array(field)) {
    return vectis_xml_stream_array_scalar_content(reader, field, config,
                                                  field_value, error);
  }
  return vectis_xml_stream_scalar_content(reader, field, config, out, error);
}

static vectis_status vectis_xml_stream_text_field(
    vectis_xml_field_state *states, size_t *open_index, const lonejson_map *map,
    const vectis_xml_config *config, void *out, const char *data, size_t size,
    vectis_error *error) {
  const lonejson_field *field;
  void *field_value;
  const char *text;
  size_t text_size;
  size_t index;
  vectis_status status;

  if (config->text_key == NULL || config->text_key[0] == '\0') {
    return VECTIS_OK;
  }
  text = data;
  text_size = size;
  if (config->trim_text) {
    vectis_xml_trim_span(&text, &text_size);
  }
  if (text_size == 0u) {
    return VECTIS_OK;
  }
  field = vectis_xml_find_field(map, config->text_key);
  if (field == NULL) {
    if (!config->skip_unknown_disabled) {
      return VECTIS_OK;
    }
    vectis_set_errorf(error, VECTIS_ERR_INVALID, "unknown XML text field '%s'",
                      config->text_key);
    return VECTIS_ERR_INVALID;
  }
  index = (size_t)(field - map->fields);
  status = vectis_xml_begin_field(states, open_index, index, field, error);
  if (status != VECTIS_OK) {
    return status;
  }
  field_value = (unsigned char *)out + field->struct_offset;
  if (vectis_xml_field_is_array(field)) {
    return vectis_xml_set_array_scalar_field(field, field_value, text,
                                             text_size, config, error);
  }
  return vectis_xml_set_lonejson_scalar_field(field, out, text, text_size,
                                              config, error);
}

static vectis_status
vectis_xml_stream_object(xmlTextReaderPtr reader, const lonejson_map *map,
                         const vectis_xml_config *config, lonejson *runtime,
                         void *out, size_t depth, vectis_error *error) {
  vectis_xml_field_state *states;
  vectis_string_builder attr_key;
  const lonejson_field *field;
  const char *name;
  const xmlChar *value;
  size_t i;
  size_t index;
  size_t open_index;
  int empty;
  int start_depth;
  int type;
  int rc;
  vectis_status status;

  if (depth > config->max_depth) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "XML nesting exceeds max_depth");
    return VECTIS_ERR_INVALID;
  }
  if (map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  states =
      (vectis_xml_field_state *)calloc(map->field_count, sizeof(states[0]));
  if (states == NULL && map->field_count > 0u) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate XML field state");
    return VECTIS_ERR_NOMEM;
  }
  for (i = 0u; i < map->field_count; ++i) {
    states[i].array = vectis_xml_field_is_array(&map->fields[i]);
  }
  attr_key.data = NULL;
  attr_key.size = 0u;
  attr_key.capacity = 0u;
  open_index = SIZE_MAX;
  empty = xmlTextReaderIsEmptyElement(reader);
  start_depth = xmlTextReaderDepth(reader);
  status = VECTIS_OK;
  if (xmlTextReaderMoveToFirstAttribute(reader) == 1) {
    do {
      name = vectis_xml_reader_local_name(reader);
      if (name == NULL) {
        continue;
      }
      field =
          vectis_xml_find_attribute_field(map, config, name, &attr_key, error);
      if (field == NULL) {
        if (error != NULL && error->code != VECTIS_OK) {
          status = error->code;
          goto cleanup;
        }
        if (!config->skip_unknown_disabled) {
          continue;
        }
        vectis_set_errorf(error, VECTIS_ERR_INVALID,
                          "unknown XML attribute '%s'", name);
        status = VECTIS_ERR_INVALID;
        goto cleanup;
      }
      index = (size_t)(field - map->fields);
      status = vectis_xml_begin_field(states, &open_index, index, field, error);
      if (status != VECTIS_OK) {
        goto cleanup;
      }
      value = xmlTextReaderConstValue(reader);
      if (vectis_xml_field_is_array(field)) {
        status = vectis_xml_set_array_scalar_field(
            field, (unsigned char *)out + field->struct_offset,
            value != NULL ? (const char *)value : "",
            value != NULL ? strlen((const char *)value) : 0u, config, error);
      } else {
        status = vectis_xml_set_lonejson_scalar_field(
            field, out, value != NULL ? (const char *)value : "",
            value != NULL ? strlen((const char *)value) : 0u, config, error);
      }
      if (status != VECTIS_OK) {
        goto cleanup;
      }
    } while (xmlTextReaderMoveToNextAttribute(reader) == 1);
    (void)xmlTextReaderMoveToElement(reader);
  }
  if (!empty) {
    for (;;) {
      rc = xmlTextReaderRead(reader);
      if (rc != 1) {
        vectis_set_error(error, VECTIS_ERR_INVALID,
                         rc == 0 ? "unexpected end of XML object"
                                 : "libxml2 failed while reading XML object");
        status = VECTIS_ERR_INVALID;
        goto cleanup;
      }
      type = xmlTextReaderNodeType(reader);
      if (type == XML_READER_TYPE_END_ELEMENT &&
          xmlTextReaderDepth(reader) == start_depth) {
        break;
      }
      if (type == XML_READER_TYPE_ELEMENT) {
        name = vectis_xml_reader_local_name(reader);
        field = vectis_xml_find_field(map, name);
        if (field == NULL) {
          if (config->skip_unknown_disabled) {
            vectis_set_errorf(error, VECTIS_ERR_INVALID,
                              "unknown XML element '%s'", name);
            status = VECTIS_ERR_INVALID;
            goto cleanup;
          }
          status = vectis_xml_skip_element(reader, error);
          if (status != VECTIS_OK) {
            goto cleanup;
          }
          continue;
        }
        index = (size_t)(field - map->fields);
        status =
            vectis_xml_begin_field(states, &open_index, index, field, error);
        if (status != VECTIS_OK) {
          goto cleanup;
        }
        status = vectis_xml_stream_field_value(reader, field, config, runtime,
                                               out, depth + 1u, error);
        if (status != VECTIS_OK) {
          goto cleanup;
        }
      } else if (type == XML_READER_TYPE_TEXT ||
                 type == XML_READER_TYPE_CDATA ||
                 type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE ||
                 type == XML_READER_TYPE_WHITESPACE) {
        value = xmlTextReaderConstValue(reader);
        if (value != NULL &&
            vectis_xml_span_has_nonspace((const char *)value,
                                         strlen((const char *)value))) {
          status = vectis_xml_stream_text_field(
              states, &open_index, map, config, out, (const char *)value,
              strlen((const char *)value), error);
          if (status != VECTIS_OK) {
            goto cleanup;
          }
        }
      }
    }
  }
  status = vectis_xml_finish_array(states, &open_index);
  if (status != VECTIS_OK) {
    goto cleanup;
  }
  for (i = 0u; i < map->field_count; ++i) {
    if ((map->fields[i].flags & LONEJSON_FIELD_REQUIRED) != 0u &&
        states[i].count == 0u) {
      vectis_set_errorf(error, VECTIS_ERR_INVALID,
                        "missing required XML field '%s'",
                        map->fields[i].json_key);
      status = VECTIS_ERR_INVALID;
      goto cleanup;
    }
  }

cleanup:
  vectis_string_builder_cleanup(&attr_key);
  free(states);
  return status;
}

static vectis_status vectis_xml_stream_document(xmlTextReaderPtr reader,
                                                const lonejson_map *map,
                                                const vectis_xml_config *config,
                                                lonejson *runtime, void *out,
                                                vectis_error *error) {
  const char *name;
  int rc;

  for (;;) {
    rc = xmlTextReaderRead(reader);
    if (rc != 1) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "XML document has no root element");
      return VECTIS_ERR_INVALID;
    }
    if (xmlTextReaderNodeType(reader) == XML_READER_TYPE_ELEMENT) {
      break;
    }
  }
  name = vectis_xml_reader_local_name(reader);
  if (config->root_element != NULL &&
      !vectis_xml_name_matches(name, config->root_element)) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "XML root element '%s' does not match expected '%s'",
                      name != NULL ? name : "", config->root_element);
    return VECTIS_ERR_INVALID;
  }
  return vectis_xml_stream_object(reader, map, config, runtime, out, 1u, error);
}

static vectis_status vectis_xml_open_reader(const vectis_source *source,
                                            xmlTextReaderPtr *out,
                                            vectis_xml_lc_reader *lc_reader,
                                            vectis_error *error) {
  int options;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "XML reader output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  if (source == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML source is required");
    return VECTIS_ERR_INVALID;
  }
  options = XML_PARSE_NONET | XML_PARSE_COMPACT | XML_PARSE_NOBLANKS;
  if (source->path != NULL) {
    *out = xmlReaderForFile(source->path, NULL, options);
  } else if (source->memory != NULL || source->memory_size > 0u) {
    if (source->memory == NULL) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "XML memory source data is required");
      return VECTIS_ERR_INVALID;
    }
    if (source->memory_size > (size_t)INT_MAX) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "XML memory source is too large for libxml2 reader");
      return VECTIS_ERR_INVALID;
    }
    *out = xmlReaderForMemory((const char *)source->memory,
                              (int)source->memory_size, NULL, NULL, options);
  } else if (source->source != NULL) {
    lc_reader->source = source->source;
    lc_reader->status = VECTIS_OK;
    lc_reader->has_error = 0;
    lc_error_init(&lc_reader->error);
    *out = xmlReaderForIO(vectis_xml_lc_read, NULL, lc_reader, NULL, NULL,
                          options);
  } else {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML source is empty");
    return VECTIS_ERR_INVALID;
  }
  if (*out == NULL) {
    if (lc_reader != NULL && lc_reader->has_error) {
      (void)vectis_source_error(error, lc_reader->error.code, &lc_reader->error,
                                "failed to read XML source");
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
    vectis_set_error(error, VECTIS_ERR_INVALID, "failed to open XML reader");
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

vectis_status vectis_xml_parse_lonejson_source(const vectis_source *source,
                                               const lonejson_map *map,
                                               const vectis_xml_config *config,
                                               void *out, vectis_error *error) {
  vectis_xml_config effective;
  vectis_source xml_source;
  vectis_xml_lc_reader lc_reader;
  xmlTextReaderPtr reader;
  lonejson *runtime;
  vectis_status status;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "XML lonejson output struct is required");
    return VECTIS_ERR_INVALID;
  }
  if (map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_xml_effective_config(config, &effective, error);
  if (status != VECTIS_OK) {
    return status;
  }
  xml_source = source != NULL ? *source : vectis_source_from_lc(NULL);
  reader = NULL;
  lc_reader.source = NULL;
  lc_reader.status = VECTIS_OK;
  lc_reader.has_error = 0;
  lc_error_init(&lc_reader.error);
  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    lc_error_cleanup(&lc_reader.error);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  status = vectis_xml_open_reader(&xml_source, &reader, &lc_reader, error);
  if (status != VECTIS_OK) {
    lonejson_free(runtime);
    lc_error_cleanup(&lc_reader.error);
    return status;
  }
  lonejson_init(runtime, map, out);
  status =
      vectis_xml_stream_document(reader, map, &effective, runtime, out, error);
  if (reader != NULL) {
    xmlFreeTextReader(reader);
  }
  if (lc_reader.has_error && status == VECTIS_OK) {
    status = vectis_source_error(error, lc_reader.error.code, &lc_reader.error,
                                 "failed to read XML source");
  }
  lc_error_cleanup(&lc_reader.error);
  if (status != VECTIS_OK) {
    lonejson_cleanup(map, out);
    lonejson_free(runtime);
    return status;
  }
  lonejson_free(runtime);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_xml_parse_lonejson(struct lc_source *source,
                                        const lonejson_map *map,
                                        const vectis_xml_config *config,
                                        void *out, vectis_error *error) {
  vectis_source wrapped_source;

  wrapped_source = vectis_source_from_lc(source);
  return vectis_xml_parse_lonejson_source(&wrapped_source, map, config, out,
                                          error);
}

vectis_status vectis_format_key(char *out, size_t out_size, vectis_error *error,
                                const char *format, ...) {
  va_list ap;
  int written;

  if (out == NULL || out_size == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "key output buffer is required");
    return VECTIS_ERR_INVALID;
  }
  out[0] = '\0';
  if (format == NULL || format[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "key format is required");
    return VECTIS_ERR_INVALID;
  }
  va_start(ap, format);
  written = vsnprintf(out, out_size, format, ap);
  va_end(ap);
  if (written < 0 || (size_t)written >= out_size) {
    out[0] = '\0';
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "formatted key exceeds output buffer");
    return VECTIS_ERR_INVALID;
  }
  if (strstr(out, "/../") != NULL || strstr(out, "/./") != NULL ||
      strcmp(out, "..") == 0 || strcmp(out, ".") == 0 ||
      strncmp(out, "../", 3u) == 0 || strncmp(out, "./", 2u) == 0 ||
      (strlen(out) >= 3u && strcmp(out + strlen(out) - 3u, "/..") == 0) ||
      (strlen(out) >= 2u && strcmp(out + strlen(out) - 2u, "/.") == 0)) {
    out[0] = '\0';
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "formatted key must not contain dot segments");
    return VECTIS_ERR_INVALID;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status
vectis_lockd_acquire_state(struct lc_client *client, const char *key,
                           const char *owner, long ttl_seconds,
                           struct lc_lease **out, vectis_error *error) {
  lc_acquire_req acquire;
  lc_error lcerr;
  int rc;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lease output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  if (client == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lockd client is required");
    return VECTIS_ERR_INVALID;
  }
  if (key == NULL || key[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lockd state key is required");
    return VECTIS_ERR_INVALID;
  }
  if (owner == NULL || owner[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "lockd state owner is required");
    return VECTIS_ERR_INVALID;
  }
  lc_acquire_req_init(&acquire);
  lc_error_init(&lcerr);
  acquire.key = key;
  acquire.owner = owner;
  acquire.ttl_seconds = ttl_seconds > 0L ? ttl_seconds : 30L;
  rc = lc_acquire(client, &acquire, out, &lcerr);
  if (rc != LC_OK) {
    (void)vectis_set_lockdc_error(error, rc, &lcerr,
                                  "failed to acquire lockd state");
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_lockd_state_load(struct lc_client *client, const char *key,
                                      const char *owner, long ttl_seconds,
                                      const lonejson_map *map, void *out,
                                      vectis_error *error) {
  struct lc_lease *lease;
  lc_release_req release;
  lc_get_res get_response;
  lc_error lcerr;
  int rc;
  vectis_status status;

  if (map == NULL || out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "lockd state load requires map and output");
    return VECTIS_ERR_INVALID;
  }
  lease = NULL;
  status = vectis_lockd_acquire_state(client, key, owner, ttl_seconds, &lease,
                                      error);
  if (status != VECTIS_OK) {
    return status;
  }
  lc_release_req_init(&release);
  memset(&get_response, 0, sizeof(get_response));
  lc_error_init(&lcerr);
  rc = lease->load(lease, map, out, NULL, &get_response, &lcerr);
  lc_get_res_cleanup(&get_response);
  if (rc == LC_OK) {
    rc = lease->release(lease, &release, &lcerr);
  }
  if (rc != LC_OK) {
    lc_lease_close(lease);
    (void)vectis_set_lockdc_error(error, rc, &lcerr,
                                  "failed to load lockd state");
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_lockd_state_save(struct lc_client *client, const char *key,
                                      const char *owner, long ttl_seconds,
                                      const lonejson_map *map,
                                      const void *value, vectis_error *error) {
  struct lc_lease *lease;
  lc_release_req release;
  lc_error lcerr;
  int rc;
  vectis_status status;

  if (map == NULL || value == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "lockd state save requires map and value");
    return VECTIS_ERR_INVALID;
  }
  lease = NULL;
  status = vectis_lockd_acquire_state(client, key, owner, ttl_seconds, &lease,
                                      error);
  if (status != VECTIS_OK) {
    return status;
  }
  lc_release_req_init(&release);
  lc_error_init(&lcerr);
  rc = lease->save(lease, map, value, &lcerr);
  if (rc == LC_OK) {
    rc = lease->release(lease, &release, &lcerr);
  }
  if (rc != LC_OK) {
    lc_lease_close(lease);
    (void)vectis_set_lockdc_error(error, rc, &lcerr,
                                  "failed to save lockd state");
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_lockd_state_update(struct lc_client *client,
                                        const char *key, const char *owner,
                                        long ttl_seconds,
                                        const lonejson_map *map, void *state,
                                        vectis_lockd_state_update_fn update,
                                        void *userdata, vectis_error *error) {
  struct lc_lease *lease;
  lc_release_req release;
  lc_get_res get_response;
  lc_error lcerr;
  int rc;
  int save;
  vectis_status status;

  if (map == NULL || state == NULL || update == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "lockd state update requires map, state, and callback");
    return VECTIS_ERR_INVALID;
  }
  lease = NULL;
  status = vectis_lockd_acquire_state(client, key, owner, ttl_seconds, &lease,
                                      error);
  if (status != VECTIS_OK) {
    return status;
  }
  lc_release_req_init(&release);
  memset(&get_response, 0, sizeof(get_response));
  lc_error_init(&lcerr);
  rc = lease->load(lease, map, state, NULL, &get_response, &lcerr);
  lc_get_res_cleanup(&get_response);
  if (rc == LC_OK) {
    save = 1;
    status = update(lease, state, &save, userdata, error);
    if (status != VECTIS_OK) {
      (void)lease->release(lease, &release, &lcerr);
      lc_lease_close(lease);
      lc_error_cleanup(&lcerr);
      return status;
    }
    if (save) {
      rc = lease->save(lease, map, state, &lcerr);
    }
  }
  if (rc == LC_OK) {
    rc = lease->release(lease, &release, &lcerr);
  }
  if (rc != LC_OK) {
    lc_lease_close(lease);
    (void)vectis_set_lockdc_error(error, rc, &lcerr,
                                  "failed to update lockd state");
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_request *vectis_internal_request_new(vectis_error *error) {
  vectis_request *request;

  request = (vectis_request *)calloc(1u, sizeof(*request));
  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate request");
    return NULL;
  }
  vectis_internal_request_init(request);
  vectis_error_clear(error);
  return request;
}

void vectis_internal_request_init(vectis_request *request) {
  if (request == NULL) {
    return;
  }
  memset(request, 0, sizeof(*request));
  request->method = VECTIS_HTTP_ANY;
}

void vectis_internal_request_cleanup(vectis_request *request) {
  if (request == NULL) {
    return;
  }
  vectis_kv_free_all(request->path_params, request->path_param_count);
  vectis_kv_free_all(request->query, request->query_count);
  vectis_kv_free_all(request->headers, request->header_count);
  vectis_request_close_body_reader(request);
  free(request->path);
  free(request->body_path);
  memset(request, 0, sizeof(*request));
  request->method = VECTIS_HTTP_ANY;
}

void vectis_internal_request_free(vectis_request *request) {
  if (request == NULL) {
    return;
  }
  vectis_internal_request_cleanup(request);
  free(request);
}

void vectis_internal_request_set_method(vectis_request *request,
                                        vectis_http_method method) {
  if (request == NULL) {
    return;
  }
  request->method = method;
}

vectis_status vectis_internal_request_set_path(vectis_request *request,
                                               const char *path,
                                               vectis_error *error) {
  char *copy;

  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request is required");
    return VECTIS_ERR_INVALID;
  }
  if (path == NULL || path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request path is required");
    return VECTIS_ERR_INVALID;
  }
  copy = vectis_strdup(path);
  if (copy == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy request path");
    return VECTIS_ERR_NOMEM;
  }
  free(request->path);
  request->path = copy;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_internal_request_set_body(vectis_request *request,
                                               const void *body,
                                               size_t body_size,
                                               vectis_error *error) {
  lc_error lcerr;
  lc_source *source;
  int rc;

  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request is required");
    return VECTIS_ERR_INVALID;
  }
  if (body == NULL && body_size > 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request body is invalid");
    return VECTIS_ERR_INVALID;
  }
  lc_error_init(&lcerr);
  source = NULL;
  rc = lc_source_from_memory(body, body_size, &source, &lcerr);
  if (rc != LC_OK) {
    (void)vectis_source_error(error, rc, &lcerr,
                              "failed to create request body reader");
    lc_error_cleanup(&lcerr);
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  free(request->body_path);
  request->body_path = NULL;
  request->body_spooled = 0;
  request->body_streaming_upload = 0;
  request->body.data = body;
  request->body.size = body_size;
  vectis_body_policy_init(&request->body_policy);
  vectis_request_close_body_reader(request);
  request->body_reader = source;
  request->owns_body_reader = 1;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_internal_request_set_body_path(vectis_request *request,
                                                    const char *body_path,
                                                    size_t body_size,
                                                    vectis_error *error) {
  char *path_copy;
  lc_error lcerr;
  lc_source *source;
  int rc;

  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request is required");
    return VECTIS_ERR_INVALID;
  }
  if (body_path == NULL || body_path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "request body path is required");
    return VECTIS_ERR_INVALID;
  }
  path_copy = vectis_strdup(body_path);
  if (path_copy == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy request body path");
    return VECTIS_ERR_NOMEM;
  }
  lc_error_init(&lcerr);
  source = NULL;
  rc = lc_source_from_file(body_path, &source, &lcerr);
  if (rc != LC_OK) {
    free(path_copy);
    (void)vectis_source_error(error, rc, &lcerr,
                              "failed to create request body file reader");
    lc_error_cleanup(&lcerr);
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  free(request->body_path);
  request->body_path = path_copy;
  request->body_spooled = 1;
  request->body_streaming_upload = 0;
  request->body.data = NULL;
  request->body.size = body_size;
  vectis_body_policy_init(&request->body_policy);
  vectis_request_close_body_reader(request);
  request->body_reader = source;
  request->owns_body_reader = 1;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_internal_request_set_body_reader(
    vectis_request *request, struct lc_source *source, size_t body_size,
    int owned, const vectis_body_policy *policy, vectis_error *error) {
  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request is required");
    return VECTIS_ERR_INVALID;
  }
  if (source == NULL && body_size > 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "request body reader is required");
    return VECTIS_ERR_INVALID;
  }
  free(request->body_path);
  request->body_path = NULL;
  request->body_spooled = 0;
  request->body_streaming_upload = 0;
  request->body.data = NULL;
  request->body.size = body_size;
  vectis_request_close_body_reader(request);
  request->body_reader = source;
  request->owns_body_reader = owned ? 1 : 0;
  if (policy != NULL) {
    request->body_policy = *policy;
  } else {
    vectis_body_policy_init(&request->body_policy);
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

void vectis_internal_request_set_streaming_upload(
    vectis_request *request, const vectis_body_policy *policy) {
  if (request == NULL) {
    return;
  }
  free(request->body_path);
  request->body_path = NULL;
  request->body_spooled = 0;
  request->body_streaming_upload = 1;
  request->body.data = NULL;
  request->body.size = 0u;
  vectis_request_close_body_reader(request);
  if (policy != NULL) {
    request->body_policy = *policy;
  } else {
    vectis_body_policy_init(&request->body_policy);
  }
}

void vectis_internal_request_set_kore(vectis_request *request,
                                      struct http_request *kore_request) {
  if (request == NULL) {
    return;
  }
  request->kore_request = kore_request;
}

vectis_status vectis_internal_request_add_path_param(vectis_request *request,
                                                     const char *name,
                                                     const char *value,
                                                     vectis_error *error) {
  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_kv_add(&request->path_params, &request->path_param_count,
                       &request->path_param_capacity, name, value,
                       "path parameter", error);
}

vectis_status vectis_internal_request_add_query(vectis_request *request,
                                                const char *name,
                                                const char *value,
                                                vectis_error *error) {
  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_kv_add(&request->query, &request->query_count,
                       &request->query_capacity, name, value, "query parameter",
                       error);
}

vectis_status vectis_internal_request_add_header(vectis_request *request,
                                                 const char *name,
                                                 const char *value,
                                                 vectis_error *error) {
  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_kv_add(&request->headers, &request->header_count,
                       &request->header_capacity, name, value, "request header",
                       error);
}

vectis_response *vectis_internal_response_new(vectis_error *error) {
  vectis_response *response;

  response = (vectis_response *)calloc(1u, sizeof(*response));
  if (response == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate response");
    return NULL;
  }
  vectis_internal_response_init(response);
  vectis_error_clear(error);
  return response;
}

void vectis_internal_response_init(vectis_response *response) {
  if (response == NULL) {
    return;
  }
  memset(response, 0, sizeof(*response));
}

void vectis_internal_response_cleanup(vectis_response *response) {
  if (response == NULL) {
    return;
  }
  free(response->content_type);
  free(response->body);
  if (response->file_path_temporary && response->file_path != NULL) {
    (void)unlink(response->file_path);
  }
  free(response->file_path);
  vectis_kv_free_all(response->headers, response->header_count);
  memset(response, 0, sizeof(*response));
}

static void vectis_response_clear_payload(vectis_response *response) {
  if (response == NULL) {
    return;
  }
  free(response->content_type);
  response->content_type = NULL;
  free(response->body);
  response->body = NULL;
  response->body_size = 0u;
  if (response->file_path_temporary && response->file_path != NULL) {
    (void)unlink(response->file_path);
  }
  free(response->file_path);
  response->file_path = NULL;
  response->file_path_temporary = 0;
}

void vectis_internal_response_free(vectis_response *response) {
  if (response == NULL) {
    return;
  }
  vectis_internal_response_cleanup(response);
  free(response);
}

int vectis_internal_response_status_code(const vectis_response *response) {
  return response != NULL ? response->status_code : 0;
}

const char *
vectis_internal_response_content_type(const vectis_response *response) {
  return response != NULL ? response->content_type : NULL;
}

vectis_bytes vectis_internal_response_body(const vectis_response *response) {
  vectis_bytes body;

  body.data = NULL;
  body.size = 0u;
  if (response != NULL) {
    body.data = response->body;
    body.size = response->body_size;
  }
  return body;
}

const char *
vectis_internal_response_file_path(const vectis_response *response) {
  return response != NULL ? response->file_path : NULL;
}

int vectis_internal_response_file_temporary(const vectis_response *response) {
  return response != NULL && response->file_path_temporary;
}

size_t vectis_internal_response_header_count(const vectis_response *response) {
  return response != NULL ? response->header_count : 0u;
}

const char *
vectis_internal_response_header_name(const vectis_response *response,
                                     size_t index) {
  if (response == NULL || index >= response->header_count) {
    return NULL;
  }
  return response->headers[index].name;
}

const char *
vectis_internal_response_header_value(const vectis_response *response,
                                      size_t index) {
  if (response == NULL || index >= response->header_count) {
    return NULL;
  }
  return response->headers[index].value;
}

vectis_status vectis_request_json_into(vectis_request *request,
                                       const lonejson_map *map, void *out,
                                       vectis_error *error) {
  lonejson_error json_error;
  lonejson_status json_status;
  lonejson_curl_parse parse;
  lonejson *runtime;
  lc_error lcerr;
  unsigned char chunk[8192];
  size_t nread;
  size_t accepted;

  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request is required");
    return VECTIS_ERR_INVALID;
  }
  if (map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "json output struct is required");
    return VECTIS_ERR_INVALID;
  }
  if (request->body_streaming_upload) {
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "streaming upload body is only available through the upload reader");
    return VECTIS_ERR_INVALID;
  }
  if (request->body_reader == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "request body reader is not available");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_request_reset_body_reader(request, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  json_status = lonejson_curl_parse_init(&parse, runtime, map, out);
  if (json_status != LONEJSON_STATUS_OK) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to initialize request JSON parser: %s",
                      parse.error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
      error->dependency_code = (long)json_status;
    }
    lonejson_free(runtime);
    return VECTIS_ERR_INVALID;
  }
  lc_error_init(&lcerr);
  for (;;) {
    nread = request->body_reader->read(request->body_reader, chunk,
                                       sizeof(chunk), &lcerr);
    if (nread == 0u) {
      if (lcerr.code != 0) {
        (void)vectis_source_error(error, lcerr.code, &lcerr,
                                  "failed to read request body");
        lonejson_curl_parse_cleanup(&parse);
        lonejson_free(runtime);
        lc_error_cleanup(&lcerr);
        return error != NULL ? error->code : VECTIS_ERR_STATE;
      }
      break;
    }
    accepted = lonejson_curl_write_callback((char *)chunk, 1u, nread, &parse);
    if (accepted != nread) {
      json_error = parse.error;
      vectis_set_errorf(
          error, VECTIS_ERR_INVALID,
          "failed to parse request JSON at line %lu column %lu: %s",
          (unsigned long)json_error.line, (unsigned long)json_error.column,
          json_error.message);
      if (error != NULL) {
        error->source = VECTIS_ERROR_SOURCE_LONEJSON;
        error->dependency_code = (long)LONEJSON_STATUS_INVALID_JSON;
      }
      lonejson_curl_parse_cleanup(&parse);
      lonejson_free(runtime);
      lc_error_cleanup(&lcerr);
      return VECTIS_ERR_INVALID;
    }
  }
  lc_error_cleanup(&lcerr);
  json_status = lonejson_curl_parse_finish(&parse);
  if (json_status != LONEJSON_STATUS_OK) {
    json_error = parse.error;
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to parse request JSON at line %lu column %lu: %s",
                      (unsigned long)json_error.line,
                      (unsigned long)json_error.column, json_error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
      error->dependency_code = (long)json_status;
    }
    lonejson_curl_parse_cleanup(&parse);
    lonejson_free(runtime);
    return VECTIS_ERR_INVALID;
  }
  lonejson_curl_parse_cleanup(&parse);
  lonejson_free(runtime);
  (void)vectis_request_reset_body_reader(request, NULL);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status
vectis_request_json_array_each(vectis_request *request, const char *array_path,
                               const lonejson_map *map, void *item,
                               vectis_json_array_item_fn callback,
                               void *userdata, vectis_error *error) {
  vectis_source source;
  vectis_status status;

  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request is required");
    return VECTIS_ERR_INVALID;
  }
  if (request->body_reader == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "request body reader is not available");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_request_reset_body_reader(request, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  source = vectis_source_from_lc(request->body_reader);
  status = vectis_json_array_each_source(&source, array_path, map, item,
                                         callback, userdata, error);
  (void)vectis_request_reset_body_reader(request, NULL);
  return status;
}

vectis_http_method vectis_request_method(vectis_request *request) {
  if (request == NULL) {
    return VECTIS_HTTP_ANY;
  }
  return request->method;
}

const char *vectis_request_path(vectis_request *request) {
  if (request == NULL) {
    return NULL;
  }
  return request->path;
}

const char *vectis_request_path_param(vectis_request *request,
                                      const char *name) {
  if (request == NULL) {
    return NULL;
  }
  return vectis_kv_find(request->path_params, request->path_param_count, name);
}

const char *vectis_request_query(vectis_request *request, const char *name) {
  if (request == NULL) {
    return NULL;
  }
  return vectis_kv_find(request->query, request->query_count, name);
}

const char *vectis_request_header(vectis_request *request, const char *name) {
  size_t i;

  if (request == NULL) {
    return NULL;
  }
  if (name == NULL) {
    return NULL;
  }
  for (i = 0u; i < request->header_count; ++i) {
    if (request->headers[i].name != NULL &&
        strcasecmp(request->headers[i].name, name) == 0) {
      return request->headers[i].value;
    }
  }
  return NULL;
}

struct http_request *vectis_request_kore(vectis_request *request) {
  if (request == NULL) {
    return NULL;
  }
  return request->kore_request;
}

struct lc_source *vectis_request_body_reader(vectis_request *request) {
  if (request == NULL || request->body_streaming_upload) {
    return NULL;
  }
  return request->body_reader;
}

void vectis_body_spill_config_init(vectis_body_spill_config *config) {
  if (config == NULL) {
    return;
  }
  config->memory_limit_bytes = VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
  config->directory = NULL;
  config->prefix = NULL;
}

void vectis_body_materialize_config_init(
    vectis_body_materialize_config *config) {
  if (config == NULL) {
    return;
  }
  config->buffer = NULL;
  config->buffer_size = 0u;
  config->memory_limit_bytes = VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
  config->directory = NULL;
  config->prefix = NULL;
}

void vectis_body_materialized_cleanup(vectis_body_materialized *body) {
  if (body == NULL) {
    return;
  }
  if (body->owns_memory) {
    free(body->memory.data);
  }
  free(body->path);
  body->kind = VECTIS_BODY_MATERIALIZED_NONE;
  body->memory.data = NULL;
  body->memory.size = 0u;
  body->path = NULL;
  body->size = 0u;
  body->owns_memory = 0;
}

vectis_status
vectis_body_materialized_open_reader(const vectis_body_materialized *body,
                                     struct lc_source **out,
                                     vectis_error *error) {
  lc_error lcerr;
  int rc;

  if (body == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "materialized body is required");
    return VECTIS_ERR_INVALID;
  }
  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "body reader output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  lc_error_init(&lcerr);
  if (body->kind == VECTIS_BODY_MATERIALIZED_MEMORY) {
    rc = lc_source_from_memory(body->memory.data, body->memory.size, out,
                               &lcerr);
  } else if (body->kind == VECTIS_BODY_MATERIALIZED_FILE) {
    rc = lc_source_from_file(body->path, out, &lcerr);
  } else {
    lc_error_cleanup(&lcerr);
    vectis_set_error(error, VECTIS_ERR_INVALID, "materialized body is empty");
    return VECTIS_ERR_INVALID;
  }
  if (rc != LC_OK) {
    (void)vectis_source_error(error, rc, &lcerr,
                              "failed to open materialized body reader");
    lc_error_cleanup(&lcerr);
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

void vectis_body_spill_result_cleanup(vectis_body_spill_result *result) {
  if (result == NULL) {
    return;
  }
  vectis_mutable_bytes_cleanup(&result->memory);
  free(result->path);
  result->path = NULL;
  result->size = 0u;
  result->spooled_to_disk = 0;
}

vectis_status vectis_request_body_materialize(
    vectis_request *request, const vectis_body_materialize_config *config,
    vectis_body_materialized *out, vectis_error *error) {
  unsigned char chunk[8192];
  lc_error lcerr;
  FILE *fp;
  char tmp_template[PATH_MAX];
  char *path;
  void *next;
  unsigned char *memory;
  unsigned char *fixed_buffer;
  size_t memory_size;
  size_t memory_capacity;
  size_t memory_limit;
  size_t fixed_buffer_size;
  size_t nread;
  size_t total;
  int fd;
  int spooled;

  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request is required");
    return VECTIS_ERR_INVALID;
  }
  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "materialized request body output is required");
    return VECTIS_ERR_INVALID;
  }
  if (request->body_streaming_upload) {
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "streaming upload body is only available through the upload reader");
    return VECTIS_ERR_INVALID;
  }
  if (request->body_reader == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "request body reader is not available");
    return VECTIS_ERR_INVALID;
  }
  out->kind = VECTIS_BODY_MATERIALIZED_NONE;
  out->memory.data = NULL;
  out->memory.size = 0u;
  out->path = NULL;
  out->size = 0u;
  out->owns_memory = 0;
  memory_limit = config != NULL ? config->memory_limit_bytes : 0u;
  fixed_buffer = config != NULL ? config->buffer : NULL;
  fixed_buffer_size = config != NULL ? config->buffer_size : 0u;
  if (fixed_buffer == NULL) {
    fixed_buffer_size = 0u;
  }
  if (fixed_buffer_size > 0u &&
      (memory_limit == 0u || memory_limit > fixed_buffer_size)) {
    memory_limit = fixed_buffer_size;
  }
  if (memory_limit == 0u) {
    memory_limit = request->body_policy.memory_buffer_limit_bytes;
  }
  if (memory_limit == 0u) {
    memory_limit = VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
  }
  if (!vectis_tmp_template(tmp_template, sizeof(tmp_template), config)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "request body materialization path is too long");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_request_reset_body_reader(request, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  memory = NULL;
  memory_size = 0u;
  memory_capacity = 0u;
  fp = NULL;
  path = NULL;
  fd = -1;
  total = 0u;
  spooled = 0;
  if (fixed_buffer_size > 0u) {
    memory = fixed_buffer;
    memory_capacity = fixed_buffer_size;
  }
  lc_error_init(&lcerr);
  for (;;) {
    nread = request->body_reader->read(request->body_reader, chunk,
                                       sizeof(chunk), &lcerr);
    if (nread == 0u) {
      if (lcerr.code != 0) {
        (void)vectis_source_error(error, lcerr.code, &lcerr,
                                  "failed to read request body");
        lc_error_cleanup(&lcerr);
        if (fp != NULL) {
          (void)fclose(fp);
        } else if (fd >= 0) {
          (void)close(fd);
        }
        if (path != NULL) {
          (void)unlink(path);
        }
        free(path);
        if (memory != fixed_buffer) {
          free(memory);
        }
        return error != NULL ? error->code : VECTIS_ERR_STATE;
      }
      break;
    }
    if ((size_t)-1 - total < nread) {
      vectis_set_error(error, VECTIS_ERR_STATE, "request body size overflow");
      lc_error_cleanup(&lcerr);
      if (fp != NULL) {
        (void)fclose(fp);
      }
      if (path != NULL) {
        (void)unlink(path);
      }
      free(path);
      if (memory != fixed_buffer) {
        free(memory);
      }
      return VECTIS_ERR_STATE;
    }
    if (!spooled && total + nread > memory_limit) {
      fd = mkstemp(tmp_template);
      if (fd < 0) {
        vectis_set_error(error, VECTIS_ERR_STATE,
                         "failed to create request body materialization file");
        lc_error_cleanup(&lcerr);
        if (memory != fixed_buffer) {
          free(memory);
        }
        return VECTIS_ERR_STATE;
      }
      path = vectis_strdup(tmp_template);
      if (path == NULL) {
        (void)close(fd);
        (void)unlink(tmp_template);
        vectis_set_error(error, VECTIS_ERR_NOMEM,
                         "failed to copy request body materialization path");
        lc_error_cleanup(&lcerr);
        if (memory != fixed_buffer) {
          free(memory);
        }
        return VECTIS_ERR_NOMEM;
      }
      fp = fdopen(fd, "wb");
      if (fp == NULL) {
        (void)close(fd);
        (void)unlink(path);
        free(path);
        if (memory != fixed_buffer) {
          free(memory);
        }
        vectis_set_error(error, VECTIS_ERR_STATE,
                         "failed to open request body materialization file");
        lc_error_cleanup(&lcerr);
        return VECTIS_ERR_STATE;
      }
      fd = -1;
      if (memory_size > 0u &&
          fwrite(memory, 1u, memory_size, fp) != memory_size) {
        (void)fclose(fp);
        (void)unlink(path);
        free(path);
        if (memory != fixed_buffer) {
          free(memory);
        }
        vectis_set_error(error, VECTIS_ERR_STATE,
                         "failed to write request body materialization file");
        lc_error_cleanup(&lcerr);
        return VECTIS_ERR_STATE;
      }
      if (memory != fixed_buffer) {
        free(memory);
      }
      memory = NULL;
      memory_size = 0u;
      memory_capacity = 0u;
      spooled = 1;
    }
    if (spooled) {
      if (fwrite(chunk, 1u, nread, fp) != nread) {
        (void)fclose(fp);
        (void)unlink(path);
        free(path);
        vectis_set_error(error, VECTIS_ERR_STATE,
                         "failed to write request body materialization file");
        lc_error_cleanup(&lcerr);
        return VECTIS_ERR_STATE;
      }
    } else if (nread > 0u) {
      if (memory_size + nread > memory_capacity) {
        memory_capacity = memory_capacity == 0u ? nread : memory_capacity;
        while (memory_capacity < memory_size + nread) {
          if (memory_capacity > ((size_t)-1 / 2u)) {
            memory_capacity = memory_size + nread;
            break;
          }
          memory_capacity *= 2u;
        }
        next = memory == fixed_buffer ? malloc(memory_capacity)
                                      : realloc(memory, memory_capacity);
        if (next == NULL) {
          if (memory != fixed_buffer) {
            free(memory);
          }
          vectis_set_error(error, VECTIS_ERR_NOMEM,
                           "failed to allocate request body memory");
          lc_error_cleanup(&lcerr);
          return VECTIS_ERR_NOMEM;
        }
        if (memory == fixed_buffer && memory_size > 0u) {
          memcpy(next, fixed_buffer, memory_size);
        }
        memory = (unsigned char *)next;
      }
      memcpy(memory + memory_size, chunk, nread);
      memory_size += nread;
    }
    total += nread;
  }
  lc_error_cleanup(&lcerr);
  if (fp != NULL && fclose(fp) != 0) {
    (void)unlink(path);
    free(path);
    if (memory != fixed_buffer) {
      free(memory);
    }
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to close request body materialization file");
    return VECTIS_ERR_STATE;
  }
  (void)vectis_request_reset_body_reader(request, NULL);
  out->size = total;
  if (spooled) {
    out->kind = VECTIS_BODY_MATERIALIZED_FILE;
    out->path = path;
  } else {
    out->kind = VECTIS_BODY_MATERIALIZED_MEMORY;
    out->memory.data = memory;
    out->memory.size = memory_size;
    out->owns_memory = memory != fixed_buffer;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_request_body_spill(vectis_request *request,
                                        const vectis_body_spill_config *config,
                                        vectis_body_spill_result *out,
                                        vectis_error *error) {
  vectis_body_materialize_config materialize_config;
  vectis_body_materialized body;
  vectis_status status;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "request body spill output is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_body_materialize_config_init(&materialize_config);
  if (config != NULL) {
    materialize_config.memory_limit_bytes = config->memory_limit_bytes;
    materialize_config.directory = config->directory;
    materialize_config.prefix = config->prefix;
  }
  status = vectis_request_body_materialize(request, &materialize_config, &body,
                                           error);
  if (status != VECTIS_OK) {
    return status;
  }
  out->memory.data = NULL;
  out->memory.size = 0u;
  out->path = NULL;
  out->size = body.size;
  out->spooled_to_disk = body.kind == VECTIS_BODY_MATERIALIZED_FILE;
  if (body.kind == VECTIS_BODY_MATERIALIZED_FILE) {
    out->path = body.path;
    body.path = NULL;
  } else {
    out->memory.data = body.memory.data;
    out->memory.size = body.memory.size;
    body.memory.data = NULL;
    body.memory.size = 0u;
    body.owns_memory = 0;
  }
  vectis_body_materialized_cleanup(&body);
  return VECTIS_OK;
}

vectis_status vectis_request_body_read_all(vectis_request *request,
                                           vectis_mutable_bytes *out,
                                           vectis_error *error) {
  vectis_body_materialize_config config;
  vectis_body_materialized body;
  vectis_status status;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "request body output is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_body_materialize_config_init(&config);
  config.memory_limit_bytes = (size_t)-1;
  status = vectis_request_body_materialize(request, &config, &body, error);
  if (status != VECTIS_OK) {
    return status;
  }
  if (body.kind != VECTIS_BODY_MATERIALIZED_MEMORY ||
      (body.memory.size > 0u && !body.owns_memory)) {
    vectis_body_materialized_cleanup(&body);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "request body unexpectedly spooled while reading all");
    return VECTIS_ERR_STATE;
  }
  out->data = body.memory.data;
  out->size = body.memory.size;
  body.memory.data = NULL;
  body.memory.size = 0u;
  body.owns_memory = 0;
  vectis_body_materialized_cleanup(&body);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_request_body_bytes(vectis_request *request,
                                        vectis_bytes *out,
                                        vectis_error *error) {
  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request is required");
    return VECTIS_ERR_INVALID;
  }
  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "request body output is required");
    return VECTIS_ERR_INVALID;
  }
  if (request->body_streaming_upload) {
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "streaming upload body is only available through the upload reader");
    return VECTIS_ERR_INVALID;
  }
  if (request->body.data == NULL && request->body.size > 0u) {
    vectis_set_error(
        error, VECTIS_ERR_STATE,
        "request body is reader-backed; use vectis_request_body_reader or "
        "vectis_request_body_read_all");
    return VECTIS_ERR_STATE;
  }
  *out = request->body;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_request_body_copy(vectis_request *request,
                                       vectis_mutable_bytes *out,
                                       vectis_error *error) {
  return vectis_request_body_read_all(request, out, error);
}

void vectis_mutable_bytes_cleanup(vectis_mutable_bytes *bytes) {
  if (bytes == NULL) {
    return;
  }
  free(bytes->data);
  bytes->data = NULL;
  bytes->size = 0u;
}

const char *vectis_request_body_path(vectis_request *request) {
  if (request == NULL || request->body_streaming_upload ||
      !request->body_spooled) {
    return NULL;
  }
  return request->body_path;
}

int vectis_request_body_is_spooled(vectis_request *request) {
  return request != NULL && !request->body_streaming_upload &&
         request->body_spooled;
}

vectis_status vectis_response_status(vectis_response *response, int status_code,
                                     vectis_error *error) {
  if (response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response is required");
    return VECTIS_ERR_INVALID;
  }
  if (status_code < 100 || status_code > 599) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP status code is invalid");
    return VECTIS_ERR_INVALID;
  }
  vectis_response_clear_payload(response);
  response->status_code = status_code;
  response->sent = 1;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_response_header(vectis_response *response,
                                     const char *name, const char *value,
                                     vectis_error *error) {
  if (response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response is required");
    return VECTIS_ERR_INVALID;
  }
  if (!vectis_header_name_valid(name)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "response header name is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (!vectis_header_value_valid(value)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "response header value is invalid");
    return VECTIS_ERR_INVALID;
  }
  return vectis_kv_add(&response->headers, &response->header_count,
                       &response->header_capacity, name, value,
                       "response header", error);
}

vectis_status vectis_response_text(vectis_response *response, int status_code,
                                   const char *content_type, const char *text,
                                   vectis_error *error) {
  vectis_bytes body;

  if (response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response is required");
    return VECTIS_ERR_INVALID;
  }
  if (status_code < 100 || status_code > 599) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP status code is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (content_type == NULL || content_type[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "response content_type is required");
    return VECTIS_ERR_INVALID;
  }
  if (text == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response text is required");
    return VECTIS_ERR_INVALID;
  }
  body.data = text;
  body.size = strlen(text);
  return vectis_response_bytes(response, status_code, content_type, body,
                               error);
}

vectis_status vectis_response_bytes(vectis_response *response, int status_code,
                                    const char *content_type, vectis_bytes body,
                                    vectis_error *error) {
  char *content_type_copy;
  void *body_copy;

  if (response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response is required");
    return VECTIS_ERR_INVALID;
  }
  if (status_code < 100 || status_code > 599) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP status code is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (content_type == NULL || content_type[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "response content_type is required");
    return VECTIS_ERR_INVALID;
  }
  if (body.data == NULL && body.size > 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response body is invalid");
    return VECTIS_ERR_INVALID;
  }
  content_type_copy = vectis_strdup(content_type);
  if (content_type_copy == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy response content type");
    return VECTIS_ERR_NOMEM;
  }
  body_copy = NULL;
  if (body.size > 0u) {
    body_copy = malloc(body.size);
    if (body_copy == NULL) {
      free(content_type_copy);
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy response body");
      return VECTIS_ERR_NOMEM;
    }
    memcpy(body_copy, body.data, body.size);
  }
  vectis_response_clear_payload(response);
  response->status_code = status_code;
  response->content_type = content_type_copy;
  response->body = body_copy;
  response->body_size = body.size;
  response->sent = 1;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_response_file(vectis_response *response, int status_code,
                                   const char *content_type, const char *path,
                                   vectis_error *error) {
  char *content_type_copy;
  char *path_copy;

  if (response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response is required");
    return VECTIS_ERR_INVALID;
  }
  if (status_code < 100 || status_code > 599) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP status code is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (content_type == NULL || content_type[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "response content_type is required");
    return VECTIS_ERR_INVALID;
  }
  if (path == NULL || path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "response file path is required");
    return VECTIS_ERR_INVALID;
  }
  content_type_copy = vectis_strdup(content_type);
  if (content_type_copy == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy response content type");
    return VECTIS_ERR_NOMEM;
  }
  path_copy = vectis_strdup(path);
  if (path_copy == NULL) {
    free(content_type_copy);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy response file path");
    return VECTIS_ERR_NOMEM;
  }
  vectis_response_clear_payload(response);
  response->status_code = status_code;
  response->content_type = content_type_copy;
  response->body = NULL;
  response->body_size = 0u;
  response->file_path = path_copy;
  response->file_path_temporary = 0;
  response->sent = 1;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status vectis_response_file_owned(vectis_response *response,
                                                int status_code,
                                                const char *content_type,
                                                char *path, int temporary,
                                                vectis_error *error) {
  char *content_type_copy;

  if (response == NULL) {
    free(path);
    vectis_set_error(error, VECTIS_ERR_INVALID, "response is required");
    return VECTIS_ERR_INVALID;
  }
  if (status_code < 100 || status_code > 599) {
    free(path);
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP status code is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (content_type == NULL || content_type[0] == '\0') {
    free(path);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "response content type is required");
    return VECTIS_ERR_INVALID;
  }
  if (path == NULL || path[0] == '\0') {
    free(path);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "response file path is required");
    return VECTIS_ERR_INVALID;
  }
  content_type_copy = vectis_strdup(content_type);
  if (content_type_copy == NULL) {
    if (temporary) {
      (void)unlink(path);
    }
    free(path);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy response content type");
    return VECTIS_ERR_NOMEM;
  }
  vectis_response_clear_payload(response);
  response->status_code = status_code;
  response->content_type = content_type_copy;
  response->body = NULL;
  response->body_size = 0u;
  response->file_path = path;
  response->file_path_temporary = temporary ? 1 : 0;
  response->sent = 1;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status vectis_create_response_temp_file(FILE **out_fp,
                                                      char **out_path,
                                                      vectis_error *error) {
  char tmp_template[PATH_MAX];
  int fd;
  FILE *fp;
  char *path;

  if (out_fp == NULL || out_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "temp file outputs are required");
    return VECTIS_ERR_INVALID;
  }
  *out_fp = NULL;
  *out_path = NULL;
  if (!vectis_response_tmp_template(tmp_template, sizeof(tmp_template))) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "response temp path is too long");
    return VECTIS_ERR_INVALID;
  }
  fd = mkstemp(tmp_template);
  if (fd < 0) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to create response temp file");
    return VECTIS_ERR_STATE;
  }
  path = vectis_strdup(tmp_template);
  if (path == NULL) {
    (void)close(fd);
    (void)unlink(tmp_template);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy response temp path");
    return VECTIS_ERR_NOMEM;
  }
  fp = fdopen(fd, "wb");
  if (fp == NULL) {
    (void)close(fd);
    (void)unlink(path);
    free(path);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to open response temp file");
    return VECTIS_ERR_STATE;
  }
  *out_fp = fp;
  *out_path = path;
  return VECTIS_OK;
}

vectis_status vectis_response_source(vectis_response *response, int status_code,
                                     const char *content_type,
                                     struct lc_source *source,
                                     vectis_error *error) {
  unsigned char buffer[8192];
  lc_error lcerr;
  FILE *fp;
  char *path;
  size_t nread;

  if (source == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response source is required");
    return VECTIS_ERR_INVALID;
  }
  fp = NULL;
  path = NULL;
  if (vectis_create_response_temp_file(&fp, &path, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  if (source->reset != NULL) {
    lc_error_init(&lcerr);
    if (source->reset(source, &lcerr) != LC_OK) {
      (void)fclose(fp);
      (void)unlink(path);
      free(path);
      (void)vectis_source_error(error, lcerr.code, &lcerr,
                                "failed to reset response source");
      lc_error_cleanup(&lcerr);
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
    lc_error_cleanup(&lcerr);
  }
  lc_error_init(&lcerr);
  for (;;) {
    nread = source->read(source, buffer, sizeof(buffer), &lcerr);
    if (nread == 0u) {
      if (lcerr.code != 0) {
        (void)fclose(fp);
        (void)unlink(path);
        free(path);
        (void)vectis_source_error(error, lcerr.code, &lcerr,
                                  "failed to read response source");
        lc_error_cleanup(&lcerr);
        return error != NULL ? error->code : VECTIS_ERR_STATE;
      }
      break;
    }
    if (fwrite(buffer, 1u, nread, fp) != nread) {
      (void)fclose(fp);
      (void)unlink(path);
      free(path);
      lc_error_cleanup(&lcerr);
      vectis_set_error(error, VECTIS_ERR_STATE,
                       "failed to write response temp file");
      return VECTIS_ERR_STATE;
    }
  }
  lc_error_cleanup(&lcerr);
  if (fclose(fp) != 0) {
    (void)unlink(path);
    free(path);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to close response temp file");
    return VECTIS_ERR_STATE;
  }
  return vectis_response_file_owned(response, status_code, content_type, path,
                                    1, error);
}

static lonejson_status vectis_lonejson_file_sink(void *user, const void *data,
                                                 size_t length,
                                                 lonejson_error *error) {
  vectis_file_sink *sink;

  (void)error;
  sink = (vectis_file_sink *)user;
  if (sink == NULL || sink->fp == NULL || (data == NULL && length > 0u)) {
    return LONEJSON_STATUS_INVALID_ARGUMENT;
  }
  if (length > 0u && fwrite(data, 1u, length, sink->fp) != length) {
    return LONEJSON_STATUS_IO_ERROR;
  }
  return LONEJSON_STATUS_OK;
}

typedef struct vectis_http_json_array_stream {
  lonejson *runtime;
  lonejson_array_stream *stream;
  const lonejson_map *map;
  void *item;
  vectis_json_array_callback_state callback;
  lonejson_error json_error;
  lonejson_status json_status;
} vectis_http_json_array_stream;

static lonejson_status vectis_http_json_array_stream_item(void *user,
                                                          void *dst) {
  vectis_http_json_array_stream *stream;
  vectis_status status;

  stream = (vectis_http_json_array_stream *)user;
  if (stream == NULL || stream->callback.callback == NULL) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  status = stream->callback.callback(stream->callback.userdata,
                                     stream->callback.index, dst,
                                     stream->callback.error);
  if (status != VECTIS_OK) {
    stream->callback.status = status;
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  ++stream->callback.index;
  return LONEJSON_STATUS_OK;
}

static vectis_status vectis_http_json_array_stream_body(const void *data,
                                                        size_t size,
                                                        void *userdata,
                                                        vectis_error *error) {
  vectis_http_json_array_stream *stream;

  stream = (vectis_http_json_array_stream *)userdata;
  if (stream == NULL || stream->stream == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "JSON array stream is not initialized");
    return VECTIS_ERR_INVALID;
  }
  stream->callback.error = error;
  stream->json_status = lonejson_array_stream_push(
      stream->stream, stream->map, stream->item, data, size,
      vectis_http_json_array_stream_item, stream, &stream->json_error);
  if (stream->json_status != LONEJSON_STATUS_OK) {
    if (stream->callback.status != VECTIS_OK) {
      return stream->callback.status;
    }
    return vectis_set_lonejson_error(error, stream->json_status,
                                     &stream->json_error,
                                     "failed to stream HTTP JSON array");
  }
  return VECTIS_OK;
}

vectis_status vectis_response_json(vectis_response *response, int status_code,
                                   const lonejson_map *map, const void *value,
                                   vectis_error *error) {
  lonejson_error json_error;
  lonejson *runtime;
  char *json;
  size_t json_size;
  vectis_bytes body;
  vectis_status status;

  if (response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response is required");
    return VECTIS_ERR_INVALID;
  }
  if (status_code < 100 || status_code > 599) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP status code is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  if (value == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "json response value is required");
    return VECTIS_ERR_INVALID;
  }
  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  json = lonejson_serialize_alloc(runtime, map, value, &json_size, &json_error);
  lonejson_free(runtime);
  if (json == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to serialize response JSON: %s",
                      json_error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
    }
    return VECTIS_ERR_INVALID;
  }
  body.data = json;
  body.size = json_size;
  status = vectis_response_bytes(response, status_code, "application/json",
                                 body, error);
  free(json);
  return status;
}

vectis_status vectis_response_json_generated(vectis_response *response,
                                             int status_code,
                                             const lonejson_map *map,
                                             const void *value,
                                             vectis_error *error) {
  vectis_file_sink sink;
  lonejson_error json_error;
  lonejson_status json_status;
  lonejson *runtime;
  FILE *fp;
  char *path;

  if (map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  if (value == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "json response value is required");
    return VECTIS_ERR_INVALID;
  }
  fp = NULL;
  path = NULL;
  if (vectis_create_response_temp_file(&fp, &path, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  sink.fp = fp;
  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    (void)fclose(fp);
    (void)unlink(path);
    free(path);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  json_status = lonejson_serialize_sink(
      runtime, map, value, vectis_lonejson_file_sink, &sink, &json_error);
  lonejson_free(runtime);
  if (json_status != LONEJSON_STATUS_OK) {
    (void)fclose(fp);
    (void)unlink(path);
    free(path);
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to serialize generated response JSON: %s",
                      json_error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
      error->dependency_code = (long)json_status;
    }
    return VECTIS_ERR_INVALID;
  }
  if (fclose(fp) != 0) {
    (void)unlink(path);
    free(path);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to close generated JSON response file");
    return VECTIS_ERR_STATE;
  }
  return vectis_response_file_owned(response, status_code, "application/json",
                                    path, 1, error);
}

vectis_status vectis_json_reply(vectis_json_response *response, int status_code,
                                const lonejson_map *map, const void *value,
                                vectis_error *error) {
  vectis_status status;

  if (response == NULL || response->response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json response is required");
    return VECTIS_ERR_INVALID;
  }
  status =
      vectis_response_json(response->response, status_code, map, value, error);
  if (status == VECTIS_OK) {
    response->sent = 1;
  }
  return status;
}

vectis_status vectis_json_reply_status(vectis_json_response *response,
                                       int status_code, vectis_error *error) {
  vectis_status status;

  if (response == NULL || response->response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json response is required");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_response_status(response->response, status_code, error);
  if (status == VECTIS_OK) {
    response->sent = 1;
  }
  return status;
}

vectis_status vectis_response_error_json(vectis_response *response,
                                         int status_code, const char *code,
                                         const char *message,
                                         const char *detail,
                                         vectis_error *error) {
  vectis_error_response_body body;

  memset(&body, 0, sizeof(body));
  if (status_code < 100 || status_code > 599) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP status code is invalid");
    return VECTIS_ERR_INVALID;
  }
  (void)snprintf(body.code, sizeof(body.code), "%s",
                 code != NULL && code[0] != '\0' ? code : "error");
  (void)snprintf(body.message, sizeof(body.message), "%s",
                 message != NULL && message[0] != '\0' ? message
                                                       : "request failed");
  if (detail != NULL) {
    (void)snprintf(body.detail, sizeof(body.detail), "%s", detail);
  }
  return vectis_response_json(response, status_code, &vectis_error_response_map,
                              &body, error);
}

void vectis_http_client_config_init(vectis_http_client_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->timeout_ms = 30000L;
  config->connect_timeout_ms = 10000L;
  config->retry_max_attempts = 1u;
  config->retry_initial_delay_ms = 250L;
  config->retry_max_delay_ms = 2000L;
  config->retry_conditions = VECTIS_HTTP_RETRY_DEFAULT;
}

static vectis_http_client_config
vectis_effective_http_client_config(const vectis_http_client_config *config) {
  vectis_http_client_config effective;

  vectis_http_client_config_init(&effective);
  if (config == NULL) {
    return effective;
  }
  effective = *config;
  effective.timeout_ms = vectis_default_long(config->timeout_ms, 30000L);
  effective.connect_timeout_ms =
      vectis_default_long(config->connect_timeout_ms, 10000L);
  effective.retry_max_attempts =
      vectis_default_unsigned(config->retry_max_attempts, 1u);
  effective.retry_initial_delay_ms =
      vectis_default_long(config->retry_initial_delay_ms, 250L);
  effective.retry_max_delay_ms =
      vectis_default_long(config->retry_max_delay_ms, 2000L);
  return effective;
}

vectis_status vectis_http_client_new(const vectis_http_client_config *config,
                                     vectis_http_client **out,
                                     vectis_error *error) {
  vectis_http_client_config defaults;
  const vectis_http_client_config *effective;
  vectis_http_client_config normalized;
  vectis_http_client *client;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "HTTP client output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  vectis_http_client_config_init(&defaults);
  effective = config != NULL ? config : &defaults;
  normalized = vectis_effective_http_client_config(effective);
  if (effective->timeout_ms < 0L || effective->connect_timeout_ms < 0L ||
      effective->low_speed_limit_bytes_per_sec < 0L ||
      effective->low_speed_time_seconds < 0L ||
      effective->retry_initial_delay_ms < 0L ||
      effective->retry_max_delay_ms < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "HTTP client timeouts must be non-negative");
    return VECTIS_ERR_INVALID;
  }

  client = (vectis_http_client *)calloc(1u, sizeof(*client));
  if (client == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate HTTP client");
    return VECTIS_ERR_NOMEM;
  }
  client->execute = vectis_http_client_execute;
  client->get = vectis_http_client_get;
  client->get_json_array = vectis_http_client_get_json_array;
  client->del = vectis_http_client_delete;
  client->head = vectis_http_client_head;
  client->options = vectis_http_client_options;
  client->download_file = vectis_http_client_download_file;
  client->upload_file = vectis_http_client_upload_file;
  client->post_json = vectis_http_client_post_json;
  client->put_json = vectis_http_client_put_json;
  client->patch_json = vectis_http_client_patch_json;
  client->close = vectis_http_client_destroy;
  client->config = normalized;
  vectis_error_clear(error);
  *out = client;
  return VECTIS_OK;
}

vectis_status
vectis_http_client_from_app(vectis_app *app,
                            const vectis_http_client_config *config,
                            vectis_http_client **out, vectis_error *error) {
  vectis_http_client_config effective;
  vectis_status status;

  if (app == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  if (config != NULL) {
    effective = *config;
  } else {
    vectis_http_client_config_init(&effective);
  }
  if (effective.logger == NULL) {
    effective.logger = vectis_logger(app);
  }
  status = vectis_http_client_new(&effective, out, error);
  return status;
}

void vectis_http_client_destroy(vectis_http_client *client) { free(client); }

void vectis_http_client_close(vectis_http_client *client) {
  vectis_http_client_destroy(client);
}

void vectis_http_request_init(vectis_http_request *request) {
  if (request == NULL) {
    return;
  }
  memset(request, 0, sizeof(*request));
  request->method = VECTIS_HTTP_GET;
  request->retry_conditions = VECTIS_HTTP_RETRY_INHERIT;
}

void vectis_http_response_cleanup(vectis_http_response *response) {
  if (response == NULL) {
    return;
  }
  free(response->content_type);
  vectis_http_response_headers_cleanup(response);
  free(response->body);
  memset(response, 0, sizeof(*response));
}

const char *vectis_http_response_header(const vectis_http_response *response,
                                        const char *name) {
  size_t i;

  if (response == NULL || name == NULL || name[0] == '\0') {
    return NULL;
  }
  for (i = response->header_count; i > 0u; --i) {
    if (response->headers[i - 1u].name != NULL &&
        strcasecmp(response->headers[i - 1u].name, name) == 0) {
      return response->headers[i - 1u].value;
    }
  }
  return NULL;
}

vectis_status
vectis_http_response_json_into(const vectis_http_response *response,
                               const lonejson_map *map, void *out,
                               vectis_error *error) {
  lonejson_error json_error;
  lonejson *runtime;
  lonejson_status json_status;

  if (response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP response is required");
    return VECTIS_ERR_INVALID;
  }
  if (map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "JSON output is required");
    return VECTIS_ERR_INVALID;
  }
  if (response->body == NULL && response->body_size > 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "HTTP response body is invalid");
    return VECTIS_ERR_INVALID;
  }
  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  json_status = lonejson_parse_buffer(runtime, map, out, response->body,
                                      response->body_size, &json_error);
  lonejson_free(runtime);
  if (json_status != LONEJSON_STATUS_OK) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to parse JSON response: %s", json_error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
    }
    return VECTIS_ERR_INVALID;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_http_response_json_array_each(
    const vectis_http_response *response, const char *array_path,
    const lonejson_map *map, void *item, vectis_json_array_item_fn callback,
    void *userdata, vectis_error *error) {
  vectis_source source;

  if (response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP response is required");
    return VECTIS_ERR_INVALID;
  }
  if (response->body == NULL && response->body_size > 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "HTTP response body is invalid");
    return VECTIS_ERR_INVALID;
  }
  source = vectis_source_from_memory(response->body, response->body_size);
  return vectis_json_array_each_source(&source, array_path, map, item, callback,
                                       userdata, error);
}

vectis_status vectis_http_client_execute(vectis_http_client *client,
                                         const vectis_http_request *request,
                                         vectis_http_response *response,
                                         vectis_error *error) {
  if (client == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP client is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_http_execute(&client->config, request, response, error);
}

vectis_status vectis_http_client_get(vectis_http_client *client,
                                     const char *url,
                                     vectis_http_response *response,
                                     vectis_error *error) {
  vectis_http_request request;

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = url;
  return vectis_http_client_execute(client, &request, response, error);
}

vectis_status vectis_http_client_get_json_array(
    vectis_http_client *client, const char *url, const char *array_path,
    const lonejson_map *map, void *item, vectis_json_array_item_fn callback,
    void *userdata, vectis_http_response *response, vectis_error *error) {
  if (client == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP client is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_http_get_json_array(&client->config, url, array_path, map, item,
                                    callback, userdata, response, error);
}

vectis_status vectis_http_client_delete(vectis_http_client *client,
                                        const char *url,
                                        vectis_http_response *response,
                                        vectis_error *error) {
  vectis_http_request request;

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_DELETE;
  request.url = url;
  return vectis_http_client_execute(client, &request, response, error);
}

vectis_status vectis_http_client_head(vectis_http_client *client,
                                      const char *url,
                                      vectis_http_response *response,
                                      vectis_error *error) {
  vectis_http_request request;

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_HEAD;
  request.url = url;
  return vectis_http_client_execute(client, &request, response, error);
}

vectis_status vectis_http_client_options(vectis_http_client *client,
                                         const char *url,
                                         vectis_http_response *response,
                                         vectis_error *error) {
  vectis_http_request request;

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_OPTIONS;
  request.url = url;
  return vectis_http_client_execute(client, &request, response, error);
}

vectis_status vectis_http_client_download_file(vectis_http_client *client,
                                               const char *url,
                                               const char *local_path,
                                               vectis_http_response *response,
                                               vectis_error *error) {
  vectis_http_request request;

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = url;
  request.download_path = local_path;
  return vectis_http_client_execute(client, &request, response, error);
}

vectis_status vectis_http_client_upload_file(
    vectis_http_client *client, vectis_http_method method, const char *url,
    const char *local_path, const char *content_type,
    vectis_http_response *response, vectis_error *error) {
  vectis_http_request request;

  vectis_http_request_init(&request);
  request.method = method;
  request.url = url;
  request.body_path = local_path;
  request.content_type = content_type;
  return vectis_http_client_execute(client, &request, response, error);
}

static vectis_status vectis_http_client_send_json(
    vectis_http_client *client, vectis_http_method method, const char *url,
    const lonejson_map *map, const void *value, vectis_http_response *response,
    vectis_error *error) {
  vectis_http_request request;

  if (map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  if (value == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json value is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_http_request_init(&request);
  request.method = method;
  request.url = url;
  request.content_type = "application/json";
  request.json_map = map;
  request.json_value = value;
  return vectis_http_client_execute(client, &request, response, error);
}

static size_t vectis_curl_write_memory(char *ptr, size_t size, size_t nmemb,
                                       void *userdata) {
  vectis_curl_buffer *buffer;
  size_t bytes;
  char *grown;

  buffer = (vectis_curl_buffer *)userdata;
  bytes = size * nmemb;
  if (buffer == NULL || bytes == 0u) {
    return bytes;
  }
  if (buffer->size > ((size_t)-1) - bytes - 1u) {
    buffer->failed = 1;
    return 0u;
  }
  grown = (char *)realloc(buffer->data, buffer->size + bytes + 1u);
  if (grown == NULL) {
    buffer->failed = 1;
    return 0u;
  }
  buffer->data = grown;
  memcpy(buffer->data + buffer->size, ptr, bytes);
  buffer->size += bytes;
  buffer->data[buffer->size] = '\0';
  return bytes;
}

static size_t vectis_curl_write_file(char *ptr, size_t size, size_t nmemb,
                                     void *userdata) {
  return fwrite(ptr, size, nmemb, (FILE *)userdata);
}

static size_t vectis_curl_write_stream(char *ptr, size_t size, size_t nmemb,
                                       void *userdata) {
  vectis_curl_response_stream *stream;
  size_t bytes;

  stream = (vectis_curl_response_stream *)userdata;
  bytes = size * nmemb;
  if (stream == NULL || stream->callback == NULL || bytes == 0u) {
    return bytes;
  }
  if (stream->callback(ptr, bytes, stream->userdata, stream->error) !=
      VECTIS_OK) {
    stream->failed = 1;
    return 0u;
  }
  return bytes;
}

static size_t vectis_curl_read_file(char *ptr, size_t size, size_t nmemb,
                                    void *userdata) {
  return fread(ptr, size, nmemb, (FILE *)userdata);
}

static size_t vectis_curl_read_memory(char *ptr, size_t size, size_t nmemb,
                                      void *userdata) {
  vectis_curl_request_body *body;
  size_t capacity;
  size_t remaining;
  size_t n;

  body = (vectis_curl_request_body *)userdata;
  capacity = size * nmemb;
  if (body == NULL || capacity == 0u || body->offset >= body->size) {
    return 0u;
  }
  remaining = body->size - body->offset;
  n = remaining < capacity ? remaining : capacity;
  memcpy(ptr, (const char *)body->data + body->offset, n);
  body->offset += n;
  return n;
}

static vectis_status vectis_file_size(FILE *file, long *out_size,
                                      vectis_error *error) {
  long current;
  long end;

  if (file == NULL || out_size == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "file is required");
    return VECTIS_ERR_INVALID;
  }
  current = ftell(file);
  if (current < 0L) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to inspect file position");
    return VECTIS_ERR_STATE;
  }
  if (fseek(file, 0L, SEEK_END) != 0) {
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to seek file");
    return VECTIS_ERR_STATE;
  }
  end = ftell(file);
  if (end < 0L || fseek(file, current, SEEK_SET) != 0) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to restore file position");
    return VECTIS_ERR_STATE;
  }
  *out_size = end;
  return VECTIS_OK;
}

static void vectis_curl_request_body_cleanup(vectis_curl_request_body *body) {
  if (body == NULL) {
    return;
  }
  if (body->file != NULL) {
    (void)fclose(body->file);
  }
  if (body->json_upload_active) {
    lonejson_curl_upload_cleanup(&body->json_upload);
  }
  if (body->json_runtime != NULL) {
    lonejson_free(body->json_runtime);
  }
  free(body->owned_data);
  memset(body, 0, sizeof(*body));
}

static vectis_status vectis_lonejson_count_upload_size(const lonejson_map *map,
                                                       const void *value,
                                                       curl_off_t *out_size,
                                                       vectis_error *error) {
  lonejson_error json_error;
  lonejson_status json_status;
  lonejson *runtime;
  size_t measured_size;
  size_t max_curl_size;

  if (out_size == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "JSON upload size output is required");
    return VECTIS_ERR_INVALID;
  }
  *out_size = (curl_off_t)-1;

  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  lonejson_error_init(&json_error);
  json_status = lonejson_generator_measure(runtime, map, value, &measured_size,
                                           &json_error);
  lonejson_free(runtime);
  if (json_status == LONEJSON_STATUS_OK) {
    max_curl_size =
        (sizeof(curl_off_t) <= sizeof(size_t))
            ? (((size_t)1u << ((sizeof(curl_off_t) * CHAR_BIT) - 1u)) - 1u)
            : (size_t)-1;
    if (measured_size > max_curl_size) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "JSON request body is too large for curl");
      return VECTIS_ERR_INVALID;
    }
    *out_size = (curl_off_t)measured_size;
    return VECTIS_OK;
  }
  if (json_status == LONEJSON_STATUS_INVALID_ARGUMENT &&
      strstr(json_error.message, "cannot measure non-rewindable") != NULL) {
    return VECTIS_OK;
  }

  vectis_set_errorf(
      error, VECTIS_ERR_INVALID, "failed to inspect JSON request size: %s",
      json_error.message[0] != '\0' ? json_error.message
                                    : lonejson_status_string(json_status));
  if (error != NULL) {
    error->source = VECTIS_ERROR_SOURCE_LONEJSON;
    error->dependency_code = (long)json_status;
  }
  return VECTIS_ERR_INVALID;
}

static vectis_status
vectis_prepare_curl_body(const vectis_http_request *request,
                         vectis_curl_request_body *body, vectis_error *error) {
  lonejson_error json_error;
  lonejson_status json_status;
  vectis_status status;

  memset(body, 0, sizeof(*body));
  if (request->json_map != NULL || request->json_value != NULL) {
    if (request->json_map == NULL || request->json_value == NULL) {
      vectis_set_error(
          error, VECTIS_ERR_INVALID,
          "JSON HTTP request requires both json_map and json_value");
      return VECTIS_ERR_INVALID;
    }
    body->json_runtime = vectis_lonejson_new(error);
    if (body->json_runtime == NULL) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    json_status =
        lonejson_curl_upload_init(&body->json_upload, body->json_runtime,
                                  request->json_map, request->json_value);
    if (json_status != LONEJSON_STATUS_OK) {
      json_error = body->json_upload.generator.error;
      vectis_set_errorf(
          error, VECTIS_ERR_INVALID, "failed to serialize JSON request: %s",
          json_error.message[0] != '\0' ? json_error.message
                                        : lonejson_status_string(json_status));
      if (error != NULL) {
        error->source = VECTIS_ERROR_SOURCE_LONEJSON;
        error->dependency_code = (long)json_status;
      }
      lonejson_free(body->json_runtime);
      body->json_runtime = NULL;
      return VECTIS_ERR_INVALID;
    }
    status = vectis_lonejson_count_upload_size(
        request->json_map, request->json_value, &body->json_size, error);
    if (status != VECTIS_OK) {
      lonejson_curl_upload_cleanup(&body->json_upload);
      lonejson_free(body->json_runtime);
      body->json_runtime = NULL;
      return status;
    }
    body->json_upload_active = 1;
    return VECTIS_OK;
  }
  if (request->body != NULL || request->body_size > 0u) {
    if (request->body == NULL) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "HTTP request body pointer is required");
      return VECTIS_ERR_INVALID;
    }
    body->data = request->body;
    body->size = request->body_size;
    return VECTIS_OK;
  }
  if (request->body_path != NULL) {
    body->file = fopen(request->body_path, "rb");
    if (body->file == NULL) {
      vectis_set_errorf(error, VECTIS_ERR_INVALID,
                        "failed to open request body path: %s",
                        request->body_path);
      return VECTIS_ERR_INVALID;
    }
    if (vectis_file_size(body->file, &body->file_size, error) != VECTIS_OK) {
      vectis_curl_request_body_cleanup(body);
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
  }
  return VECTIS_OK;
}

static vectis_status vectis_curl_set_common_tls(
    CURL *curl, const vectis_source *client_bundle,
    const char *client_bundle_path, const void *client_bundle_pem,
    size_t client_bundle_pem_size, const vectis_source *ca_bundle,
    const char *ca_bundle_path, vectis_error *error) {
  const char *client_path;
  const char *ca_path;
  const void *client_pem;
  void *client_pem_owned;
  size_t client_pem_size;
#if defined(CURL_AT_LEAST_VERSION) && CURL_AT_LEAST_VERSION(7, 71, 0)
  struct curl_blob cert_blob;
#endif

  client_path = vectis_source_path_or_old(client_bundle, client_bundle_path);
  ca_path = vectis_source_path_or_old(ca_bundle, ca_bundle_path);
  client_pem_owned = NULL;
  client_pem_size = 0u;
  client_pem =
      vectis_source_memory_or_old(client_bundle, client_bundle_pem,
                                  &client_pem_size, client_bundle_pem_size);
  if (client_path == NULL && client_pem == NULL && client_bundle != NULL &&
      client_bundle->source != NULL) {
#if defined(CURL_AT_LEAST_VERSION) && CURL_AT_LEAST_VERSION(7, 71, 0)
    if (vectis_read_source_bytes(client_bundle, &client_pem_owned,
                                 &client_pem_size, "client certificate bundle",
                                 error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
    client_pem = client_pem_owned;
#else
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "this libcurl build does not support in-memory client certificates");
    return VECTIS_ERR_INVALID;
#endif
  }
  if (ca_path != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_CAINFO, ca_path);
  } else if (ca_bundle != NULL &&
             (ca_bundle->memory != NULL || ca_bundle->source != NULL)) {
#if defined(CURL_AT_LEAST_VERSION) && CURL_AT_LEAST_VERSION(7, 77, 0)
    void *ca_pem;
    size_t ca_pem_size;
    struct curl_blob ca_blob;

    ca_pem = NULL;
    ca_pem_size = 0u;
    if (vectis_read_source_bytes(ca_bundle, &ca_pem, &ca_pem_size, "CA bundle",
                                 error) != VECTIS_OK) {
      free(client_pem_owned);
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
    memset(&ca_blob, 0, sizeof(ca_blob));
    ca_blob.data = ca_pem;
    ca_blob.len = ca_pem_size;
    ca_blob.flags = CURL_BLOB_COPY;
    (void)curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca_blob);
    free(ca_pem);
#else
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "this libcurl build does not support in-memory CA bundles");
    free(client_pem_owned);
    return VECTIS_ERR_INVALID;
#endif
  }
  if (client_path != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_SSLCERT, client_path);
    (void)curl_easy_setopt(curl, CURLOPT_SSLKEY, client_path);
  } else if (client_pem != NULL && client_pem_size > 0u) {
#if defined(CURL_AT_LEAST_VERSION) && CURL_AT_LEAST_VERSION(7, 71, 0)
    memset(&cert_blob, 0, sizeof(cert_blob));
    memcpy(&cert_blob.data, &client_pem, sizeof(cert_blob.data));
    cert_blob.len = client_pem_size;
    cert_blob.flags = CURL_BLOB_COPY;
    (void)curl_easy_setopt(curl, CURLOPT_SSLCERT_BLOB, &cert_blob);
    (void)curl_easy_setopt(curl, CURLOPT_SSLKEY_BLOB, &cert_blob);
    free(client_pem_owned);
#else
    free(client_pem_owned);
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "this libcurl build does not support in-memory client certificates");
    return VECTIS_ERR_INVALID;
#endif
  }
  return VECTIS_OK;
}

static vectis_status vectis_curl_set_error(vectis_error *error, CURLcode code,
                                           const char *message,
                                           char *curl_error) {
  vectis_set_errorf(error, VECTIS_ERR_STATE, "%s: %s", message,
                    curl_error != NULL && curl_error[0] != '\0'
                        ? curl_error
                        : curl_easy_strerror(code));
  if (error != NULL) {
    error->source = VECTIS_ERROR_SOURCE_CURL;
    error->dependency_code = (long)code;
  }
  return VECTIS_ERR_STATE;
}

static int vectis_auth_email_header_safe(const char *value) {
  const char *p;

  if (value == NULL || value[0] == '\0') {
    return 0;
  }
  for (p = value; *p != '\0'; ++p) {
    if (*p == '\r' || *p == '\n') {
      return 0;
    }
  }
  return 1;
}

static int
vectis_auth_smtp_recipient_allowed(const vectis_auth_smtp_config *config,
                                   const char *email) {
  const char *at;
  size_t i;

  if (config == NULL || email == NULL || email[0] == '\0') {
    return 0;
  }
  if (config->allowed_recipients != NULL &&
      config->allowed_recipient_count > 0u) {
    for (i = 0u; i < config->allowed_recipient_count; ++i) {
      if (config->allowed_recipients[i] != NULL &&
          strcasecmp(config->allowed_recipients[i], email) == 0) {
        return 1;
      }
    }
    return 0;
  }
  if (config->allowed_recipient_domain != NULL &&
      config->allowed_recipient_domain[0] != '\0') {
    at = strrchr(email, '@');
    return at != NULL && at[1] != '\0' &&
           strcasecmp(at + 1, config->allowed_recipient_domain) == 0;
  }
  return 1;
}

static vectis_status
vectis_auth_smtp_build_body(const vectis_auth_smtp_config *config,
                            const vectis_auth_email_message *message,
                            vectis_string_builder *body, vectis_error *error) {
  vectis_status status;

  memset(body, 0, sizeof(*body));
  status = vectis_string_builder_appendf(
      body, error,
      "From: %s\r\nTo: %s\r\nSubject: %s\r\nContent-Type: text/plain; "
      "charset=utf-8\r\n\r\n",
      config->mail_from, message->email,
      config->subject != NULL && config->subject[0] != '\0'
          ? config->subject
          : "Vectis login token");
  if (status == VECTIS_OK) {
    status = vectis_string_builder_appendf(
        body, error,
        "Your Vectis login token is: %s\r\n\r\nTransaction: %s\r\nRealm: "
        "%s\r\nExpires at: %llu\r\n",
        message->token, message->transaction_id,
        message->realm != NULL ? message->realm : "vectis",
        (unsigned long long)message->expires_at);
  }
  return status;
}

vectis_status
vectis_auth_email_token_deliver_smtp(const vectis_auth_smtp_config *config,
                                     const vectis_auth_email_message *message,
                                     vectis_error *error) {
  CURL *curl;
  CURLcode curl_code;
  struct curl_slist *recipients;
  vectis_curl_request_body upload;
  vectis_string_builder body;
  char curl_error[CURL_ERROR_SIZE];
  vectis_status status;

  if (config == NULL || config->url == NULL || config->url[0] == '\0' ||
      config->mail_from == NULL || config->mail_from[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SMTP url and mail_from are required");
    return VECTIS_ERR_INVALID;
  }
  if (message == NULL || message->email == NULL || message->email[0] == '\0' ||
      message->transaction_id == NULL || message->transaction_id[0] == '\0' ||
      message->token == NULL || message->token[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SMTP email token message is incomplete");
    return VECTIS_ERR_INVALID;
  }
  if (!vectis_auth_email_header_safe(config->mail_from) ||
      !vectis_auth_email_header_safe(message->email) ||
      (config->subject != NULL && config->subject[0] != '\0' &&
       !vectis_auth_email_header_safe(config->subject))) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SMTP email fields must not contain newlines");
    return VECTIS_ERR_INVALID;
  }
  if (!vectis_auth_smtp_recipient_allowed(config, message->email)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SMTP recipient is not allowed");
    return VECTIS_ERR_INVALID;
  }

  memset(&body, 0, sizeof(body));
  status = vectis_auth_smtp_build_body(config, message, &body, error);
  if (status != VECTIS_OK) {
    vectis_string_builder_cleanup(&body);
    return status;
  }
  memset(&upload, 0, sizeof(upload));
  upload.data = body.data;
  upload.size = body.size;
  recipients = NULL;
  memset(curl_error, 0, sizeof(curl_error));

  (void)pthread_once(&vectis_curl_once, vectis_curl_global_init_once);
  curl = curl_easy_init();
  if (curl == NULL) {
    vectis_string_builder_cleanup(&body);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to initialize curl easy handle");
    return VECTIS_ERR_NOMEM;
  }
  recipients = curl_slist_append(recipients, message->email);
  if (recipients == NULL) {
    curl_easy_cleanup(curl);
    vectis_string_builder_cleanup(&body);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate SMTP recipient");
    return VECTIS_ERR_NOMEM;
  }

  (void)curl_easy_setopt(curl, CURLOPT_URL, config->url);
  (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
  (void)curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (config->timeout_ms > 0L) {
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, config->timeout_ms);
  }
  if (config->connect_timeout_ms > 0L) {
    (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                           config->connect_timeout_ms);
  }
  if (config->username != NULL && config->username[0] != '\0') {
    (void)curl_easy_setopt(curl, CURLOPT_USERNAME, config->username);
  }
  if (config->password != NULL && config->password[0] != '\0') {
    (void)curl_easy_setopt(curl, CURLOPT_PASSWORD, config->password);
  }
  if (config->use_ssl) {
    (void)curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
  }
  if (config->tls_verify_peer_disabled) {
    (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  }
  if (config->tls_verify_host_disabled) {
    (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  if (config->ca_bundle_path != NULL && config->ca_bundle_path[0] != '\0') {
    (void)curl_easy_setopt(curl, CURLOPT_CAINFO, config->ca_bundle_path);
  }
  (void)curl_easy_setopt(curl, CURLOPT_MAIL_FROM, config->mail_from);
  (void)curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
  (void)curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_READFUNCTION, vectis_curl_read_memory);
  (void)curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
  (void)curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                         (curl_off_t)upload.size);
  if (config->configure_curl != NULL &&
      config->configure_curl(curl, config->configure_curl_userdata, error) !=
          VECTIS_OK) {
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);
    vectis_string_builder_cleanup(&body);
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  curl_code = curl_easy_perform(curl);
  curl_slist_free_all(recipients);
  curl_easy_cleanup(curl);
  vectis_string_builder_cleanup(&body);
  if (curl_code != CURLE_OK) {
    return vectis_curl_set_error(error, curl_code, "SMTP delivery failed",
                                 curl_error);
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

static int vectis_http_method_upload_capable(vectis_http_method method) {
  return method != VECTIS_HTTP_GET && method != VECTIS_HTTP_HEAD &&
         method != VECTIS_HTTP_OPTIONS;
}

static unsigned
vectis_http_effective_retry_attempts(const vectis_http_client_config *client,
                                     const vectis_http_request *request) {
  if (request != NULL && request->retry_max_attempts > 0u) {
    return request->retry_max_attempts;
  }
  if (client != NULL && client->retry_max_attempts > 0u) {
    return client->retry_max_attempts;
  }
  return 1u;
}

static long vectis_http_effective_retry_initial_delay(
    const vectis_http_client_config *client,
    const vectis_http_request *request) {
  if (request != NULL && request->retry_initial_delay_ms > 0L) {
    return request->retry_initial_delay_ms;
  }
  if (client != NULL && client->retry_initial_delay_ms > 0L) {
    return client->retry_initial_delay_ms;
  }
  return 0L;
}

static long
vectis_http_effective_retry_max_delay(const vectis_http_client_config *client,
                                      const vectis_http_request *request) {
  if (request != NULL && request->retry_max_delay_ms > 0L) {
    return request->retry_max_delay_ms;
  }
  if (client != NULL && client->retry_max_delay_ms > 0L) {
    return client->retry_max_delay_ms;
  }
  return 0L;
}

static vectis_http_retry_conditions
vectis_http_effective_retry_conditions(const vectis_http_client_config *client,
                                       const vectis_http_request *request) {
  if (request != NULL &&
      request->retry_conditions != VECTIS_HTTP_RETRY_INHERIT) {
    return request->retry_conditions;
  }
  if (client != NULL) {
    return client->retry_conditions;
  }
  return VECTIS_HTTP_RETRY_NONE;
}

static int
vectis_http_status_retryable(long status_code,
                             vectis_http_retry_conditions conditions) {
  if ((conditions & VECTIS_HTTP_RETRY_429) != 0u && status_code == 429L) {
    return 1;
  }
  if ((conditions & VECTIS_HTTP_RETRY_5XX) != 0u && status_code >= 500L &&
      status_code <= 599L) {
    return 1;
  }
  return 0;
}

static int
vectis_http_error_retryable(const vectis_error *error,
                            vectis_http_retry_conditions conditions) {
  if ((conditions & VECTIS_HTTP_RETRY_TRANSPORT) == 0u) {
    return 0;
  }
  return error != NULL && error->source == VECTIS_ERROR_SOURCE_CURL;
}

static void vectis_sleep_ms(long delay_ms) {
  struct timespec ts;

  if (delay_ms <= 0L) {
    return;
  }
  ts.tv_sec = delay_ms / 1000L;
  ts.tv_nsec = (delay_ms % 1000L) * 1000000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

static vectis_status vectis_append_output(char **out, size_t *out_size,
                                          const char *data, size_t data_size,
                                          vectis_error *error) {
  char *grown;

  if (data_size == 0u) {
    return VECTIS_OK;
  }
  if (out == NULL || out_size == NULL || data == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "output buffer is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (*out_size > ((size_t)-1) - data_size - 1u) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "output buffer is too large");
    return VECTIS_ERR_NOMEM;
  }
  grown = (char *)realloc(*out, *out_size + data_size + 1u);
  if (grown == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to grow output buffer");
    return VECTIS_ERR_NOMEM;
  }
  *out = grown;
  memcpy(*out + *out_size, data, data_size);
  *out_size += data_size;
  (*out)[*out_size] = '\0';
  return VECTIS_OK;
}

vectis_status vectis_http_client_post_json(
    vectis_http_client *client, const char *url, const lonejson_map *map,
    const void *value, vectis_http_response *response, vectis_error *error) {
  return vectis_http_client_send_json(client, VECTIS_HTTP_POST, url, map, value,
                                      response, error);
}

vectis_status vectis_http_client_put_json(
    vectis_http_client *client, const char *url, const lonejson_map *map,
    const void *value, vectis_http_response *response, vectis_error *error) {
  return vectis_http_client_send_json(client, VECTIS_HTTP_PUT, url, map, value,
                                      response, error);
}

vectis_status vectis_http_client_patch_json(
    vectis_http_client *client, const char *url, const lonejson_map *map,
    const void *value, vectis_http_response *response, vectis_error *error) {
  return vectis_http_client_send_json(client, VECTIS_HTTP_PATCH, url, map,
                                      value, response, error);
}

static vectis_status
vectis_http_execute_once(const vectis_http_client_config *client,
                         const vectis_http_request *request,
                         vectis_http_response *response, vectis_error *error) {
  CURL *curl;
  CURLcode curl_code;
  struct curl_slist *headers;
  vectis_curl_buffer response_buffer;
  vectis_curl_response_stream response_stream;
  vectis_curl_header_capture header_capture;
  vectis_curl_request_body request_body;
  char curl_error[CURL_ERROR_SIZE];
  char *url;
  char *response_content_type;
  FILE *download_file;
  long timeout_ms;
  long connect_timeout_ms;
  long low_speed_limit;
  long low_speed_time;
  size_t i;
  const char *method;
  char content_type_header[256];

  if (client == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "HTTP client config is required");
    return VECTIS_ERR_INVALID;
  }
  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP request is required");
    return VECTIS_ERR_INVALID;
  }
  if (request->url == NULL && client->base_url == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "HTTP request requires url or client base_url");
    return VECTIS_ERR_INVALID;
  }
  if (response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP response is required");
    return VECTIS_ERR_INVALID;
  }
  if (request->body != NULL && request->body_path != NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "HTTP request cannot use both body and body_path");
    return VECTIS_ERR_INVALID;
  }
  if (request->download_path != NULL && request->download_path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "HTTP download_path must not be empty");
    return VECTIS_ERR_INVALID;
  }
  if (request->download_path != NULL && request->response_body != NULL) {
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "HTTP request cannot use both download_path and response_body");
    return VECTIS_ERR_INVALID;
  }
  if (request->low_speed_limit_bytes_per_sec < 0L ||
      request->low_speed_time_seconds < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "HTTP request low-speed settings must be non-negative");
    return VECTIS_ERR_INVALID;
  }

  memset(response, 0, sizeof(*response));
  memset(&response_buffer, 0, sizeof(response_buffer));
  memset(&response_stream, 0, sizeof(response_stream));
  memset(&header_capture, 0, sizeof(header_capture));
  memset(&request_body, 0, sizeof(request_body));
  memset(curl_error, 0, sizeof(curl_error));
  headers = NULL;
  curl = NULL;
  url = NULL;
  response_content_type = NULL;
  download_file = NULL;

  if (request->method < VECTIS_HTTP_GET || request->method > VECTIS_HTTP_MOVE) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "HTTP request method is invalid");
    return VECTIS_ERR_INVALID;
  }

  url = vectis_join_url(client->base_url, request->url, error);
  if (url == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  if (vectis_prepare_curl_body(request, &request_body, error) != VECTIS_OK) {
    free(url);
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }

  (void)pthread_once(&vectis_curl_once, vectis_curl_global_init_once);
  curl = curl_easy_init();
  if (curl == NULL) {
    vectis_curl_request_body_cleanup(&request_body);
    free(url);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to initialize curl easy handle");
    return VECTIS_ERR_NOMEM;
  }

  timeout_ms =
      request->timeout_ms > 0L ? request->timeout_ms : client->timeout_ms;
  connect_timeout_ms = client->connect_timeout_ms;
  low_speed_limit = request->low_speed_limit_bytes_per_sec > 0L
                        ? request->low_speed_limit_bytes_per_sec
                        : client->low_speed_limit_bytes_per_sec;
  low_speed_time = request->low_speed_time_seconds > 0L
                       ? request->low_speed_time_seconds
                       : client->low_speed_time_seconds;
  (void)curl_easy_setopt(curl, CURLOPT_URL, url);
  (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
  header_capture.response = response;
  (void)curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
                         vectis_curl_header_callback);
  (void)curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_capture);
  if (timeout_ms > 0L) {
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  }
  if (connect_timeout_ms > 0L) {
    (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
  }
  if (!client->follow_redirects_disabled) {
    (void)curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  }
  if (request->proxy_url != NULL && request->proxy_url[0] != '\0') {
    (void)curl_easy_setopt(curl, CURLOPT_PROXY, request->proxy_url);
  } else if (client->proxy_url != NULL && client->proxy_url[0] != '\0') {
    (void)curl_easy_setopt(curl, CURLOPT_PROXY, client->proxy_url);
  }
  if (low_speed_limit > 0L) {
    (void)curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, low_speed_limit);
  }
  if (low_speed_time > 0L) {
    (void)curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, low_speed_time);
  }
  if (vectis_curl_set_common_tls(
          curl, &client->client_bundle, client->client_bundle_path,
          client->client_bundle_pem, client->client_bundle_pem_size,
          &client->ca_bundle, client->ca_bundle_path, error) != VECTIS_OK) {
    curl_easy_cleanup(curl);
    vectis_curl_request_body_cleanup(&request_body);
    free(url);
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }

  for (i = 0u; i < request->header_count; ++i) {
    if (request->headers == NULL || request->headers[i] == NULL) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      vectis_curl_request_body_cleanup(&request_body);
      free(url);
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "HTTP request headers are invalid");
      return VECTIS_ERR_INVALID;
    }
    headers = curl_slist_append(headers, request->headers[i]);
    if (headers == NULL) {
      curl_easy_cleanup(curl);
      vectis_curl_request_body_cleanup(&request_body);
      free(url);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to allocate curl headers");
      return VECTIS_ERR_NOMEM;
    }
  }
  if (request->content_type != NULL && request->content_type[0] != '\0') {
    (void)snprintf(content_type_header, sizeof(content_type_header),
                   "Content-Type: %s", request->content_type);
    headers = curl_slist_append(headers, content_type_header);
    if (headers == NULL) {
      curl_easy_cleanup(curl);
      vectis_curl_request_body_cleanup(&request_body);
      free(url);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to allocate content-type header");
      return VECTIS_ERR_NOMEM;
    }
  }
  if (request_body.json_upload_active && request_body.json_size < 0) {
    headers = curl_slist_append(headers, "Transfer-Encoding: chunked");
    if (headers == NULL) {
      curl_easy_cleanup(curl);
      vectis_curl_request_body_cleanup(&request_body);
      free(url);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to allocate transfer-encoding header");
      return VECTIS_ERR_NOMEM;
    }
  }
  if (headers != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }

  method = vectis_http_method_string(request->method);
  if (request->method == VECTIS_HTTP_HEAD) {
    (void)curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  } else if (request->method == VECTIS_HTTP_POST) {
    (void)curl_easy_setopt(curl, CURLOPT_POST, 1L);
  } else if (request->method != VECTIS_HTTP_GET) {
    (void)curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  }

  if (request_body.json_upload_active) {
    if (!vectis_http_method_upload_capable(request->method)) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      vectis_curl_request_body_cleanup(&request_body);
      free(url);
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "JSON streaming requires an upload-capable HTTP method");
      return VECTIS_ERR_INVALID;
    }
    if (request->method != VECTIS_HTTP_POST) {
      (void)curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    }
    (void)curl_easy_setopt(curl, CURLOPT_READFUNCTION,
                           lonejson_curl_read_callback);
    (void)curl_easy_setopt(curl, CURLOPT_READDATA, &request_body.json_upload);
    if (request->method == VECTIS_HTTP_POST) {
      (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                             request_body.json_size);
    } else {
      (void)curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                             request_body.json_size);
    }
  } else if (request_body.file != NULL) {
    if (!vectis_http_method_upload_capable(request->method)) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      vectis_curl_request_body_cleanup(&request_body);
      free(url);
      vectis_set_error(
          error, VECTIS_ERR_INVALID,
          "body_path streaming requires an upload-capable HTTP method");
      return VECTIS_ERR_INVALID;
    }
    if (request->method != VECTIS_HTTP_POST) {
      (void)curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    }
    (void)curl_easy_setopt(curl, CURLOPT_READFUNCTION, vectis_curl_read_file);
    (void)curl_easy_setopt(curl, CURLOPT_READDATA, request_body.file);
    if (request_body.file_size >= 0L) {
      if (request->method == VECTIS_HTTP_POST) {
        (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                               (curl_off_t)request_body.file_size);
      } else {
        (void)curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                               (curl_off_t)request_body.file_size);
      }
    }
  } else if (request_body.data != NULL || request_body.size > 0u) {
    if (request_body.data == NULL) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      vectis_curl_request_body_cleanup(&request_body);
      free(url);
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "HTTP request body is invalid");
      return VECTIS_ERR_INVALID;
    }
    if (!vectis_http_method_upload_capable(request->method)) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      vectis_curl_request_body_cleanup(&request_body);
      free(url);
      vectis_set_error(
          error, VECTIS_ERR_INVALID,
          "raw request bodies require an upload-capable HTTP method");
      return VECTIS_ERR_INVALID;
    }
    if (request->method == VECTIS_HTTP_POST) {
      (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.data);
      (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                             (curl_off_t)request_body.size);
    } else {
      (void)curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
      (void)curl_easy_setopt(curl, CURLOPT_READFUNCTION,
                             vectis_curl_read_memory);
      (void)curl_easy_setopt(curl, CURLOPT_READDATA, &request_body);
      (void)curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                             (curl_off_t)request_body.size);
    }
  }

  if (request->download_path != NULL) {
    download_file = fopen(request->download_path, "wb");
    if (download_file == NULL) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      vectis_curl_request_body_cleanup(&request_body);
      free(url);
      vectis_set_errorf(error, VECTIS_ERR_INVALID,
                        "failed to open download path: %s",
                        request->download_path);
      return VECTIS_ERR_INVALID;
    }
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, vectis_curl_write_file);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, download_file);
  } else if (request->response_body != NULL) {
    response_stream.callback = request->response_body;
    response_stream.userdata = request->response_body_userdata;
    response_stream.error = error;
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                           vectis_curl_write_stream);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_stream);
  } else {
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                           vectis_curl_write_memory);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);
  }

  if (client->configure_curl != NULL &&
      client->configure_curl(curl, client->configure_curl_userdata, error) !=
          VECTIS_OK) {
    if (download_file != NULL) {
      (void)fclose(download_file);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    vectis_curl_request_body_cleanup(&request_body);
    free(response_buffer.data);
    free(url);
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (request->configure_curl != NULL &&
      request->configure_curl(curl, request->configure_curl_userdata, error) !=
          VECTIS_OK) {
    if (download_file != NULL) {
      (void)fclose(download_file);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    vectis_curl_request_body_cleanup(&request_body);
    free(response_buffer.data);
    free(url);
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }

  curl_code = curl_easy_perform(curl);
  if (header_capture.failed) {
    if (download_file != NULL) {
      (void)fclose(download_file);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    vectis_curl_request_body_cleanup(&request_body);
    free(response_buffer.data);
    free(url);
    vectis_http_response_headers_cleanup(response);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to buffer curl response headers");
    return VECTIS_ERR_NOMEM;
  }
  if (download_file != NULL && fclose(download_file) != 0) {
    download_file = NULL;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    vectis_curl_request_body_cleanup(&request_body);
    free(response_buffer.data);
    free(url);
    vectis_http_response_headers_cleanup(response);
    vectis_set_errorf(error, VECTIS_ERR_STATE,
                      "failed to flush download path: %s",
                      request->download_path);
    return VECTIS_ERR_STATE;
  }
  download_file = NULL;
  if (response_stream.failed) {
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    vectis_curl_request_body_cleanup(&request_body);
    free(response_buffer.data);
    free(url);
    vectis_http_response_headers_cleanup(response);
    if (error != NULL && error->code != VECTIS_OK) {
      return error->code;
    }
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "HTTP response body callback failed");
    return VECTIS_ERR_STATE;
  }
  if (curl_code != CURLE_OK) {
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    vectis_curl_request_body_cleanup(&request_body);
    free(response_buffer.data);
    free(url);
    vectis_http_response_headers_cleanup(response);
    return vectis_curl_set_error(error, curl_code, "curl request failed",
                                 curl_error);
  }
  if (response_buffer.failed) {
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    vectis_curl_request_body_cleanup(&request_body);
    free(response_buffer.data);
    free(url);
    vectis_http_response_headers_cleanup(response);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to buffer curl response body");
    return VECTIS_ERR_NOMEM;
  }

  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status_code);
  (void)curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &response_content_type);
  response->content_type = vectis_strdup(response_content_type);
  if (response_content_type != NULL && response->content_type == NULL) {
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    vectis_curl_request_body_cleanup(&request_body);
    free(response_buffer.data);
    free(url);
    vectis_http_response_headers_cleanup(response);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy response content type");
    return VECTIS_ERR_NOMEM;
  }
  response->body = response_buffer.data;
  response->body_size = response_buffer.size;

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  vectis_curl_request_body_cleanup(&request_body);
  free(url);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_http_execute(const vectis_http_client_config *client,
                                  const vectis_http_request *request,
                                  vectis_http_response *response,
                                  vectis_error *error) {
  vectis_http_client_config defaults;
  vectis_http_client_config effective_client;
  const vectis_http_client_config *client_config;
  vectis_error attempt_error;
  vectis_status status;
  unsigned max_attempts;
  unsigned attempt;
  long delay_ms;
  long max_delay_ms;
  vectis_http_retry_conditions retry_conditions;

  vectis_http_client_config_init(&defaults);
  client_config = client != NULL ? client : &defaults;
  if (request != NULL && (request->retry_initial_delay_ms < 0L ||
                          request->retry_max_delay_ms < 0L)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "HTTP request retry delays must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  if (client_config->timeout_ms < 0L ||
      client_config->connect_timeout_ms < 0L ||
      client_config->low_speed_limit_bytes_per_sec < 0L ||
      client_config->low_speed_time_seconds < 0L ||
      client_config->retry_initial_delay_ms < 0L ||
      client_config->retry_max_delay_ms < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "HTTP client retry delays must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  effective_client = vectis_effective_http_client_config(client_config);

  max_attempts =
      vectis_http_effective_retry_attempts(&effective_client, request);
  retry_conditions =
      vectis_http_effective_retry_conditions(&effective_client, request);
  if (retry_conditions == VECTIS_HTTP_RETRY_NONE) {
    max_attempts = 1u;
  }
  if (max_attempts > 1u && request != NULL && request->response_body != NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "HTTP response streaming cannot be retried safely");
    return VECTIS_ERR_INVALID;
  }

  delay_ms =
      vectis_http_effective_retry_initial_delay(&effective_client, request);
  max_delay_ms =
      vectis_http_effective_retry_max_delay(&effective_client, request);
  if (max_delay_ms > 0L && delay_ms > max_delay_ms) {
    delay_ms = max_delay_ms;
  }

  vectis_error_clear(&attempt_error);
  for (attempt = 1u; attempt <= max_attempts; ++attempt) {
    status = vectis_http_execute_once(&effective_client, request, response,
                                      &attempt_error);
    if (status == VECTIS_OK) {
      if (attempt >= max_attempts ||
          !vectis_http_status_retryable(response->status_code,
                                        retry_conditions)) {
        if (error != NULL) {
          *error = attempt_error;
        }
        return VECTIS_OK;
      }
      vectis_http_response_cleanup(response);
    } else if (attempt >= max_attempts ||
               !vectis_http_error_retryable(&attempt_error, retry_conditions)) {
      if (error != NULL) {
        *error = attempt_error;
      }
      return status;
    }

    if (attempt < max_attempts) {
      vectis_sleep_ms(delay_ms);
      if (delay_ms > 0L) {
        if (delay_ms > LONG_MAX / 2L) {
          delay_ms = max_delay_ms > 0L ? max_delay_ms : delay_ms;
        } else {
          delay_ms *= 2L;
          if (max_delay_ms > 0L && delay_ms > max_delay_ms) {
            delay_ms = max_delay_ms;
          }
        }
      }
    }
  }

  if (error != NULL) {
    *error = attempt_error;
  }
  return VECTIS_ERR_STATE;
}

vectis_status vectis_http_get(const vectis_http_client_config *client,
                              const char *url, vectis_http_response *response,
                              vectis_error *error) {
  vectis_http_request request;

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = url;
  return vectis_http_execute(client, &request, response, error);
}

vectis_status vectis_http_get_json_array(
    const vectis_http_client_config *client, const char *url,
    const char *array_path, const lonejson_map *map, void *item,
    vectis_json_array_item_fn callback, void *userdata,
    vectis_http_response *response, vectis_error *error) {
  vectis_http_request request;
  vectis_http_json_array_stream stream;
  vectis_status status;

  if (map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  if (item == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "JSON array item storage is required");
    return VECTIS_ERR_INVALID;
  }
  if (callback == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "JSON array callback is required");
    return VECTIS_ERR_INVALID;
  }
  memset(&stream, 0, sizeof(stream));
  stream.map = map;
  stream.item = item;
  stream.callback.callback = callback;
  stream.callback.userdata = userdata;
  stream.callback.error = error;
  stream.callback.status = VECTIS_OK;
  stream.runtime = vectis_lonejson_new(error);
  if (stream.runtime == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  stream.stream = lonejson_array_stream_open_push(stream.runtime, array_path,
                                                  &stream.json_error);
  if (stream.stream == NULL) {
    lonejson_free(stream.runtime);
    return vectis_set_lonejson_error(error, LONEJSON_STATUS_INVALID_JSON,
                                     &stream.json_error,
                                     "failed to open HTTP JSON array stream");
  }
  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = url;
  request.response_body = vectis_http_json_array_stream_body;
  request.response_body_userdata = &stream;
  status = vectis_http_execute(client, &request, response, error);
  if (status == VECTIS_OK) {
    stream.json_status = lonejson_array_stream_finish(
        stream.stream, stream.map, stream.item,
        vectis_http_json_array_stream_item, &stream, &stream.json_error);
    if (stream.json_status != LONEJSON_STATUS_OK) {
      if (stream.callback.status != VECTIS_OK) {
        status = stream.callback.status;
      } else {
        status = vectis_set_lonejson_error(
            error, stream.json_status, &stream.json_error,
            "failed to finish HTTP JSON array stream");
      }
    }
  }
  lonejson_array_stream_close(stream.stream);
  lonejson_free(stream.runtime);
  return status;
}

vectis_status vectis_http_delete(const vectis_http_client_config *client,
                                 const char *url,
                                 vectis_http_response *response,
                                 vectis_error *error) {
  vectis_http_request request;

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_DELETE;
  request.url = url;
  return vectis_http_execute(client, &request, response, error);
}

vectis_status vectis_http_head(const vectis_http_client_config *client,
                               const char *url, vectis_http_response *response,
                               vectis_error *error) {
  vectis_http_request request;

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_HEAD;
  request.url = url;
  return vectis_http_execute(client, &request, response, error);
}

vectis_status vectis_http_options(const vectis_http_client_config *client,
                                  const char *url,
                                  vectis_http_response *response,
                                  vectis_error *error) {
  vectis_http_request request;

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_OPTIONS;
  request.url = url;
  return vectis_http_execute(client, &request, response, error);
}

vectis_status vectis_http_download_file(const vectis_http_client_config *client,
                                        const char *url, const char *local_path,
                                        vectis_http_response *response,
                                        vectis_error *error) {
  vectis_http_request request;

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = url;
  request.download_path = local_path;
  return vectis_http_execute(client, &request, response, error);
}

vectis_status vectis_http_upload_file(const vectis_http_client_config *client,
                                      vectis_http_method method,
                                      const char *url, const char *local_path,
                                      const char *content_type,
                                      vectis_http_response *response,
                                      vectis_error *error) {
  vectis_http_request request;

  vectis_http_request_init(&request);
  request.method = method;
  request.url = url;
  request.body_path = local_path;
  request.content_type = content_type;
  return vectis_http_execute(client, &request, response, error);
}

static vectis_status
vectis_http_send_json(const vectis_http_client_config *client,
                      vectis_http_method method, const char *url,
                      const lonejson_map *map, const void *value,
                      vectis_http_response *response, vectis_error *error) {
  vectis_http_request request;

  if (map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  if (value == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json value is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_http_request_init(&request);
  request.method = method;
  request.url = url;
  request.content_type = "application/json";
  request.json_map = map;
  request.json_value = value;
  return vectis_http_execute(client, &request, response, error);
}

vectis_status vectis_http_post_json(const vectis_http_client_config *client,
                                    const char *url, const lonejson_map *map,
                                    const void *value,
                                    vectis_http_response *response,
                                    vectis_error *error) {
  return vectis_http_send_json(client, VECTIS_HTTP_POST, url, map, value,
                               response, error);
}

vectis_status vectis_http_put_json(const vectis_http_client_config *client,
                                   const char *url, const lonejson_map *map,
                                   const void *value,
                                   vectis_http_response *response,
                                   vectis_error *error) {
  return vectis_http_send_json(client, VECTIS_HTTP_PUT, url, map, value,
                               response, error);
}

vectis_status vectis_http_patch_json(const vectis_http_client_config *client,
                                     const char *url, const lonejson_map *map,
                                     const void *value,
                                     vectis_http_response *response,
                                     vectis_error *error) {
  return vectis_http_send_json(client, VECTIS_HTTP_PATCH, url, map, value,
                               response, error);
}

void vectis_sftp_config_init(vectis_sftp_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->timeout_ms = 30000L;
}

static vectis_sftp_config
vectis_effective_sftp_config(const vectis_sftp_config *config) {
  vectis_sftp_config effective;

  vectis_sftp_config_init(&effective);
  if (config != NULL) {
    effective = *config;
    effective.timeout_ms = vectis_default_long(config->timeout_ms, 30000L);
  }
  return effective;
}

static vectis_status vectis_sftp_handle_upload_file(vectis_sftp *self,
                                                    const char *local_path,
                                                    const char *remote_path,
                                                    vectis_error *error) {
  if (self == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SFTP handle is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_sftp_upload_file(&self->config, local_path, remote_path, error);
}

static vectis_status vectis_sftp_handle_download_file(vectis_sftp *self,
                                                      const char *remote_path,
                                                      const char *local_path,
                                                      vectis_error *error) {
  if (self == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SFTP handle is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_sftp_download_file(&self->config, remote_path, local_path,
                                   error);
}

vectis_status vectis_sftp_new(const vectis_sftp_config *config,
                              vectis_sftp **out, vectis_error *error) {
  vectis_sftp_config defaults;
  const vectis_sftp_config *effective;
  vectis_sftp_config normalized;
  vectis_sftp *sftp;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SFTP handle output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  vectis_sftp_config_init(&defaults);
  effective = config != NULL ? config : &defaults;
  normalized = vectis_effective_sftp_config(effective);
  if (effective->url == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SFTP config with url is required");
    return VECTIS_ERR_INVALID;
  }
  if (effective->timeout_ms < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SFTP timeout_ms must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  sftp = (vectis_sftp *)calloc(1u, sizeof(*sftp));
  if (sftp == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate SFTP handle");
    return VECTIS_ERR_NOMEM;
  }
  sftp->upload_file = vectis_sftp_handle_upload_file;
  sftp->download_file = vectis_sftp_handle_download_file;
  sftp->close = vectis_sftp_close;
  sftp->config = normalized;
  *out = sftp;
  vectis_error_clear(error);
  return VECTIS_OK;
}

void vectis_sftp_close(vectis_sftp *sftp) { free(sftp); }

typedef struct vectis_sftp_private_key_file {
  char *path;
  int temporary;
} vectis_sftp_private_key_file;

static void
vectis_sftp_private_key_file_cleanup(vectis_sftp_private_key_file *key_file) {
  if (key_file == NULL) {
    return;
  }
  if (key_file->temporary && key_file->path != NULL) {
    (void)unlink(key_file->path);
  }
  free(key_file->path);
  key_file->path = NULL;
  key_file->temporary = 0;
}

static vectis_status vectis_sftp_private_key_file_prepare(
    const vectis_source *private_key, const char *private_key_path,
    vectis_sftp_private_key_file *out, vectis_error *error) {
  const char *path;
  const char *tmpdir;
  char tmp_template[PATH_MAX];
  const unsigned char *cursor;
  void *key_pem;
  size_t key_pem_size;
  size_t remaining;
  ssize_t nwritten;
  int fd;
  int n;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SFTP private key file output is required");
    return VECTIS_ERR_INVALID;
  }
  out->path = NULL;
  out->temporary = 0;

  path = vectis_source_path_or_old(private_key, private_key_path);
  if (path != NULL) {
    out->path = vectis_strdup(path);
    if (out->path == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy SFTP private key path");
      return VECTIS_ERR_NOMEM;
    }
    return VECTIS_OK;
  }
  if (private_key == NULL ||
      ((private_key->memory == NULL || private_key->memory_size == 0u) &&
       private_key->source == NULL)) {
    return VECTIS_OK;
  }

  key_pem = NULL;
  key_pem_size = 0u;
  if (vectis_read_source_bytes(private_key, &key_pem, &key_pem_size,
                               "SFTP private key", error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }

  tmpdir = getenv("TMPDIR");
  if (tmpdir == NULL || tmpdir[0] == '\0') {
    tmpdir = "/tmp";
  }
  n = snprintf(tmp_template, sizeof(tmp_template), "%s/vectis-sftp-key-XXXXXX",
               tmpdir);
  if (n < 0 || (size_t)n >= sizeof(tmp_template)) {
    free(key_pem);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SFTP private key temp path is too long");
    return VECTIS_ERR_INVALID;
  }
  fd = mkstemp(tmp_template);
  if (fd < 0) {
    free(key_pem);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to create SFTP private key temp file");
    return VECTIS_ERR_STATE;
  }
  (void)chmod(tmp_template, S_IRUSR | S_IWUSR);

  cursor = (const unsigned char *)key_pem;
  remaining = key_pem_size;
  while (remaining > 0u) {
    nwritten = write(fd, cursor, remaining);
    if (nwritten <= 0) {
      (void)close(fd);
      (void)unlink(tmp_template);
      free(key_pem);
      vectis_set_error(error, VECTIS_ERR_STATE,
                       "failed to write SFTP private key temp file");
      return VECTIS_ERR_STATE;
    }
    cursor += (size_t)nwritten;
    remaining -= (size_t)nwritten;
  }
  if (close(fd) != 0) {
    (void)unlink(tmp_template);
    free(key_pem);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to close SFTP private key temp file");
    return VECTIS_ERR_STATE;
  }
  free(key_pem);

  out->path = vectis_strdup(tmp_template);
  if (out->path == NULL) {
    (void)unlink(tmp_template);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy SFTP private key temp path");
    return VECTIS_ERR_NOMEM;
  }
  out->temporary = 1;
  return VECTIS_OK;
}

static vectis_status vectis_curl_set_ssh(CURL *curl, const char *username,
                                         const char *password,
                                         const char *key_path,
                                         const char *known_hosts_path) {
  if (username != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_USERNAME, username);
  }
  if (password != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_PASSWORD, password);
  }
  if (key_path != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_SSH_PRIVATE_KEYFILE, key_path);
  }
  if (known_hosts_path != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_SSH_KNOWNHOSTS, known_hosts_path);
  }
  return VECTIS_OK;
}

vectis_status vectis_sftp_upload_file(const vectis_sftp_config *config,
                                      const char *local_path,
                                      const char *remote_path,
                                      vectis_error *error) {
  CURL *curl;
  CURLcode curl_code;
  FILE *file;
  long file_size;
  char *url;
  char curl_error[CURL_ERROR_SIZE];
  vectis_sftp_config effective;
  vectis_sftp_private_key_file key_file;

  memset(&key_file, 0, sizeof(key_file));
  if (config == NULL || config->url == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SFTP config with url is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->timeout_ms < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SFTP timeout_ms must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  if (local_path == NULL || remote_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SFTP upload requires local_path and remote_path");
    return VECTIS_ERR_INVALID;
  }
  effective = vectis_effective_sftp_config(config);
  memset(curl_error, 0, sizeof(curl_error));
  file = fopen(local_path, "rb");
  if (file == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to open SFTP upload file: %s", local_path);
    return VECTIS_ERR_INVALID;
  }
  if (vectis_file_size(file, &file_size, error) != VECTIS_OK) {
    (void)fclose(file);
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  url = vectis_join_url(config->url, remote_path, error);
  if (url == NULL) {
    (void)fclose(file);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  (void)pthread_once(&vectis_curl_once, vectis_curl_global_init_once);
  curl = curl_easy_init();
  if (curl == NULL) {
    (void)fclose(file);
    free(url);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to initialize curl easy handle");
    return VECTIS_ERR_NOMEM;
  }
  if (vectis_sftp_private_key_file_prepare(&config->private_key,
                                           config->private_key_path, &key_file,
                                           error) != VECTIS_OK) {
    curl_easy_cleanup(curl);
    (void)fclose(file);
    free(url);
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  (void)curl_easy_setopt(curl, CURLOPT_URL, url);
  (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
  if (effective.timeout_ms > 0L) {
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, effective.timeout_ms);
  }
  (void)curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_READFUNCTION, vectis_curl_read_file);
  (void)curl_easy_setopt(curl, CURLOPT_READDATA, file);
  (void)curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)file_size);
  (void)vectis_curl_set_ssh(curl, config->username, config->password,
                            key_file.path, config->known_hosts_path);
  curl_code = curl_easy_perform(curl);
  vectis_sftp_private_key_file_cleanup(&key_file);
  (void)fclose(file);
  curl_easy_cleanup(curl);
  free(url);
  if (curl_code != CURLE_OK) {
    return vectis_curl_set_error(error, curl_code, "curl SFTP upload failed",
                                 curl_error);
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_sftp_download_file(const vectis_sftp_config *config,
                                        const char *remote_path,
                                        const char *local_path,
                                        vectis_error *error) {
  CURL *curl;
  CURLcode curl_code;
  FILE *file;
  char *url;
  char curl_error[CURL_ERROR_SIZE];
  vectis_sftp_config effective;
  vectis_sftp_private_key_file key_file;

  memset(&key_file, 0, sizeof(key_file));
  if (config == NULL || config->url == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SFTP config with url is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->timeout_ms < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SFTP timeout_ms must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  if (remote_path == NULL || local_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SFTP download requires remote_path and local_path");
    return VECTIS_ERR_INVALID;
  }
  effective = vectis_effective_sftp_config(config);
  memset(curl_error, 0, sizeof(curl_error));
  url = vectis_join_url(config->url, remote_path, error);
  if (url == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  file = fopen(local_path, "wb");
  if (file == NULL) {
    free(url);
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to open SFTP download file: %s", local_path);
    return VECTIS_ERR_INVALID;
  }
  (void)pthread_once(&vectis_curl_once, vectis_curl_global_init_once);
  curl = curl_easy_init();
  if (curl == NULL) {
    (void)fclose(file);
    free(url);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to initialize curl easy handle");
    return VECTIS_ERR_NOMEM;
  }
  if (vectis_sftp_private_key_file_prepare(&config->private_key,
                                           config->private_key_path, &key_file,
                                           error) != VECTIS_OK) {
    curl_easy_cleanup(curl);
    (void)fclose(file);
    free(url);
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  (void)curl_easy_setopt(curl, CURLOPT_URL, url);
  (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
  if (effective.timeout_ms > 0L) {
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, effective.timeout_ms);
  }
  (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, vectis_curl_write_file);
  (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
  (void)vectis_curl_set_ssh(curl, config->username, config->password,
                            key_file.path, config->known_hosts_path);
  curl_code = curl_easy_perform(curl);
  vectis_sftp_private_key_file_cleanup(&key_file);
  if (fclose(file) != 0 && curl_code == CURLE_OK) {
    curl_code = CURLE_WRITE_ERROR;
  }
  curl_easy_cleanup(curl);
  free(url);
  if (curl_code != CURLE_OK) {
    return vectis_curl_set_error(error, curl_code, "curl SFTP download failed",
                                 curl_error);
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

void vectis_ssh_config_init(vectis_ssh_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->port = 22u;
  config->timeout_ms = 30000L;
}

static vectis_ssh_config
vectis_effective_ssh_config(const vectis_ssh_config *config) {
  vectis_ssh_config effective;

  vectis_ssh_config_init(&effective);
  if (config != NULL) {
    effective = *config;
    effective.port = vectis_default_ushort(config->port, 22u);
    effective.timeout_ms = vectis_default_long(config->timeout_ms, 30000L);
  }
  return effective;
}

static vectis_status vectis_ssh_handle_exec(vectis_ssh *self,
                                            const char *command,
                                            vectis_ssh_exec_result *result,
                                            vectis_error *error) {
  if (self == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SSH handle is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_ssh_exec(&self->config, command, result, error);
}

static vectis_status vectis_ssh_handle_sftp_upload_file(vectis_ssh *self,
                                                        const char *local_path,
                                                        const char *remote_path,
                                                        vectis_error *error) {
  if (self == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SSH handle is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_ssh_sftp_upload_file(&self->config, local_path, remote_path,
                                     error);
}

static vectis_status
vectis_ssh_handle_sftp_download_file(vectis_ssh *self, const char *remote_path,
                                     const char *local_path,
                                     vectis_error *error) {
  if (self == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SSH handle is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_ssh_sftp_download_file(&self->config, remote_path, local_path,
                                       error);
}

vectis_status vectis_ssh_new(const vectis_ssh_config *config, vectis_ssh **out,
                             vectis_error *error) {
  vectis_ssh_config defaults;
  const vectis_ssh_config *effective;
  vectis_ssh_config normalized;
  vectis_ssh *ssh;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SSH handle output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  vectis_ssh_config_init(&defaults);
  effective = config != NULL ? config : &defaults;
  normalized = vectis_effective_ssh_config(effective);
  if (effective->host == NULL || effective->username == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SSH config requires host and username");
    return VECTIS_ERR_INVALID;
  }
  if (effective->timeout_ms < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SSH timeout_ms must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  ssh = (vectis_ssh *)calloc(1u, sizeof(*ssh));
  if (ssh == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate SSH handle");
    return VECTIS_ERR_NOMEM;
  }
  ssh->exec = vectis_ssh_handle_exec;
  ssh->sftp_upload_file = vectis_ssh_handle_sftp_upload_file;
  ssh->sftp_download_file = vectis_ssh_handle_sftp_download_file;
  ssh->close = vectis_ssh_close;
  ssh->config = normalized;
  *out = ssh;
  vectis_error_clear(error);
  return VECTIS_OK;
}

void vectis_ssh_close(vectis_ssh *ssh) { free(ssh); }

void vectis_ssh_exec_result_cleanup(vectis_ssh_exec_result *result) {
  if (result == NULL) {
    return;
  }
  free(result->stdout_data);
  free(result->stderr_data);
  memset(result, 0, sizeof(*result));
}

static vectis_status vectis_ssh_connect_socket(const vectis_ssh_config *config,
                                               int *out_fd,
                                               vectis_error *error) {
  struct addrinfo hints;
  struct addrinfo *results;
  struct addrinfo *rp;
  char port_text[16];
  int gai_rc;
  int fd;
  struct timeval timeout;

  if (out_fd == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "socket output is required");
    return VECTIS_ERR_INVALID;
  }
  *out_fd = -1;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  (void)snprintf(port_text, sizeof(port_text), "%u", (unsigned)config->port);
  results = NULL;
  gai_rc = getaddrinfo(config->host, port_text, &hints, &results);
  if (gai_rc != 0) {
    vectis_set_errorf(error, VECTIS_ERR_STATE, "failed to resolve SSH host: %s",
                      gai_strerror(gai_rc));
    return VECTIS_ERR_STATE;
  }
  fd = -1;
  for (rp = results; rp != NULL; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0) {
      continue;
    }
    if (config->timeout_ms > 0L) {
      timeout.tv_sec = config->timeout_ms / 1000L;
      timeout.tv_usec = (config->timeout_ms % 1000L) * 1000L;
      (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }
    if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;
    }
    (void)close(fd);
    fd = -1;
  }
  freeaddrinfo(results);
  if (fd < 0) {
    vectis_set_errorf(error, VECTIS_ERR_STATE,
                      "failed to connect to SSH host %s:%u", config->host,
                      (unsigned)config->port);
    return VECTIS_ERR_STATE;
  }
  *out_fd = fd;
  return VECTIS_OK;
}

static int vectis_ssh_knownhost_key_type(int hostkey_type) {
  switch (hostkey_type) {
  case LIBSSH2_HOSTKEY_TYPE_RSA:
    return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
  case LIBSSH2_HOSTKEY_TYPE_DSS:
    return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
  case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
    return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
  case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
    return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
  case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
    return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
  case LIBSSH2_HOSTKEY_TYPE_ED25519:
    return LIBSSH2_KNOWNHOST_KEY_ED25519;
  default:
    return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
  }
}

static vectis_status
vectis_ssh_verify_known_host(LIBSSH2_SESSION *session,
                             const vectis_ssh_config *config,
                             vectis_error *error) {
  LIBSSH2_KNOWNHOSTS *known_hosts;
  const char *host_key;
  size_t host_key_size;
  int host_key_type;
  int type_mask;
  int rc;

  if (config->known_hosts_path == NULL || config->known_hosts_path[0] == '\0') {
    return VECTIS_OK;
  }
  known_hosts = libssh2_knownhost_init(session);
  if (known_hosts == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to initialize SSH known_hosts verifier");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
    }
    return VECTIS_ERR_NOMEM;
  }
  rc = libssh2_knownhost_readfile(known_hosts, config->known_hosts_path,
                                  LIBSSH2_KNOWNHOST_FILE_OPENSSH);
  if (rc < 0) {
    libssh2_knownhost_free(known_hosts);
    vectis_set_errorf(error, VECTIS_ERR_STATE,
                      "failed to read SSH known_hosts file: %s",
                      config->known_hosts_path);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
      error->dependency_code = (long)rc;
    }
    return VECTIS_ERR_STATE;
  }
  host_key = libssh2_session_hostkey(session, &host_key_size, &host_key_type);
  if (host_key == NULL || host_key_size == 0u) {
    libssh2_knownhost_free(known_hosts);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to read SSH server host key");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
    }
    return VECTIS_ERR_STATE;
  }
  type_mask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW |
              vectis_ssh_knownhost_key_type(host_key_type);
  rc = libssh2_knownhost_checkp(known_hosts, config->host, (int)config->port,
                                host_key, host_key_size, type_mask, NULL);
  libssh2_knownhost_free(known_hosts);
  if (rc != LIBSSH2_KNOWNHOST_CHECK_MATCH) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "SSH host key verification failed");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
      error->dependency_code = (long)rc;
    }
    return VECTIS_ERR_STATE;
  }
  return VECTIS_OK;
}

static vectis_status vectis_ssh_authenticate(LIBSSH2_SESSION *session,
                                             const vectis_ssh_config *config,
                                             vectis_error *error) {
  const char *key_path;
  void *key_pem;
  size_t key_pem_size;
  int rc;

  key_path =
      vectis_source_path_or_old(&config->private_key, config->private_key_path);
  key_pem = NULL;
  key_pem_size = 0u;
  if (key_path != NULL) {
    rc = libssh2_userauth_publickey_fromfile(session, config->username, NULL,
                                             key_path, config->password);
  } else if (config->private_key.memory != NULL ||
             config->private_key.source != NULL) {
    if (vectis_read_source_bytes(&config->private_key, &key_pem, &key_pem_size,
                                 "SSH private key", error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
    rc = libssh2_userauth_publickey_frommemory(
        session, config->username, strlen(config->username), NULL, 0u,
        (const char *)key_pem, key_pem_size, config->password);
    free(key_pem);
  } else if (config->password != NULL) {
    rc = libssh2_userauth_password(session, config->username, config->password);
  } else {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SSH authentication requires password or private key");
    return VECTIS_ERR_INVALID;
  }
  if (rc != 0) {
    vectis_set_error(error, VECTIS_ERR_STATE, "SSH authentication failed");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
      error->dependency_code = (long)rc;
    }
    return VECTIS_ERR_STATE;
  }
  return VECTIS_OK;
}

static vectis_status vectis_ssh_read_channel(LIBSSH2_CHANNEL *channel,
                                             vectis_ssh_exec_result *result,
                                             vectis_error *error) {
  char buffer[8192];
  ssize_t n;
  ssize_t nerr;
  int active;
  vectis_status status;

  active = 1;
  while (active) {
    active = 0;
    n = libssh2_channel_read(channel, buffer, sizeof(buffer));
    while (n > 0) {
      status = vectis_append_output(&result->stdout_data, &result->stdout_size,
                                    buffer, (size_t)n, error);
      if (status != VECTIS_OK) {
        return status;
      }
      active = 1;
      n = libssh2_channel_read(channel, buffer, sizeof(buffer));
    }
    nerr = libssh2_channel_read_stderr(channel, buffer, sizeof(buffer));
    while (nerr > 0) {
      status = vectis_append_output(&result->stderr_data, &result->stderr_size,
                                    buffer, (size_t)nerr, error);
      if (status != VECTIS_OK) {
        return status;
      }
      active = 1;
      nerr = libssh2_channel_read_stderr(channel, buffer, sizeof(buffer));
    }
    if (libssh2_channel_eof(channel)) {
      break;
    }
    if (n == LIBSSH2_ERROR_EAGAIN || nerr == LIBSSH2_ERROR_EAGAIN || active) {
      continue;
    }
    if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) {
      vectis_set_error(error, VECTIS_ERR_STATE, "failed to read SSH stdout");
      return VECTIS_ERR_STATE;
    }
    if (nerr < 0 && nerr != LIBSSH2_ERROR_EAGAIN) {
      vectis_set_error(error, VECTIS_ERR_STATE, "failed to read SSH stderr");
      return VECTIS_ERR_STATE;
    }
  }
  return VECTIS_OK;
}

vectis_status vectis_ssh_exec(const vectis_ssh_config *config,
                              const char *command,
                              vectis_ssh_exec_result *result,
                              vectis_error *error) {
  vectis_ssh_config effective;
  LIBSSH2_SESSION *session;
  LIBSSH2_CHANNEL *channel;
  int fd;
  int rc;
  vectis_status status;

  if (config == NULL || config->host == NULL || config->username == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SSH config requires host and username");
    return VECTIS_ERR_INVALID;
  }
  if (config->timeout_ms < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SSH timeout_ms must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  if (command == NULL || command[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SSH command is required");
    return VECTIS_ERR_INVALID;
  }
  if (result == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SSH exec result is required");
    return VECTIS_ERR_INVALID;
  }
  memset(result, 0, sizeof(*result));
  fd = -1;
  session = NULL;
  channel = NULL;
  effective = vectis_effective_ssh_config(config);
  config = &effective;

  status = vectis_ssh_connect_socket(config, &fd, error);
  if (status != VECTIS_OK) {
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
    }
    return status;
  }

  (void)pthread_once(&vectis_libssh2_once, vectis_libssh2_global_init_once);
  session = libssh2_session_init();
  if (session == NULL) {
    (void)close(fd);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to initialize SSH session");
    return VECTIS_ERR_NOMEM;
  }
  if (config->timeout_ms > 0L) {
    libssh2_session_set_timeout(session, (long)config->timeout_ms);
  }
  rc = libssh2_session_handshake(session, fd);
  if (rc != 0) {
    libssh2_session_free(session);
    (void)close(fd);
    vectis_set_error(error, VECTIS_ERR_STATE, "SSH handshake failed");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
      error->dependency_code = (long)rc;
    }
    return VECTIS_ERR_STATE;
  }
  status = vectis_ssh_verify_known_host(session, config, error);
  if (status != VECTIS_OK) {
    libssh2_session_disconnect(session, "vectis shutdown");
    libssh2_session_free(session);
    (void)close(fd);
    return status;
  }
  status = vectis_ssh_authenticate(session, config, error);
  if (status != VECTIS_OK) {
    libssh2_session_disconnect(session, "vectis shutdown");
    libssh2_session_free(session);
    (void)close(fd);
    return status;
  }
  channel = libssh2_channel_open_session(session);
  if (channel == NULL) {
    libssh2_session_disconnect(session, "vectis shutdown");
    libssh2_session_free(session);
    (void)close(fd);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to open SSH session channel");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
    }
    return VECTIS_ERR_STATE;
  }
  rc = libssh2_channel_exec(channel, command);
  if (rc != 0) {
    libssh2_channel_free(channel);
    libssh2_session_disconnect(session, "vectis shutdown");
    libssh2_session_free(session);
    (void)close(fd);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to execute SSH command");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
      error->dependency_code = (long)rc;
    }
    return VECTIS_ERR_STATE;
  }
  status = vectis_ssh_read_channel(channel, result, error);
  result->exit_status = libssh2_channel_get_exit_status(channel);
  (void)libssh2_channel_close(channel);
  libssh2_channel_free(channel);
  libssh2_session_disconnect(session, "vectis shutdown");
  libssh2_session_free(session);
  (void)close(fd);
  if (status != VECTIS_OK) {
    vectis_ssh_exec_result_cleanup(result);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
    }
    return status;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_ssh_sftp_upload_file(const vectis_ssh_config *config,
                                          const char *local_path,
                                          const char *remote_path,
                                          vectis_error *error) {
  vectis_ssh_config effective;
  int fd;
  int rc;
  FILE *local;
  LIBSSH2_SESSION *session;
  LIBSSH2_SFTP *sftp;
  LIBSSH2_SFTP_HANDLE *remote;
  char buffer[32768];
  size_t nread;
  char *cursor;
  size_t remaining;
  ssize_t nwritten;
  vectis_status status;

  if (config == NULL || config->host == NULL || config->username == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SSH config requires host and username");
    return VECTIS_ERR_INVALID;
  }
  if (config->timeout_ms < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SSH timeout_ms must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  if (local_path == NULL || remote_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SSH SFTP upload requires local_path and remote_path");
    return VECTIS_ERR_INVALID;
  }
  effective = vectis_effective_ssh_config(config);
  config = &effective;
  fd = -1;
  session = NULL;
  sftp = NULL;
  remote = NULL;
  local = fopen(local_path, "rb");
  if (local == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to open SSH SFTP upload file: %s", local_path);
    return VECTIS_ERR_INVALID;
  }
  status = vectis_ssh_connect_socket(config, &fd, error);
  if (status != VECTIS_OK) {
    (void)fclose(local);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
    }
    return status;
  }
  (void)pthread_once(&vectis_libssh2_once, vectis_libssh2_global_init_once);
  session = libssh2_session_init();
  if (session == NULL) {
    (void)fclose(local);
    (void)close(fd);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to initialize SSH session");
    return VECTIS_ERR_NOMEM;
  }
  if (config->timeout_ms > 0L) {
    libssh2_session_set_timeout(session, (long)config->timeout_ms);
  }
  rc = libssh2_session_handshake(session, fd);
  if (rc != 0) {
    (void)fclose(local);
    libssh2_session_free(session);
    (void)close(fd);
    vectis_set_error(error, VECTIS_ERR_STATE, "SSH handshake failed");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
      error->dependency_code = (long)rc;
    }
    return VECTIS_ERR_STATE;
  }
  status = vectis_ssh_verify_known_host(session, config, error);
  if (status != VECTIS_OK) {
    (void)fclose(local);
    libssh2_session_disconnect(session, "vectis shutdown");
    libssh2_session_free(session);
    (void)close(fd);
    return status;
  }
  status = vectis_ssh_authenticate(session, config, error);
  if (status != VECTIS_OK) {
    (void)fclose(local);
    libssh2_session_disconnect(session, "vectis shutdown");
    libssh2_session_free(session);
    (void)close(fd);
    return status;
  }
  sftp = libssh2_sftp_init(session);
  if (sftp == NULL) {
    (void)fclose(local);
    libssh2_session_disconnect(session, "vectis shutdown");
    libssh2_session_free(session);
    (void)close(fd);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to initialize SSH SFTP session");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
    }
    return VECTIS_ERR_STATE;
  }
  remote = libssh2_sftp_open(
      sftp, remote_path,
      LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, 0644);
  if (remote == NULL) {
    libssh2_sftp_shutdown(sftp);
    (void)fclose(local);
    libssh2_session_disconnect(session, "vectis shutdown");
    libssh2_session_free(session);
    (void)close(fd);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to open remote SFTP file for upload");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
    }
    return VECTIS_ERR_STATE;
  }
  while ((nread = fread(buffer, 1u, sizeof(buffer), local)) > 0u) {
    cursor = buffer;
    remaining = nread;
    while (remaining > 0u) {
      nwritten = libssh2_sftp_write(remote, cursor, remaining);
      if (nwritten < 0) {
        libssh2_sftp_close(remote);
        libssh2_sftp_shutdown(sftp);
        (void)fclose(local);
        libssh2_session_disconnect(session, "vectis shutdown");
        libssh2_session_free(session);
        (void)close(fd);
        vectis_set_error(error, VECTIS_ERR_STATE,
                         "failed to write remote SFTP file");
        if (error != NULL) {
          error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
          error->dependency_code = (long)nwritten;
        }
        return VECTIS_ERR_STATE;
      }
      cursor += nwritten;
      remaining -= (size_t)nwritten;
    }
  }
  if (ferror(local)) {
    status = VECTIS_ERR_STATE;
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to read local SFTP upload file");
  } else {
    status = VECTIS_OK;
    vectis_error_clear(error);
  }
  libssh2_sftp_close(remote);
  libssh2_sftp_shutdown(sftp);
  (void)fclose(local);
  libssh2_session_disconnect(session, "vectis shutdown");
  libssh2_session_free(session);
  (void)close(fd);
  return status;
}

vectis_status vectis_ssh_sftp_download_file(const vectis_ssh_config *config,
                                            const char *remote_path,
                                            const char *local_path,
                                            vectis_error *error) {
  vectis_ssh_config effective;
  int fd;
  int rc;
  FILE *local;
  LIBSSH2_SESSION *session;
  LIBSSH2_SFTP *sftp;
  LIBSSH2_SFTP_HANDLE *remote;
  char buffer[32768];
  ssize_t nread;
  vectis_status status;

  if (config == NULL || config->host == NULL || config->username == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SSH config requires host and username");
    return VECTIS_ERR_INVALID;
  }
  if (config->timeout_ms < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SSH timeout_ms must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  if (remote_path == NULL || local_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "SSH SFTP download requires remote_path and local_path");
    return VECTIS_ERR_INVALID;
  }
  effective = vectis_effective_ssh_config(config);
  config = &effective;
  fd = -1;
  session = NULL;
  sftp = NULL;
  remote = NULL;
  local = fopen(local_path, "wb");
  if (local == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to open SSH SFTP download file: %s", local_path);
    return VECTIS_ERR_INVALID;
  }
  status = vectis_ssh_connect_socket(config, &fd, error);
  if (status != VECTIS_OK) {
    (void)fclose(local);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
    }
    return status;
  }
  (void)pthread_once(&vectis_libssh2_once, vectis_libssh2_global_init_once);
  session = libssh2_session_init();
  if (session == NULL) {
    (void)fclose(local);
    (void)close(fd);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to initialize SSH session");
    return VECTIS_ERR_NOMEM;
  }
  if (config->timeout_ms > 0L) {
    libssh2_session_set_timeout(session, (long)config->timeout_ms);
  }
  rc = libssh2_session_handshake(session, fd);
  if (rc != 0) {
    (void)fclose(local);
    libssh2_session_free(session);
    (void)close(fd);
    vectis_set_error(error, VECTIS_ERR_STATE, "SSH handshake failed");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
      error->dependency_code = (long)rc;
    }
    return VECTIS_ERR_STATE;
  }
  status = vectis_ssh_verify_known_host(session, config, error);
  if (status != VECTIS_OK) {
    (void)fclose(local);
    libssh2_session_disconnect(session, "vectis shutdown");
    libssh2_session_free(session);
    (void)close(fd);
    return status;
  }
  status = vectis_ssh_authenticate(session, config, error);
  if (status != VECTIS_OK) {
    (void)fclose(local);
    libssh2_session_disconnect(session, "vectis shutdown");
    libssh2_session_free(session);
    (void)close(fd);
    return status;
  }
  sftp = libssh2_sftp_init(session);
  if (sftp == NULL) {
    (void)fclose(local);
    libssh2_session_disconnect(session, "vectis shutdown");
    libssh2_session_free(session);
    (void)close(fd);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to initialize SSH SFTP session");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
    }
    return VECTIS_ERR_STATE;
  }
  remote = libssh2_sftp_open(sftp, remote_path, LIBSSH2_FXF_READ, 0);
  if (remote == NULL) {
    libssh2_sftp_shutdown(sftp);
    (void)fclose(local);
    libssh2_session_disconnect(session, "vectis shutdown");
    libssh2_session_free(session);
    (void)close(fd);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to open remote SFTP file for download");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
    }
    return VECTIS_ERR_STATE;
  }
  status = VECTIS_OK;
  nread = libssh2_sftp_read(remote, buffer, sizeof(buffer));
  while (nread > 0) {
    if (fwrite(buffer, 1u, (size_t)nread, local) != (size_t)nread) {
      status = VECTIS_ERR_STATE;
      vectis_set_error(error, VECTIS_ERR_STATE,
                       "failed to write local SFTP download file");
      break;
    }
    nread = libssh2_sftp_read(remote, buffer, sizeof(buffer));
  }
  if (status == VECTIS_OK && nread < 0) {
    status = VECTIS_ERR_STATE;
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to read remote SFTP file");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
      error->dependency_code = (long)nread;
    }
  }
  if (fclose(local) != 0 && status == VECTIS_OK) {
    status = VECTIS_ERR_STATE;
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to flush local SFTP download file");
  }
  libssh2_sftp_close(remote);
  libssh2_sftp_shutdown(sftp);
  libssh2_session_disconnect(session, "vectis shutdown");
  libssh2_session_free(session);
  (void)close(fd);
  if (status == VECTIS_OK) {
    vectis_error_clear(error);
  }
  return status;
}

void vectis_mqtt_config_init(vectis_mqtt_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->timeout_ms = 30000L;
}

static vectis_mqtt_config
vectis_effective_mqtt_config(const vectis_mqtt_config *config) {
  vectis_mqtt_config effective;

  vectis_mqtt_config_init(&effective);
  if (config != NULL) {
    effective = *config;
    effective.timeout_ms = vectis_default_long(config->timeout_ms, 30000L);
  }
  return effective;
}

static vectis_status
vectis_mqtt_handle_publish(vectis_mqtt *self, const char *topic,
                           const void *payload, size_t payload_size,
                           const char *content_type, vectis_error *error) {
  if (self == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "MQTT handle is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_mqtt_publish(&self->config, topic, payload, payload_size,
                             content_type, error);
}

static vectis_status vectis_mqtt_handle_publish_json(vectis_mqtt *self,
                                                     const char *topic,
                                                     const lonejson_map *map,
                                                     const void *value,
                                                     vectis_error *error) {
  if (self == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "MQTT handle is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_mqtt_publish_json(&self->config, topic, map, value, error);
}

vectis_status vectis_mqtt_new(const vectis_mqtt_config *config,
                              vectis_mqtt **out, vectis_error *error) {
  vectis_mqtt_config defaults;
  const vectis_mqtt_config *effective;
  vectis_mqtt_config normalized;
  vectis_mqtt *mqtt;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "MQTT handle output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  vectis_mqtt_config_init(&defaults);
  effective = config != NULL ? config : &defaults;
  normalized = vectis_effective_mqtt_config(effective);
  if (effective->broker_url == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "MQTT config with broker_url is required");
    return VECTIS_ERR_INVALID;
  }
  if (effective->timeout_ms < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "MQTT timeout_ms must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  mqtt = (vectis_mqtt *)calloc(1u, sizeof(*mqtt));
  if (mqtt == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate MQTT handle");
    return VECTIS_ERR_NOMEM;
  }
  mqtt->publish = vectis_mqtt_handle_publish;
  mqtt->publish_json = vectis_mqtt_handle_publish_json;
  mqtt->close = vectis_mqtt_close;
  mqtt->config = normalized;
  *out = mqtt;
  vectis_error_clear(error);
  return VECTIS_OK;
}

void vectis_mqtt_close(vectis_mqtt *mqtt) { free(mqtt); }

vectis_status vectis_mqtt_publish(const vectis_mqtt_config *config,
                                  const char *topic, const void *payload,
                                  size_t payload_size, const char *content_type,
                                  vectis_error *error) {
  CURL *curl;
  CURLcode curl_code;
  char *url;
  char curl_error[CURL_ERROR_SIZE];
  vectis_mqtt_config effective;

  (void)content_type;
  if (config == NULL || config->broker_url == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "MQTT config with broker_url is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->timeout_ms < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "MQTT timeout_ms must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  if (topic == NULL || topic[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "MQTT topic is required");
    return VECTIS_ERR_INVALID;
  }
  if (payload == NULL && payload_size > 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "MQTT payload is invalid");
    return VECTIS_ERR_INVALID;
  }
  effective = vectis_effective_mqtt_config(config);
  memset(curl_error, 0, sizeof(curl_error));
  url = vectis_join_url(effective.broker_url, topic, error);
  if (url == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  (void)pthread_once(&vectis_curl_once, vectis_curl_global_init_once);
  curl = curl_easy_init();
  if (curl == NULL) {
    free(url);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to initialize curl easy handle");
    return VECTIS_ERR_NOMEM;
  }
  (void)curl_easy_setopt(curl, CURLOPT_URL, url);
  (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
  if (effective.timeout_ms > 0L) {
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, effective.timeout_ms);
  }
  if (effective.username != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_USERNAME, effective.username);
  }
  if (effective.password != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_PASSWORD, effective.password);
  }
  if (vectis_curl_set_common_tls(
          curl, &effective.client_bundle, effective.client_bundle_path,
          effective.client_bundle_pem, effective.client_bundle_pem_size,
          &effective.ca_bundle, effective.ca_bundle_path, error) != VECTIS_OK) {
    curl_easy_cleanup(curl);
    free(url);
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (payload != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                           (curl_off_t)payload_size);
  } else {
    (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)0);
  }
  curl_code = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  free(url);
  if (curl_code != CURLE_OK) {
    return vectis_curl_set_error(error, curl_code, "curl MQTT publish failed",
                                 curl_error);
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_mqtt_publish_json(const vectis_mqtt_config *config,
                                       const char *topic,
                                       const lonejson_map *map,
                                       const void *value, vectis_error *error) {
  lonejson_error json_error;
  lonejson *runtime;
  char *json;
  size_t json_size;
  vectis_status status;

  if (map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  if (value == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json value is required");
    return VECTIS_ERR_INVALID;
  }
  runtime = vectis_lonejson_new(error);
  if (runtime == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  json = lonejson_serialize_alloc(runtime, map, value, &json_size, &json_error);
  lonejson_free(runtime);
  if (json == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to serialize MQTT JSON payload: %s",
                      json_error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
    }
    return VECTIS_ERR_INVALID;
  }
  status = vectis_mqtt_publish(config, topic, json, json_size,
                               "application/json", error);
  free(json);
  return status;
}

void vectis_cert_subject_init(vectis_cert_subject *subject) {
  if (subject == NULL) {
    return;
  }
  memset(subject, 0, sizeof(*subject));
}

void vectis_cert_bundle_config_init(vectis_cert_bundle_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->key_bits = 4096u;
  config->valid_days = 397L;
}

void vectis_private_key_config_init(vectis_private_key_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->key_bits = 4096u;
}

void vectis_csr_config_init(vectis_csr_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  vectis_source_init(&config->private_key);
}

static int vectis_cert_add_name_entry(X509_NAME *name, const char *field,
                                      const char *value) {
  if (value == NULL || value[0] == '\0') {
    return 1;
  }
  return X509_NAME_add_entry_by_txt(name, field, MBSTRING_ASC,
                                    (const unsigned char *)value, -1, -1,
                                    0) == 1;
}

static vectis_status vectis_cert_set_name(X509_NAME *name,
                                          const vectis_cert_subject *subject,
                                          const char *label,
                                          vectis_error *error) {
  if (!vectis_cert_add_name_entry(name, "CN", subject->common_name) ||
      !vectis_cert_add_name_entry(name, "O", subject->organization) ||
      !vectis_cert_add_name_entry(name, "OU", subject->organizational_unit) ||
      !vectis_cert_add_name_entry(name, "C", subject->country) ||
      !vectis_cert_add_name_entry(name, "ST", subject->state) ||
      !vectis_cert_add_name_entry(name, "L", subject->locality)) {
    vectis_set_errorf(error, VECTIS_ERR_STATE, "failed to set %s subject",
                      label);
    return VECTIS_ERR_STATE;
  }
  return VECTIS_OK;
}

static vectis_status vectis_cert_set_subject(X509 *cert,
                                             const vectis_cert_subject *subject,
                                             vectis_error *error) {
  X509_NAME *name;

  name = X509_get_subject_name(cert);
  if (name == NULL) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to allocate certificate subject");
    return VECTIS_ERR_STATE;
  }
  return vectis_cert_set_name(name, subject, "certificate", error);
}

static char *vectis_cert_san_string(const char *dns_names,
                                    const char *ip_addresses,
                                    vectis_error *error) {
  size_t len;
  size_t out_len;
  const char *p;
  const char *start;
  char *out;
  const char *prefix;
  const char *inputs[2];
  size_t i;
  size_t part_len;

  inputs[0] = dns_names;
  inputs[1] = ip_addresses;
  len = 1u;
  for (i = 0u; i < 2u; ++i) {
    p = inputs[i];
    prefix = i == 0u ? "DNS:" : "IP:";
    while (p != NULL && *p != '\0') {
      while (*p == ',' || *p == ' ') {
        p++;
      }
      start = p;
      while (*p != '\0' && *p != ',') {
        p++;
      }
      if (p > start) {
        len += strlen(prefix) + (size_t)(p - start) + 1u;
      }
    }
  }
  if (len == 1u) {
    return NULL;
  }
  out = (char *)malloc(len);
  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate subjectAltName");
    return NULL;
  }
  out[0] = '\0';
  out_len = 0u;
  for (i = 0u; i < 2u; ++i) {
    p = inputs[i];
    prefix = i == 0u ? "DNS:" : "IP:";
    while (p != NULL && *p != '\0') {
      while (*p == ',' || *p == ' ') {
        p++;
      }
      start = p;
      while (*p != '\0' && *p != ',') {
        p++;
      }
      if (p > start) {
        if (out_len > 0u) {
          out[out_len] = ',';
          out_len++;
        }
        part_len = strlen(prefix);
        memcpy(out + out_len, prefix, part_len);
        out_len += part_len;
        part_len = (size_t)(p - start);
        memcpy(out + out_len, start, part_len);
        out_len += part_len;
        out[out_len] = '\0';
      }
    }
  }
  return out;
}

static vectis_status vectis_cert_add_extension(X509 *cert, int nid,
                                               const char *value,
                                               vectis_error *error) {
  X509V3_CTX ctx;
  X509_EXTENSION *extension;
  char *value_copy;

  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, cert, cert, NULL, NULL, 0);
  value_copy = vectis_strdup(value);
  if (value_copy == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy certificate extension value");
    return VECTIS_ERR_NOMEM;
  }
  extension = X509V3_EXT_conf_nid(NULL, &ctx, nid, value_copy);
  free(value_copy);
  if (extension == NULL) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to create certificate extension");
    return VECTIS_ERR_STATE;
  }
  if (X509_add_ext(cert, extension, -1) != 1) {
    X509_EXTENSION_free(extension);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to add certificate extension");
    return VECTIS_ERR_STATE;
  }
  X509_EXTENSION_free(extension);
  return VECTIS_OK;
}

static EVP_PKEY *vectis_cert_generate_rsa_key(unsigned key_bits,
                                              vectis_error *error) {
  EVP_PKEY_CTX *key_ctx;
  EVP_PKEY *key;

  key = NULL;
  key_ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
  if (key_ctx == NULL || EVP_PKEY_keygen_init(key_ctx) <= 0 ||
      EVP_PKEY_CTX_set_rsa_keygen_bits(key_ctx, (int)key_bits) <= 0 ||
      EVP_PKEY_keygen(key_ctx, &key) <= 0) {
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(key_ctx);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to generate RSA private key");
    return NULL;
  }
  EVP_PKEY_CTX_free(key_ctx);
  return key;
}

static vectis_status
vectis_cert_write_outputs(const vectis_cert_bundle_config *config,
                          EVP_PKEY *key, X509 *cert, vectis_error *error) {
  FILE *fp;

  if (config->output_bundle_path != NULL) {
    fp = fopen(config->output_bundle_path, "wb");
    if (fp == NULL) {
      vectis_set_errorf(error, VECTIS_ERR_INVALID,
                        "failed to open certificate bundle output: %s",
                        config->output_bundle_path);
      return VECTIS_ERR_INVALID;
    }
    if (PEM_write_X509(fp, cert) != 1 ||
        PEM_write_PrivateKey(fp, key, NULL, NULL, 0, NULL, NULL) != 1) {
      (void)fclose(fp);
      vectis_set_error(error, VECTIS_ERR_STATE,
                       "failed to write certificate bundle");
      return VECTIS_ERR_STATE;
    }
    if (fclose(fp) != 0) {
      vectis_set_error(error, VECTIS_ERR_STATE,
                       "failed to close certificate bundle");
      return VECTIS_ERR_STATE;
    }
  }
  if (config->output_cert_path != NULL) {
    fp = fopen(config->output_cert_path, "wb");
    if (fp == NULL) {
      vectis_set_errorf(error, VECTIS_ERR_INVALID,
                        "failed to open certificate output: %s",
                        config->output_cert_path);
      return VECTIS_ERR_INVALID;
    }
    if (PEM_write_X509(fp, cert) != 1) {
      (void)fclose(fp);
      vectis_set_error(error, VECTIS_ERR_STATE, "failed to write certificate");
      return VECTIS_ERR_STATE;
    }
    if (fclose(fp) != 0) {
      vectis_set_error(error, VECTIS_ERR_STATE, "failed to close certificate");
      return VECTIS_ERR_STATE;
    }
  }
  if (config->output_key_path != NULL) {
    fp = fopen(config->output_key_path, "wb");
    if (fp == NULL) {
      vectis_set_errorf(error, VECTIS_ERR_INVALID,
                        "failed to open private key output: %s",
                        config->output_key_path);
      return VECTIS_ERR_INVALID;
    }
    if (PEM_write_PrivateKey(fp, key, NULL, NULL, 0, NULL, NULL) != 1) {
      (void)fclose(fp);
      vectis_set_error(error, VECTIS_ERR_STATE, "failed to write private key");
      return VECTIS_ERR_STATE;
    }
    if (fclose(fp) != 0) {
      vectis_set_error(error, VECTIS_ERR_STATE, "failed to close private key");
      return VECTIS_ERR_STATE;
    }
  }
  return VECTIS_OK;
}

static vectis_status
vectis_cert_load_ca(const vectis_cert_bundle_config *config, X509 **out_cert,
                    EVP_PKEY **out_key, vectis_error *error) {
  FILE *fp;

  if (config->ca_cert_path == NULL && config->ca_key_path == NULL) {
    *out_cert = NULL;
    *out_key = NULL;
    return VECTIS_OK;
  }
  if (config->ca_cert_path == NULL || config->ca_key_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "CA-signed certificate generation requires ca_cert_path "
                     "and ca_key_path");
    return VECTIS_ERR_INVALID;
  }

  fp = fopen(config->ca_cert_path, "rb");
  if (fp == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to open CA certificate: %s",
                      config->ca_cert_path);
    return VECTIS_ERR_INVALID;
  }
  *out_cert = PEM_read_X509(fp, NULL, NULL, NULL);
  (void)fclose(fp);
  if (*out_cert == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "failed to parse CA certificate");
    return VECTIS_ERR_INVALID;
  }

  fp = fopen(config->ca_key_path, "rb");
  if (fp == NULL) {
    X509_free(*out_cert);
    *out_cert = NULL;
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to open CA private key: %s", config->ca_key_path);
    return VECTIS_ERR_INVALID;
  }
  *out_key = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
  (void)fclose(fp);
  if (*out_key == NULL) {
    X509_free(*out_cert);
    *out_cert = NULL;
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "failed to parse CA private key");
    return VECTIS_ERR_INVALID;
  }
  if (X509_check_private_key(*out_cert, *out_key) != 1) {
    X509_free(*out_cert);
    EVP_PKEY_free(*out_key);
    *out_cert = NULL;
    *out_key = NULL;
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "CA certificate and private key do not match");
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static vectis_status vectis_read_source_bytes(const vectis_source *source,
                                              void **out, size_t *out_size,
                                              const char *label,
                                              vectis_error *error) {
  FILE *fp;
  long length;
  void *buffer;
  size_t nread;
  unsigned char chunk[4096];
  unsigned char *grown;
  size_t size;
  size_t capacity;
  lc_error lcerr;

  if (source == NULL || out == NULL || out_size == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID, "%s source is required",
                      label);
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  *out_size = 0u;
  if (source->memory != NULL && source->memory_size > 0u) {
    buffer = malloc(source->memory_size);
    if (buffer == NULL) {
      vectis_set_errorf(error, VECTIS_ERR_NOMEM, "failed to copy %s source",
                        label);
      return VECTIS_ERR_NOMEM;
    }
    memcpy(buffer, source->memory, source->memory_size);
    *out = buffer;
    *out_size = source->memory_size;
    return VECTIS_OK;
  }
  if (source->path != NULL) {
    fp = fopen(source->path, "rb");
    if (fp == NULL) {
      vectis_set_errorf(error, VECTIS_ERR_INVALID, "failed to open %s: %s",
                        label, source->path);
      return VECTIS_ERR_INVALID;
    }
    if (fseek(fp, 0L, SEEK_END) != 0) {
      (void)fclose(fp);
      vectis_set_errorf(error, VECTIS_ERR_STATE, "failed to seek %s: %s", label,
                        source->path);
      return VECTIS_ERR_STATE;
    }
    length = ftell(fp);
    if (length <= 0L || fseek(fp, 0L, SEEK_SET) != 0) {
      (void)fclose(fp);
      vectis_set_errorf(error, VECTIS_ERR_INVALID,
                        "%s is empty or unreadable: %s", label, source->path);
      return VECTIS_ERR_INVALID;
    }
    buffer = malloc((size_t)length);
    if (buffer == NULL) {
      (void)fclose(fp);
      vectis_set_errorf(error, VECTIS_ERR_NOMEM, "failed to allocate %s buffer",
                        label);
      return VECTIS_ERR_NOMEM;
    }
    nread = fread(buffer, 1u, (size_t)length, fp);
    if (fclose(fp) != 0 || nread != (size_t)length) {
      free(buffer);
      vectis_set_errorf(error, VECTIS_ERR_STATE, "failed to read %s: %s", label,
                        source->path);
      return VECTIS_ERR_STATE;
    }
    *out = buffer;
    *out_size = (size_t)length;
    return VECTIS_OK;
  }
  if (source->source != NULL) {
    buffer = NULL;
    size = 0u;
    capacity = 0u;
    lc_error_init(&lcerr);
    if (source->source->reset != NULL &&
        source->source->reset(source->source, &lcerr) != LC_OK) {
      vectis_set_errorf(
          error, VECTIS_ERR_STATE, "failed to reset %s source: %s", label,
          lcerr.message != NULL ? lcerr.message : "unknown lockdc error");
      lc_error_cleanup(&lcerr);
      return VECTIS_ERR_STATE;
    }
    for (;;) {
      nread =
          source->source->read(source->source, chunk, sizeof(chunk), &lcerr);
      if (nread == 0u) {
        if (lcerr.code != LC_OK) {
          free(buffer);
          vectis_set_errorf(
              error, VECTIS_ERR_STATE, "failed to read %s source: %s", label,
              lcerr.message != NULL ? lcerr.message : "unknown lockdc error");
          lc_error_cleanup(&lcerr);
          return VECTIS_ERR_STATE;
        }
        break;
      }
      if (size + nread < size) {
        free(buffer);
        lc_error_cleanup(&lcerr);
        vectis_set_errorf(error, VECTIS_ERR_NOMEM, "%s source is too large",
                          label);
        return VECTIS_ERR_NOMEM;
      }
      if (size + nread > capacity) {
        capacity = capacity == 0u ? 8192u : capacity * 2u;
        while (capacity < size + nread) {
          capacity *= 2u;
        }
        grown = (unsigned char *)realloc(buffer, capacity);
        if (grown == NULL) {
          free(buffer);
          lc_error_cleanup(&lcerr);
          vectis_set_errorf(error, VECTIS_ERR_NOMEM,
                            "failed to grow %s source buffer", label);
          return VECTIS_ERR_NOMEM;
        }
        buffer = grown;
      }
      memcpy((unsigned char *)buffer + size, chunk, nread);
      size += nread;
    }
    lc_error_cleanup(&lcerr);
    if (size == 0u) {
      free(buffer);
      vectis_set_errorf(error, VECTIS_ERR_INVALID, "%s source is empty", label);
      return VECTIS_ERR_INVALID;
    }
    *out = buffer;
    *out_size = size;
    return VECTIS_OK;
  }
  vectis_set_errorf(error, VECTIS_ERR_INVALID, "%s source is empty", label);
  return VECTIS_ERR_INVALID;
}

static X509 *vectis_cert_read_x509(const void *pem, size_t pem_size) {
  BIO *bio;
  X509 *cert;

  bio = BIO_new_mem_buf(pem, (int)pem_size);
  if (bio == NULL) {
    return NULL;
  }
  cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free(bio);
  return cert;
}

static EVP_PKEY *vectis_cert_read_key(const void *pem, size_t pem_size) {
  BIO *bio;
  EVP_PKEY *key;

  bio = BIO_new_mem_buf(pem, (int)pem_size);
  if (bio == NULL) {
    return NULL;
  }
  key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
  BIO_free(bio);
  return key;
}

static vectis_status vectis_cert_write_private_key(const char *path,
                                                   EVP_PKEY *key,
                                                   vectis_error *error) {
  FILE *fp;

  fp = fopen(path, "wb");
  if (fp == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to open private key output: %s", path);
    return VECTIS_ERR_INVALID;
  }
  if (PEM_write_PrivateKey(fp, key, NULL, NULL, 0, NULL, NULL) != 1) {
    (void)fclose(fp);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to write private key");
    return VECTIS_ERR_STATE;
  }
  if (fclose(fp) != 0) {
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to close private key");
    return VECTIS_ERR_STATE;
  }
  return VECTIS_OK;
}

static vectis_status vectis_cert_add_csr_extension(X509_REQ *request, int nid,
                                                   const char *value,
                                                   vectis_error *error) {
  X509V3_CTX ctx;
  X509_EXTENSION *extension;
  STACK_OF(X509_EXTENSION) * extensions;
  char *value_copy;
  vectis_status status;

  extensions = NULL;
  extension = NULL;
  status = VECTIS_OK;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, NULL, NULL, request, NULL, 0);
  value_copy = vectis_strdup(value);
  if (value_copy == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy CSR extension value");
    return VECTIS_ERR_NOMEM;
  }
  extension = X509V3_EXT_conf_nid(NULL, &ctx, nid, value_copy);
  free(value_copy);
  if (extension == NULL) {
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to create CSR extension");
    return VECTIS_ERR_STATE;
  }
  extensions = sk_X509_EXTENSION_new_null();
  if (extensions == NULL) {
    X509_EXTENSION_free(extension);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate CSR extension stack");
    return VECTIS_ERR_NOMEM;
  }
  if (sk_X509_EXTENSION_push(extensions, extension) <= 0) {
    X509_EXTENSION_free(extension);
    sk_X509_EXTENSION_free(extensions);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to append CSR extension");
    return VECTIS_ERR_NOMEM;
  }
  extension = NULL;
  if (X509_REQ_add_extensions(request, extensions) != 1) {
    status = VECTIS_ERR_STATE;
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to add CSR extension");
  }
  sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);
  return status;
}

static vectis_status vectis_cert_validate_time(X509 *cert,
                                               vectis_error *error) {
  if (X509_cmp_current_time(X509_get0_notBefore(cert)) > 0) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "certificate is not valid yet");
    return VECTIS_ERR_INVALID;
  }
  if (X509_cmp_current_time(X509_get0_notAfter(cert)) < 0) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "certificate is expired");
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static vectis_status vectis_cert_verify_ca(X509 *cert, X509 *ca_cert,
                                           vectis_error *error) {
  X509_STORE *store;
  X509_STORE_CTX *ctx;
  int ok;

  store = X509_STORE_new();
  if (store == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate certificate store");
    return VECTIS_ERR_NOMEM;
  }
  if (X509_STORE_add_cert(store, ca_cert) != 1) {
    X509_STORE_free(store);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to add CA certificate to store");
    return VECTIS_ERR_STATE;
  }
  ctx = X509_STORE_CTX_new();
  if (ctx == NULL) {
    X509_STORE_free(store);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate certificate verify context");
    return VECTIS_ERR_NOMEM;
  }
  if (X509_STORE_CTX_init(ctx, store, cert, NULL) != 1) {
    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to initialize certificate verification");
    return VECTIS_ERR_STATE;
  }
  ok = X509_verify_cert(ctx);
  X509_STORE_CTX_free(ctx);
  X509_STORE_free(store);
  if (ok != 1) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "certificate failed CA verification");
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

vectis_status vectis_cert_validate_bundle(const vectis_source *bundle,
                                          vectis_error *error) {
  void *pem;
  size_t pem_size;
  X509 *cert;
  EVP_PKEY *key;
  vectis_status status;

  pem = NULL;
  pem_size = 0u;
  cert = NULL;
  key = NULL;
  status = vectis_read_source_bytes(bundle, &pem, &pem_size,
                                    "certificate bundle", error);
  if (status != VECTIS_OK) {
    return status;
  }
  cert = vectis_cert_read_x509(pem, pem_size);
  if (cert == NULL) {
    free(pem);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "failed to parse certificate from bundle");
    return VECTIS_ERR_INVALID;
  }
  key = vectis_cert_read_key(pem, pem_size);
  if (key == NULL) {
    X509_free(cert);
    free(pem);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "failed to parse private key from bundle");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_cert_validate_time(cert, error);
  if (status == VECTIS_OK && X509_check_private_key(cert, key) != 1) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "certificate and private key do not match");
    status = VECTIS_ERR_INVALID;
  }
  EVP_PKEY_free(key);
  X509_free(cert);
  free(pem);
  if (status == VECTIS_OK) {
    vectis_error_clear(error);
  }
  return status;
}

vectis_status vectis_cert_validate_pair(const vectis_source *certificate,
                                        const vectis_source *private_key,
                                        const vectis_source *ca_bundle,
                                        vectis_error *error) {
  void *cert_pem;
  void *key_pem;
  void *ca_pem;
  size_t cert_pem_size;
  size_t key_pem_size;
  size_t ca_pem_size;
  X509 *cert;
  X509 *ca_cert;
  EVP_PKEY *key;
  vectis_status status;

  cert_pem = NULL;
  key_pem = NULL;
  ca_pem = NULL;
  cert_pem_size = 0u;
  key_pem_size = 0u;
  ca_pem_size = 0u;
  cert = NULL;
  ca_cert = NULL;
  key = NULL;

  status = vectis_read_source_bytes(certificate, &cert_pem, &cert_pem_size,
                                    "certificate", error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_read_source_bytes(private_key, &key_pem, &key_pem_size,
                                    "private key", error);
  if (status != VECTIS_OK) {
    free(cert_pem);
    return status;
  }
  cert = vectis_cert_read_x509(cert_pem, cert_pem_size);
  key = vectis_cert_read_key(key_pem, key_pem_size);
  if (cert == NULL || key == NULL) {
    status = VECTIS_ERR_INVALID;
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     cert == NULL ? "failed to parse certificate"
                                  : "failed to parse private key");
    goto done;
  }
  status = vectis_cert_validate_time(cert, error);
  if (status != VECTIS_OK) {
    goto done;
  }
  if (X509_check_private_key(cert, key) != 1) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "certificate and private key do not match");
    status = VECTIS_ERR_INVALID;
    goto done;
  }
  if (ca_bundle != NULL &&
      (ca_bundle->path != NULL || ca_bundle->memory != NULL ||
       ca_bundle->source != NULL)) {
    status = vectis_read_source_bytes(ca_bundle, &ca_pem, &ca_pem_size,
                                      "CA bundle", error);
    if (status != VECTIS_OK) {
      goto done;
    }
    ca_cert = vectis_cert_read_x509(ca_pem, ca_pem_size);
    if (ca_cert == NULL) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "failed to parse CA certificate");
      status = VECTIS_ERR_INVALID;
      goto done;
    }
    status = vectis_cert_verify_ca(cert, ca_cert, error);
    if (status != VECTIS_OK) {
      goto done;
    }
  }
  vectis_error_clear(error);
  status = VECTIS_OK;

done:
  X509_free(ca_cert);
  EVP_PKEY_free(key);
  X509_free(cert);
  free(ca_pem);
  free(key_pem);
  free(cert_pem);
  return status;
}

vectis_status
vectis_cert_generate_private_key(const vectis_private_key_config *config,
                                 vectis_error *error) {
  EVP_PKEY *key;
  vectis_status status;
  unsigned key_bits;

  if (config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "private key config is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->output_key_path == NULL || config->output_key_path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "private key output_key_path is required");
    return VECTIS_ERR_INVALID;
  }
  key_bits = vectis_default_unsigned(config->key_bits, 4096u);
  if (key_bits < 1024u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "private key key_bits must be at least 1024");
    return VECTIS_ERR_INVALID;
  }

  key = vectis_cert_generate_rsa_key(key_bits, error);
  if (key == NULL) {
    if (error != NULL && error->source == VECTIS_ERROR_SOURCE_VECTIS) {
      error->source = VECTIS_ERROR_SOURCE_OPENSSL;
    }
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  status = vectis_cert_write_private_key(config->output_key_path, key, error);
  EVP_PKEY_free(key);
  if (status == VECTIS_OK) {
    vectis_error_clear(error);
  } else if (error != NULL && error->source == VECTIS_ERROR_SOURCE_VECTIS) {
    error->source = VECTIS_ERROR_SOURCE_OPENSSL;
  }
  return status;
}

vectis_status vectis_cert_generate_csr(const vectis_csr_config *config,
                                       vectis_error *error) {
  vectis_source private_key_source;
  void *key_pem;
  size_t key_pem_size;
  EVP_PKEY *key;
  X509_REQ *request;
  X509_NAME *name;
  FILE *fp;
  char *san;
  vectis_status status;

  if (config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "CSR config is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->subject.common_name == NULL ||
      config->subject.common_name[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "CSR subject common_name is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->output_csr_path == NULL || config->output_csr_path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "CSR output_csr_path is required");
    return VECTIS_ERR_INVALID;
  }

  private_key_source = config->private_key;
  if (private_key_source.path == NULL && private_key_source.memory == NULL &&
      private_key_source.source == NULL && config->private_key_path != NULL) {
    private_key_source = vectis_source_from_path(config->private_key_path);
  }
  key_pem = NULL;
  key_pem_size = 0u;
  key = NULL;
  request = NULL;
  fp = NULL;
  san = NULL;
  status = vectis_read_source_bytes(&private_key_source, &key_pem,
                                    &key_pem_size, "private key", error);
  if (status != VECTIS_OK) {
    return status;
  }
  key = vectis_cert_read_key(key_pem, key_pem_size);
  if (key == NULL) {
    status = VECTIS_ERR_INVALID;
    vectis_set_error(error, VECTIS_ERR_INVALID, "failed to parse private key");
    goto done;
  }
  request = X509_REQ_new();
  if (request == NULL) {
    status = VECTIS_ERR_NOMEM;
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate CSR");
    goto done;
  }
  if (X509_REQ_set_version(request, 0L) != 1 ||
      X509_REQ_set_pubkey(request, key) != 1) {
    status = VECTIS_ERR_STATE;
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to initialize CSR");
    goto done;
  }
  name = X509_REQ_get_subject_name(request);
  if (name == NULL) {
    status = VECTIS_ERR_STATE;
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to allocate CSR subject");
    goto done;
  }
  status = vectis_cert_set_name(name, &config->subject, "CSR", error);
  if (status != VECTIS_OK) {
    goto done;
  }
  san = vectis_cert_san_string(config->dns_names, config->ip_addresses, error);
  if (san != NULL) {
    status = vectis_cert_add_csr_extension(request, NID_subject_alt_name, san,
                                           error);
    if (status != VECTIS_OK) {
      goto done;
    }
  } else if (error != NULL && error->code != VECTIS_OK) {
    status = error->code;
    goto done;
  }
  if (X509_REQ_sign(request, key, EVP_sha256()) <= 0) {
    status = VECTIS_ERR_STATE;
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to sign CSR");
    goto done;
  }
  fp = fopen(config->output_csr_path, "wb");
  if (fp == NULL) {
    status = VECTIS_ERR_INVALID;
    vectis_set_errorf(error, VECTIS_ERR_INVALID,
                      "failed to open CSR output: %s", config->output_csr_path);
    goto done;
  }
  if (PEM_write_X509_REQ(fp, request) != 1) {
    status = VECTIS_ERR_STATE;
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to write CSR");
    goto done;
  }
  if (fclose(fp) != 0) {
    fp = NULL;
    status = VECTIS_ERR_STATE;
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to close CSR");
    goto done;
  }
  fp = NULL;
  vectis_error_clear(error);
  status = VECTIS_OK;

done:
  if (fp != NULL) {
    (void)fclose(fp);
  }
  free(san);
  X509_REQ_free(request);
  EVP_PKEY_free(key);
  free(key_pem);
  if (status != VECTIS_OK && error != NULL &&
      error->source == VECTIS_ERROR_SOURCE_VECTIS) {
    error->source = VECTIS_ERROR_SOURCE_OPENSSL;
  }
  return status;
}

vectis_status
vectis_cert_generate_bundle(const vectis_cert_bundle_config *config,
                            vectis_error *error) {
  EVP_PKEY *key;
  EVP_PKEY *signing_key;
  X509 *ca_cert;
  EVP_PKEY *ca_key;
  X509 *cert;
  char *san;
  vectis_status status;
  unsigned key_bits;
  long valid_days;

  if (config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "certificate bundle config is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->subject.common_name == NULL ||
      config->subject.common_name[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "certificate subject common_name is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->output_bundle_path == NULL &&
      (config->output_cert_path == NULL || config->output_key_path == NULL)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "certificate output requires output_bundle_path or "
                     "output_cert_path + output_key_path");
    return VECTIS_ERR_INVALID;
  }
  key_bits = vectis_default_unsigned(config->key_bits, 4096u);
  valid_days = vectis_default_long(config->valid_days, 397L);
  if (key_bits < 1024u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "certificate key_bits must be at least 1024");
    return VECTIS_ERR_INVALID;
  }
  if (valid_days < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "certificate valid_days must be non-negative");
    return VECTIS_ERR_INVALID;
  }

  key = NULL;
  signing_key = NULL;
  ca_cert = NULL;
  ca_key = NULL;
  cert = NULL;
  san = NULL;
  status = VECTIS_OK;
  vectis_error_clear(error);

  status = vectis_cert_load_ca(config, &ca_cert, &ca_key, error);
  if (status != VECTIS_OK) {
    goto done;
  }
  key = vectis_cert_generate_rsa_key(key_bits, error);
  if (key == NULL) {
    status = VECTIS_ERR_STATE;
    goto done;
  }

  cert = X509_new();
  if (cert == NULL) {
    status = VECTIS_ERR_NOMEM;
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate certificate");
    goto done;
  }
  if (X509_set_version(cert, 2L) != 1 ||
      ASN1_INTEGER_set(X509_get_serialNumber(cert), (long)time(NULL)) != 1 ||
      X509_gmtime_adj(X509_get_notBefore(cert), 0L) == NULL ||
      X509_gmtime_adj(X509_get_notAfter(cert), valid_days * 24L * 60L * 60L) ==
          NULL ||
      X509_set_pubkey(cert, key) != 1) {
    status = VECTIS_ERR_STATE;
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to initialize certificate");
    goto done;
  }
  status = vectis_cert_set_subject(cert, &config->subject, error);
  if (status != VECTIS_OK) {
    goto done;
  }
  if (X509_set_issuer_name(cert, ca_cert != NULL
                                     ? X509_get_subject_name(ca_cert)
                                     : X509_get_subject_name(cert)) != 1) {
    status = VECTIS_ERR_STATE;
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to set certificate issuer");
    goto done;
  }
  status = vectis_cert_add_extension(
      cert, NID_basic_constraints,
      config->is_ca ? "critical,CA:TRUE" : "critical,CA:FALSE", error);
  if (status != VECTIS_OK) {
    goto done;
  }
  status = vectis_cert_add_extension(cert, NID_key_usage,
                                     config->is_ca
                                         ? "keyCertSign,cRLSign"
                                         : "digitalSignature,keyEncipherment",
                                     error);
  if (status != VECTIS_OK) {
    goto done;
  }
  if (!config->is_ca) {
    status = vectis_cert_add_extension(cert, NID_ext_key_usage,
                                       "serverAuth,clientAuth", error);
    if (status != VECTIS_OK) {
      goto done;
    }
  }
  san = vectis_cert_san_string(config->dns_names, config->ip_addresses, error);
  if (san != NULL) {
    status = vectis_cert_add_extension(cert, NID_subject_alt_name, san, error);
    if (status != VECTIS_OK) {
      goto done;
    }
  } else if (error != NULL && error->code != VECTIS_OK) {
    status = error->code;
    goto done;
  }
  signing_key = ca_key != NULL ? ca_key : key;
  if (X509_sign(cert, signing_key, EVP_sha256()) <= 0) {
    status = VECTIS_ERR_STATE;
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to sign certificate");
    goto done;
  }
  status = vectis_cert_write_outputs(config, key, cert, error);
  if (status == VECTIS_OK) {
    vectis_error_clear(error);
  }

done:
  free(san);
  X509_free(cert);
  X509_free(ca_cert);
  EVP_PKEY_free(ca_key);
  EVP_PKEY_free(key);
  if (status != VECTIS_OK && error != NULL &&
      error->source == VECTIS_ERROR_SOURCE_VECTIS) {
    error->source = VECTIS_ERROR_SOURCE_OPENSSL;
  }
  return status;
}

struct lc_client *vectis_internal_lockd_client(vectis_app *app) {
  vectis_app_impl *impl;

  if (app == NULL || app->impl == NULL) {
    return NULL;
  }

  impl = (vectis_app_impl *)app->impl;
  return impl->lockd_client;
}
