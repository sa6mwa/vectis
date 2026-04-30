#include "vectis_internal.h"

#include <curl/curl.h>
#include <lc/lc.h>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <libxml/parser.h>
#include <libxml/xmlreader.h>
#include <lonejson.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <pthread.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct vectis_route_entry {
  vectis_http_method method;
  vectis_http_methods methods;
  char *path;
  vectis_route_path_kind path_kind;
  vectis_body_policy body;
  vectis_route_handler_fn handler;
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

typedef struct vectis_static_route_data {
  int directory;
  const char *path_prefix;
  const char *file_path;
  const char *root_dir;
  const char *content_type;
  const char *index_file;
} vectis_static_route_data;

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

typedef struct vectis_xml_pipe_reader {
  int fd;
} vectis_xml_pipe_reader;

typedef struct vectis_xml_pipe_writer {
  vectis_source source;
  const lonejson_map *map;
  vectis_xml_config config;
  int fd;
  vectis_status status;
  vectis_error error;
} vectis_xml_pipe_writer;

typedef struct vectis_dsv_row_pipe_writer {
  const lonejson_map *map;
  const char *const *columns;
  size_t column_count;
  const vectis_dsv_fields *fields;
  int fd;
  vectis_status status;
  vectis_error error;
} vectis_dsv_row_pipe_writer;

typedef struct vectis_json_string_box {
  char *value;
} vectis_json_string_box;

typedef struct vectis_app_impl {
  pthread_mutex_t mutex;
  int started;
  int owns_logger;
  char *app_name;
  char *bind;
  char *domain;
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

static const lonejson_field vectis_json_string_box_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(vectis_json_string_box, value, "value")};

LONEJSON_MAP_DEFINE(vectis_json_string_box_map,
                    vectis_json_string_box,
                    vectis_json_string_box_fields);

typedef struct vectis_error_response_body {
  char code[64];
  char message[256];
  char detail[256];
} vectis_error_response_body;

static const lonejson_field vectis_error_response_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(vectis_error_response_body,
                                    code,
                                    "code",
                                    LONEJSON_OVERFLOW_TRUNCATE),
    LONEJSON_FIELD_STRING_FIXED_REQ(vectis_error_response_body,
                                    message,
                                    "message",
                                    LONEJSON_OVERFLOW_TRUNCATE),
    LONEJSON_FIELD_STRING_FIXED(vectis_error_response_body,
                                detail,
                                "detail",
                                LONEJSON_OVERFLOW_TRUNCATE)};

LONEJSON_MAP_DEFINE(vectis_error_response_map,
                    vectis_error_response_body,
                    vectis_error_response_fields);

struct vectis_http_client {
  vectis_http_client_config config;
  pslog_logger *logger;
};

struct vectis_consumer_service {
  lc_consumer_service *service;
};

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

typedef struct vectis_curl_request_body {
  const void *data;
  size_t size;
  size_t offset;
  char *owned_data;
  FILE *file;
  long file_size;
} vectis_curl_request_body;

static vectis_status vectis_app_start_impl(vectis_app *app, vectis_error *error);
static vectis_status vectis_app_stop_impl(vectis_app *app, vectis_error *error);
static void vectis_close_lockd_client_for_current_process(vectis_app_impl *impl);
static vectis_status vectis_app_register_route_impl(vectis_app *app,
                                                    const vectis_route_config *route,
                                                    vectis_error *error);
static vectis_status vectis_app_register_route_owned_userdata(vectis_app *app,
                                                              const vectis_route_config *route,
                                                              int owns_userdata,
                                                              vectis_error *error);
static size_t vectis_app_route_count_impl(const vectis_app *app);
static size_t vectis_app_max_streaming_body_bytes(vectis_app_impl *impl);
static size_t vectis_app_min_streaming_memory_limit_bytes(vectis_app_impl *impl);
static pslog_logger *vectis_app_logger_impl(vectis_app *app);

static const vectis_methods vectis_default_methods = {
    vectis_destroy,
    vectis_app_start_impl,
    vectis_app_stop_impl,
    vectis_app_register_route_impl,
    vectis_app_route_count_impl,
    vectis_app_logger_impl};

static pthread_once_t vectis_curl_once = PTHREAD_ONCE_INIT;
static pthread_once_t vectis_libssh2_once = PTHREAD_ONCE_INIT;

static void vectis_curl_global_init_once(void) {
  (void)curl_global_init(CURL_GLOBAL_DEFAULT);
}

static void vectis_libssh2_global_init_once(void) {
  (void)libssh2_init(0);
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

void vectis_set_error(vectis_error *error, vectis_status code, const char *message) {
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

static void vectis_set_errorf(vectis_error *error,
                              vectis_status code,
                              const char *fmt,
                              ...) {
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

static vectis_status vectis_set_lockdc_error(vectis_error *error,
                                             int rc,
                                             const lc_error *lcerr,
                                             const char *message) {
  vectis_set_errorf(error,
                    VECTIS_ERR_STATE,
                    "%s: %s",
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
        (void)snprintf(error->detail, sizeof(error->detail), "%s", lcerr->detail);
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

static vectis_status vectis_source_error(vectis_error *error,
                                         int rc,
                                         const lc_error *lcerr,
                                         const char *message) {
  vectis_set_errorf(error,
                    rc == LC_ERR_NOMEM ? VECTIS_ERR_NOMEM : VECTIS_ERR_STATE,
                    "%s: %s",
                    message != NULL ? message : "source operation failed",
                    lcerr != NULL && lcerr->message != NULL ?
                        lcerr->message : "unknown source error");
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "request body reader is not available");
    return VECTIS_ERR_INVALID;
  }
  if (request->body_reader->reset == NULL) {
    return VECTIS_OK;
  }
  lc_error_init(&lcerr);
  rc = request->body_reader->reset(request->body_reader, &lcerr);
  if (rc != LC_OK) {
    (void)vectis_source_error(error, rc, &lcerr, "failed to reset request body reader");
    lc_error_cleanup(&lcerr);
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  return VECTIS_OK;
}

static int vectis_tmp_template(char *buffer,
                               size_t buffer_size,
                               const vectis_body_materialize_config *config) {
  const char *directory;
  const char *prefix;
  int n;

  if (buffer == NULL || buffer_size == 0u) {
    return 0;
  }
  directory = config != NULL && config->directory != NULL &&
              config->directory[0] != '\0' ? config->directory : "/tmp";
  prefix = config != NULL && config->prefix != NULL &&
           config->prefix[0] != '\0' ? config->prefix : "vectis-body";
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

static vectis_status vectis_string_builder_reserve(vectis_string_builder *builder,
                                                   size_t extra,
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

static vectis_status vectis_string_builder_append_n(vectis_string_builder *builder,
                                                    const char *text,
                                                    size_t length,
                                                    vectis_error *error) {
  if (text == NULL && length > 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "string builder text is required");
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

static vectis_status vectis_string_builder_append(vectis_string_builder *builder,
                                                  const char *text,
                                                  vectis_error *error) {
  return vectis_string_builder_append_n(builder,
                                        text != NULL ? text : "",
                                        text != NULL ? strlen(text) : 0u,
                                        error);
}

static vectis_status vectis_string_builder_appendf(vectis_string_builder *builder,
                                                   vectis_error *error,
                                                   const char *fmt,
                                                   ...) {
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
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to format string builder content");
    return VECTIS_ERR_STATE;
  }
  if (vectis_string_builder_reserve(builder, (size_t)n, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  old_size = builder->size;
  va_start(ap, fmt);
  (void)vsnprintf(builder->data + builder->size,
                  builder->capacity - builder->size,
                  fmt,
                  ap);
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

vectis_source vectis_source_from_memory(const void *memory, size_t memory_size) {
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
  config->max_request_header_bytes = VECTIS_SERVER_DEFAULT_MAX_REQUEST_HEADER_BYTES;
  config->max_request_body_bytes = VECTIS_SERVER_DEFAULT_MAX_REQUEST_BODY_BYTES;
  config->request_header_timeout_ms = VECTIS_SERVER_DEFAULT_REQUEST_HEADER_TIMEOUT_MS;
  config->request_body_idle_timeout_ms = VECTIS_SERVER_DEFAULT_REQUEST_BODY_IDLE_TIMEOUT_MS;
  config->response_write_idle_timeout_ms = VECTIS_SERVER_DEFAULT_RESPONSE_WRITE_IDLE_TIMEOUT_MS;
  config->request_body_min_rate_bytes_per_sec =
      VECTIS_SERVER_DEFAULT_REQUEST_BODY_MIN_RATE_BYTES_PER_SEC;
  config->request_body_min_rate_grace_ms = VECTIS_SERVER_DEFAULT_REQUEST_BODY_MIN_RATE_GRACE_MS;
  config->idle_timeout_ms = VECTIS_SERVER_DEFAULT_IDLE_TIMEOUT_MS;
  config->keepalive_timeout_ms = VECTIS_SERVER_DEFAULT_KEEPALIVE_TIMEOUT_MS;
  config->keepalive_max_requests = VECTIS_SERVER_DEFAULT_KEEPALIVE_MAX_REQUESTS;
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
  policy.memory_buffer_limit_bytes = VECTIS_SERVER_DEFAULT_MAX_REQUEST_BODY_BYTES;
  policy.disk_spool_disabled = 0;
  policy.idle_timeout_ms = VECTIS_SERVER_DEFAULT_REQUEST_BODY_IDLE_TIMEOUT_MS;
  policy.min_rate_bytes_per_sec = VECTIS_SERVER_DEFAULT_REQUEST_BODY_MIN_RATE_BYTES_PER_SEC;
  policy.min_rate_grace_ms = VECTIS_SERVER_DEFAULT_REQUEST_BODY_MIN_RATE_GRACE_MS;
  return policy;
}

vectis_body_policy vectis_body_buffered_max(size_t max_bytes) {
  vectis_body_policy policy;

  policy = vectis_body_json_default();
  policy.mode = VECTIS_BODY_BUFFERED;
  policy.max_bytes = max_bytes;
  policy.memory_buffer_limit_bytes = max_bytes;
  return policy;
}

vectis_body_policy vectis_body_upload_max(size_t max_bytes) {
  vectis_body_policy policy;

  vectis_body_policy_init(&policy);
  policy.mode = VECTIS_BODY_STREAMING_UPLOAD;
  policy.max_bytes = max_bytes;
  policy.memory_buffer_limit_bytes = VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
  policy.idle_timeout_ms = VECTIS_SERVER_DEFAULT_REQUEST_BODY_IDLE_TIMEOUT_MS;
  policy.min_rate_bytes_per_sec = VECTIS_BODY_DEFAULT_UPLOAD_MIN_RATE_BYTES_PER_SEC;
  policy.min_rate_grace_ms = VECTIS_BODY_DEFAULT_UPLOAD_MIN_RATE_GRACE_MS;
  return policy;
}

vectis_body_policy vectis_body_upload(void) {
  return vectis_body_upload_max(VECTIS_BODY_DEFAULT_UPLOAD_MAX_BYTES);
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

void vectis_json_typed_route_config_init(vectis_json_typed_route_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->method = VECTIS_HTTP_ANY;
  config->methods = VECTIS_HTTP_METHODS_NONE;
  config->path_kind = VECTIS_ROUTE_PATH_LITERAL;
  config->body = vectis_body_json_default();
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "OpenAPI route doc is required");
    return VECTIS_ERR_INVALID;
  }
  if (schema.map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "OpenAPI request schema map is required");
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "OpenAPI route doc is required");
    return VECTIS_ERR_INVALID;
  }
  if (status_code < 100 || status_code > 599) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "OpenAPI response status is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (schema.map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "OpenAPI response schema map is required");
    return VECTIS_ERR_INVALID;
  }
  if (doc->response_count == doc->response_capacity) {
    next_capacity = doc->response_capacity == 0u ? 4u : doc->response_capacity * 2u;
    grown = (vectis_openapi_response *)realloc(doc->responses, next_capacity * sizeof(*grown));
    if (grown == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to grow OpenAPI responses");
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
  if (method < VECTIS_HTTP_GET || method > VECTIS_HTTP_OPTIONS) {
    return VECTIS_HTTP_METHODS_NONE;
  }
  return VECTIS_HTTP_METHOD_MASK(method);
}

static vectis_http_methods vectis_normalize_methods(vectis_http_method method,
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
  return VECTIS_HTTP_ANY;
}

vectis_route_config vectis_route(vectis_http_method method,
                                 const char *path,
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
  route.method = methods == VECTIS_HTTP_METHODS_NONE ? (vectis_http_method)-1 : vectis_first_method(methods);
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

vectis_route_config vectis_json_body_route_methods(vectis_http_methods methods,
                                                   const char *path,
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
                                            const char *path,
                                            size_t max_bytes,
                                            vectis_route_handler_fn handler,
                                            void *userdata) {
  vectis_route_config route;

  route = vectis_route(method, path, handler, userdata);
  route.body = vectis_body_upload_max(max_bytes);
  return route;
}

vectis_route_config vectis_upload_route_max_methods(vectis_http_methods methods,
                                                    const char *path,
                                                    size_t max_bytes,
                                                    vectis_route_handler_fn handler,
                                                    void *userdata) {
  vectis_route_config route;

  route = vectis_route_methods(methods, path, handler, userdata);
  route.body = vectis_body_upload_max(max_bytes);
  return route;
}

vectis_json_route_config vectis_json_route(vectis_http_method method,
                                           const char *path,
                                           const lonejson_map *input_map,
                                           size_t input_size,
                                           const lonejson_map *output_map,
                                           size_t output_size,
                                           vectis_json_route_handler_fn handler,
                                           void *userdata) {
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

vectis_json_route_config vectis_json_route_methods(vectis_http_methods methods,
                                                   const char *path,
                                                   const lonejson_map *input_map,
                                                   size_t input_size,
                                                   const lonejson_map *output_map,
                                                   size_t output_size,
                                                   vectis_json_route_handler_fn handler,
                                                   void *userdata) {
  vectis_json_route_config route;

  route = vectis_json_route(vectis_first_method(methods),
                            path,
                            input_map,
                            input_size,
                            output_map,
                            output_size,
                            handler,
                            userdata);
  route.methods = methods;
  return route;
}

vectis_json_typed_route_config vectis_json_typed_route(vectis_http_method method,
                                                       const char *path,
                                                       const lonejson_map *input_map,
                                                       size_t input_size,
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

vectis_json_typed_route_config vectis_json_typed_route_methods(vectis_http_methods methods,
                                                               const char *path,
                                                               const lonejson_map *input_map,
                                                               size_t input_size,
                                                               vectis_json_typed_route_handler_fn handler,
                                                               void *userdata) {
  vectis_json_typed_route_config route;

  route = vectis_json_typed_route(vectis_first_method(methods),
                                  path,
                                  input_map,
                                  input_size,
                                  handler,
                                  userdata);
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

void vectis_static_directory_config_init(vectis_static_directory_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->content_type = "application/octet-stream";
  config->index_file = "index.html";
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

static const char *vectis_kv_find(const vectis_kv *items,
                                  size_t count,
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

static vectis_status vectis_kv_add(vectis_kv **items,
                                   size_t *count,
                                   size_t *capacity,
                                   const char *name,
                                   const char *value,
                                   const char *label,
                                   vectis_error *error) {
  vectis_kv *grown;
  char *name_copy;
  char *value_copy;
  size_t next_capacity;

  if (items == NULL || count == NULL || capacity == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "key/value storage is required");
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
      vectis_set_errorf(error, VECTIS_ERR_NOMEM, "failed to grow %s storage", label);
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
  while ((*p >= 'A' && *p <= 'Z') ||
         (*p >= 'a' && *p <= 'z') ||
         (*p >= '0' && *p <= '9') ||
         *p == '+' || *p == '-' || *p == '.') {
    p++;
  }
  return p > url && p[0] == ':' && p[1] == '/' && p[2] == '/';
}

static char *vectis_join_url(const char *base_url,
                             const char *url,
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

static vectis_status vectis_copy_bytes(const void *bytes,
                                       size_t size,
                                       void **out,
                                       size_t *out_size,
                                       const char *label,
                                       vectis_error *error) {
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

static int vectis_tls_material_present(const char *path,
                                       const void *pem,
                                       struct lc_source *source) {
  return path != NULL || pem != NULL || source != NULL;
}

static vectis_status vectis_copy_source_bytes(const vectis_source *source,
                                              const void *old_bytes,
                                              size_t old_size,
                                              void **out,
                                              size_t *out_size,
                                              const char *label,
                                              vectis_error *error) {
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
        vectis_set_error(error, VECTIS_ERR_INVALID, "route path must not contain empty segments");
        return VECTIS_ERR_INVALID;
      }
      segment_start = 1;
      p++;
      continue;
    }
    if (segment_start && p[0] == '.' && (p[1] == '/' || p[1] == '\0' ||
                                         (p[1] == '.' && (p[2] == '/' || p[2] == '\0')))) {
      vectis_set_error(error, VECTIS_ERR_INVALID, "route path must not contain dot segments");
      return VECTIS_ERR_INVALID;
    }
    if (*p == ':') {
      if (!segment_start) {
        vectis_set_error(error, VECTIS_ERR_INVALID, "route path parameters must occupy a full segment");
        return VECTIS_ERR_INVALID;
      }
      p++;
      if (!vectis_is_param_start((unsigned char)*p)) {
        vectis_set_error(error, VECTIS_ERR_INVALID, "route path parameter name is invalid");
        return VECTIS_ERR_INVALID;
      }
      while (vectis_is_param_char((unsigned char)*p)) {
        p++;
      }
      if (*p == '?') {
        p++;
      }
      if (*p != '/' && *p != '\0') {
        vectis_set_error(error, VECTIS_ERR_INVALID, "route path parameter name is invalid");
        return VECTIS_ERR_INVALID;
      }
      segment_start = 0;
      continue;
    }
    if (*p == '?') {
      vectis_set_error(error, VECTIS_ERR_INVALID, "route path '?' is only allowed after a path parameter name");
      return VECTIS_ERR_INVALID;
    }
    if (*p == '%' || *p == '\\') {
      vectis_set_error(error, VECTIS_ERR_INVALID, "route path must not contain percent escapes or backslashes");
      return VECTIS_ERR_INVALID;
    }
    segment_start = 0;
    p++;
  }
  if (segment_start && path[1] != '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "route path must not end with an empty segment");
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static vectis_status vectis_validate_route_path(const char *path,
                                                vectis_route_path_kind path_kind,
                                                vectis_error *error) {
  regex_t compiled;
  int regex_rc;

  if (path_kind == VECTIS_ROUTE_PATH_LITERAL) {
    if (path == NULL || path[0] != '/') {
      vectis_set_error(error, VECTIS_ERR_INVALID, "route path must start with '/'");
      return VECTIS_ERR_INVALID;
    }
    if (strchr(path, ':') != NULL) {
      vectis_set_error(error,
                       VECTIS_ERR_INVALID,
                       "route path contains ':' but path_kind is not VECTIS_ROUTE_PATH_PARAMS");
      return VECTIS_ERR_INVALID;
    }
    return vectis_validate_param_path(path, error);
  }
  if (path_kind == VECTIS_ROUTE_PATH_PARAMS) {
    if (path == NULL || path[0] != '/') {
      vectis_set_error(error, VECTIS_ERR_INVALID, "route path must start with '/'");
      return VECTIS_ERR_INVALID;
    }
    return vectis_validate_param_path(path, error);
  }
  if (path_kind == VECTIS_ROUTE_PATH_REGEX) {
    if (path == NULL || path[0] == '\0') {
      vectis_set_error(error, VECTIS_ERR_INVALID, "regex route path is required");
      return VECTIS_ERR_INVALID;
    }
    if (path[0] != '^' || path[1] != '/') {
      vectis_set_error(error, VECTIS_ERR_INVALID, "regex route path must start with '^/'");
      return VECTIS_ERR_INVALID;
    }
    regex_rc = regcomp(&compiled, path, REG_EXTENDED | REG_NOSUB);
    if (regex_rc != 0) {
      vectis_set_error(error, VECTIS_ERR_INVALID, "regex route path is invalid POSIX extended regex");
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "request path must start with '/'");
    return VECTIS_ERR_INVALID;
  }
  if (strchr(path, ':') != NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request path must not contain ':'");
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "route requires at least one HTTP method");
    return VECTIS_ERR_INVALID;
  }
  if ((normalized & ~VECTIS_HTTP_METHODS_ALL) != 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "route HTTP methods contain unsupported bits");
    return VECTIS_ERR_INVALID;
  }
  if (method != VECTIS_HTTP_ANY && vectis_method_mask(method) == VECTIS_HTTP_METHODS_NONE) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "route HTTP method is invalid");
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static vectis_status vectis_validate_body_policy(const vectis_body_policy *policy,
                                                 const vectis_server_config *server,
                                                 vectis_error *error) {
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
  if (policy->max_bytes == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "body policy max_bytes must be greater than zero");
    return VECTIS_ERR_INVALID;
  }
  if (policy->idle_timeout_ms <= 0L) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "body policy idle_timeout_ms must be greater than zero");
    return VECTIS_ERR_INVALID;
  }
  if (policy->min_rate_grace_ms < 0L) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "body policy min_rate_grace_ms must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  if (policy->mode != VECTIS_BODY_STREAMING_UPLOAD) {
    if (policy->memory_buffer_limit_bytes == 0u) {
      vectis_set_error(error,
                       VECTIS_ERR_INVALID,
                       "buffered body policy memory_buffer_limit_bytes must be greater than zero");
      return VECTIS_ERR_INVALID;
    }
    if (policy->memory_buffer_limit_bytes > policy->max_bytes) {
      vectis_set_error(error,
                       VECTIS_ERR_INVALID,
                       "buffered body policy memory_buffer_limit_bytes must not exceed max_bytes");
      return VECTIS_ERR_INVALID;
    }
  } else if (policy->disk_spool_disabled && policy->memory_buffer_limit_bytes < policy->max_bytes) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "streaming upload body policy cannot disable spooling unless fully buffered");
    return VECTIS_ERR_INVALID;
  }
  if (server != NULL && server->max_request_body_bytes > 0u &&
      policy->mode != VECTIS_BODY_STREAMING_UPLOAD &&
      policy->max_bytes > server->max_request_body_bytes) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "non-streaming body policy max_bytes exceeds server max_request_body_bytes");
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

static vectis_status vectis_copy_endpoints(vectis_app_impl *impl,
                                           const vectis_lockd_config *lockd,
                                           vectis_error *error) {
  size_t i;

  if (lockd->endpoint_count == 0u) {
    return VECTIS_OK;
  }
  impl->endpoints = (char **)calloc(lockd->endpoint_count, sizeof(*impl->endpoints));
  if (impl->endpoints == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate lockd endpoints");
    return VECTIS_ERR_NOMEM;
  }
  impl->endpoint_count = lockd->endpoint_count;
  for (i = 0u; i < lockd->endpoint_count; ++i) {
    impl->endpoints[i] = vectis_strdup(lockd->endpoints[i]);
    if (impl->endpoints[i] == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy lockd endpoint");
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
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to derive vectis logger fields");
    return NULL;
  }

  return scoped;
}

static void vectis_free_routes(vectis_app_impl *impl) {
  size_t i;

  for (i = 0u; i < impl->route_count; ++i) {
    free(impl->routes[i].path);
    if (impl->routes[i].owns_userdata) {
      free(impl->routes[i].userdata);
    }
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

static void vectis_openapi_route_doc_deep_cleanup(vectis_openapi_route_doc *doc) {
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

static vectis_status vectis_openapi_route_doc_deep_copy(vectis_openapi_route_doc *dst,
                                                        const vectis_openapi_route_doc *src,
                                                        vectis_error *error) {
  const char **tags;
  vectis_openapi_response *responses;
  size_t i;

  if (dst == NULL || src == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "OpenAPI route doc copy requires source and destination");
    return VECTIS_ERR_INVALID;
  }
  memset(dst, 0, sizeof(*dst));
  if (src->summary != NULL) {
    dst->summary = vectis_strdup(src->summary);
    if (dst->summary == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy OpenAPI summary");
      return VECTIS_ERR_NOMEM;
    }
  }
  if (src->operation_id != NULL) {
    dst->operation_id = vectis_strdup(src->operation_id);
    if (dst->operation_id == NULL) {
      vectis_openapi_route_doc_deep_cleanup(dst);
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy OpenAPI operation id");
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
          vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy OpenAPI tag");
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
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy OpenAPI request schema name");
      return VECTIS_ERR_NOMEM;
    }
  }
  if (src->response_count > 0u) {
    responses = (vectis_openapi_response *)calloc(src->response_count, sizeof(*responses));
    if (responses == NULL) {
      vectis_openapi_route_doc_deep_cleanup(dst);
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy OpenAPI responses");
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
          vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy OpenAPI response description");
          return VECTIS_ERR_NOMEM;
        }
      }
      if (src->responses[i].schema.name != NULL) {
        responses[i].schema.name = vectis_strdup(src->responses[i].schema.name);
        if (responses[i].schema.name == NULL) {
          vectis_openapi_route_doc_deep_cleanup(dst);
          vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy OpenAPI response schema name");
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
  if (vectis_validate_methods(route->method, route->methods, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (vectis_validate_route_path(route->path, route->path_kind, error) != VECTIS_OK) {
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

static vectis_status vectis_validate_server_config(const vectis_server_config *config,
                                                   vectis_error *error) {
  if (config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "server config is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->max_connections == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "server max_connections must be greater than zero");
    return VECTIS_ERR_INVALID;
  }
  if (config->max_request_header_bytes < 1024u) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "server max_request_header_bytes must be at least 1024");
    return VECTIS_ERR_INVALID;
  }
  if (config->max_request_body_bytes == 0u) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "server max_request_body_bytes must be greater than zero");
    return VECTIS_ERR_INVALID;
  }
  if (config->request_header_timeout_ms <= 0L) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "server request_header_timeout_ms must be greater than zero");
    return VECTIS_ERR_INVALID;
  }
  if (config->request_body_idle_timeout_ms <= 0L) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "server request_body_idle_timeout_ms must be greater than zero");
    return VECTIS_ERR_INVALID;
  }
  if (config->response_write_idle_timeout_ms <= 0L) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "server response_write_idle_timeout_ms must be greater than zero");
    return VECTIS_ERR_INVALID;
  }
  if (config->request_body_min_rate_grace_ms < 0L) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "server request_body_min_rate_grace_ms must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  if (config->idle_timeout_ms <= 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "server idle_timeout_ms must be greater than zero");
    return VECTIS_ERR_INVALID;
  }
  if (!config->keepalive_disabled) {
    if (config->keepalive_timeout_ms <= 0L) {
      vectis_set_error(error,
                       VECTIS_ERR_INVALID,
                       "server keepalive_timeout_ms must be greater than zero when keepalive is enabled");
      return VECTIS_ERR_INVALID;
    }
    if (config->keepalive_max_requests == 0u) {
      vectis_set_error(error,
                       VECTIS_ERR_INVALID,
                       "server keepalive_max_requests must be greater than zero when keepalive is enabled");
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

  if (impl->tls_mode == VECTIS_TLS_MODE_MANUAL) {
    has_cert_key_bundle = vectis_tls_material_present(impl->cert_key_bundle_path,
                                                      impl->cert_key_bundle_pem,
                                                      impl->cert_key_bundle_source);
    has_split_certificate = vectis_tls_material_present(impl->certificate_path,
                                                        impl->certificate_pem,
                                                        impl->certificate_source);
    has_split_private_key = vectis_tls_material_present(impl->private_key_path,
                                                        impl->private_key_pem,
                                                        impl->private_key_source);
    if (!has_cert_key_bundle && (!has_split_certificate || !has_split_private_key)) {
      vectis_set_error(error,
                       VECTIS_ERR_INVALID,
                       "manual TLS requires cert_key_bundle or certificate + private_key from path, source, or memory");
      return VECTIS_ERR_INVALID;
    }
    has_client_ca = vectis_tls_material_present(impl->client_ca_bundle_path,
                                                impl->client_ca_bundle_pem,
                                                impl->client_ca_bundle_source);
    if (impl->require_client_certificate && !has_client_ca) {
      vectis_set_error(error,
                       VECTIS_ERR_INVALID,
                       "client certificate verification requires client_ca_bundle from path, source, or memory");
      return VECTIS_ERR_INVALID;
    }
  } else if (impl->tls_mode == VECTIS_TLS_MODE_ACME) {
    if (impl->acme_email == NULL || impl->acme_email[0] == '\0') {
      vectis_set_error(error, VECTIS_ERR_INVALID, "ACME mode requires acme_email");
      return VECTIS_ERR_INVALID;
    }
    if (impl->domain == NULL || impl->domain[0] == '\0' || strcmp(impl->domain, "*") == 0 ||
        strchr(impl->domain, '*') != NULL) {
      vectis_set_error(error, VECTIS_ERR_INVALID, "ACME mode requires a non-wildcard tls.domain");
      return VECTIS_ERR_INVALID;
    }
  }

  return VECTIS_OK;
}

static vectis_status vectis_validate_lockd_startable(const vectis_app_impl *impl,
                                                     vectis_error *error) {
  int has_lockd_transport;

  if (impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  has_lockd_transport = (impl->endpoint_count > 0u) || (impl->unix_socket_path != NULL);
  if (has_lockd_transport &&
      impl->client_bundle_path == NULL &&
      impl->client_bundle_source == NULL &&
      impl->client_bundle_pem == NULL &&
      impl->unix_socket_path == NULL) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "tcp lockd transport requires client_bundle_path, client_bundle_source, or client_bundle_pem");
    return VECTIS_ERR_INVALID;
  }

  return VECTIS_OK;
}

static int vectis_lockd_is_configured(const vectis_app_impl *impl) {
  return impl != NULL && (impl->endpoint_count > 0u || impl->unix_socket_path != NULL);
}

static void vectis_close_lockd_client_for_current_process(vectis_app_impl *impl) {
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
  config.logger = impl->lockd_logger_disabled ? NULL :
      (impl->lockd_logger != NULL ? impl->lockd_logger : impl->logger);

  if (impl->client_bundle_pem != NULL && impl->client_bundle_pem_size > 0u) {
    rc = lc_source_from_memory(impl->client_bundle_pem,
                               impl->client_bundle_pem_size,
                               &memory_source,
                               &lcerr);
    if (rc != LC_OK) {
      vectis_set_errorf(error,
                        VECTIS_ERR_STATE,
                        "failed to create lockd client bundle source: %s",
                        lcerr.message != NULL ? lcerr.message : "unknown lockdc error");
      if (error != NULL) {
        error->source = VECTIS_ERROR_SOURCE_LOCKDC;
        error->dependency_code = (long)rc;
        error->http_status = lcerr.http_status;
        if (lcerr.detail != NULL) {
          (void)snprintf(error->detail, sizeof(error->detail), "%s", lcerr.detail);
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
    vectis_set_errorf(error,
                      VECTIS_ERR_STATE,
                      "failed to open lockd client: %s",
                      lcerr.message != NULL ? lcerr.message : "unknown lockdc error");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LOCKDC;
      error->dependency_code = (long)rc;
      error->http_status = lcerr.http_status;
      if (lcerr.detail != NULL) {
        (void)snprintf(error->detail, sizeof(error->detail), "%s", lcerr.detail);
      }
    }
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }

  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_app *vectis_new(const vectis_app_config *config, vectis_error *error) {
  vectis_app_config defaults;
  const vectis_app_config *effective;
  vectis_app *app;
  vectis_app_impl *impl;
  vectis_status status;

  vectis_error_clear(error);
  vectis_app_config_init(&defaults);
  effective = config != NULL ? config : &defaults;
  status = vectis_validate_server_config(&effective->server, error);
  if (status != VECTIS_OK) {
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

  impl->app_name = vectis_strdup(effective->app_name != NULL ? effective->app_name : "vectis");
  impl->bind = vectis_strdup(effective->tls.bind != NULL ? effective->tls.bind : "0.0.0.0");
  impl->domain = vectis_strdup(effective->tls.domain != NULL ? effective->tls.domain : "*");
  impl->cert_key_bundle_path = vectis_strdup(vectis_source_path_or_old(&effective->tls.cert_key_bundle,
                                                                       effective->tls.cert_key_bundle_path));
  impl->cert_key_bundle_source = vectis_source_lc_or_old(&effective->tls.cert_key_bundle,
                                                         effective->tls.cert_key_bundle_source);
  impl->certificate_path = vectis_strdup(vectis_source_path_or_old(&effective->tls.certificate,
                                                                   effective->tls.certificate_path));
  impl->certificate_source = vectis_source_lc_or_old(&effective->tls.certificate,
                                                     effective->tls.certificate_source);
  impl->private_key_path = vectis_strdup(vectis_source_path_or_old(&effective->tls.private_key,
                                                                  effective->tls.private_key_path));
  impl->private_key_source = vectis_source_lc_or_old(&effective->tls.private_key,
                                                     effective->tls.private_key_source);
  impl->ca_bundle_path = vectis_strdup(vectis_source_path_or_old(&effective->tls.ca_bundle,
                                                                effective->tls.ca_bundle_path));
  impl->ca_bundle_source = vectis_source_lc_or_old(&effective->tls.ca_bundle,
                                                  effective->tls.ca_bundle_source);
  impl->client_ca_bundle_path = vectis_strdup(vectis_source_path_or_old(&effective->tls.client_ca_bundle,
                                                                       effective->tls.client_ca_bundle_path));
  impl->client_ca_bundle_source = vectis_source_lc_or_old(&effective->tls.client_ca_bundle,
                                                          effective->tls.client_ca_bundle_source);
  impl->acme_email = vectis_strdup(effective->tls.acme_email);
  impl->acme_directory_url = vectis_strdup(effective->tls.acme_directory_url);
  impl->unix_socket_path = vectis_strdup(effective->lockd.unix_socket_path);
  impl->client_bundle_path = vectis_strdup(vectis_source_path_or_old(&effective->lockd.client_bundle,
                                                                    effective->lockd.client_bundle_path));
  impl->client_bundle_source = vectis_source_lc_or_old(&effective->lockd.client_bundle,
                                                       effective->lockd.client_bundle_source);
  impl->default_namespace = vectis_strdup(effective->lockd.default_namespace);
  impl->lockd_logger = effective->lockd.logger;
  impl->lockd_logger_disabled = effective->lockd.logger_disabled;
  impl->timeout_ms = effective->lockd.timeout_ms;
  impl->port = effective->tls.port;
  impl->tls_mode = effective->tls.mode;
  impl->require_client_certificate = effective->tls.require_client_certificate;
  impl->server = effective->server;

  status = vectis_copy_source_bytes(&effective->tls.cert_key_bundle,
                                    effective->tls.cert_key_bundle_pem,
                                    effective->tls.cert_key_bundle_pem_size,
                                    &impl->cert_key_bundle_pem,
                                    &impl->cert_key_bundle_pem_size,
                                    "TLS cert/key bundle PEM",
                                    error);
  if (status != VECTIS_OK) {
    vectis_destroy_impl(impl);
    free(app);
    return NULL;
  }
  status = vectis_copy_source_bytes(&effective->tls.certificate,
                                    effective->tls.certificate_pem,
                                    effective->tls.certificate_pem_size,
                                    &impl->certificate_pem,
                                    &impl->certificate_pem_size,
                                    "TLS certificate PEM",
                                    error);
  if (status != VECTIS_OK) {
    vectis_destroy_impl(impl);
    free(app);
    return NULL;
  }
  status = vectis_copy_source_bytes(&effective->tls.private_key,
                                    effective->tls.private_key_pem,
                                    effective->tls.private_key_pem_size,
                                    &impl->private_key_pem,
                                    &impl->private_key_pem_size,
                                    "TLS private key PEM",
                                    error);
  if (status != VECTIS_OK) {
    vectis_destroy_impl(impl);
    free(app);
    return NULL;
  }
  status = vectis_copy_source_bytes(&effective->tls.ca_bundle,
                                    effective->tls.ca_bundle_pem,
                                    effective->tls.ca_bundle_pem_size,
                                    &impl->ca_bundle_pem,
                                    &impl->ca_bundle_pem_size,
                                    "TLS CA bundle PEM",
                                    error);
  if (status != VECTIS_OK) {
    vectis_destroy_impl(impl);
    free(app);
    return NULL;
  }
  status = vectis_copy_source_bytes(&effective->tls.client_ca_bundle,
                                    effective->tls.client_ca_bundle_pem,
                                    effective->tls.client_ca_bundle_pem_size,
                                    &impl->client_ca_bundle_pem,
                                    &impl->client_ca_bundle_pem_size,
                                    "TLS client CA bundle PEM",
                                    error);
  if (status != VECTIS_OK) {
    vectis_destroy_impl(impl);
    free(app);
    return NULL;
  }

  status = vectis_copy_source_bytes(&effective->lockd.client_bundle,
                                    effective->lockd.client_bundle_pem,
                                    effective->lockd.client_bundle_pem_size,
                                    &impl->client_bundle_pem,
                                    &impl->client_bundle_pem_size,
                                    "lockd client bundle PEM",
                                    error);
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

  app->vt = &vectis_default_methods;
  app->impl = impl;
  return app;
}

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

static vectis_status vectis_app_start_impl(vectis_app *app, vectis_error *error) {
  vectis_app_impl *impl;
  vectis_status status;
  vectis_kore_runtime_config kore_config;
  size_t route_count;
  size_t max_streaming_body_bytes;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  impl = (vectis_app_impl *)app->impl;

  status = vectis_validate_startable(impl, error);
  if (status != VECTIS_OK) {
    return status;
  }
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
    kore_config.tls_mode = impl->tls_mode;
    kore_config.acme_email = impl->acme_email;
    kore_config.acme_directory_url = impl->acme_directory_url;
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
    max_streaming_body_bytes = vectis_app_max_streaming_body_bytes(impl);
    if (max_streaming_body_bytes > kore_config.server.max_request_body_bytes) {
      kore_config.server.max_request_body_bytes = max_streaming_body_bytes;
    }
    kore_config.body_disk_offload_bytes = vectis_app_min_streaming_memory_limit_bytes(impl);
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
  if (app == NULL || app->vt == NULL || app->vt->start == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  return app->vt->start(app, error);
}

static vectis_status vectis_app_stop_impl(vectis_app *app, vectis_error *error) {
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
  if (app == NULL || app->vt == NULL || app->vt->stop == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  return app->vt->stop(app, error);
}

static int vectis_route_conflicts(const vectis_route_entry *existing,
                                  const vectis_route_config *candidate) {
  vectis_http_methods methods;

  methods = vectis_normalize_methods(candidate->method, candidate->methods);
  return (existing->methods & methods) != 0u &&
         existing->path_kind == candidate->path_kind &&
         strcmp(existing->path, candidate->path) == 0;
}

static vectis_status vectis_app_register_route_impl(vectis_app *app,
                                                    const vectis_route_config *route,
                                                    vectis_error *error) {
  return vectis_app_register_route_owned_userdata(app, route, 0, error);
}

static vectis_status vectis_app_register_route_owned_userdata(vectis_app *app,
                                                              const vectis_route_config *route,
                                                              int owns_userdata,
                                                              vectis_error *error) {
  vectis_app_impl *impl;
  vectis_route_entry *grown;
  size_t i;
  size_t next_capacity;
  vectis_status status;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }

  status = vectis_validate_route(route, error);
  if (status != VECTIS_OK) {
    return status;
  }

  impl = (vectis_app_impl *)app->impl;
  status = vectis_validate_body_policy(&route->body, &impl->server, error);
  if (status != VECTIS_OK) {
    return status;
  }
  (void)pthread_mutex_lock(&impl->mutex);
  for (i = 0u; i < impl->route_count; ++i) {
    if (vectis_route_conflicts(&impl->routes[i], route)) {
      (void)pthread_mutex_unlock(&impl->mutex);
      vectis_set_errorf(error,
                        VECTIS_ERR_CONFLICT,
                        "duplicate route registration for %s",
                        route->path);
      return VECTIS_ERR_CONFLICT;
    }
  }

  if (impl->route_count == impl->route_capacity) {
    next_capacity = impl->route_capacity == 0u ? 4u : impl->route_capacity * 2u;
    grown = (vectis_route_entry *)realloc(impl->routes, next_capacity * sizeof(*grown));
    if (grown == NULL) {
      (void)pthread_mutex_unlock(&impl->mutex);
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to grow route registry");
      return VECTIS_ERR_NOMEM;
    }
    impl->routes = grown;
    impl->route_capacity = next_capacity;
  }

  impl->routes[impl->route_count].method = route->method;
  impl->routes[impl->route_count].methods = vectis_normalize_methods(route->method, route->methods);
  impl->routes[impl->route_count].path_kind = route->path_kind;
  impl->routes[impl->route_count].path = vectis_strdup(route->path);
  impl->routes[impl->route_count].body = route->body;
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
  if (app == NULL || app->vt == NULL || app->vt->register_route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  return app->vt->register_route(app, route, error);
}

static char *vectis_join_route_prefix(const char *prefix,
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
  if (prefix == NULL || prefix[0] == '\0') {
    return vectis_strdup(path);
  }
  if (prefix[0] != '/') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "route prefix must start with /");
    return NULL;
  }
  prefix_len = strlen(prefix);
  path_len = strlen(path);
  need_slash = prefix[prefix_len - 1u] == '/' || path[0] == '/' ? 0u : 1u;
  joined = (char *)malloc(prefix_len + need_slash + path_len + 1u);
  if (joined == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate prefixed route path");
    return NULL;
  }
  (void)snprintf(joined,
                 prefix_len + need_slash + path_len + 1u,
                 "%s%s%s",
                 prefix,
                 need_slash ? "/" : "",
                 path[0] == '/' && prefix[prefix_len - 1u] == '/' ? path + 1 : path);
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
  path = vectis_join_route_prefix(prefix, route->path, error);
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

static vectis_http_methods vectis_static_methods_or_default(vectis_http_methods methods) {
  if (methods == VECTIS_HTTP_METHODS_NONE) {
    return VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD;
  }
  return methods;
}

static vectis_static_route_data *vectis_static_route_data_new(int directory,
                                                              const char *path_prefix,
                                                              const char *file_path,
                                                              const char *root_dir,
                                                              const char *content_type,
                                                              const char *index_file,
                                                              vectis_error *error) {
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
  data = (vectis_static_route_data *)calloc(1u, total);
  if (data == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate static route data");
    return NULL;
  }
  data->directory = directory;
  cursor = (char *)(data + 1);
#define VECTIS_COPY_STATIC_FIELD(field, value)       \
  do {                                               \
    if ((value) != NULL) {                           \
      len = strlen(value) + 1u;                      \
      memcpy(cursor, value, len);                    \
      data->field = cursor;                          \
      cursor += len;                                 \
    }                                                \
  } while (0)
  VECTIS_COPY_STATIC_FIELD(path_prefix, path_prefix);
  VECTIS_COPY_STATIC_FIELD(file_path, file_path);
  VECTIS_COPY_STATIC_FIELD(root_dir, root_dir);
  VECTIS_COPY_STATIC_FIELD(content_type, content_type);
  VECTIS_COPY_STATIC_FIELD(index_file, index_file);
#undef VECTIS_COPY_STATIC_FIELD
  (void)len;
  return data;
}

static vectis_status vectis_static_response(vectis_request *request,
                                            vectis_response *response,
                                            const char *content_type,
                                            const char *file_path,
                                            vectis_error *error) {
  if (vectis_request_method(request) == VECTIS_HTTP_HEAD) {
    if (content_type != NULL &&
        vectis_response_header(response, "content-type", content_type, error) != VECTIS_OK) {
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
  return vectis_static_response(request, response, data->content_type, data->file_path, error);
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
      if (len == 0u ||
          (len == 1u && segment[0] == '.') ||
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

static char *vectis_join_file_path(const char *root,
                                   const char *relative,
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
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate static file path");
    return NULL;
  }
  (void)snprintf(path,
                 root_len + need_slash + relative_len + 1u,
                 "%s%s%s",
                 root,
                 need_slash ? "/" : "",
                 relative);
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
  if (data == NULL || data->path_prefix == NULL || data->root_dir == NULL || path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "static directory route is invalid");
    return VECTIS_ERR_INVALID;
  }
  prefix_len = strlen(data->path_prefix);
  if (strncmp(path, data->path_prefix, prefix_len) != 0) {
    return vectis_response_status(response, 404, error);
  }
  if (path[prefix_len] == '\0') {
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
  status = vectis_static_response(request, response, data->content_type, file_path, error);
  free(file_path);
  return status;
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
  len = 1u + strlen(prefix) + extra + strlen("(/.*)?$") + 1u;
  regex = (char *)malloc(len);
  if (regex == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate static directory route regex");
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

vectis_status vectis_register_static_file(vectis_app *app,
                                          const vectis_static_file_config *config,
                                          vectis_error *error) {
  vectis_route_config route;
  vectis_static_route_data *data;
  vectis_status status;

  if (config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "static file config is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->path == NULL || config->file_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "static file path and file_path are required");
    return VECTIS_ERR_INVALID;
  }
  data = vectis_static_route_data_new(0,
                                      NULL,
                                      config->file_path,
                                      NULL,
                                      config->content_type != NULL ? config->content_type :
                                                                     "application/octet-stream",
                                      NULL,
                                      error);
  if (data == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  route = vectis_route_methods(vectis_static_methods_or_default(config->methods),
                               config->path,
                               vectis_static_file_dispatch,
                               data);
  status = vectis_app_register_route_owned_userdata(app, &route, 1, error);
  if (status != VECTIS_OK) {
    free(data);
  }
  return status;
}

vectis_status vectis_register_static_directory(vectis_app *app,
                                               const vectis_static_directory_config *config,
                                               vectis_error *error) {
  vectis_route_config route;
  vectis_static_route_data *data;
  vectis_status status;
  char *regex;

  if (config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "static directory config is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->path_prefix == NULL || config->root_dir == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "static directory path_prefix and root_dir are required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_validate_route_path(config->path_prefix, VECTIS_ROUTE_PATH_LITERAL, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  regex = vectis_static_directory_regex(config->path_prefix, error);
  if (regex == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  data = vectis_static_route_data_new(1,
                                      config->path_prefix,
                                      NULL,
                                      config->root_dir,
                                      config->content_type != NULL ? config->content_type :
                                                                     "application/octet-stream",
                                      config->index_file != NULL ? config->index_file : "index.html",
                                      error);
  if (data == NULL) {
    free(regex);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  vectis_route_config_init(&route);
  route.method = vectis_first_method(vectis_static_methods_or_default(config->methods));
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

static vectis_status vectis_json_route_alloc_input(vectis_request *request,
                                                   const lonejson_map *map,
                                                   size_t size,
                                                   void **out,
                                                   vectis_error *error) {
  void *input;
  vectis_status status;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json input output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  if (map == NULL) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  input = calloc(1u, size);
  if (input == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate json route input");
    return VECTIS_ERR_NOMEM;
  }
  lonejson_init(map, input);
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
  vectis_status status;

  adapter = (vectis_json_route_adapter *)userdata;
  if (adapter == NULL || adapter->handler == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json route adapter is invalid");
    return VECTIS_ERR_INVALID;
  }

  input = NULL;
  output = NULL;
  status = vectis_json_route_alloc_input(request,
                                         adapter->input_map,
                                         adapter->input_size,
                                         &input,
                                         error);
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
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate json route output");
      return VECTIS_ERR_NOMEM;
    }
    lonejson_init(adapter->output_map, output);
  }

  status = adapter->handler(app, request, input, output, adapter->userdata, error);
  if (status == VECTIS_OK) {
    if (adapter->output_map != NULL) {
      status = vectis_response_json(response, 200, adapter->output_map, output, error);
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "typed json route adapter is invalid");
    return VECTIS_ERR_INVALID;
  }
  input = NULL;
  status = vectis_json_route_alloc_input(request,
                                         adapter->input_map,
                                         adapter->input_size,
                                         &input,
                                         error);
  if (status != VECTIS_OK) {
    return status;
  }
  json_response.response = response;
  json_response.sent = 0;
  status = adapter->handler(app, request, input, &json_response, adapter->userdata, error);
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
  if (vectis_validate_methods(route->method, route->methods, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (vectis_validate_route_path(route->path, route->path_kind, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (route->handler == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json route handler is required");
    return VECTIS_ERR_INVALID;
  }
  if (route->input_map != NULL && (route->input_size == 0u || route->input_size > 10485760u)) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json route input_size is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (route->input_map != NULL && route->input_size != route->input_map->struct_size) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json route input_size does not match lonejson map");
    return VECTIS_ERR_INVALID;
  }
  if (route->output_map != NULL && (route->output_size == 0u || route->output_size > 10485760u)) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json route output_size is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (route->output_map != NULL && route->output_size != route->output_map->struct_size) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json route output_size does not match lonejson map");
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
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate json route adapter");
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

vectis_status vectis_register_json_typed_route(vectis_app *app,
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
  if (vectis_validate_methods(route->method, route->methods, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (vectis_validate_route_path(route->path, route->path_kind, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (route->handler == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "typed json route handler is required");
    return VECTIS_ERR_INVALID;
  }
  if (route->input_map != NULL && (route->input_size == 0u || route->input_size > 10485760u)) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "typed json route input_size is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (route->input_map != NULL && route->input_size != route->input_map->struct_size) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "typed json route input_size does not match lonejson map");
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
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate typed json route adapter");
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

vectis_status vectis_register_prefixed_json_route(vectis_app *app,
                                                  const char *prefix,
                                                  const vectis_json_route_config *route,
                                                  vectis_error *error) {
  vectis_json_route_config prefixed;
  char *path;
  vectis_status status;

  if (route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json route is required");
    return VECTIS_ERR_INVALID;
  }
  path = vectis_join_route_prefix(prefix, route->path, error);
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

vectis_status vectis_register_prefixed_json_typed_route(vectis_app *app,
                                                        const char *prefix,
                                                        const vectis_json_typed_route_config *route,
                                                        vectis_error *error) {
  vectis_json_typed_route_config prefixed;
  char *path;
  vectis_status status;

  if (route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "typed json route is required");
    return VECTIS_ERR_INVALID;
  }
  path = vectis_join_route_prefix(prefix, route->path, error);
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
  if (methods == VECTIS_HTTP_METHODS_NONE || (methods & ~VECTIS_HTTP_METHODS_ALL) != 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "OpenAPI methods are invalid");
    return VECTIS_ERR_INVALID;
  }
  if (path == NULL || path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "OpenAPI path is required");
    return VECTIS_ERR_INVALID;
  }
  if (doc == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "OpenAPI route doc is required");
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
    next_capacity = impl->openapi_doc_capacity == 0u ? 4u : impl->openapi_doc_capacity * 2u;
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
  if (schema.map != NULL && schema.map->name != NULL && schema.map->name[0] != '\0') {
    return schema.map->name;
  }
  return "Schema";
}

static vectis_status vectis_append_lonejson_string(vectis_string_builder *builder,
                                                   const char *value,
                                                   vectis_error *error) {
  vectis_json_string_box box;
  lonejson_error json_error;
  char *json;
  char *start;
  size_t json_size;
  size_t length;
  union {
    const char *const_value;
    char *mutable_value;
  } ptr;

  ptr.const_value = value != NULL ? value : "";
  box.value = ptr.mutable_value;
  json = lonejson_serialize_alloc(&vectis_json_string_box_map,
                                  &box,
                                  &json_size,
                                  NULL,
                                  &json_error);
  if (json == NULL) {
    vectis_set_errorf(error,
                      VECTIS_ERR_INVALID,
                      "failed to serialize JSON string: %s",
                      json_error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
    }
    return VECTIS_ERR_INVALID;
  }
  start = strchr(json, ':');
  if (start == NULL || json_size < 2u) {
    free(json);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to extract serialized JSON string");
    return VECTIS_ERR_STATE;
  }
  ++start;
  length = json_size - (size_t)(start - json);
  if (length == 0u || start[length - 1u] != '}') {
    free(json);
    vectis_set_error(error, VECTIS_ERR_STATE, "serialized JSON string shape is invalid");
    return VECTIS_ERR_STATE;
  }
  length--;
  if (vectis_string_builder_append_n(builder, start, length, error) != VECTIS_OK) {
    free(json);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  free(json);
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
          vectis_string_builder_append_n(builder, start, (size_t)(p - start), error) != VECTIS_OK ||
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

static vectis_status vectis_openapi_append_schema_json(vectis_string_builder *builder,
                                                       vectis_openapi_schema schema,
                                                       vectis_error *error) {
  const lonejson_map *map;
  const lonejson_field *field;
  size_t i;
  size_t required_count;
  int first;

  map = schema.map;
  if (map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "OpenAPI schema map is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_string_builder_append(builder, "{\"type\":\"object\",\"properties\":{", error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  for (i = 0u; i < map->field_count; ++i) {
    field = &map->fields[i];
    if (i > 0u && vectis_string_builder_append(builder, ",", error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    if (vectis_append_lonejson_string(builder, field->json_key, error) != VECTIS_OK ||
        vectis_string_builder_append(builder, ":", error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    if (vectis_openapi_field_is_array(field)) {
      if (vectis_string_builder_appendf(builder,
                                        error,
                                        "{\"type\":\"array\",\"items\":{\"type\":\"%s\"}}",
                                        vectis_openapi_field_type(field)) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
    } else if ((field->kind == LONEJSON_FIELD_KIND_OBJECT ||
                field->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY) &&
               field->submap != NULL) {
      if (vectis_string_builder_append(builder, "{\"$ref\":\"#/components/schemas/", error) != VECTIS_OK ||
          vectis_string_builder_append(builder, field->submap->name, error) != VECTIS_OK ||
          vectis_string_builder_append(builder, "\"}", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
    } else if (vectis_string_builder_appendf(builder,
                                             error,
                                             "{\"type\":\"%s\"}",
                                             vectis_openapi_field_type(field)) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
  }
  required_count = 0u;
  for (i = 0u; i < map->field_count; ++i) {
    if ((map->fields[i].flags & LONEJSON_FIELD_REQUIRED) != 0u) {
      required_count++;
    }
  }
  if (vectis_string_builder_append(builder, "}", error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  if (required_count > 0u) {
    if (vectis_string_builder_append(builder, ",\"required\":[", error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    first = 1;
    for (i = 0u; i < map->field_count; ++i) {
      if ((map->fields[i].flags & LONEJSON_FIELD_REQUIRED) != 0u) {
        if (!first && vectis_string_builder_append(builder, ",", error) != VECTIS_OK) {
          return error != NULL ? error->code : VECTIS_ERR_NOMEM;
        }
        if (vectis_append_lonejson_string(builder, map->fields[i].json_key, error) != VECTIS_OK) {
          return error != NULL ? error->code : VECTIS_ERR_NOMEM;
        }
        first = 0;
      }
    }
    if (vectis_string_builder_append(builder, "]", error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
  }
  return vectis_string_builder_append(builder, "}", error);
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

static vectis_status vectis_openapi_collect_schema(vectis_openapi_schema **schemas,
                                                   size_t *count,
                                                   size_t *capacity,
                                                   vectis_openapi_schema schema,
                                                   vectis_error *error) {
  vectis_openapi_schema *grown;
  size_t next_capacity;

  if (schema.map == NULL || vectis_openapi_schema_seen(*schemas, *count, schema)) {
    return VECTIS_OK;
  }
  if (*count == *capacity) {
    next_capacity = *capacity == 0u ? 8u : *capacity * 2u;
    grown = (vectis_openapi_schema *)realloc(*schemas, next_capacity * sizeof(*grown));
    if (grown == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to collect OpenAPI schemas");
      return VECTIS_ERR_NOMEM;
    }
    *schemas = grown;
    *capacity = next_capacity;
  }
  (*schemas)[*count] = schema;
  (*count)++;
  return VECTIS_OK;
}

static vectis_status vectis_openapi_collect_doc_schemas(const vectis_openapi_route_doc *doc,
                                                        vectis_openapi_schema **schemas,
                                                        size_t *count,
                                                        size_t *capacity,
                                                        vectis_error *error) {
  size_t i;

  if (doc == NULL) {
    return VECTIS_OK;
  }
  if (vectis_openapi_collect_schema(schemas, count, capacity, doc->request_schema, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  for (i = 0u; i < doc->response_count; ++i) {
    if (vectis_openapi_collect_schema(schemas,
                                      count,
                                      capacity,
                                      doc->responses[i].schema,
                                      error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
  }
  return VECTIS_OK;
}

static vectis_status vectis_openapi_append_ref_json(vectis_string_builder *builder,
                                                    vectis_openapi_schema schema,
                                                    vectis_error *error) {
  if (vectis_string_builder_append(builder, "{\"$ref\":\"#/components/schemas/", error) != VECTIS_OK ||
      vectis_string_builder_append(builder, vectis_openapi_schema_name(schema), error) != VECTIS_OK ||
      vectis_string_builder_append(builder, "\"}", error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  return VECTIS_OK;
}

static vectis_status vectis_generate_openapi_json(vectis_app_impl *impl,
                                                  const vectis_openapi_document *document,
                                                  vectis_string_builder *builder,
                                                  vectis_error *error) {
  vectis_openapi_schema *schemas;
  size_t schema_count;
  size_t schema_capacity;
  size_t i;
  size_t j;
  int first_path;
  int first_method;
  int first_response;
  vectis_http_method method;
  vectis_string_builder path_builder;
  const vectis_openapi_route_doc *doc;

  schemas = NULL;
  schema_count = 0u;
  schema_capacity = 0u;
  memset(&path_builder, 0, sizeof(path_builder));
  for (i = 0u; i < impl->openapi_doc_count; ++i) {
    if (vectis_openapi_collect_doc_schemas(&impl->openapi_docs[i].doc,
                                           &schemas,
                                           &schema_count,
                                           &schema_capacity,
                                           error) != VECTIS_OK) {
      free(schemas);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
  }
  if (vectis_string_builder_append(builder, "{\"openapi\":\"3.1.0\",\"info\":{\"title\":", error) != VECTIS_OK ||
      vectis_append_lonejson_string(builder, document->title, error) != VECTIS_OK ||
      vectis_string_builder_append(builder, ",\"version\":", error) != VECTIS_OK ||
      vectis_append_lonejson_string(builder, document->version, error) != VECTIS_OK ||
      vectis_string_builder_append(builder, "},\"paths\":{", error) != VECTIS_OK) {
    free(schemas);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  first_path = 1;
  for (i = 0u; i < impl->openapi_doc_count; ++i) {
    doc = &impl->openapi_docs[i].doc;
    path_builder.size = 0u;
    if (path_builder.data != NULL) {
      path_builder.data[0] = '\0';
    }
    if (vectis_openapi_append_path(&path_builder, impl->openapi_docs[i].path, error) != VECTIS_OK) {
      vectis_string_builder_cleanup(&path_builder);
      free(schemas);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    if (!first_path && vectis_string_builder_append(builder, ",", error) != VECTIS_OK) {
      vectis_string_builder_cleanup(&path_builder);
      free(schemas);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    if (vectis_append_lonejson_string(builder, path_builder.data, error) != VECTIS_OK ||
        vectis_string_builder_append(builder, ":{", error) != VECTIS_OK) {
      vectis_string_builder_cleanup(&path_builder);
      free(schemas);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    first_method = 1;
    for (method = VECTIS_HTTP_GET; method <= VECTIS_HTTP_OPTIONS; ++method) {
      if ((impl->openapi_docs[i].methods & VECTIS_HTTP_METHOD_MASK(method)) != 0u) {
        if (!first_method && vectis_string_builder_append(builder, ",", error) != VECTIS_OK) {
          vectis_string_builder_cleanup(&path_builder);
          free(schemas);
          return error != NULL ? error->code : VECTIS_ERR_NOMEM;
        }
        if (vectis_append_lonejson_string(builder, vectis_openapi_method_name(method), error) != VECTIS_OK ||
            vectis_string_builder_append(builder, ":{", error) != VECTIS_OK) {
          vectis_string_builder_cleanup(&path_builder);
          free(schemas);
          return error != NULL ? error->code : VECTIS_ERR_NOMEM;
        }
        if (doc->operation_id != NULL) {
          if (vectis_string_builder_append(builder, "\"operationId\":", error) != VECTIS_OK ||
              vectis_append_lonejson_string(builder, doc->operation_id, error) != VECTIS_OK ||
              vectis_string_builder_append(builder, ",", error) != VECTIS_OK) {
            vectis_string_builder_cleanup(&path_builder);
            free(schemas);
            return error != NULL ? error->code : VECTIS_ERR_NOMEM;
          }
        }
        if (doc->summary != NULL) {
          if (vectis_string_builder_append(builder, "\"summary\":", error) != VECTIS_OK ||
              vectis_append_lonejson_string(builder, doc->summary, error) != VECTIS_OK ||
              vectis_string_builder_append(builder, ",", error) != VECTIS_OK) {
            vectis_string_builder_cleanup(&path_builder);
            free(schemas);
            return error != NULL ? error->code : VECTIS_ERR_NOMEM;
          }
        }
        if (doc->tag_count > 0u) {
          if (vectis_string_builder_append(builder, "\"tags\":[", error) != VECTIS_OK) {
            vectis_string_builder_cleanup(&path_builder);
            free(schemas);
            return error != NULL ? error->code : VECTIS_ERR_NOMEM;
          }
          for (j = 0u; j < doc->tag_count; ++j) {
            if (j > 0u && vectis_string_builder_append(builder, ",", error) != VECTIS_OK) {
              vectis_string_builder_cleanup(&path_builder);
              free(schemas);
              return error != NULL ? error->code : VECTIS_ERR_NOMEM;
            }
            if (vectis_append_lonejson_string(builder, doc->tags[j], error) != VECTIS_OK) {
              vectis_string_builder_cleanup(&path_builder);
              free(schemas);
              return error != NULL ? error->code : VECTIS_ERR_NOMEM;
            }
          }
          if (vectis_string_builder_append(builder, "],", error) != VECTIS_OK) {
            vectis_string_builder_cleanup(&path_builder);
            free(schemas);
            return error != NULL ? error->code : VECTIS_ERR_NOMEM;
          }
        }
        if (doc->request_schema.map != NULL) {
          if (vectis_string_builder_append(builder,
                                           "\"requestBody\":{\"content\":{\"application/json\":{\"schema\":",
                                           error) != VECTIS_OK ||
              vectis_openapi_append_ref_json(builder, doc->request_schema, error) != VECTIS_OK ||
              vectis_string_builder_append(builder, "}},\"required\":true},", error) != VECTIS_OK) {
            vectis_string_builder_cleanup(&path_builder);
            free(schemas);
            return error != NULL ? error->code : VECTIS_ERR_NOMEM;
          }
        }
        if (vectis_string_builder_append(builder, "\"responses\":{", error) != VECTIS_OK) {
          vectis_string_builder_cleanup(&path_builder);
          free(schemas);
          return error != NULL ? error->code : VECTIS_ERR_NOMEM;
        }
        first_response = 1;
        for (j = 0u; j < doc->response_count; ++j) {
          if (!first_response && vectis_string_builder_append(builder, ",", error) != VECTIS_OK) {
            vectis_string_builder_cleanup(&path_builder);
            free(schemas);
            return error != NULL ? error->code : VECTIS_ERR_NOMEM;
          }
          if (vectis_string_builder_appendf(builder, error, "\"%d\":{\"description\":", doc->responses[j].status_code) != VECTIS_OK ||
              vectis_append_lonejson_string(builder,
                                        doc->responses[j].description != NULL ?
                                            doc->responses[j].description : "",
                                        error) != VECTIS_OK ||
              vectis_string_builder_append(builder,
                                           ",\"content\":{\"application/json\":{\"schema\":",
                                           error) != VECTIS_OK ||
              vectis_openapi_append_ref_json(builder, doc->responses[j].schema, error) != VECTIS_OK ||
              vectis_string_builder_append(builder, "}}}", error) != VECTIS_OK) {
            vectis_string_builder_cleanup(&path_builder);
            free(schemas);
            return error != NULL ? error->code : VECTIS_ERR_NOMEM;
          }
          first_response = 0;
        }
        if (vectis_string_builder_append(builder, "}}", error) != VECTIS_OK) {
          vectis_string_builder_cleanup(&path_builder);
          free(schemas);
          return error != NULL ? error->code : VECTIS_ERR_NOMEM;
        }
        first_method = 0;
      }
    }
    if (vectis_string_builder_append(builder, "}", error) != VECTIS_OK) {
      vectis_string_builder_cleanup(&path_builder);
      free(schemas);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    first_path = 0;
  }
  if (vectis_string_builder_append(builder, "},\"components\":{\"schemas\":{", error) != VECTIS_OK) {
    vectis_string_builder_cleanup(&path_builder);
    free(schemas);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  for (i = 0u; i < schema_count; ++i) {
    if (i > 0u && vectis_string_builder_append(builder, ",", error) != VECTIS_OK) {
      vectis_string_builder_cleanup(&path_builder);
      free(schemas);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    if (vectis_append_lonejson_string(builder, vectis_openapi_schema_name(schemas[i]), error) != VECTIS_OK ||
        vectis_string_builder_append(builder, ":", error) != VECTIS_OK ||
        vectis_openapi_append_schema_json(builder, schemas[i], error) != VECTIS_OK) {
      vectis_string_builder_cleanup(&path_builder);
      free(schemas);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
  }
  vectis_string_builder_cleanup(&path_builder);
  free(schemas);
  return vectis_string_builder_append(builder, "}}}", error);
}

static vectis_status vectis_generate_openapi_yaml(vectis_app_impl *impl,
                                                  const vectis_openapi_document *document,
                                                  vectis_string_builder *builder,
                                                  vectis_error *error) {
  vectis_openapi_schema *schemas;
  size_t schema_count;
  size_t schema_capacity;
  size_t i;
  size_t j;
  vectis_http_method method;
  vectis_string_builder path_builder;
  const vectis_openapi_route_doc *doc;
  const lonejson_map *map;
  const lonejson_field *field;

  schemas = NULL;
  schema_count = 0u;
  schema_capacity = 0u;
  memset(&path_builder, 0, sizeof(path_builder));
  for (i = 0u; i < impl->openapi_doc_count; ++i) {
    if (vectis_openapi_collect_doc_schemas(&impl->openapi_docs[i].doc,
                                           &schemas,
                                           &schema_count,
                                           &schema_capacity,
                                           error) != VECTIS_OK) {
      free(schemas);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
  }
  if (vectis_string_builder_append(builder, "openapi: 3.1.0\ninfo:\n  title: ", error) != VECTIS_OK ||
      vectis_append_lonejson_string(builder, document->title, error) != VECTIS_OK ||
      vectis_string_builder_append(builder, "\n  version: ", error) != VECTIS_OK ||
      vectis_append_lonejson_string(builder, document->version, error) != VECTIS_OK ||
      vectis_string_builder_append(builder, "\npaths:\n", error) != VECTIS_OK) {
    free(schemas);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  for (i = 0u; i < impl->openapi_doc_count; ++i) {
    doc = &impl->openapi_docs[i].doc;
    path_builder.size = 0u;
    if (path_builder.data != NULL) {
      path_builder.data[0] = '\0';
    }
    if (vectis_openapi_append_path(&path_builder, impl->openapi_docs[i].path, error) != VECTIS_OK ||
        vectis_string_builder_append(builder, "  ", error) != VECTIS_OK ||
        vectis_append_lonejson_string(builder, path_builder.data, error) != VECTIS_OK ||
        vectis_string_builder_append(builder, ":\n", error) != VECTIS_OK) {
      vectis_string_builder_cleanup(&path_builder);
      free(schemas);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    for (method = VECTIS_HTTP_GET; method <= VECTIS_HTTP_OPTIONS; ++method) {
      if ((impl->openapi_docs[i].methods & VECTIS_HTTP_METHOD_MASK(method)) != 0u) {
        if (vectis_string_builder_appendf(builder, error, "    %s:\n", vectis_openapi_method_name(method)) != VECTIS_OK) {
          vectis_string_builder_cleanup(&path_builder);
          free(schemas);
          return error != NULL ? error->code : VECTIS_ERR_NOMEM;
        }
        if (doc->operation_id != NULL &&
            (vectis_string_builder_append(builder, "      operationId: ", error) != VECTIS_OK ||
             vectis_append_lonejson_string(builder, doc->operation_id, error) != VECTIS_OK ||
             vectis_string_builder_append(builder, "\n", error) != VECTIS_OK)) {
          vectis_string_builder_cleanup(&path_builder);
          free(schemas);
          return error != NULL ? error->code : VECTIS_ERR_NOMEM;
        }
        if (doc->summary != NULL &&
            (vectis_string_builder_append(builder, "      summary: ", error) != VECTIS_OK ||
             vectis_append_lonejson_string(builder, doc->summary, error) != VECTIS_OK ||
             vectis_string_builder_append(builder, "\n", error) != VECTIS_OK)) {
          vectis_string_builder_cleanup(&path_builder);
          free(schemas);
          return error != NULL ? error->code : VECTIS_ERR_NOMEM;
        }
        if (doc->tag_count > 0u) {
          if (vectis_string_builder_append(builder, "      tags:\n", error) != VECTIS_OK) {
            vectis_string_builder_cleanup(&path_builder);
            free(schemas);
            return error != NULL ? error->code : VECTIS_ERR_NOMEM;
          }
          for (j = 0u; j < doc->tag_count; ++j) {
            if (vectis_string_builder_append(builder, "        - ", error) != VECTIS_OK ||
                vectis_append_lonejson_string(builder, doc->tags[j], error) != VECTIS_OK ||
                vectis_string_builder_append(builder, "\n", error) != VECTIS_OK) {
              vectis_string_builder_cleanup(&path_builder);
              free(schemas);
              return error != NULL ? error->code : VECTIS_ERR_NOMEM;
            }
          }
        }
        if (doc->request_schema.map != NULL &&
            (vectis_string_builder_append(builder,
                                          "      requestBody:\n        required: true\n        content:\n          application/json:\n            schema:\n              $ref: \"#/components/schemas/",
                                          error) != VECTIS_OK ||
             vectis_string_builder_append(builder, vectis_openapi_schema_name(doc->request_schema), error) != VECTIS_OK ||
             vectis_string_builder_append(builder, "\"\n", error) != VECTIS_OK)) {
          vectis_string_builder_cleanup(&path_builder);
          free(schemas);
          return error != NULL ? error->code : VECTIS_ERR_NOMEM;
        }
        if (vectis_string_builder_append(builder, "      responses:\n", error) != VECTIS_OK) {
          vectis_string_builder_cleanup(&path_builder);
          free(schemas);
          return error != NULL ? error->code : VECTIS_ERR_NOMEM;
        }
        for (j = 0u; j < doc->response_count; ++j) {
          if (vectis_string_builder_appendf(builder,
                                            error,
                                            "        \"%d\":\n          description: ",
                                            doc->responses[j].status_code) != VECTIS_OK ||
              vectis_append_lonejson_string(builder,
                                        doc->responses[j].description != NULL ?
                                            doc->responses[j].description : "",
                                        error) != VECTIS_OK ||
              vectis_string_builder_append(builder,
                                           "\n          content:\n            application/json:\n              schema:\n                $ref: \"#/components/schemas/",
                                           error) != VECTIS_OK ||
              vectis_string_builder_append(builder, vectis_openapi_schema_name(doc->responses[j].schema), error) != VECTIS_OK ||
              vectis_string_builder_append(builder, "\"\n", error) != VECTIS_OK) {
            vectis_string_builder_cleanup(&path_builder);
            free(schemas);
            return error != NULL ? error->code : VECTIS_ERR_NOMEM;
          }
        }
      }
    }
  }
  if (vectis_string_builder_append(builder, "components:\n  schemas:\n", error) != VECTIS_OK) {
    vectis_string_builder_cleanup(&path_builder);
    free(schemas);
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  for (i = 0u; i < schema_count; ++i) {
    map = schemas[i].map;
    if (vectis_string_builder_appendf(builder,
                                      error,
                                      "    %s:\n      type: object\n      properties:\n",
                                      vectis_openapi_schema_name(schemas[i])) != VECTIS_OK) {
      vectis_string_builder_cleanup(&path_builder);
      free(schemas);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    for (j = 0u; j < map->field_count; ++j) {
      field = &map->fields[j];
      if (vectis_string_builder_appendf(builder,
                                        error,
                                        "        %s:\n          type: %s\n",
                                        field->json_key,
                                        vectis_openapi_field_is_array(field) ?
                                            "array" : vectis_openapi_field_type(field)) != VECTIS_OK) {
        vectis_string_builder_cleanup(&path_builder);
        free(schemas);
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
      if (vectis_openapi_field_is_array(field) &&
          vectis_string_builder_appendf(builder,
                                        error,
                                        "          items:\n            type: %s\n",
                                        vectis_openapi_field_type(field)) != VECTIS_OK) {
        vectis_string_builder_cleanup(&path_builder);
        free(schemas);
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
    }
    if (vectis_string_builder_append(builder, "      required:\n", error) != VECTIS_OK) {
      vectis_string_builder_cleanup(&path_builder);
      free(schemas);
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    for (j = 0u; j < map->field_count; ++j) {
      if ((map->fields[j].flags & LONEJSON_FIELD_REQUIRED) != 0u &&
          vectis_string_builder_appendf(builder,
                                        error,
                                        "        - %s\n",
                                        map->fields[j].json_key) != VECTIS_OK) {
        vectis_string_builder_cleanup(&path_builder);
        free(schemas);
        return error != NULL ? error->code : VECTIS_ERR_NOMEM;
      }
    }
  }
  vectis_string_builder_cleanup(&path_builder);
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
                                          document,
                                          &builder,
                                          error);
  } else {
    status = vectis_generate_openapi_yaml((vectis_app_impl *)app->impl,
                                          document,
                                          &builder,
                                          error);
  }
  if (status != VECTIS_OK) {
    vectis_string_builder_cleanup(&builder);
    return status;
  }
  if (format == VECTIS_OPENAPI_JSON) {
    lonejson_json_value_init(&json_value);
    json_status = lonejson_json_value_set_buffer(&json_value,
                                                 builder.data,
                                                 builder.size,
                                                 &json_error);
    if (json_status != LONEJSON_STATUS_OK) {
      lonejson_json_value_cleanup(&json_value);
      vectis_string_builder_cleanup(&builder);
      vectis_set_errorf(error,
                        VECTIS_ERR_INVALID,
                        "failed to validate generated OpenAPI JSON: %s",
                        json_error.message);
      if (error != NULL) {
        error->source = VECTIS_ERROR_SOURCE_LONEJSON;
        error->dependency_code = (long)json_status;
      }
      return VECTIS_ERR_INVALID;
    }
    lonejson_json_value_cleanup(&json_value);
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

static size_t vectis_app_max_streaming_body_bytes(vectis_app_impl *impl) {
  size_t max_bytes;
  size_t i;

  if (impl == NULL) {
    return 0u;
  }
  max_bytes = 0u;
  (void)pthread_mutex_lock(&impl->mutex);
  for (i = 0u; i < impl->route_count; ++i) {
    if (impl->routes[i].body.mode == VECTIS_BODY_STREAMING_UPLOAD &&
        impl->routes[i].body.max_bytes > max_bytes) {
      max_bytes = impl->routes[i].body.max_bytes;
    }
  }
  (void)pthread_mutex_unlock(&impl->mutex);
  return max_bytes;
}

static size_t vectis_app_min_streaming_memory_limit_bytes(vectis_app_impl *impl) {
  size_t min_bytes;
  size_t limit;
  size_t i;

  if (impl == NULL) {
    return 0u;
  }
  min_bytes = 0u;
  (void)pthread_mutex_lock(&impl->mutex);
  for (i = 0u; i < impl->route_count; ++i) {
    if (impl->routes[i].body.mode == VECTIS_BODY_STREAMING_UPLOAD &&
        !impl->routes[i].body.disk_spool_disabled) {
      limit = impl->routes[i].body.memory_buffer_limit_bytes;
      if (limit > 0u && (min_bytes == 0u || limit < min_bytes)) {
        min_bytes = limit;
      }
    }
  }
  (void)pthread_mutex_unlock(&impl->mutex);
  return min_bytes;
}

size_t vectis_route_count(const vectis_app *app) {
  if (app == NULL || app->vt == NULL || app->vt->route_count == NULL) {
    return 0u;
  }
  return app->vt->route_count(app);
}

vectis_status vectis_internal_invoke_route(vectis_app *app,
                                           size_t index,
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

static vectis_status vectis_add_path_param_segment(vectis_request *request,
                                                   const char *name,
                                                   size_t name_len,
                                                   const char *value,
                                                   size_t value_len,
                                                   vectis_error *error) {
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
  status = vectis_internal_request_add_path_param(request, name_copy, value_copy, error);
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
    name_len = optional ? (size_t)(route_end - route - 2) : (size_t)(route_end - route - 1);
    saved_count = request->path_param_count;

    if (optional &&
        vectis_match_param_route_segments(next_route, path, request, error)) {
      return 1;
    }
    if (!vectis_path_segment_is_safe(path, path_len)) {
      vectis_kv_truncate(&request->path_params, &request->path_param_count, saved_count);
      return 0;
    }
    if (vectis_add_path_param_segment(request,
                                      name_start,
                                      name_len,
                                      path,
                                      path_len,
                                      error) != VECTIS_OK) {
      vectis_kv_truncate(&request->path_params, &request->path_param_count, saved_count);
      return 0;
    }
    if (vectis_match_param_route_segments(next_route, next_path, request, error)) {
      return 1;
    }
    vectis_kv_truncate(&request->path_params, &request->path_param_count, saved_count);
    return 0;
  }

  if (route_len != path_len || memcmp(route, path, route_len) != 0) {
    return 0;
  }
  return vectis_match_param_route_segments(next_route, next_path, request, error);
}

static int vectis_route_path_matches(const vectis_route_entry *route,
                                     const char *path,
                                     vectis_request *request,
                                     vectis_error *error) {
  regex_t compiled;
  int regex_rc;

  if (route->path_kind == VECTIS_ROUTE_PATH_LITERAL) {
    return strcmp(route->path, path) == 0;
  }
  if (route->path_kind == VECTIS_ROUTE_PATH_PARAMS) {
    return vectis_match_param_route_segments(route->path + 1, path + 1, request, error);
  }
  if (route->path_kind == VECTIS_ROUTE_PATH_REGEX) {
    regex_rc = regcomp(&compiled, route->path, REG_EXTENDED | REG_NOSUB);
    if (regex_rc != 0) {
      vectis_set_error(error, VECTIS_ERR_INVALID, "registered regex route is invalid");
      return 0;
    }
    regex_rc = regexec(&compiled, path, 0u, NULL, 0);
    regfree(&compiled);
    return regex_rc == 0;
  }
  return 0;
}

vectis_status vectis_internal_dispatch_route(vectis_app *app,
                                             vectis_http_method method,
                                             const char *path,
                                             vectis_request *request,
                                             vectis_response *response,
                                             vectis_error *error) {
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
    vectis_kv_truncate(&request->path_params, &request->path_param_count, saved_count);
    if (!vectis_route_method_matches(&impl->routes[i], method)) {
      continue;
    }
    if (vectis_route_path_matches(&impl->routes[i], path, request, error)) {
      handler = impl->routes[i].handler;
      userdata = impl->routes[i].userdata;
      break;
    }
    if (error != NULL && error->code == VECTIS_ERR_NOMEM) {
      vectis_kv_truncate(&request->path_params, &request->path_param_count, saved_count);
      (void)pthread_mutex_unlock(&impl->mutex);
      return VECTIS_ERR_NOMEM;
    }
  }
  if (handler == NULL) {
    vectis_kv_truncate(&request->path_params, &request->path_param_count, saved_count);
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "body policy output is required");
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

static pslog_logger *vectis_app_logger_impl(vectis_app *app) {
  vectis_app_impl *impl;

  if (app == NULL || app->impl == NULL) {
    return NULL;
  }
  impl = (vectis_app_impl *)app->impl;
  return impl->logger;
}

pslog_logger *vectis_logger(vectis_app *app) {
  if (app == NULL || app->vt == NULL || app->vt->logger == NULL) {
    return NULL;
  }
  return app->vt->logger(app);
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
        impl->logger->errorf(impl->logger,
                             "vectis.lockd.open_failed",
                             "error=%s detail=%s",
                             error.message,
                             error.detail);
      }
    }
  }
  (void)pthread_mutex_unlock(&impl->mutex);
  return impl->lockd_client;
}

vectis_status vectis_consumer_service_new(vectis_app *app,
                                          const struct lc_consumer_service_config *config,
                                          vectis_consumer_service **out,
                                          vectis_error *error) {
  vectis_app_impl *impl;
  vectis_consumer_service *service;
  lc_error lcerr;
  int rc;
  vectis_status status;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "consumer service output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  if (config == NULL || config->consumers == NULL || config->consumer_count == 0u) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "consumer service config requires at least one consumer");
    return VECTIS_ERR_INVALID;
  }

  impl = (vectis_app_impl *)app->impl;
  if (!vectis_lockd_is_configured(impl)) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
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
  if (service == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate consumer service");
    return VECTIS_ERR_NOMEM;
  }

  lc_error_init(&lcerr);
  rc = lc_client_new_consumer_service(impl->lockd_client, config, &service->service, &lcerr);
  if (rc != LC_OK) {
    free(service);
    status = vectis_set_lockdc_error(error, rc, &lcerr, "failed to create lockd consumer service");
    lc_error_cleanup(&lcerr);
    return status;
  }
  lc_error_cleanup(&lcerr);
  *out = service;
  vectis_error_clear(error);
  return VECTIS_OK;
}

struct lc_consumer_service *vectis_consumer_service_raw(vectis_consumer_service *service) {
  if (service == NULL) {
    return NULL;
  }
  return service->service;
}

vectis_status vectis_consumer_service_run(vectis_consumer_service *service,
                                          vectis_error *error) {
  lc_error lcerr;
  int rc;

  if (service == NULL || service->service == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "consumer service is required");
    return VECTIS_ERR_INVALID;
  }
  lc_error_init(&lcerr);
  rc = service->service->run(service->service, &lcerr);
  if (rc != LC_OK) {
    (void)vectis_set_lockdc_error(error, rc, &lcerr, "lockd consumer service run failed");
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_consumer_service_start(vectis_consumer_service *service,
                                            vectis_error *error) {
  lc_error lcerr;
  int rc;

  if (service == NULL || service->service == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "consumer service is required");
    return VECTIS_ERR_INVALID;
  }
  lc_error_init(&lcerr);
  rc = service->service->start(service->service, &lcerr);
  if (rc != LC_OK) {
    (void)vectis_set_lockdc_error(error, rc, &lcerr, "lockd consumer service start failed");
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_consumer_service_stop(vectis_consumer_service *service,
                                           vectis_error *error) {
  int rc;

  if (service == NULL || service->service == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "consumer service is required");
    return VECTIS_ERR_INVALID;
  }
  rc = service->service->stop(service->service);
  if (rc != LC_OK) {
    vectis_set_error(error, VECTIS_ERR_STATE, "lockd consumer service stop failed");
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
  lc_error lcerr;
  int rc;

  if (service == NULL || service->service == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "consumer service is required");
    return VECTIS_ERR_INVALID;
  }
  lc_error_init(&lcerr);
  rc = service->service->wait(service->service, &lcerr);
  if (rc != LC_OK) {
    (void)vectis_set_lockdc_error(error, rc, &lcerr, "lockd consumer service wait failed");
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

vectis_status vectis_consumer_service_run_until(vectis_consumer_service *service,
                                                const volatile int *done,
                                                long timeout_ms,
                                                vectis_error *error) {
  long deadline;
  struct timespec pause_time;
  vectis_status status;

  if (service == NULL || service->service == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "consumer service is required");
    return VECTIS_ERR_INVALID;
  }
  if (done == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "done flag is required");
    return VECTIS_ERR_INVALID;
  }
  if (timeout_ms <= 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "timeout_ms must be greater than zero");
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
  if (service == NULL) {
    return;
  }
  if (service->service != NULL) {
    service->service->close(service->service);
  }
  free(service);
}

vectis_status vectis_json_validate_cstr(const char *json, vectis_error *error) {
  lonejson_error json_error;
  lonejson_status status;

  if (json == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json is required");
    return VECTIS_ERR_INVALID;
  }

  status = lonejson_validate_cstr(json, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    vectis_set_errorf(error,
                      VECTIS_ERR_INVALID,
                      "invalid json at line %lu column %lu: %s",
                      (unsigned long)json_error.line,
                      (unsigned long)json_error.column,
                      json_error.message);
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
  config->has_header = 1;
  config->strict_row_width = 1;
  config->trim_cr = 1;
  config->allow_indented_comments = 1;
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
  config.has_header = 0;
  return config;
}

vectis_dsv_config vectis_dsv_tsv_rows(void) {
  vectis_dsv_config config;

  config = vectis_dsv_tsv();
  config.has_header = 0;
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
                                            const char *data,
                                            size_t size,
                                            vectis_error *error) {
  char **next_items;
  size_t *next_sizes;
  size_t capacity;
  char *copy;

  if (fields->count == fields->capacity) {
    capacity = fields->capacity == 0u ? 8u : fields->capacity * 2u;
    next_items = (char **)realloc(fields->items, capacity * sizeof(fields->items[0]));
    if (next_items == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to grow DSV field list");
      return VECTIS_ERR_NOMEM;
    }
    fields->items = next_items;
    next_sizes = (size_t *)realloc(fields->sizes, capacity * sizeof(fields->sizes[0]));
    if (next_sizes == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to grow DSV field sizes");
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
    nread = parser->source->read(parser->source,
                                 parser->buffer,
                                 sizeof(parser->buffer),
                                 &lcerr);
    if (nread == 0u) {
      if (lcerr.code != 0) {
        (void)vectis_source_error(error, lcerr.code, &lcerr, "failed to read DSV source");
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

  status = vectis_dsv_fields_push(&parser->fields,
                                  parser->field.data,
                                  parser->field.size,
                                  error);
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
        vectis_set_error(error, VECTIS_ERR_INVALID, "unterminated quoted DSV field");
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
      if (c == parser->config.quote && parser->config.escape == parser->config.quote) {
        if (vectis_string_builder_append_n(&parser->field, "\"", 1u, error) != VECTIS_OK) {
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
          vectis_set_error(error, VECTIS_ERR_INVALID, "DSV field exceeds max_field_bytes");
          return VECTIS_ERR_INVALID;
        }
        if (vectis_string_builder_append_n(&parser->field, &ch, 1u, error) != VECTIS_OK) {
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
        parser->skip_next_lf = parser->config.trim_cr;
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
      vectis_set_error(error, VECTIS_ERR_INVALID, "DSV field exceeds max_field_bytes");
      return VECTIS_ERR_INVALID;
    }
    ch = (char)c;
    if (vectis_string_builder_append_n(&parser->field, &ch, 1u, error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    at_field_start = 0;
  }
}

static int vectis_dsv_record_is_comment(const vectis_dsv_parser *parser) {
  const char *value;
  const char *prefix;
  size_t prefix_size;

  if (parser == NULL ||
      parser->config.comment_prefix == NULL ||
      parser->config.comment_prefix[0] == '\0' ||
      parser->fields.count == 0u ||
      parser->first_field_quoted) {
    return 0;
  }
  value = parser->fields.items[0];
  if (value == NULL) {
    return 0;
  }
  if (parser->config.allow_indented_comments) {
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

static int vectis_dsv_field_uses_json_string(const lonejson_field *field) {
  if (field == NULL) {
    return 1;
  }
  return field->kind == LONEJSON_FIELD_KIND_STRING ||
         field->kind == LONEJSON_FIELD_KIND_STRING_STREAM ||
         field->kind == LONEJSON_FIELD_KIND_BASE64_STREAM ||
         field->kind == LONEJSON_FIELD_KIND_STRING_SOURCE ||
         field->kind == LONEJSON_FIELD_KIND_BASE64_SOURCE;
}

static vectis_status vectis_xml_write_all(int fd,
                                          const void *data,
                                          size_t size,
                                          vectis_error *error);
static vectis_status vectis_xml_write_cstr(int fd, const char *value, vectis_error *error);
static vectis_status vectis_xml_write_json_string_chunk(int fd,
                                                        const char *data,
                                                        size_t size,
                                                        vectis_error *error);
static vectis_status vectis_xml_write_json_key(int fd,
                                               const char *key,
                                               vectis_error *error);
static lonejson_read_result vectis_xml_pipe_read(void *user,
                                                 unsigned char *buffer,
                                                 size_t capacity);

static vectis_status vectis_dsv_append_row_json(vectis_string_builder *builder,
                                                const lonejson_map *map,
                                                const char *const *columns,
                                                size_t column_count,
                                                const vectis_dsv_fields *fields,
                                                int all_strings,
                                                vectis_error *error) {
  size_t i;
  const char *value;
  size_t value_size;
  const lonejson_field *field;
  int first;

  if (vectis_string_builder_append(builder, "{", error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  first = 1;
  for (i = 0u; i < column_count; ++i) {
    value = i < fields->count ? fields->items[i] : "";
    value_size = i < fields->count ? fields->sizes[i] : 0u;
    field = all_strings ? NULL : vectis_dsv_find_field(map, columns[i]);
    if (!all_strings && field == NULL) {
      continue;
    }
    if (!all_strings && value_size == 0u && !vectis_dsv_field_uses_json_string(field)) {
      continue;
    }
    if (!first && vectis_string_builder_append(builder, ",", error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    first = 0;
    if (vectis_append_lonejson_string(builder, columns[i], error) != VECTIS_OK ||
        vectis_string_builder_append(builder, ":", error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_INVALID;
    }
    if (all_strings || vectis_dsv_field_uses_json_string(field)) {
      if (vectis_append_lonejson_string(builder, value, error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_INVALID;
      }
    } else if (vectis_string_builder_append_n(builder, value, value_size, error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
  }
  return vectis_string_builder_append(builder, "}", error);
}

static vectis_status vectis_dsv_effective_config(const vectis_dsv_config *config,
                                                 vectis_dsv_config *out,
                                                 vectis_error *error) {
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
  return VECTIS_OK;
}

static vectis_status vectis_dsv_open_source(const vectis_source *source,
                                            lc_source **out,
                                            int *owned,
                                            vectis_error *error) {
  lc_error lcerr;
  int rc;

  if (out == NULL || owned == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV source output is required");
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
    rc = lc_source_from_memory(source->memory, source->memory_size, out, &lcerr);
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV column output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  *out_count = 0u;
  if (map == NULL || map->field_count == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV lonejson map has no fields");
    return VECTIS_ERR_INVALID;
  }
  columns = (const char **)calloc(map->field_count, sizeof(columns[0]));
  if (columns == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate DSV map columns");
    return VECTIS_ERR_NOMEM;
  }
  for (i = 0u; i < map->field_count; ++i) {
    columns[i] = map->fields[i].json_key;
  }
  *out = columns;
  *out_count = map->field_count;
  return VECTIS_OK;
}

static vectis_status vectis_dsv_write_row_json(int fd,
                                               const lonejson_map *map,
                                               const char *const *columns,
                                               size_t column_count,
                                               const vectis_dsv_fields *fields,
                                               vectis_error *error) {
  const lonejson_field *field;
  const char *value;
  size_t value_size;
  size_t i;
  int first;

  if (vectis_xml_write_cstr(fd, "{", error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  first = 1;
  for (i = 0u; i < column_count; ++i) {
    value = i < fields->count ? fields->items[i] : "";
    value_size = i < fields->count ? fields->sizes[i] : 0u;
    field = vectis_dsv_find_field(map, columns[i]);
    if (field == NULL) {
      continue;
    }
    if (value_size == 0u && !vectis_dsv_field_uses_json_string(field)) {
      continue;
    }
    if (!first && vectis_xml_write_cstr(fd, ",", error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
    first = 0;
    if (vectis_xml_write_json_key(fd, columns[i], error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
    if (vectis_dsv_field_uses_json_string(field)) {
      if (vectis_xml_write_cstr(fd, "\"", error) != VECTIS_OK ||
          vectis_xml_write_json_string_chunk(fd, value, value_size, error) != VECTIS_OK ||
          vectis_xml_write_cstr(fd, "\"", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_STATE;
      }
    } else if (vectis_xml_write_all(fd, value, value_size, error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
  }
  return vectis_xml_write_cstr(fd, "}", error);
}

static void *vectis_dsv_row_pipe_writer_main(void *userdata) {
  vectis_dsv_row_pipe_writer *writer;
  sigset_t set;

  writer = (vectis_dsv_row_pipe_writer *)userdata;
  if (writer == NULL) {
    return NULL;
  }
  (void)sigemptyset(&set);
  (void)sigaddset(&set, SIGPIPE);
  (void)pthread_sigmask(SIG_BLOCK, &set, NULL);
  writer->status = vectis_dsv_write_row_json(writer->fd,
                                             writer->map,
                                             writer->columns,
                                             writer->column_count,
                                             writer->fields,
                                             &writer->error);
  (void)close(writer->fd);
  writer->fd = -1;
  if (writer->status == VECTIS_OK) {
    vectis_error_clear(&writer->error);
  }
  return NULL;
}

static vectis_status vectis_dsv_parse_row_lonejson(const lonejson_map *map,
                                                   const char *const *columns,
                                                   size_t column_count,
                                                   const vectis_dsv_fields *fields,
                                                   void *value,
                                                   lonejson_error *json_error,
                                                   lonejson_status *json_status,
                                                   vectis_error *error) {
  vectis_dsv_row_pipe_writer writer;
  vectis_xml_pipe_reader reader;
  pthread_t thread;
  int pipefd[2];

  writer.map = map;
  writer.columns = columns;
  writer.column_count = column_count;
  writer.fields = fields;
  writer.fd = -1;
  writer.status = VECTIS_OK;
  vectis_error_clear(&writer.error);
  if (pipe(pipefd) != 0) {
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to create DSV row streaming pipe");
    return VECTIS_ERR_STATE;
  }
  writer.fd = pipefd[1];
  reader.fd = pipefd[0];
  if (pthread_create(&thread, NULL, vectis_dsv_row_pipe_writer_main, &writer) != 0) {
    (void)close(pipefd[0]);
    (void)close(pipefd[1]);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to start DSV row streaming writer");
    return VECTIS_ERR_STATE;
  }
  *json_status = lonejson_parse_reader(map, value, vectis_xml_pipe_read, &reader, NULL, json_error);
  (void)close(reader.fd);
  (void)pthread_join(thread, NULL);
  if (writer.status != VECTIS_OK) {
    if (error != NULL) {
      *error = writer.error;
    }
    return writer.status;
  }
  return VECTIS_OK;
}

vectis_status vectis_dsv_parse_lonejson(struct lc_source *source,
                                        const lonejson_map *map,
                                        const vectis_dsv_config *config,
                                        vectis_dsv_lonejson_row_fn row,
                                        void *userdata,
                                        vectis_error *error) {
  vectis_dsv_parser parser;
  vectis_dsv_config effective;
  vectis_dsv_fields headers;
  const char *const *columns;
  const char **header_columns;
  size_t column_count;
  size_t data_row;
  int has_record;
  void *value;
  lonejson_error json_error;
  lonejson_status json_status;
  vectis_status status;

  if (source == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV source is required");
    return VECTIS_ERR_INVALID;
  }
  if (map == NULL || row == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV map and row callback are required");
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
  if (effective.has_header) {
    status = vectis_dsv_read_data_record(&parser, &has_record, error);
    if (status != VECTIS_OK) {
      vectis_dsv_fields_cleanup(&parser.fields);
      vectis_string_builder_cleanup(&parser.field);
      return status;
    }
    if (!has_record) {
      vectis_dsv_fields_cleanup(&parser.fields);
      vectis_string_builder_cleanup(&parser.field);
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
        vectis_set_error(error, VECTIS_ERR_INVALID, "DSV header row is empty");
        return VECTIS_ERR_INVALID;
      }
      header_columns = (const char **)calloc(headers.count, sizeof(header_columns[0]));
      if (header_columns == NULL) {
        vectis_dsv_fields_cleanup(&headers);
        vectis_dsv_fields_cleanup(&parser.fields);
        vectis_string_builder_cleanup(&parser.field);
        vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate DSV header columns");
        return VECTIS_ERR_NOMEM;
      }
      for (column_count = 0u; column_count < headers.count; ++column_count) {
        header_columns[column_count] = headers.items[column_count];
      }
      columns = header_columns;
      column_count = headers.count;
    }
  } else if (columns == NULL) {
    status = vectis_dsv_columns_from_map(map, &header_columns, &column_count, error);
    if (status != VECTIS_OK) {
      vectis_dsv_fields_cleanup(&headers);
      vectis_dsv_fields_cleanup(&parser.fields);
      vectis_string_builder_cleanup(&parser.field);
      return status;
    }
    columns = header_columns;
  }
  if (columns == NULL || column_count == 0u) {
    vectis_dsv_fields_cleanup(&headers);
    vectis_dsv_fields_cleanup(&parser.fields);
    vectis_string_builder_cleanup(&parser.field);
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV columns are required when no header row is used");
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
    if (effective.strict_row_width && parser.fields.count != column_count) {
      vectis_set_error(error, VECTIS_ERR_INVALID, "DSV row width does not match columns");
      status = VECTIS_ERR_INVALID;
      break;
    }
    value = calloc(1u, map->struct_size);
    if (value == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate DSV row struct");
      status = VECTIS_ERR_NOMEM;
      break;
    }
    lonejson_init(map, value);
    status = vectis_dsv_parse_row_lonejson(map,
                                           columns,
                                           column_count,
                                           &parser.fields,
                                           value,
                                           &json_error,
                                           &json_status,
                                           error);
    if (status != VECTIS_OK) {
      lonejson_cleanup(map, value);
      free(value);
      break;
    }
    if (json_status != LONEJSON_STATUS_OK) {
      vectis_set_errorf(error,
                        VECTIS_ERR_INVALID,
                        "failed to parse DSV row %lu through lonejson: %s",
                        (unsigned long)(data_row + 1u),
                        json_error.message);
      if (error != NULL) {
        error->source = VECTIS_ERROR_SOURCE_LONEJSON;
        error->dependency_code = (long)json_status;
      }
      lonejson_cleanup(map, value);
      free(value);
      status = VECTIS_ERR_INVALID;
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

static vectis_status vectis_dsv_to_json_array_impl(struct lc_source *source,
                                                   const lonejson_map *map,
                                                   const vectis_dsv_config *config,
                                                   vectis_mutable_bytes *out,
                                                   int all_strings,
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
  if (effective.has_header) {
    status = vectis_dsv_read_data_record(&parser, &has_record, error);
    if (status != VECTIS_OK) {
      goto cleanup;
    }
    if (has_record) {
      headers = parser.fields;
      memset(&parser.fields, 0, sizeof(parser.fields));
      if (columns == NULL) {
        if (headers.count == 0u) {
          vectis_set_error(error, VECTIS_ERR_INVALID, "DSV header row is empty");
          status = VECTIS_ERR_INVALID;
          goto cleanup;
        }
        header_columns = (const char **)calloc(headers.count, sizeof(header_columns[0]));
        if (header_columns == NULL) {
          vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate DSV header columns");
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
    status = vectis_dsv_columns_from_map(map, &header_columns, &column_count, error);
    if (status != VECTIS_OK) {
      goto cleanup;
    }
    columns = header_columns;
  }
  if (columns == NULL || column_count == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "DSV columns are required when no header row is used");
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
    if (effective.strict_row_width && parser.fields.count != column_count) {
      vectis_set_error(error, VECTIS_ERR_INVALID, "DSV row width does not match columns");
      status = VECTIS_ERR_INVALID;
      break;
    }
    if (!first && vectis_string_builder_append(&json, ",", error) != VECTIS_OK) {
      status = error != NULL ? error->code : VECTIS_ERR_NOMEM;
      break;
    }
    first = 0;
    status = vectis_dsv_append_row_json(&json,
                                        map,
                                        columns,
                                        column_count,
                                        &parser.fields,
                                        all_strings,
                                        error);
  }
  if (status == VECTIS_OK && vectis_string_builder_append(&json, "]", error) == VECTIS_OK) {
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

vectis_status vectis_dsv_source_to_lonejson_array(const vectis_source *source,
                                                 const lonejson_map *map,
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
  status = vectis_dsv_to_lonejson_array(reader, map, config, out, error);
  if (owned) {
    lc_source_close(reader);
  }
  return status;
}

void vectis_xml_config_init(vectis_xml_config *config) {
  if (config == NULL) {
    return;
  }
  config->root_element = NULL;
  config->text_key = "text";
  config->attribute_prefix = "";
  config->trim_text = 1;
  config->skip_unknown = 1;
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
  nread = reader->source->read(reader->source, buffer, (size_t)len, &reader->error);
  if (nread == 0u && reader->error.code != 0) {
    reader->has_error = 1;
    reader->status = VECTIS_ERR_STATE;
    return -1;
  }
  return (int)nread;
}

static vectis_status vectis_xml_effective_config(const vectis_xml_config *config,
                                                 vectis_xml_config *out,
                                                 vectis_error *error) {
  vectis_xml_config defaults;

  vectis_xml_config_init(&defaults);
  *out = config != NULL ? *config : defaults;
  if (out->max_depth == 0u) {
    out->max_depth = defaults.max_depth;
  }
  if (out->text_key == NULL) {
    out->text_key = defaults.text_key;
  }
  if (out->attribute_prefix == NULL) {
    out->attribute_prefix = defaults.attribute_prefix;
  }
  if (out->root_element != NULL && out->root_element[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML root_element must not be empty");
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

static const lonejson_field *vectis_xml_find_attribute_field(const lonejson_map *map,
                                                            const vectis_xml_config *config,
                                                            const char *name,
                                                            vectis_string_builder *key,
                                                            vectis_error *error) {
  const lonejson_field *field;

  field = vectis_xml_find_field(map, name);
  if (field != NULL || config->attribute_prefix[0] == '\0') {
    return field;
  }
  key->size = 0u;
  if (key->data != NULL) {
    key->data[0] = '\0';
  }
  if (vectis_string_builder_append(key, config->attribute_prefix, error) != VECTIS_OK ||
      vectis_string_builder_append(key, name, error) != VECTIS_OK) {
    return NULL;
  }
  return vectis_xml_find_field(map, key->data);
}

static int vectis_xml_field_is_array(const lonejson_field *field) {
  return field != NULL &&
         (field->kind == LONEJSON_FIELD_KIND_STRING_ARRAY ||
          field->kind == LONEJSON_FIELD_KIND_I64_ARRAY ||
          field->kind == LONEJSON_FIELD_KIND_U64_ARRAY ||
          field->kind == LONEJSON_FIELD_KIND_F64_ARRAY ||
          field->kind == LONEJSON_FIELD_KIND_BOOL_ARRAY ||
          field->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY);
}

static int vectis_xml_field_is_object(const lonejson_field *field) {
  return field != NULL &&
         (field->kind == LONEJSON_FIELD_KIND_OBJECT ||
          field->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY);
}

static int vectis_xml_field_uses_string_value(const lonejson_field *field) {
  if (field == NULL) {
    return 1;
  }
  return field->kind == LONEJSON_FIELD_KIND_STRING ||
         field->kind == LONEJSON_FIELD_KIND_STRING_STREAM ||
         field->kind == LONEJSON_FIELD_KIND_BASE64_STREAM ||
         field->kind == LONEJSON_FIELD_KIND_STRING_SOURCE ||
         field->kind == LONEJSON_FIELD_KIND_BASE64_SOURCE ||
         field->kind == LONEJSON_FIELD_KIND_JSON_VALUE ||
         field->kind == LONEJSON_FIELD_KIND_STRING_ARRAY;
}

static int vectis_xml_field_is_spooled_value(const lonejson_field *field) {
  return field != NULL &&
         (field->kind == LONEJSON_FIELD_KIND_STRING_STREAM ||
          field->kind == LONEJSON_FIELD_KIND_BASE64_STREAM);
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

static vectis_status vectis_xml_write_all(int fd,
                                          const void *data,
                                          size_t size,
                                          vectis_error *error) {
  const unsigned char *cursor;
  ssize_t nwritten;

  cursor = (const unsigned char *)data;
  while (size > 0u) {
    nwritten = write(fd, cursor, size);
    if (nwritten < 0) {
      vectis_set_error(error, VECTIS_ERR_STATE, "failed to stream XML into lonejson");
      return VECTIS_ERR_STATE;
    }
    if (nwritten == 0) {
      vectis_set_error(error, VECTIS_ERR_STATE, "short write while streaming XML into lonejson");
      return VECTIS_ERR_STATE;
    }
    cursor += (size_t)nwritten;
    size -= (size_t)nwritten;
  }
  return VECTIS_OK;
}

static vectis_status vectis_xml_write_cstr(int fd, const char *value, vectis_error *error) {
  return vectis_xml_write_all(fd, value, strlen(value), error);
}

static vectis_status vectis_xml_write_json_string_chunk(int fd,
                                                        const char *data,
                                                        size_t size,
                                                        vectis_error *error) {
  char escaped[7];
  unsigned char ch;
  size_t i;

  for (i = 0u; i < size; ++i) {
    ch = (unsigned char)data[i];
    switch (ch) {
    case '"':
      if (vectis_xml_write_cstr(fd, "\\\"", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_STATE;
      }
      break;
    case '\\':
      if (vectis_xml_write_cstr(fd, "\\\\", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_STATE;
      }
      break;
    case '\b':
      if (vectis_xml_write_cstr(fd, "\\b", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_STATE;
      }
      break;
    case '\f':
      if (vectis_xml_write_cstr(fd, "\\f", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_STATE;
      }
      break;
    case '\n':
      if (vectis_xml_write_cstr(fd, "\\n", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_STATE;
      }
      break;
    case '\r':
      if (vectis_xml_write_cstr(fd, "\\r", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_STATE;
      }
      break;
    case '\t':
      if (vectis_xml_write_cstr(fd, "\\t", error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_STATE;
      }
      break;
    default:
      if (ch < 0x20u) {
        (void)snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)ch);
        if (vectis_xml_write_cstr(fd, escaped, error) != VECTIS_OK) {
          return error != NULL ? error->code : VECTIS_ERR_STATE;
        }
      } else if (vectis_xml_write_all(fd, &data[i], 1u, error) != VECTIS_OK) {
        return error != NULL ? error->code : VECTIS_ERR_STATE;
      }
      break;
    }
  }
  return VECTIS_OK;
}

static vectis_status vectis_xml_write_json_key(int fd,
                                               const char *key,
                                               vectis_error *error) {
  if (vectis_xml_write_cstr(fd, "\"", error) != VECTIS_OK ||
      vectis_xml_write_json_string_chunk(fd, key, strlen(key), error) != VECTIS_OK ||
      vectis_xml_write_cstr(fd, "\":", error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  return VECTIS_OK;
}

static vectis_status vectis_xml_write_trimmed_json_string(int fd,
                                                          const char *data,
                                                          size_t size,
                                                          const vectis_xml_config *config,
                                                          vectis_error *error) {
  const char *text;
  size_t text_size;

  text = data;
  text_size = size;
  if (config->trim_text) {
    vectis_xml_trim_span(&text, &text_size);
  }
  if (vectis_xml_write_cstr(fd, "\"", error) != VECTIS_OK ||
      vectis_xml_write_json_string_chunk(fd, text, text_size, error) != VECTIS_OK ||
      vectis_xml_write_cstr(fd, "\"", error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  return VECTIS_OK;
}

static vectis_status vectis_xml_write_scalar_text(int fd,
                                                  const lonejson_field *field,
                                                  const char *data,
                                                  size_t size,
                                                  const vectis_xml_config *config,
                                                  vectis_error *error) {
  const char *text;
  size_t text_size;

  text = data;
  text_size = size;
  if (config->trim_text) {
    vectis_xml_trim_span(&text, &text_size);
  }
  if (vectis_xml_field_uses_string_value(field)) {
    return vectis_xml_write_trimmed_json_string(fd, data, size, config, error);
  }
  if (field != NULL &&
      (field->kind == LONEJSON_FIELD_KIND_BOOL ||
       field->kind == LONEJSON_FIELD_KIND_BOOL_ARRAY)) {
    if ((text_size == 4u && strncasecmp(text, "true", 4u) == 0) ||
        (text_size == 1u && text[0] == '1')) {
      return vectis_xml_write_cstr(fd, "true", error);
    }
    if ((text_size == 5u && strncasecmp(text, "false", 5u) == 0) ||
        (text_size == 1u && text[0] == '0')) {
      return vectis_xml_write_cstr(fd, "false", error);
    }
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML boolean value must be true, false, 1, or 0");
    return VECTIS_ERR_INVALID;
  }
  if (text_size == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML numeric value must not be empty");
    return VECTIS_ERR_INVALID;
  }
  return vectis_xml_write_all(fd, text, text_size, error);
}

static vectis_status vectis_xml_skip_element(xmlTextReaderPtr reader, vectis_error *error) {
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
        vectis_set_error(error, VECTIS_ERR_INVALID, "unexpected end of XML while skipping element");
      } else {
        vectis_set_error(error, VECTIS_ERR_INVALID, "libxml2 failed while skipping XML element");
      }
      return VECTIS_ERR_INVALID;
    }
    type = xmlTextReaderNodeType(reader);
    if (type == XML_READER_TYPE_END_ELEMENT && xmlTextReaderDepth(reader) == start_depth) {
      return VECTIS_OK;
    }
  }
}

static vectis_status vectis_xml_stream_object(xmlTextReaderPtr reader,
                                              const lonejson_map *map,
                                              const vectis_xml_config *config,
                                              size_t depth,
                                              int fd,
                                              vectis_error *error);

static vectis_status vectis_xml_finish_array(vectis_xml_field_state *states,
                                             size_t *open_index,
                                             int fd,
                                             vectis_error *error) {
  if (*open_index == SIZE_MAX) {
    return VECTIS_OK;
  }
  if (vectis_xml_write_cstr(fd, "]", error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  states[*open_index].array_open = 0;
  states[*open_index].array_closed = 1;
  *open_index = SIZE_MAX;
  return VECTIS_OK;
}

static vectis_status vectis_xml_begin_field(vectis_xml_field_state *states,
                                            size_t *open_index,
                                            size_t index,
                                            const lonejson_field *field,
                                            int *first,
                                            int fd,
                                            vectis_error *error) {
  vectis_status status;

  if (!states[index].array && states[index].count > 0u) {
    vectis_set_errorf(error,
                      VECTIS_ERR_INVALID,
                      "duplicate XML value for field '%s'",
                      field->json_key);
    return VECTIS_ERR_INVALID;
  }
  if (states[index].array && states[index].array_closed) {
    vectis_set_errorf(error,
                      VECTIS_ERR_INVALID,
                      "non-contiguous XML array field '%s'",
                      field->json_key);
    return VECTIS_ERR_INVALID;
  }
  if (states[index].array && *open_index != SIZE_MAX && *open_index == index) {
    status = vectis_xml_write_cstr(fd, ",", error);
    states[index].count++;
    return status;
  }
  status = vectis_xml_finish_array(states, open_index, fd, error);
  if (status != VECTIS_OK) {
    return status;
  }
  if (!*first && vectis_xml_write_cstr(fd, ",", error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  *first = 0;
  if (vectis_xml_write_json_key(fd, field->json_key, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  if (states[index].array) {
    if (vectis_xml_write_cstr(fd, "[", error) != VECTIS_OK) {
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
    states[index].array_open = 1;
    *open_index = index;
  }
  states[index].count++;
  return VECTIS_OK;
}

static vectis_status vectis_xml_stream_scalar_content(xmlTextReaderPtr reader,
                                                      const lonejson_field *field,
                                                      const vectis_xml_config *config,
                                                      int fd,
                                                      vectis_error *error) {
  vectis_string_builder text;
  const xmlChar *value;
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
  if (vectis_xml_field_is_spooled_value(field) && config->trim_text) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "XML spooled lonejson fields require trim_text=0 for true streaming");
    return VECTIS_ERR_INVALID;
  }
  if (xmlTextReaderIsEmptyElement(reader)) {
    status = vectis_xml_write_scalar_text(fd, field, "", 0u, config, error);
    vectis_string_builder_cleanup(&text);
    return status;
  }
  if (vectis_xml_field_is_spooled_value(field)) {
    if (vectis_xml_write_cstr(fd, "\"", error) != VECTIS_OK) {
      vectis_string_builder_cleanup(&text);
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
  }
  start_depth = xmlTextReaderDepth(reader);
  for (;;) {
    rc = xmlTextReaderRead(reader);
    if (rc != 1) {
      vectis_set_error(error,
                       VECTIS_ERR_INVALID,
                       rc == 0 ? "unexpected end of XML while reading scalar"
                               : "libxml2 failed while reading XML scalar");
      status = VECTIS_ERR_INVALID;
      break;
    }
    type = xmlTextReaderNodeType(reader);
    if (type == XML_READER_TYPE_END_ELEMENT && xmlTextReaderDepth(reader) == start_depth) {
      break;
    }
    if (type == XML_READER_TYPE_ELEMENT) {
      vectis_set_error(error, VECTIS_ERR_INVALID, "XML scalar field contains nested elements");
      status = VECTIS_ERR_INVALID;
      break;
    }
    if (type == XML_READER_TYPE_TEXT ||
        type == XML_READER_TYPE_CDATA ||
        type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE ||
        type == XML_READER_TYPE_WHITESPACE) {
      value = xmlTextReaderConstValue(reader);
      if (value != NULL) {
        chunk = (const char *)value;
        chunk_size = strlen(chunk);
        if (config->max_text_bytes > 0u && text.size + chunk_size > config->max_text_bytes) {
          vectis_set_error(error, VECTIS_ERR_INVALID, "XML text exceeds max_text_bytes");
          status = VECTIS_ERR_INVALID;
          break;
        }
        if (vectis_xml_field_is_spooled_value(field)) {
          text.size += chunk_size;
          if (vectis_xml_write_json_string_chunk(fd, chunk, chunk_size, error) != VECTIS_OK) {
            status = error != NULL ? error->code : VECTIS_ERR_STATE;
            break;
          }
        } else {
          if (vectis_string_builder_append_n(&text, chunk, chunk_size, error) != VECTIS_OK) {
            status = error != NULL ? error->code : VECTIS_ERR_NOMEM;
            break;
          }
        }
      }
    }
  }
  if (vectis_xml_field_is_spooled_value(field)) {
    if (status == VECTIS_OK) {
      status = vectis_xml_write_cstr(fd, "\"", error);
    }
  } else if (status == VECTIS_OK) {
    status = vectis_xml_write_scalar_text(fd,
                                          field,
                                          text.data != NULL ? text.data : "",
                                          text.size,
                                          config,
                                          error);
  }
  vectis_string_builder_cleanup(&text);
  return status;
}

static vectis_status vectis_xml_stream_field_value(xmlTextReaderPtr reader,
                                                   const lonejson_field *field,
                                                   const vectis_xml_config *config,
                                                   size_t depth,
                                                   int fd,
                                                   vectis_error *error) {
  if (vectis_xml_field_is_object(field)) {
    return vectis_xml_stream_object(reader, field->submap, config, depth, fd, error);
  }
  return vectis_xml_stream_scalar_content(reader, field, config, fd, error);
}

static vectis_status vectis_xml_stream_text_field(vectis_xml_field_state *states,
                                                  size_t *open_index,
                                                  const lonejson_map *map,
                                                  const vectis_xml_config *config,
                                                  int *first,
                                                  int fd,
                                                  const char *data,
                                                  size_t size,
                                                  vectis_error *error) {
  const lonejson_field *field;
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
    if (config->skip_unknown) {
      return VECTIS_OK;
    }
    vectis_set_errorf(error, VECTIS_ERR_INVALID, "unknown XML text field '%s'", config->text_key);
    return VECTIS_ERR_INVALID;
  }
  index = (size_t)(field - map->fields);
  status = vectis_xml_begin_field(states, open_index, index, field, first, fd, error);
  if (status != VECTIS_OK) {
    return status;
  }
  return vectis_xml_write_scalar_text(fd, field, data, size, config, error);
}

static vectis_status vectis_xml_stream_object(xmlTextReaderPtr reader,
                                              const lonejson_map *map,
                                              const vectis_xml_config *config,
                                              size_t depth,
                                              int fd,
                                              vectis_error *error) {
  vectis_xml_field_state *states;
  vectis_string_builder attr_key;
  const lonejson_field *field;
  const char *name;
  const xmlChar *value;
  size_t i;
  size_t index;
  size_t open_index;
  int first;
  int empty;
  int start_depth;
  int type;
  int rc;
  vectis_status status;

  if (depth > config->max_depth) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML nesting exceeds max_depth");
    return VECTIS_ERR_INVALID;
  }
  if (map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  states = (vectis_xml_field_state *)calloc(map->field_count, sizeof(states[0]));
  if (states == NULL && map->field_count > 0u) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate XML field state");
    return VECTIS_ERR_NOMEM;
  }
  for (i = 0u; i < map->field_count; ++i) {
    states[i].array = vectis_xml_field_is_array(&map->fields[i]);
  }
  attr_key.data = NULL;
  attr_key.size = 0u;
  attr_key.capacity = 0u;
  open_index = SIZE_MAX;
  first = 1;
  empty = xmlTextReaderIsEmptyElement(reader);
  start_depth = xmlTextReaderDepth(reader);
  status = vectis_xml_write_cstr(fd, "{", error);
  if (status != VECTIS_OK) {
    goto cleanup;
  }
  if (xmlTextReaderMoveToFirstAttribute(reader) == 1) {
    do {
      name = vectis_xml_reader_local_name(reader);
      if (name == NULL) {
        continue;
      }
      field = vectis_xml_find_attribute_field(map, config, name, &attr_key, error);
      if (field == NULL) {
        if (error != NULL && error->code != VECTIS_OK) {
          status = error->code;
          goto cleanup;
        }
        if (config->skip_unknown) {
          continue;
        }
        vectis_set_errorf(error, VECTIS_ERR_INVALID, "unknown XML attribute '%s'", name);
        status = VECTIS_ERR_INVALID;
        goto cleanup;
      }
      index = (size_t)(field - map->fields);
      status = vectis_xml_begin_field(states, &open_index, index, field, &first, fd, error);
      if (status != VECTIS_OK) {
        goto cleanup;
      }
      value = xmlTextReaderConstValue(reader);
      status = vectis_xml_write_scalar_text(fd,
                                            field,
                                            value != NULL ? (const char *)value : "",
                                            value != NULL ? strlen((const char *)value) : 0u,
                                            config,
                                            error);
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
        vectis_set_error(error,
                         VECTIS_ERR_INVALID,
                         rc == 0 ? "unexpected end of XML object"
                                 : "libxml2 failed while reading XML object");
        status = VECTIS_ERR_INVALID;
        goto cleanup;
      }
      type = xmlTextReaderNodeType(reader);
      if (type == XML_READER_TYPE_END_ELEMENT && xmlTextReaderDepth(reader) == start_depth) {
        break;
      }
      if (type == XML_READER_TYPE_ELEMENT) {
        name = vectis_xml_reader_local_name(reader);
        field = vectis_xml_find_field(map, name);
        if (field == NULL) {
          if (!config->skip_unknown) {
            vectis_set_errorf(error, VECTIS_ERR_INVALID, "unknown XML element '%s'", name);
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
        status = vectis_xml_begin_field(states, &open_index, index, field, &first, fd, error);
        if (status != VECTIS_OK) {
          goto cleanup;
        }
        status = vectis_xml_stream_field_value(reader, field, config, depth + 1u, fd, error);
        if (status != VECTIS_OK) {
          goto cleanup;
        }
      } else if (type == XML_READER_TYPE_TEXT ||
                 type == XML_READER_TYPE_CDATA ||
                 type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE ||
                 type == XML_READER_TYPE_WHITESPACE) {
        value = xmlTextReaderConstValue(reader);
        if (value != NULL &&
            vectis_xml_span_has_nonspace((const char *)value, strlen((const char *)value))) {
          status = vectis_xml_stream_text_field(states,
                                                &open_index,
                                                map,
                                                config,
                                                &first,
                                                fd,
                                                (const char *)value,
                                                strlen((const char *)value),
                                                error);
          if (status != VECTIS_OK) {
            goto cleanup;
          }
        }
      }
    }
  }
  status = vectis_xml_finish_array(states, &open_index, fd, error);
  if (status == VECTIS_OK) {
    status = vectis_xml_write_cstr(fd, "}", error);
  }

cleanup:
  vectis_string_builder_cleanup(&attr_key);
  free(states);
  return status;
}

static vectis_status vectis_xml_stream_document(xmlTextReaderPtr reader,
                                                const lonejson_map *map,
                                                const vectis_xml_config *config,
                                                int fd,
                                                vectis_error *error) {
  const char *name;
  int rc;

  for (;;) {
    rc = xmlTextReaderRead(reader);
    if (rc != 1) {
      vectis_set_error(error, VECTIS_ERR_INVALID, "XML document has no root element");
      return VECTIS_ERR_INVALID;
    }
    if (xmlTextReaderNodeType(reader) == XML_READER_TYPE_ELEMENT) {
      break;
    }
  }
  name = vectis_xml_reader_local_name(reader);
  if (config->root_element != NULL && !vectis_xml_name_matches(name, config->root_element)) {
    vectis_set_errorf(error,
                      VECTIS_ERR_INVALID,
                      "XML root element '%s' does not match expected '%s'",
                      name != NULL ? name : "",
                      config->root_element);
    return VECTIS_ERR_INVALID;
  }
  return vectis_xml_stream_object(reader, map, config, 1u, fd, error);
}

static vectis_status vectis_xml_open_reader(const vectis_source *source,
                                            xmlTextReaderPtr *out,
                                            vectis_xml_lc_reader *lc_reader,
                                            vectis_error *error) {
  int options;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML reader output is required");
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
      vectis_set_error(error, VECTIS_ERR_INVALID, "XML memory source data is required");
      return VECTIS_ERR_INVALID;
    }
    if (source->memory_size > (size_t)INT_MAX) {
      vectis_set_error(error, VECTIS_ERR_INVALID, "XML memory source is too large for libxml2 reader");
      return VECTIS_ERR_INVALID;
    }
    *out = xmlReaderForMemory((const char *)source->memory,
                              (int)source->memory_size,
                              NULL,
                              NULL,
                              options);
  } else if (source->source != NULL) {
    lc_reader->source = source->source;
    lc_reader->status = VECTIS_OK;
    lc_reader->has_error = 0;
    lc_error_init(&lc_reader->error);
    *out = xmlReaderForIO(vectis_xml_lc_read, NULL, lc_reader, NULL, NULL, options);
  } else {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML source is empty");
    return VECTIS_ERR_INVALID;
  }
  if (*out == NULL) {
    if (lc_reader != NULL && lc_reader->has_error) {
      (void)vectis_source_error(error,
                                lc_reader->error.code,
                                &lc_reader->error,
                                "failed to read XML source");
      return error != NULL ? error->code : VECTIS_ERR_STATE;
    }
    vectis_set_error(error, VECTIS_ERR_INVALID, "failed to open XML reader");
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static void *vectis_xml_pipe_writer_main(void *userdata) {
  vectis_xml_pipe_writer *writer;
  xmlTextReaderPtr reader;
  vectis_xml_lc_reader lc_reader;
  sigset_t set;

  writer = (vectis_xml_pipe_writer *)userdata;
  if (writer == NULL) {
    return NULL;
  }
  (void)sigemptyset(&set);
  (void)sigaddset(&set, SIGPIPE);
  (void)pthread_sigmask(SIG_BLOCK, &set, NULL);
  lc_reader.source = NULL;
  lc_reader.status = VECTIS_OK;
  lc_reader.has_error = 0;
  lc_error_init(&lc_reader.error);
  reader = NULL;
  writer->status = vectis_xml_open_reader(&writer->source, &reader, &lc_reader, &writer->error);
  if (writer->status == VECTIS_OK) {
    writer->status = vectis_xml_stream_document(reader,
                                                writer->map,
                                                &writer->config,
                                                writer->fd,
                                                &writer->error);
  }
  if (reader != NULL) {
    xmlFreeTextReader(reader);
  }
  (void)close(writer->fd);
  writer->fd = -1;
  if (lc_reader.has_error && writer->status == VECTIS_OK) {
    (void)vectis_source_error(&writer->error,
                              lc_reader.error.code,
                              &lc_reader.error,
                              "failed to read XML source");
    writer->status = writer->error.code;
  }
  if (writer->status == VECTIS_OK) {
    vectis_error_clear(&writer->error);
  }
  lc_error_cleanup(&lc_reader.error);
  return NULL;
}

static lonejson_read_result vectis_xml_pipe_read(void *user,
                                                 unsigned char *buffer,
                                                 size_t capacity) {
  vectis_xml_pipe_reader *reader;
  lonejson_read_result result;
  ssize_t nread;

  result = lonejson_default_read_result();
  reader = (vectis_xml_pipe_reader *)user;
  if (reader == NULL || reader->fd < 0 || buffer == NULL) {
    result.error_code = EINVAL;
    return result;
  }
  if (capacity == 0u) {
    return result;
  }
  nread = read(reader->fd, buffer, capacity);
  if (nread < 0) {
    result.error_code = errno != 0 ? errno : EIO;
    return result;
  }
  if (nread == 0) {
    result.eof = 1;
    return result;
  }
  result.bytes_read = (size_t)nread;
  return result;
}

vectis_status vectis_xml_parse_lonejson_source(const vectis_source *source,
                                               const lonejson_map *map,
                                               const vectis_xml_config *config,
                                               void *out,
                                               vectis_error *error) {
  vectis_xml_pipe_writer writer;
  vectis_xml_pipe_reader pipe_reader;
  lonejson_error json_error;
  lonejson_status json_status;
  pthread_t thread;
  int pipefd[2];
  int thread_started;
  vectis_status status;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML lonejson output struct is required");
    return VECTIS_ERR_INVALID;
  }
  if (map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "XML lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_xml_effective_config(config, &writer.config, error);
  if (status != VECTIS_OK) {
    return status;
  }
  writer.source = source != NULL ? *source : vectis_source_from_lc(NULL);
  writer.map = map;
  writer.fd = -1;
  writer.status = VECTIS_OK;
  vectis_error_clear(&writer.error);
  if (pipe(pipefd) != 0) {
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to create XML streaming pipe");
    return VECTIS_ERR_STATE;
  }
  writer.fd = pipefd[1];
  pipe_reader.fd = pipefd[0];
  thread_started = 0;
  if (pthread_create(&thread, NULL, vectis_xml_pipe_writer_main, &writer) != 0) {
    (void)close(pipefd[0]);
    (void)close(pipefd[1]);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to start XML streaming writer");
    return VECTIS_ERR_STATE;
  }
  thread_started = 1;
  lonejson_init(map, out);
  json_status = lonejson_parse_reader(map, out, vectis_xml_pipe_read, &pipe_reader, NULL, &json_error);
  (void)close(pipe_reader.fd);
  pipe_reader.fd = -1;
  if (thread_started) {
    (void)pthread_join(thread, NULL);
  }
  if (writer.status != VECTIS_OK) {
    if (error != NULL) {
      *error = writer.error;
    }
    lonejson_cleanup(map, out);
    return writer.status;
  }
  if (json_status != LONEJSON_STATUS_OK) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID, "failed to parse XML through lonejson: %s", json_error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
      error->dependency_code = (long)json_status;
    }
    lonejson_cleanup(map, out);
    return VECTIS_ERR_INVALID;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_xml_parse_lonejson(struct lc_source *source,
                                        const lonejson_map *map,
                                        const vectis_xml_config *config,
                                        void *out,
                                        vectis_error *error) {
  vectis_source wrapped_source;

  wrapped_source = vectis_source_from_lc(source);
  return vectis_xml_parse_lonejson_source(&wrapped_source, map, config, out, error);
}

vectis_status vectis_format_key(char *out,
                                size_t out_size,
                                vectis_error *error,
                                const char *format,
                                ...) {
  va_list ap;
  int written;

  if (out == NULL || out_size == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "key output buffer is required");
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "formatted key exceeds output buffer");
    return VECTIS_ERR_INVALID;
  }
  if (strstr(out, "/../") != NULL ||
      strstr(out, "/./") != NULL ||
      strcmp(out, "..") == 0 ||
      strcmp(out, ".") == 0 ||
      strncmp(out, "../", 3u) == 0 ||
      strncmp(out, "./", 2u) == 0 ||
      (strlen(out) >= 3u && strcmp(out + strlen(out) - 3u, "/..") == 0) ||
      (strlen(out) >= 2u && strcmp(out + strlen(out) - 2u, "/.") == 0)) {
    out[0] = '\0';
    vectis_set_error(error, VECTIS_ERR_INVALID, "formatted key must not contain dot segments");
    return VECTIS_ERR_INVALID;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status vectis_lockd_acquire_state(struct lc_client *client,
                                                const char *key,
                                                const char *owner,
                                                long ttl_seconds,
                                                struct lc_lease **out,
                                                vectis_error *error) {
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "lockd state owner is required");
    return VECTIS_ERR_INVALID;
  }
  lc_acquire_req_init(&acquire);
  lc_error_init(&lcerr);
  acquire.key = key;
  acquire.owner = owner;
  acquire.ttl_seconds = ttl_seconds > 0L ? ttl_seconds : 30L;
  rc = lc_acquire(client, &acquire, out, &lcerr);
  if (rc != LC_OK) {
    (void)vectis_set_lockdc_error(error, rc, &lcerr, "failed to acquire lockd state");
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_lockd_state_load(struct lc_client *client,
                                      const char *key,
                                      const char *owner,
                                      long ttl_seconds,
                                      const lonejson_map *map,
                                      void *out,
                                      vectis_error *error) {
  struct lc_lease *lease;
  lc_release_req release;
  lc_get_res get_response;
  lc_error lcerr;
  int rc;
  vectis_status status;

  if (map == NULL || out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lockd state load requires map and output");
    return VECTIS_ERR_INVALID;
  }
  lease = NULL;
  status = vectis_lockd_acquire_state(client, key, owner, ttl_seconds, &lease, error);
  if (status != VECTIS_OK) {
    return status;
  }
  lc_release_req_init(&release);
  memset(&get_response, 0, sizeof(get_response));
  lc_error_init(&lcerr);
  rc = lease->load(lease, map, out, NULL, NULL, &get_response, &lcerr);
  lc_get_res_cleanup(&get_response);
  if (rc == LC_OK) {
    rc = lease->release(lease, &release, &lcerr);
  }
  if (rc != LC_OK) {
    lc_lease_close(lease);
    (void)vectis_set_lockdc_error(error, rc, &lcerr, "failed to load lockd state");
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_lockd_state_save(struct lc_client *client,
                                      const char *key,
                                      const char *owner,
                                      long ttl_seconds,
                                      const lonejson_map *map,
                                      const void *value,
                                      vectis_error *error) {
  struct lc_lease *lease;
  lc_release_req release;
  lc_error lcerr;
  int rc;
  vectis_status status;

  if (map == NULL || value == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lockd state save requires map and value");
    return VECTIS_ERR_INVALID;
  }
  lease = NULL;
  status = vectis_lockd_acquire_state(client, key, owner, ttl_seconds, &lease, error);
  if (status != VECTIS_OK) {
    return status;
  }
  lc_release_req_init(&release);
  lc_error_init(&lcerr);
  rc = lease->save(lease, map, value, NULL, &lcerr);
  if (rc == LC_OK) {
    rc = lease->release(lease, &release, &lcerr);
  }
  if (rc != LC_OK) {
    lc_lease_close(lease);
    (void)vectis_set_lockdc_error(error, rc, &lcerr, "failed to save lockd state");
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_lockd_state_update(struct lc_client *client,
                                        const char *key,
                                        const char *owner,
                                        long ttl_seconds,
                                        const lonejson_map *map,
                                        void *state,
                                        vectis_lockd_state_update_fn update,
                                        void *userdata,
                                        vectis_error *error) {
  struct lc_lease *lease;
  lc_release_req release;
  lc_get_res get_response;
  lc_error lcerr;
  int rc;
  int save;
  vectis_status status;

  if (map == NULL || state == NULL || update == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lockd state update requires map, state, and callback");
    return VECTIS_ERR_INVALID;
  }
  lease = NULL;
  status = vectis_lockd_acquire_state(client, key, owner, ttl_seconds, &lease, error);
  if (status != VECTIS_OK) {
    return status;
  }
  lc_release_req_init(&release);
  memset(&get_response, 0, sizeof(get_response));
  lc_error_init(&lcerr);
  rc = lease->load(lease, map, state, NULL, NULL, &get_response, &lcerr);
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
      rc = lease->save(lease, map, state, NULL, &lcerr);
    }
  }
  if (rc == LC_OK) {
    rc = lease->release(lease, &release, &lcerr);
  }
  if (rc != LC_OK) {
    lc_lease_close(lease);
    (void)vectis_set_lockdc_error(error, rc, &lcerr, "failed to update lockd state");
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
    (void)vectis_source_error(error, rc, &lcerr, "failed to create request body reader");
    lc_error_cleanup(&lcerr);
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  free(request->body_path);
  request->body_path = NULL;
  request->body_spooled = 0;
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "request body path is required");
    return VECTIS_ERR_INVALID;
  }
  path_copy = vectis_strdup(body_path);
  if (path_copy == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy request body path");
    return VECTIS_ERR_NOMEM;
  }
  lc_error_init(&lcerr);
  source = NULL;
  rc = lc_source_from_file(body_path, &source, &lcerr);
  if (rc != LC_OK) {
    free(path_copy);
    (void)vectis_source_error(error, rc, &lcerr, "failed to create request body file reader");
    lc_error_cleanup(&lcerr);
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  free(request->body_path);
  request->body_path = path_copy;
  request->body_spooled = 1;
  request->body.data = NULL;
  request->body.size = body_size;
  vectis_body_policy_init(&request->body_policy);
  vectis_request_close_body_reader(request);
  request->body_reader = source;
  request->owns_body_reader = 1;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_internal_request_set_body_reader(vectis_request *request,
                                                      struct lc_source *source,
                                                      size_t body_size,
                                                      int owned,
                                                      const vectis_body_policy *policy,
                                                      vectis_error *error) {
  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request is required");
    return VECTIS_ERR_INVALID;
  }
  if (source == NULL && body_size > 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request body reader is required");
    return VECTIS_ERR_INVALID;
  }
  free(request->body_path);
  request->body_path = NULL;
  request->body_spooled = 0;
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
  return vectis_kv_add(&request->path_params,
                       &request->path_param_count,
                       &request->path_param_capacity,
                       name,
                       value,
                       "path parameter",
                       error);
}

vectis_status vectis_internal_request_add_query(vectis_request *request,
                                                const char *name,
                                                const char *value,
                                                vectis_error *error) {
  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_kv_add(&request->query,
                       &request->query_count,
                       &request->query_capacity,
                       name,
                       value,
                       "query parameter",
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
  return vectis_kv_add(&request->headers,
                       &request->header_count,
                       &request->header_capacity,
                       name,
                       value,
                       "request header",
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

const char *vectis_internal_response_content_type(const vectis_response *response) {
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

const char *vectis_internal_response_file_path(const vectis_response *response) {
  return response != NULL ? response->file_path : NULL;
}

int vectis_internal_response_file_temporary(const vectis_response *response) {
  return response != NULL && response->file_path_temporary;
}

size_t vectis_internal_response_header_count(const vectis_response *response) {
  return response != NULL ? response->header_count : 0u;
}

const char *vectis_internal_response_header_name(const vectis_response *response,
                                                 size_t index) {
  if (response == NULL || index >= response->header_count) {
    return NULL;
  }
  return response->headers[index].name;
}

const char *vectis_internal_response_header_value(const vectis_response *response,
                                                  size_t index) {
  if (response == NULL || index >= response->header_count) {
    return NULL;
  }
  return response->headers[index].value;
}

vectis_status vectis_request_json_into(vectis_request *request,
                                       const lonejson_map *map,
                                       void *out,
                                       vectis_error *error) {
  lonejson_error json_error;
  lonejson_status json_status;
  lonejson_curl_parse parse;
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "json output struct is required");
    return VECTIS_ERR_INVALID;
  }
  if (request->body_reader == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request body reader is not available");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_request_reset_body_reader(request, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  json_status = lonejson_curl_parse_init(&parse, map, out, NULL);
  if (json_status != LONEJSON_STATUS_OK) {
    vectis_set_errorf(error,
                      VECTIS_ERR_INVALID,
                      "failed to initialize request JSON parser: %s",
                      parse.error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
      error->dependency_code = (long)json_status;
    }
    return VECTIS_ERR_INVALID;
  }
  lc_error_init(&lcerr);
  for (;;) {
    nread = request->body_reader->read(request->body_reader,
                                       chunk,
                                       sizeof(chunk),
                                       &lcerr);
    if (nread == 0u) {
      if (lcerr.code != 0) {
        (void)vectis_source_error(error, lcerr.code, &lcerr, "failed to read request body");
        lonejson_curl_parse_cleanup(&parse);
        lc_error_cleanup(&lcerr);
        return error != NULL ? error->code : VECTIS_ERR_STATE;
      }
      break;
    }
    accepted = lonejson_curl_write_callback((char *)chunk, 1u, nread, &parse);
    if (accepted != nread) {
      json_error = parse.error;
      vectis_set_errorf(error,
                        VECTIS_ERR_INVALID,
                        "failed to parse request JSON at line %lu column %lu: %s",
                        (unsigned long)json_error.line,
                        (unsigned long)json_error.column,
                        json_error.message);
      if (error != NULL) {
        error->source = VECTIS_ERROR_SOURCE_LONEJSON;
        error->dependency_code = (long)LONEJSON_STATUS_INVALID_JSON;
      }
      lonejson_curl_parse_cleanup(&parse);
      lc_error_cleanup(&lcerr);
      return VECTIS_ERR_INVALID;
    }
  }
  lc_error_cleanup(&lcerr);
  json_status = lonejson_curl_parse_finish(&parse);
  if (json_status != LONEJSON_STATUS_OK) {
    json_error = parse.error;
    vectis_set_errorf(error,
                      VECTIS_ERR_INVALID,
                      "failed to parse request JSON at line %lu column %lu: %s",
                      (unsigned long)json_error.line,
                      (unsigned long)json_error.column,
                      json_error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
      error->dependency_code = (long)json_status;
    }
    lonejson_curl_parse_cleanup(&parse);
    return VECTIS_ERR_INVALID;
  }
  lonejson_curl_parse_cleanup(&parse);
  (void)vectis_request_reset_body_reader(request, NULL);
  vectis_error_clear(error);
  return VECTIS_OK;
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

const char *vectis_request_query(vectis_request *request,
                                 const char *name) {
  if (request == NULL) {
    return NULL;
  }
  return vectis_kv_find(request->query, request->query_count, name);
}

const char *vectis_request_header(vectis_request *request,
                                  const char *name) {
  if (request == NULL) {
    return NULL;
  }
  return vectis_kv_find(request->headers, request->header_count, name);
}

struct http_request *vectis_request_kore(vectis_request *request) {
  if (request == NULL) {
    return NULL;
  }
  return request->kore_request;
}

struct lc_source *vectis_request_body_reader(vectis_request *request) {
  if (request == NULL) {
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

void vectis_body_materialize_config_init(vectis_body_materialize_config *config) {
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

vectis_status vectis_body_materialized_open_reader(const vectis_body_materialized *body,
                                                   struct lc_source **out,
                                                   vectis_error *error) {
  lc_error lcerr;
  int rc;

  if (body == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "materialized body is required");
    return VECTIS_ERR_INVALID;
  }
  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "body reader output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  lc_error_init(&lcerr);
  if (body->kind == VECTIS_BODY_MATERIALIZED_MEMORY) {
    rc = lc_source_from_memory(body->memory.data, body->memory.size, out, &lcerr);
  } else if (body->kind == VECTIS_BODY_MATERIALIZED_FILE) {
    rc = lc_source_from_file(body->path, out, &lcerr);
  } else {
    lc_error_cleanup(&lcerr);
    vectis_set_error(error, VECTIS_ERR_INVALID, "materialized body is empty");
    return VECTIS_ERR_INVALID;
  }
  if (rc != LC_OK) {
    (void)vectis_source_error(error, rc, &lcerr, "failed to open materialized body reader");
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

vectis_status vectis_request_body_materialize(vectis_request *request,
                                              const vectis_body_materialize_config *config,
                                              vectis_body_materialized *out,
                                              vectis_error *error) {
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "materialized request body output is required");
    return VECTIS_ERR_INVALID;
  }
  if (request->body_reader == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request body reader is not available");
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
  if (fixed_buffer_size > 0u && (memory_limit == 0u ||
      memory_limit > fixed_buffer_size)) {
    memory_limit = fixed_buffer_size;
  }
  if (memory_limit == 0u) {
    memory_limit = request->body_policy.memory_buffer_limit_bytes;
  }
  if (memory_limit == 0u) {
    memory_limit = VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
  }
  if (!vectis_tmp_template(tmp_template, sizeof(tmp_template), config)) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request body materialization path is too long");
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
    nread = request->body_reader->read(request->body_reader,
                                       chunk,
                                       sizeof(chunk),
                                       &lcerr);
    if (nread == 0u) {
      if (lcerr.code != 0) {
        (void)vectis_source_error(error, lcerr.code, &lcerr, "failed to read request body");
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
        vectis_set_error(error, VECTIS_ERR_STATE, "failed to create request body materialization file");
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
        vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy request body materialization path");
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
        vectis_set_error(error, VECTIS_ERR_STATE, "failed to open request body materialization file");
        lc_error_cleanup(&lcerr);
        return VECTIS_ERR_STATE;
      }
      fd = -1;
      if (memory_size > 0u && fwrite(memory, 1u, memory_size, fp) != memory_size) {
        (void)fclose(fp);
        (void)unlink(path);
        free(path);
        if (memory != fixed_buffer) {
          free(memory);
        }
        vectis_set_error(error, VECTIS_ERR_STATE, "failed to write request body materialization file");
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
        vectis_set_error(error, VECTIS_ERR_STATE, "failed to write request body materialization file");
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
        next = memory == fixed_buffer ? malloc(memory_capacity) : realloc(memory, memory_capacity);
        if (next == NULL) {
          if (memory != fixed_buffer) {
            free(memory);
          }
          vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate request body memory");
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
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to close request body materialization file");
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "request body spill output is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_body_materialize_config_init(&materialize_config);
  if (config != NULL) {
    materialize_config.memory_limit_bytes = config->memory_limit_bytes;
    materialize_config.directory = config->directory;
    materialize_config.prefix = config->prefix;
  }
  status = vectis_request_body_materialize(request, &materialize_config, &body, error);
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "request body output is required");
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
    vectis_set_error(error, VECTIS_ERR_STATE, "request body unexpectedly spooled while reading all");
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "request body output is required");
    return VECTIS_ERR_INVALID;
  }
  if (request->body.data == NULL && request->body.size > 0u) {
    vectis_set_error(error,
                     VECTIS_ERR_STATE,
                     "request body is reader-backed; use vectis_request_body_reader or vectis_request_body_read_all");
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
  if (request == NULL || !request->body_spooled) {
    return NULL;
  }
  return request->body_path;
}

int vectis_request_body_is_spooled(vectis_request *request) {
  return request != NULL && request->body_spooled;
}

vectis_status vectis_response_status(vectis_response *response,
                                     int status_code,
                                     vectis_error *error) {
  if (response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response is required");
    return VECTIS_ERR_INVALID;
  }
  if (status_code < 100 || status_code > 599) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP status code is invalid");
    return VECTIS_ERR_INVALID;
  }
  free(response->content_type);
  response->content_type = NULL;
  free(response->body);
  response->body = NULL;
  response->body_size = 0u;
  free(response->file_path);
  response->file_path = NULL;
  response->status_code = status_code;
  response->sent = 1;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_response_header(vectis_response *response,
                                     const char *name,
                                     const char *value,
                                     vectis_error *error) {
  if (response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response is required");
    return VECTIS_ERR_INVALID;
  }
  if (!vectis_header_name_valid(name)) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response header name is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (!vectis_header_value_valid(value)) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response header value is invalid");
    return VECTIS_ERR_INVALID;
  }
  return vectis_kv_add(&response->headers,
                       &response->header_count,
                       &response->header_capacity,
                       name,
                       value,
                       "response header",
                       error);
}

vectis_status vectis_response_text(vectis_response *response,
                                   int status_code,
                                   const char *content_type,
                                   const char *text,
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "response content_type is required");
    return VECTIS_ERR_INVALID;
  }
  if (text == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response text is required");
    return VECTIS_ERR_INVALID;
  }
  body.data = text;
  body.size = strlen(text);
  return vectis_response_bytes(response, status_code, content_type, body, error);
}

vectis_status vectis_response_bytes(vectis_response *response,
                                    int status_code,
                                    const char *content_type,
                                    vectis_bytes body,
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "response content_type is required");
    return VECTIS_ERR_INVALID;
  }
  if (body.data == NULL && body.size > 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response body is invalid");
    return VECTIS_ERR_INVALID;
  }
  content_type_copy = vectis_strdup(content_type);
  if (content_type_copy == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy response content type");
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
  free(response->content_type);
  free(response->body);
  free(response->file_path);
  response->status_code = status_code;
  response->content_type = content_type_copy;
  response->body = body_copy;
  response->body_size = body.size;
  response->file_path = NULL;
  response->sent = 1;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_response_file(vectis_response *response,
                                   int status_code,
                                   const char *content_type,
                                   const char *path,
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "response content_type is required");
    return VECTIS_ERR_INVALID;
  }
  if (path == NULL || path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response file path is required");
    return VECTIS_ERR_INVALID;
  }
  content_type_copy = vectis_strdup(content_type);
  if (content_type_copy == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy response content type");
    return VECTIS_ERR_NOMEM;
  }
  path_copy = vectis_strdup(path);
  if (path_copy == NULL) {
    free(content_type_copy);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy response file path");
    return VECTIS_ERR_NOMEM;
  }
  free(response->content_type);
  free(response->body);
  free(response->file_path);
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
                                                char *path,
                                                int temporary,
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "response content type is required");
    return VECTIS_ERR_INVALID;
  }
  if (path == NULL || path[0] == '\0') {
    free(path);
    vectis_set_error(error, VECTIS_ERR_INVALID, "response file path is required");
    return VECTIS_ERR_INVALID;
  }
  content_type_copy = vectis_strdup(content_type);
  if (content_type_copy == NULL) {
    if (temporary) {
      (void)unlink(path);
    }
    free(path);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy response content type");
    return VECTIS_ERR_NOMEM;
  }
  free(response->content_type);
  free(response->body);
  if (response->file_path_temporary && response->file_path != NULL) {
    (void)unlink(response->file_path);
  }
  free(response->file_path);
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "temp file outputs are required");
    return VECTIS_ERR_INVALID;
  }
  *out_fp = NULL;
  *out_path = NULL;
  if (!vectis_response_tmp_template(tmp_template, sizeof(tmp_template))) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "response temp path is too long");
    return VECTIS_ERR_INVALID;
  }
  fd = mkstemp(tmp_template);
  if (fd < 0) {
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to create response temp file");
    return VECTIS_ERR_STATE;
  }
  path = vectis_strdup(tmp_template);
  if (path == NULL) {
    (void)close(fd);
    (void)unlink(tmp_template);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy response temp path");
    return VECTIS_ERR_NOMEM;
  }
  fp = fdopen(fd, "wb");
  if (fp == NULL) {
    (void)close(fd);
    (void)unlink(path);
    free(path);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to open response temp file");
    return VECTIS_ERR_STATE;
  }
  *out_fp = fp;
  *out_path = path;
  return VECTIS_OK;
}

vectis_status vectis_response_source(vectis_response *response,
                                     int status_code,
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
      (void)vectis_source_error(error, lcerr.code, &lcerr, "failed to reset response source");
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
        (void)vectis_source_error(error, lcerr.code, &lcerr, "failed to read response source");
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
      vectis_set_error(error, VECTIS_ERR_STATE, "failed to write response temp file");
      return VECTIS_ERR_STATE;
    }
  }
  lc_error_cleanup(&lcerr);
  if (fclose(fp) != 0) {
    (void)unlink(path);
    free(path);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to close response temp file");
    return VECTIS_ERR_STATE;
  }
  return vectis_response_file_owned(response, status_code, content_type, path, 1, error);
}

static lonejson_status vectis_lonejson_file_sink(void *user,
                                                 const void *data,
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

vectis_status vectis_response_json(vectis_response *response,
                                   int status_code,
                                   const lonejson_map *map,
                                   const void *value,
                                   vectis_error *error) {
  lonejson_error json_error;
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "json response value is required");
    return VECTIS_ERR_INVALID;
  }
  json = lonejson_serialize_alloc(map, value, &json_size, NULL, &json_error);
  if (json == NULL) {
    vectis_set_errorf(error,
                      VECTIS_ERR_INVALID,
                      "failed to serialize response JSON: %s",
                      json_error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
    }
    return VECTIS_ERR_INVALID;
  }
  body.data = json;
  body.size = json_size;
  status = vectis_response_bytes(response, status_code, "application/json", body, error);
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
  FILE *fp;
  char *path;

  if (map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  if (value == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json response value is required");
    return VECTIS_ERR_INVALID;
  }
  fp = NULL;
  path = NULL;
  if (vectis_create_response_temp_file(&fp, &path, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  sink.fp = fp;
  json_status = lonejson_serialize_sink(map,
                                        value,
                                        vectis_lonejson_file_sink,
                                        &sink,
                                        NULL,
                                        &json_error);
  if (json_status != LONEJSON_STATUS_OK) {
    (void)fclose(fp);
    (void)unlink(path);
    free(path);
    vectis_set_errorf(error,
                      VECTIS_ERR_INVALID,
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
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to close generated JSON response file");
    return VECTIS_ERR_STATE;
  }
  return vectis_response_file_owned(response, status_code, "application/json", path, 1, error);
}

vectis_status vectis_json_reply(vectis_json_response *response,
                                int status_code,
                                const lonejson_map *map,
                                const void *value,
                                vectis_error *error) {
  vectis_status status;

  if (response == NULL || response->response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json response is required");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_response_json(response->response, status_code, map, value, error);
  if (status == VECTIS_OK) {
    response->sent = 1;
  }
  return status;
}

vectis_status vectis_json_reply_status(vectis_json_response *response,
                                       int status_code,
                                       vectis_error *error) {
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
                                         int status_code,
                                         const char *code,
                                         const char *message,
                                         const char *detail,
                                         vectis_error *error) {
  vectis_error_response_body body;

  memset(&body, 0, sizeof(body));
  if (status_code < 100 || status_code > 599) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP status code is invalid");
    return VECTIS_ERR_INVALID;
  }
  (void)snprintf(body.code,
                 sizeof(body.code),
                 "%s",
                 code != NULL && code[0] != '\0' ? code : "error");
  (void)snprintf(body.message,
                 sizeof(body.message),
                 "%s",
                 message != NULL && message[0] != '\0' ? message : "request failed");
  if (detail != NULL) {
    (void)snprintf(body.detail, sizeof(body.detail), "%s", detail);
  }
  return vectis_response_json(response,
                              status_code,
                              &vectis_error_response_map,
                              &body,
                              error);
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

vectis_status vectis_http_client_new(const vectis_http_client_config *config,
                                     vectis_http_client **out,
                                     vectis_error *error) {
  vectis_http_client_config defaults;
  const vectis_http_client_config *effective;
  vectis_http_client *client;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP client output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  vectis_http_client_config_init(&defaults);
  effective = config != NULL ? config : &defaults;
  if (effective->timeout_ms < 0L ||
      effective->connect_timeout_ms < 0L ||
      effective->low_speed_limit_bytes_per_sec < 0L ||
      effective->low_speed_time_seconds < 0L ||
      effective->retry_initial_delay_ms < 0L ||
      effective->retry_max_delay_ms < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP client timeouts must be non-negative");
    return VECTIS_ERR_INVALID;
  }

  client = (vectis_http_client *)calloc(1u, sizeof(*client));
  if (client == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate HTTP client");
    return VECTIS_ERR_NOMEM;
  }
  client->config = *effective;
  client->logger = effective->logger;
  vectis_error_clear(error);
  *out = client;
  return VECTIS_OK;
}

vectis_status vectis_http_client_from_app(vectis_app *app,
                                          const vectis_http_client_config *config,
                                          vectis_http_client **out,
                                          vectis_error *error) {
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
  if (status == VECTIS_OK && out != NULL && *out != NULL) {
    (*out)->logger = effective.logger;
  }
  return status;
}

void vectis_http_client_destroy(vectis_http_client *client) {
  free(client);
}

void vectis_http_request_init(vectis_http_request *request) {
  if (request == NULL) {
    return;
  }
  memset(request, 0, sizeof(*request));
  request->method = VECTIS_HTTP_GET;
}

void vectis_http_response_cleanup(vectis_http_response *response) {
  if (response == NULL) {
    return;
  }
  free(response->content_type);
  free(response->body);
  memset(response, 0, sizeof(*response));
}

vectis_status vectis_http_response_json_into(const vectis_http_response *response,
                                             const lonejson_map *map,
                                             void *out,
                                             vectis_error *error) {
  lonejson_error json_error;

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
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP response body is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (lonejson_parse_buffer(map,
                            out,
                            response->body,
                            response->body_size,
                            NULL,
                            &json_error) != LONEJSON_STATUS_OK) {
    vectis_set_errorf(error,
                      VECTIS_ERR_INVALID,
                      "failed to parse JSON response: %s",
                      json_error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
    }
    return VECTIS_ERR_INVALID;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
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

vectis_status vectis_http_client_upload_file(vectis_http_client *client,
                                             vectis_http_method method,
                                             const char *url,
                                             const char *local_path,
                                             const char *content_type,
                                             vectis_http_response *response,
                                             vectis_error *error) {
  vectis_http_request request;

  vectis_http_request_init(&request);
  request.method = method;
  request.url = url;
  request.body_path = local_path;
  request.content_type = content_type;
  return vectis_http_client_execute(client, &request, response, error);
}

static vectis_status vectis_http_client_send_json(vectis_http_client *client,
                                                  vectis_http_method method,
                                                  const char *url,
                                                  const lonejson_map *map,
                                                  const void *value,
                                                  vectis_http_response *response,
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

static size_t vectis_curl_write_memory(char *ptr,
                                       size_t size,
                                       size_t nmemb,
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

static size_t vectis_curl_write_file(char *ptr,
                                     size_t size,
                                     size_t nmemb,
                                     void *userdata) {
  return fwrite(ptr, size, nmemb, (FILE *)userdata);
}

static size_t vectis_curl_write_stream(char *ptr,
                                       size_t size,
                                       size_t nmemb,
                                       void *userdata) {
  vectis_curl_response_stream *stream;
  size_t bytes;

  stream = (vectis_curl_response_stream *)userdata;
  bytes = size * nmemb;
  if (stream == NULL || stream->callback == NULL || bytes == 0u) {
    return bytes;
  }
  if (stream->callback(ptr, bytes, stream->userdata, stream->error) != VECTIS_OK) {
    stream->failed = 1;
    return 0u;
  }
  return bytes;
}

static size_t vectis_curl_read_file(char *ptr,
                                    size_t size,
                                    size_t nmemb,
                                    void *userdata) {
  return fread(ptr, size, nmemb, (FILE *)userdata);
}

static size_t vectis_curl_read_memory(char *ptr,
                                      size_t size,
                                      size_t nmemb,
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

static vectis_status vectis_file_size(FILE *file,
                                      long *out_size,
                                      vectis_error *error) {
  long current;
  long end;

  if (file == NULL || out_size == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "file is required");
    return VECTIS_ERR_INVALID;
  }
  current = ftell(file);
  if (current < 0L) {
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to inspect file position");
    return VECTIS_ERR_STATE;
  }
  if (fseek(file, 0L, SEEK_END) != 0) {
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to seek file");
    return VECTIS_ERR_STATE;
  }
  end = ftell(file);
  if (end < 0L || fseek(file, current, SEEK_SET) != 0) {
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to restore file position");
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
  free(body->owned_data);
  memset(body, 0, sizeof(*body));
}

static vectis_status vectis_prepare_curl_body(const vectis_http_request *request,
                                              vectis_curl_request_body *body,
                                              vectis_error *error) {
  lonejson_error json_error;
  size_t json_len;

  memset(body, 0, sizeof(*body));
  if (request->json_map != NULL || request->json_value != NULL) {
    if (request->json_map == NULL || request->json_value == NULL) {
      vectis_set_error(error,
                       VECTIS_ERR_INVALID,
                       "JSON HTTP request requires both json_map and json_value");
      return VECTIS_ERR_INVALID;
    }
    body->owned_data = lonejson_serialize_alloc(request->json_map,
                                                request->json_value,
                                                &json_len,
                                                NULL,
                                                &json_error);
    if (body->owned_data == NULL) {
      vectis_set_errorf(error,
                        VECTIS_ERR_INVALID,
                        "failed to serialize JSON request: %s",
                        json_error.message);
      if (error != NULL) {
        error->source = VECTIS_ERROR_SOURCE_LONEJSON;
      }
      return VECTIS_ERR_INVALID;
    }
    body->data = body->owned_data;
    body->size = json_len;
    return VECTIS_OK;
  }
  if (request->body != NULL || request->body_size > 0u) {
    if (request->body == NULL) {
      vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP request body pointer is required");
      return VECTIS_ERR_INVALID;
    }
    body->data = request->body;
    body->size = request->body_size;
    return VECTIS_OK;
  }
  if (request->body_path != NULL) {
    body->file = fopen(request->body_path, "rb");
    if (body->file == NULL) {
      vectis_set_errorf(error,
                        VECTIS_ERR_INVALID,
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

static vectis_status vectis_curl_set_common_tls(CURL *curl,
                                                const vectis_source *client_bundle,
                                                const char *client_bundle_path,
                                                const void *client_bundle_pem,
                                                size_t client_bundle_pem_size,
                                                const vectis_source *ca_bundle,
                                                const char *ca_bundle_path,
                                                vectis_error *error) {
  const char *client_path;
  const char *ca_path;
  const void *client_pem;
  size_t client_pem_size;
#ifdef CURLOPT_SSLCERT_BLOB
  struct curl_blob cert_blob;
#endif

  (void)error;
  client_path = vectis_source_path_or_old(client_bundle, client_bundle_path);
  ca_path = vectis_source_path_or_old(ca_bundle, ca_bundle_path);
  client_pem_size = 0u;
  client_pem = vectis_source_memory_or_old(client_bundle,
                                           client_bundle_pem,
                                           &client_pem_size,
                                           client_bundle_pem_size);
  if (ca_path != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_CAINFO, ca_path);
  }
  if (client_path != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_SSLCERT, client_path);
    (void)curl_easy_setopt(curl, CURLOPT_SSLKEY, client_path);
  } else if (client_pem != NULL && client_pem_size > 0u) {
#ifdef CURLOPT_SSLCERT_BLOB
    memset(&cert_blob, 0, sizeof(cert_blob));
    cert_blob.data = (void *)client_pem;
    cert_blob.len = client_pem_size;
    cert_blob.flags = CURL_BLOB_COPY;
    (void)curl_easy_setopt(curl, CURLOPT_SSLCERT_BLOB, &cert_blob);
    (void)curl_easy_setopt(curl, CURLOPT_SSLKEY_BLOB, &cert_blob);
#else
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "this libcurl build does not support in-memory client certificates");
    return VECTIS_ERR_INVALID;
#endif
  }
  return VECTIS_OK;
}

static vectis_status vectis_curl_set_error(vectis_error *error,
                                           CURLcode code,
                                           const char *message,
                                           char *curl_error) {
  vectis_set_errorf(error,
                    VECTIS_ERR_STATE,
                    "%s: %s",
                    message,
                    curl_error != NULL && curl_error[0] != '\0'
                        ? curl_error
                        : curl_easy_strerror(code));
  if (error != NULL) {
    error->source = VECTIS_ERROR_SOURCE_CURL;
    error->dependency_code = (long)code;
  }
  return VECTIS_ERR_STATE;
}

static unsigned vectis_http_effective_retry_attempts(const vectis_http_client_config *client,
                                                     const vectis_http_request *request) {
  if (request != NULL && request->retry_max_attempts > 0u) {
    return request->retry_max_attempts;
  }
  if (client != NULL && client->retry_max_attempts > 0u) {
    return client->retry_max_attempts;
  }
  return 1u;
}

static long vectis_http_effective_retry_initial_delay(const vectis_http_client_config *client,
                                                     const vectis_http_request *request) {
  if (request != NULL && request->retry_initial_delay_ms > 0L) {
    return request->retry_initial_delay_ms;
  }
  if (client != NULL && client->retry_initial_delay_ms > 0L) {
    return client->retry_initial_delay_ms;
  }
  return 0L;
}

static long vectis_http_effective_retry_max_delay(const vectis_http_client_config *client,
                                                  const vectis_http_request *request) {
  if (request != NULL && request->retry_max_delay_ms > 0L) {
    return request->retry_max_delay_ms;
  }
  if (client != NULL && client->retry_max_delay_ms > 0L) {
    return client->retry_max_delay_ms;
  }
  return 0L;
}

static vectis_http_retry_conditions vectis_http_effective_retry_conditions(
    const vectis_http_client_config *client,
    const vectis_http_request *request) {
  if (request != NULL && request->retry_conditions != VECTIS_HTTP_RETRY_NONE) {
    return request->retry_conditions;
  }
  if (client != NULL) {
    return client->retry_conditions;
  }
  return VECTIS_HTTP_RETRY_NONE;
}

static int vectis_http_status_retryable(long status_code,
                                        vectis_http_retry_conditions conditions) {
  if ((conditions & VECTIS_HTTP_RETRY_429) != 0u && status_code == 429L) {
    return 1;
  }
  if ((conditions & VECTIS_HTTP_RETRY_5XX) != 0u &&
      status_code >= 500L && status_code <= 599L) {
    return 1;
  }
  return 0;
}

static int vectis_http_error_retryable(const vectis_error *error,
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

static vectis_status vectis_append_output(char **out,
                                          size_t *out_size,
                                          const char *data,
                                          size_t data_size,
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

vectis_status vectis_http_client_post_json(vectis_http_client *client,
                                           const char *url,
                                           const lonejson_map *map,
                                           const void *value,
                                           vectis_http_response *response,
                                           vectis_error *error) {
  return vectis_http_client_send_json(client, VECTIS_HTTP_POST, url, map, value, response, error);
}

vectis_status vectis_http_client_put_json(vectis_http_client *client,
                                          const char *url,
                                          const lonejson_map *map,
                                          const void *value,
                                          vectis_http_response *response,
                                          vectis_error *error) {
  return vectis_http_client_send_json(client, VECTIS_HTTP_PUT, url, map, value, response, error);
}

vectis_status vectis_http_client_patch_json(vectis_http_client *client,
                                            const char *url,
                                            const lonejson_map *map,
                                            const void *value,
                                            vectis_http_response *response,
                                            vectis_error *error) {
  return vectis_http_client_send_json(client, VECTIS_HTTP_PATCH, url, map, value, response, error);
}

static vectis_status vectis_http_execute_once(const vectis_http_client_config *client,
                                             const vectis_http_request *request,
                                             vectis_http_response *response,
                                             vectis_error *error) {
  CURL *curl;
  CURLcode curl_code;
  struct curl_slist *headers;
  vectis_curl_buffer response_buffer;
  vectis_curl_response_stream response_stream;
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP client config is required");
    return VECTIS_ERR_INVALID;
  }
  if (request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP request is required");
    return VECTIS_ERR_INVALID;
  }
  if (request->url == NULL && client->base_url == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP request requires url or client base_url");
    return VECTIS_ERR_INVALID;
  }
  if (response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP response is required");
    return VECTIS_ERR_INVALID;
  }
  if (request->body != NULL && request->body_path != NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP request cannot use both body and body_path");
    return VECTIS_ERR_INVALID;
  }
  if (request->download_path != NULL && request->download_path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP download_path must not be empty");
    return VECTIS_ERR_INVALID;
  }
  if (request->download_path != NULL && request->response_body != NULL) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "HTTP request cannot use both download_path and response_body");
    return VECTIS_ERR_INVALID;
  }
  if (request->low_speed_limit_bytes_per_sec < 0L ||
      request->low_speed_time_seconds < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP request low-speed settings must be non-negative");
    return VECTIS_ERR_INVALID;
  }

  memset(response, 0, sizeof(*response));
  memset(&response_buffer, 0, sizeof(response_buffer));
  memset(&response_stream, 0, sizeof(response_stream));
  memset(&request_body, 0, sizeof(request_body));
  memset(curl_error, 0, sizeof(curl_error));
  headers = NULL;
  curl = NULL;
  url = NULL;
  response_content_type = NULL;
  download_file = NULL;

  if (request->method < VECTIS_HTTP_GET || request->method > VECTIS_HTTP_OPTIONS) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP request method is invalid");
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
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to initialize curl easy handle");
    return VECTIS_ERR_NOMEM;
  }

  timeout_ms = request->timeout_ms > 0L ? request->timeout_ms : client->timeout_ms;
  connect_timeout_ms = client->connect_timeout_ms;
  low_speed_limit = request->low_speed_limit_bytes_per_sec > 0L ?
      request->low_speed_limit_bytes_per_sec : client->low_speed_limit_bytes_per_sec;
  low_speed_time = request->low_speed_time_seconds > 0L ?
      request->low_speed_time_seconds : client->low_speed_time_seconds;
  (void)curl_easy_setopt(curl, CURLOPT_URL, url);
  (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
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
  if (vectis_curl_set_common_tls(curl,
                                 &client->client_bundle,
                                 client->client_bundle_path,
                                 client->client_bundle_pem,
                                 client->client_bundle_pem_size,
                                 &client->ca_bundle,
                                 client->ca_bundle_path,
                                 error) != VECTIS_OK) {
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
      vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP request headers are invalid");
      return VECTIS_ERR_INVALID;
    }
    headers = curl_slist_append(headers, request->headers[i]);
    if (headers == NULL) {
      curl_easy_cleanup(curl);
      vectis_curl_request_body_cleanup(&request_body);
      free(url);
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate curl headers");
      return VECTIS_ERR_NOMEM;
    }
  }
  if (request->content_type != NULL && request->content_type[0] != '\0') {
    (void)snprintf(content_type_header,
                   sizeof(content_type_header),
                   "Content-Type: %s",
                   request->content_type);
    headers = curl_slist_append(headers, content_type_header);
    if (headers == NULL) {
      curl_easy_cleanup(curl);
      vectis_curl_request_body_cleanup(&request_body);
      free(url);
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate content-type header");
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

  if (request_body.file != NULL) {
    if (request->method == VECTIS_HTTP_GET ||
        request->method == VECTIS_HTTP_HEAD ||
        request->method == VECTIS_HTTP_OPTIONS) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      vectis_curl_request_body_cleanup(&request_body);
      free(url);
      vectis_set_error(error,
                       VECTIS_ERR_INVALID,
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
        (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request_body.file_size);
      } else {
        (void)curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)request_body.file_size);
      }
    }
  } else if (request_body.data != NULL || request_body.size > 0u) {
    if (request_body.data == NULL) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      vectis_curl_request_body_cleanup(&request_body);
      free(url);
      vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP request body is invalid");
      return VECTIS_ERR_INVALID;
    }
    if (request->method == VECTIS_HTTP_POST) {
      (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.data);
      (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request_body.size);
    } else {
      (void)curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
      (void)curl_easy_setopt(curl, CURLOPT_READFUNCTION, vectis_curl_read_memory);
      (void)curl_easy_setopt(curl, CURLOPT_READDATA, &request_body);
      (void)curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)request_body.size);
    }
  }

  if (request->download_path != NULL) {
    download_file = fopen(request->download_path, "wb");
    if (download_file == NULL) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      vectis_curl_request_body_cleanup(&request_body);
      free(url);
      vectis_set_errorf(error,
                        VECTIS_ERR_INVALID,
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
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, vectis_curl_write_stream);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_stream);
  } else {
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, vectis_curl_write_memory);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);
  }

  if (client->configure_curl != NULL &&
      client->configure_curl(curl, client->configure_curl_userdata, error) != VECTIS_OK) {
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
      request->configure_curl(curl, request->configure_curl_userdata, error) != VECTIS_OK) {
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
  if (download_file != NULL && fclose(download_file) != 0) {
    download_file = NULL;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    vectis_curl_request_body_cleanup(&request_body);
    free(response_buffer.data);
    free(url);
    vectis_set_errorf(error,
                      VECTIS_ERR_STATE,
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
    if (error != NULL && error->code != VECTIS_OK) {
      return error->code;
    }
    vectis_set_error(error, VECTIS_ERR_STATE, "HTTP response body callback failed");
    return VECTIS_ERR_STATE;
  }
  if (curl_code != CURLE_OK) {
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    vectis_curl_request_body_cleanup(&request_body);
    free(response_buffer.data);
    free(url);
    return vectis_curl_set_error(error, curl_code, "curl request failed", curl_error);
  }
  if (response_buffer.failed) {
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    vectis_curl_request_body_cleanup(&request_body);
    free(response_buffer.data);
    free(url);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to buffer curl response body");
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
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy response content type");
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
  vectis_error attempt_error;
  vectis_status status;
  unsigned max_attempts;
  unsigned attempt;
  long delay_ms;
  long max_delay_ms;
  vectis_http_retry_conditions retry_conditions;

  if (request != NULL &&
      (request->retry_initial_delay_ms < 0L || request->retry_max_delay_ms < 0L)) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP request retry delays must be non-negative");
    return VECTIS_ERR_INVALID;
  }
  if (client != NULL &&
      (client->retry_initial_delay_ms < 0L || client->retry_max_delay_ms < 0L)) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "HTTP client retry delays must be non-negative");
    return VECTIS_ERR_INVALID;
  }

  max_attempts = vectis_http_effective_retry_attempts(client, request);
  retry_conditions = vectis_http_effective_retry_conditions(client, request);
  if (max_attempts > 1u && request != NULL && request->response_body != NULL) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "HTTP response streaming cannot be retried safely");
    return VECTIS_ERR_INVALID;
  }
  if (retry_conditions == VECTIS_HTTP_RETRY_NONE) {
    max_attempts = 1u;
  }

  delay_ms = vectis_http_effective_retry_initial_delay(client, request);
  max_delay_ms = vectis_http_effective_retry_max_delay(client, request);
  if (max_delay_ms > 0L && delay_ms > max_delay_ms) {
    delay_ms = max_delay_ms;
  }

  vectis_error_clear(&attempt_error);
  for (attempt = 1u; attempt <= max_attempts; ++attempt) {
    status = vectis_http_execute_once(client, request, response, &attempt_error);
    if (status == VECTIS_OK) {
      if (attempt >= max_attempts ||
          !vectis_http_status_retryable(response->status_code, retry_conditions)) {
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
                              const char *url,
                              vectis_http_response *response,
                              vectis_error *error) {
  vectis_http_request request;

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_GET;
  request.url = url;
  return vectis_http_execute(client, &request, response, error);
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
                               const char *url,
                               vectis_http_response *response,
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
                                        const char *url,
                                        const char *local_path,
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
                                      const char *url,
                                      const char *local_path,
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

static vectis_status vectis_http_send_json(const vectis_http_client_config *client,
                                           vectis_http_method method,
                                           const char *url,
                                           const lonejson_map *map,
                                           const void *value,
                                           vectis_http_response *response,
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
  return vectis_http_execute(client, &request, response, error);
}

vectis_status vectis_http_post_json(const vectis_http_client_config *client,
                                    const char *url,
                                    const lonejson_map *map,
                                    const void *value,
                                    vectis_http_response *response,
                                    vectis_error *error) {
  return vectis_http_send_json(client, VECTIS_HTTP_POST, url, map, value, response, error);
}

vectis_status vectis_http_put_json(const vectis_http_client_config *client,
                                   const char *url,
                                   const lonejson_map *map,
                                   const void *value,
                                   vectis_http_response *response,
                                   vectis_error *error) {
  return vectis_http_send_json(client, VECTIS_HTTP_PUT, url, map, value, response, error);
}

vectis_status vectis_http_patch_json(const vectis_http_client_config *client,
                                     const char *url,
                                     const lonejson_map *map,
                                     const void *value,
                                     vectis_http_response *response,
                                     vectis_error *error) {
  return vectis_http_send_json(client, VECTIS_HTTP_PATCH, url, map, value, response, error);
}

void vectis_sftp_config_init(vectis_sftp_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->timeout_ms = 30000L;
}

static vectis_status vectis_curl_set_ssh(CURL *curl,
                                         const char *username,
                                         const char *password,
                                         const vectis_source *private_key,
                                         const char *private_key_path,
                                         const char *known_hosts_path) {
  const char *key_path;

  key_path = vectis_source_path_or_old(private_key, private_key_path);
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

  if (config == NULL || config->url == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SFTP config with url is required");
    return VECTIS_ERR_INVALID;
  }
  if (local_path == NULL || remote_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SFTP upload requires local_path and remote_path");
    return VECTIS_ERR_INVALID;
  }
  memset(curl_error, 0, sizeof(curl_error));
  file = fopen(local_path, "rb");
  if (file == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID, "failed to open SFTP upload file: %s", local_path);
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
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to initialize curl easy handle");
    return VECTIS_ERR_NOMEM;
  }
  (void)curl_easy_setopt(curl, CURLOPT_URL, url);
  (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
  if (config->timeout_ms > 0L) {
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, config->timeout_ms);
  }
  (void)curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_READFUNCTION, vectis_curl_read_file);
  (void)curl_easy_setopt(curl, CURLOPT_READDATA, file);
  (void)curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)file_size);
  (void)vectis_curl_set_ssh(curl,
                            config->username,
                            config->password,
                            &config->private_key,
                            config->private_key_path,
                            config->known_hosts_path);
  curl_code = curl_easy_perform(curl);
  (void)fclose(file);
  curl_easy_cleanup(curl);
  free(url);
  if (curl_code != CURLE_OK) {
    return vectis_curl_set_error(error, curl_code, "curl SFTP upload failed", curl_error);
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

  if (config == NULL || config->url == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SFTP config with url is required");
    return VECTIS_ERR_INVALID;
  }
  if (remote_path == NULL || local_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SFTP download requires remote_path and local_path");
    return VECTIS_ERR_INVALID;
  }
  memset(curl_error, 0, sizeof(curl_error));
  url = vectis_join_url(config->url, remote_path, error);
  if (url == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  file = fopen(local_path, "wb");
  if (file == NULL) {
    free(url);
    vectis_set_errorf(error, VECTIS_ERR_INVALID, "failed to open SFTP download file: %s", local_path);
    return VECTIS_ERR_INVALID;
  }
  (void)pthread_once(&vectis_curl_once, vectis_curl_global_init_once);
  curl = curl_easy_init();
  if (curl == NULL) {
    (void)fclose(file);
    free(url);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to initialize curl easy handle");
    return VECTIS_ERR_NOMEM;
  }
  (void)curl_easy_setopt(curl, CURLOPT_URL, url);
  (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
  if (config->timeout_ms > 0L) {
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, config->timeout_ms);
  }
  (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, vectis_curl_write_file);
  (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
  (void)vectis_curl_set_ssh(curl,
                            config->username,
                            config->password,
                            &config->private_key,
                            config->private_key_path,
                            config->known_hosts_path);
  curl_code = curl_easy_perform(curl);
  if (fclose(file) != 0 && curl_code == CURLE_OK) {
    curl_code = CURLE_WRITE_ERROR;
  }
  curl_easy_cleanup(curl);
  free(url);
  if (curl_code != CURLE_OK) {
    return vectis_curl_set_error(error, curl_code, "curl SFTP download failed", curl_error);
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
    vectis_set_errorf(error,
                      VECTIS_ERR_STATE,
                      "failed to resolve SSH host: %s",
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
    vectis_set_errorf(error,
                      VECTIS_ERR_STATE,
                      "failed to connect to SSH host %s:%u",
                      config->host,
                      (unsigned)config->port);
    return VECTIS_ERR_STATE;
  }
  *out_fd = fd;
  return VECTIS_OK;
}

static vectis_status vectis_ssh_authenticate(LIBSSH2_SESSION *session,
                                             const vectis_ssh_config *config,
                                             vectis_error *error) {
  const char *key_path;
  int rc;

  key_path = vectis_source_path_or_old(&config->private_key, config->private_key_path);
  if (key_path != NULL) {
    rc = libssh2_userauth_publickey_fromfile(session,
                                             config->username,
                                             NULL,
                                             key_path,
                                             config->password);
  } else if (config->password != NULL) {
    rc = libssh2_userauth_password(session, config->username, config->password);
  } else {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "SSH authentication requires password or private_key_path");
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
      status = vectis_append_output(&result->stdout_data,
                                    &result->stdout_size,
                                    buffer,
                                    (size_t)n,
                                    error);
      if (status != VECTIS_OK) {
        return status;
      }
      active = 1;
      n = libssh2_channel_read(channel, buffer, sizeof(buffer));
    }
    nerr = libssh2_channel_read_stderr(channel, buffer, sizeof(buffer));
    while (nerr > 0) {
      status = vectis_append_output(&result->stderr_data,
                                    &result->stderr_size,
                                    buffer,
                                    (size_t)nerr,
                                    error);
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
  LIBSSH2_SESSION *session;
  LIBSSH2_CHANNEL *channel;
  int fd;
  int rc;
  vectis_status status;

  if (config == NULL || config->host == NULL || config->username == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SSH config requires host and username");
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
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to initialize SSH session");
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
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to open SSH session channel");
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "SSH config requires host and username");
    return VECTIS_ERR_INVALID;
  }
  if (local_path == NULL || remote_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SSH SFTP upload requires local_path and remote_path");
    return VECTIS_ERR_INVALID;
  }
  fd = -1;
  session = NULL;
  sftp = NULL;
  remote = NULL;
  local = fopen(local_path, "rb");
  if (local == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID, "failed to open SSH SFTP upload file: %s", local_path);
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
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to initialize SSH session");
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
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to initialize SSH SFTP session");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
    }
    return VECTIS_ERR_STATE;
  }
  remote = libssh2_sftp_open(sftp,
                             remote_path,
                             LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                             0644);
  if (remote == NULL) {
    libssh2_sftp_shutdown(sftp);
    (void)fclose(local);
    libssh2_session_disconnect(session, "vectis shutdown");
    libssh2_session_free(session);
    (void)close(fd);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to open remote SFTP file for upload");
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
        vectis_set_error(error, VECTIS_ERR_STATE, "failed to write remote SFTP file");
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
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to read local SFTP upload file");
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
    vectis_set_error(error, VECTIS_ERR_INVALID, "SSH config requires host and username");
    return VECTIS_ERR_INVALID;
  }
  if (remote_path == NULL || local_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SSH SFTP download requires remote_path and local_path");
    return VECTIS_ERR_INVALID;
  }
  fd = -1;
  session = NULL;
  sftp = NULL;
  remote = NULL;
  local = fopen(local_path, "wb");
  if (local == NULL) {
    vectis_set_errorf(error, VECTIS_ERR_INVALID, "failed to open SSH SFTP download file: %s", local_path);
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
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to initialize SSH session");
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
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to initialize SSH SFTP session");
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
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to open remote SFTP file for download");
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
      vectis_set_error(error, VECTIS_ERR_STATE, "failed to write local SFTP download file");
      break;
    }
    nread = libssh2_sftp_read(remote, buffer, sizeof(buffer));
  }
  if (status == VECTIS_OK && nread < 0) {
    status = VECTIS_ERR_STATE;
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to read remote SFTP file");
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LIBSSH2;
      error->dependency_code = (long)nread;
    }
  }
  if (fclose(local) != 0 && status == VECTIS_OK) {
    status = VECTIS_ERR_STATE;
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to flush local SFTP download file");
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

vectis_status vectis_mqtt_publish(const vectis_mqtt_config *config,
                                  const char *topic,
                                  const void *payload,
                                  size_t payload_size,
                                  const char *content_type,
                                  vectis_error *error) {
  CURL *curl;
  CURLcode curl_code;
  char *url;
  char curl_error[CURL_ERROR_SIZE];

  (void)content_type;
  if (config == NULL || config->broker_url == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "MQTT config with broker_url is required");
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
  memset(curl_error, 0, sizeof(curl_error));
  url = vectis_join_url(config->broker_url, topic, error);
  if (url == NULL) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  (void)pthread_once(&vectis_curl_once, vectis_curl_global_init_once);
  curl = curl_easy_init();
  if (curl == NULL) {
    free(url);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to initialize curl easy handle");
    return VECTIS_ERR_NOMEM;
  }
  (void)curl_easy_setopt(curl, CURLOPT_URL, url);
  (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
  if (config->timeout_ms > 0L) {
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, config->timeout_ms);
  }
  if (config->username != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_USERNAME, config->username);
  }
  if (config->password != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_PASSWORD, config->password);
  }
  if (vectis_curl_set_common_tls(curl,
                                 &config->client_bundle,
                                 config->client_bundle_path,
                                 config->client_bundle_pem,
                                 config->client_bundle_pem_size,
                                 &config->ca_bundle,
                                 config->ca_bundle_path,
                                 error) != VECTIS_OK) {
    curl_easy_cleanup(curl);
    free(url);
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (payload != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)payload_size);
  } else {
    (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)0);
  }
  curl_code = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  free(url);
  if (curl_code != CURLE_OK) {
    return vectis_curl_set_error(error, curl_code, "curl MQTT publish failed", curl_error);
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_mqtt_publish_json(const vectis_mqtt_config *config,
                                       const char *topic,
                                       const lonejson_map *map,
                                       const void *value,
                                       vectis_error *error) {
  lonejson_error json_error;
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
  json = lonejson_serialize_alloc(map, value, &json_size, NULL, &json_error);
  if (json == NULL) {
    vectis_set_errorf(error,
                      VECTIS_ERR_INVALID,
                      "failed to serialize MQTT JSON payload: %s",
                      json_error.message);
    if (error != NULL) {
      error->source = VECTIS_ERROR_SOURCE_LONEJSON;
    }
    return VECTIS_ERR_INVALID;
  }
  status = vectis_mqtt_publish(config, topic, json, json_size, "application/json", error);
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

static int vectis_cert_add_name_entry(X509_NAME *name,
                                      const char *field,
                                      const char *value) {
  if (value == NULL || value[0] == '\0') {
    return 1;
  }
  return X509_NAME_add_entry_by_txt(name,
                                    field,
                                    MBSTRING_ASC,
                                    (const unsigned char *)value,
                                    -1,
                                    -1,
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
    vectis_set_errorf(error, VECTIS_ERR_STATE, "failed to set %s subject", label);
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
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to allocate certificate subject");
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
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate subjectAltName");
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

static vectis_status vectis_cert_add_extension(X509 *cert,
                                               int nid,
                                               const char *value,
                                               vectis_error *error) {
  X509V3_CTX ctx;
  X509_EXTENSION *extension;
  char *value_copy;

  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, cert, cert, NULL, NULL, 0);
  value_copy = vectis_strdup(value);
  if (value_copy == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy certificate extension value");
    return VECTIS_ERR_NOMEM;
  }
  extension = X509V3_EXT_conf_nid(NULL, &ctx, nid, value_copy);
  free(value_copy);
  if (extension == NULL) {
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to create certificate extension");
    return VECTIS_ERR_STATE;
  }
  if (X509_add_ext(cert, extension, -1) != 1) {
    X509_EXTENSION_free(extension);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to add certificate extension");
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
  if (key_ctx == NULL ||
      EVP_PKEY_keygen_init(key_ctx) <= 0 ||
      EVP_PKEY_CTX_set_rsa_keygen_bits(key_ctx, (int)key_bits) <= 0 ||
      EVP_PKEY_keygen(key_ctx, &key) <= 0) {
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(key_ctx);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to generate RSA private key");
    return NULL;
  }
  EVP_PKEY_CTX_free(key_ctx);
  return key;
}

static vectis_status vectis_cert_write_outputs(const vectis_cert_bundle_config *config,
                                               EVP_PKEY *key,
                                               X509 *cert,
                                               vectis_error *error) {
  FILE *fp;

  if (config->output_bundle_path != NULL) {
    fp = fopen(config->output_bundle_path, "wb");
    if (fp == NULL) {
      vectis_set_errorf(error,
                        VECTIS_ERR_INVALID,
                        "failed to open certificate bundle output: %s",
                        config->output_bundle_path);
      return VECTIS_ERR_INVALID;
    }
    if (PEM_write_X509(fp, cert) != 1 ||
        PEM_write_PrivateKey(fp, key, NULL, NULL, 0, NULL, NULL) != 1) {
      (void)fclose(fp);
      vectis_set_error(error, VECTIS_ERR_STATE, "failed to write certificate bundle");
      return VECTIS_ERR_STATE;
    }
    if (fclose(fp) != 0) {
      vectis_set_error(error, VECTIS_ERR_STATE, "failed to close certificate bundle");
      return VECTIS_ERR_STATE;
    }
  }
  if (config->output_cert_path != NULL) {
    fp = fopen(config->output_cert_path, "wb");
    if (fp == NULL) {
      vectis_set_errorf(error,
                        VECTIS_ERR_INVALID,
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
      vectis_set_errorf(error,
                        VECTIS_ERR_INVALID,
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

static vectis_status vectis_cert_load_ca(const vectis_cert_bundle_config *config,
                                         X509 **out_cert,
                                         EVP_PKEY **out_key,
                                         vectis_error *error) {
  FILE *fp;

  if (config->ca_cert_path == NULL && config->ca_key_path == NULL) {
    *out_cert = NULL;
    *out_key = NULL;
    return VECTIS_OK;
  }
  if (config->ca_cert_path == NULL || config->ca_key_path == NULL) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "CA-signed certificate generation requires ca_cert_path and ca_key_path");
    return VECTIS_ERR_INVALID;
  }

  fp = fopen(config->ca_cert_path, "rb");
  if (fp == NULL) {
    vectis_set_errorf(error,
                      VECTIS_ERR_INVALID,
                      "failed to open CA certificate: %s",
                      config->ca_cert_path);
    return VECTIS_ERR_INVALID;
  }
  *out_cert = PEM_read_X509(fp, NULL, NULL, NULL);
  (void)fclose(fp);
  if (*out_cert == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "failed to parse CA certificate");
    return VECTIS_ERR_INVALID;
  }

  fp = fopen(config->ca_key_path, "rb");
  if (fp == NULL) {
    X509_free(*out_cert);
    *out_cert = NULL;
    vectis_set_errorf(error,
                      VECTIS_ERR_INVALID,
                      "failed to open CA private key: %s",
                      config->ca_key_path);
    return VECTIS_ERR_INVALID;
  }
  *out_key = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
  (void)fclose(fp);
  if (*out_key == NULL) {
    X509_free(*out_cert);
    *out_cert = NULL;
    vectis_set_error(error, VECTIS_ERR_INVALID, "failed to parse CA private key");
    return VECTIS_ERR_INVALID;
  }
  if (X509_check_private_key(*out_cert, *out_key) != 1) {
    X509_free(*out_cert);
    EVP_PKEY_free(*out_key);
    *out_cert = NULL;
    *out_key = NULL;
    vectis_set_error(error, VECTIS_ERR_INVALID, "CA certificate and private key do not match");
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static vectis_status vectis_read_source_bytes(const vectis_source *source,
                                              void **out,
                                              size_t *out_size,
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
    vectis_set_errorf(error, VECTIS_ERR_INVALID, "%s source is required", label);
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  *out_size = 0u;
  if (source->memory != NULL && source->memory_size > 0u) {
    buffer = malloc(source->memory_size);
    if (buffer == NULL) {
      vectis_set_errorf(error, VECTIS_ERR_NOMEM, "failed to copy %s source", label);
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
      vectis_set_errorf(error, VECTIS_ERR_INVALID, "failed to open %s: %s", label, source->path);
      return VECTIS_ERR_INVALID;
    }
    if (fseek(fp, 0L, SEEK_END) != 0) {
      (void)fclose(fp);
      vectis_set_errorf(error, VECTIS_ERR_STATE, "failed to seek %s: %s", label, source->path);
      return VECTIS_ERR_STATE;
    }
    length = ftell(fp);
    if (length <= 0L || fseek(fp, 0L, SEEK_SET) != 0) {
      (void)fclose(fp);
      vectis_set_errorf(error, VECTIS_ERR_INVALID, "%s is empty or unreadable: %s", label, source->path);
      return VECTIS_ERR_INVALID;
    }
    buffer = malloc((size_t)length);
    if (buffer == NULL) {
      (void)fclose(fp);
      vectis_set_errorf(error, VECTIS_ERR_NOMEM, "failed to allocate %s buffer", label);
      return VECTIS_ERR_NOMEM;
    }
    nread = fread(buffer, 1u, (size_t)length, fp);
    if (fclose(fp) != 0 || nread != (size_t)length) {
      free(buffer);
      vectis_set_errorf(error, VECTIS_ERR_STATE, "failed to read %s: %s", label, source->path);
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
    if (source->source->reset != NULL && source->source->reset(source->source, &lcerr) != LC_OK) {
      vectis_set_errorf(error,
                        VECTIS_ERR_STATE,
                        "failed to reset %s source: %s",
                        label,
                        lcerr.message != NULL ? lcerr.message : "unknown lockdc error");
      lc_error_cleanup(&lcerr);
      return VECTIS_ERR_STATE;
    }
    for (;;) {
      nread = source->source->read(source->source, chunk, sizeof(chunk), &lcerr);
      if (nread == 0u) {
        break;
      }
      if (size + nread < size) {
        free(buffer);
        lc_error_cleanup(&lcerr);
        vectis_set_errorf(error, VECTIS_ERR_NOMEM, "%s source is too large", label);
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
          vectis_set_errorf(error, VECTIS_ERR_NOMEM, "failed to grow %s source buffer", label);
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
    vectis_set_errorf(error, VECTIS_ERR_INVALID, "failed to open private key output: %s", path);
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

static vectis_status vectis_cert_add_csr_extension(X509_REQ *request,
                                                   int nid,
                                                   const char *value,
                                                   vectis_error *error) {
  X509V3_CTX ctx;
  X509_EXTENSION *extension;
  STACK_OF(X509_EXTENSION) *extensions;
  char *value_copy;
  vectis_status status;

  extensions = NULL;
  extension = NULL;
  status = VECTIS_OK;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, NULL, NULL, request, NULL, 0);
  value_copy = vectis_strdup(value);
  if (value_copy == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy CSR extension value");
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
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate CSR extension stack");
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

static vectis_status vectis_cert_validate_time(X509 *cert, vectis_error *error) {
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

static vectis_status vectis_cert_verify_ca(X509 *cert,
                                           X509 *ca_cert,
                                           vectis_error *error) {
  X509_STORE *store;
  X509_STORE_CTX *ctx;
  int ok;

  store = X509_STORE_new();
  if (store == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate certificate store");
    return VECTIS_ERR_NOMEM;
  }
  if (X509_STORE_add_cert(store, ca_cert) != 1) {
    X509_STORE_free(store);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to add CA certificate to store");
    return VECTIS_ERR_STATE;
  }
  ctx = X509_STORE_CTX_new();
  if (ctx == NULL) {
    X509_STORE_free(store);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate certificate verify context");
    return VECTIS_ERR_NOMEM;
  }
  if (X509_STORE_CTX_init(ctx, store, cert, NULL) != 1) {
    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to initialize certificate verification");
    return VECTIS_ERR_STATE;
  }
  ok = X509_verify_cert(ctx);
  X509_STORE_CTX_free(ctx);
  X509_STORE_free(store);
  if (ok != 1) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "certificate failed CA verification");
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
  status = vectis_read_source_bytes(bundle, &pem, &pem_size, "certificate bundle", error);
  if (status != VECTIS_OK) {
    return status;
  }
  cert = vectis_cert_read_x509(pem, pem_size);
  if (cert == NULL) {
    free(pem);
    vectis_set_error(error, VECTIS_ERR_INVALID, "failed to parse certificate from bundle");
    return VECTIS_ERR_INVALID;
  }
  key = vectis_cert_read_key(pem, pem_size);
  if (key == NULL) {
    X509_free(cert);
    free(pem);
    vectis_set_error(error, VECTIS_ERR_INVALID, "failed to parse private key from bundle");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_cert_validate_time(cert, error);
  if (status == VECTIS_OK && X509_check_private_key(cert, key) != 1) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "certificate and private key do not match");
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

  status = vectis_read_source_bytes(certificate, &cert_pem, &cert_pem_size, "certificate", error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_read_source_bytes(private_key, &key_pem, &key_pem_size, "private key", error);
  if (status != VECTIS_OK) {
    free(cert_pem);
    return status;
  }
  cert = vectis_cert_read_x509(cert_pem, cert_pem_size);
  key = vectis_cert_read_key(key_pem, key_pem_size);
  if (cert == NULL || key == NULL) {
    status = VECTIS_ERR_INVALID;
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     cert == NULL ? "failed to parse certificate" : "failed to parse private key");
    goto done;
  }
  status = vectis_cert_validate_time(cert, error);
  if (status != VECTIS_OK) {
    goto done;
  }
  if (X509_check_private_key(cert, key) != 1) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "certificate and private key do not match");
    status = VECTIS_ERR_INVALID;
    goto done;
  }
  if (ca_bundle != NULL &&
      (ca_bundle->path != NULL || ca_bundle->memory != NULL || ca_bundle->source != NULL)) {
    status = vectis_read_source_bytes(ca_bundle, &ca_pem, &ca_pem_size, "CA bundle", error);
    if (status != VECTIS_OK) {
      goto done;
    }
    ca_cert = vectis_cert_read_x509(ca_pem, ca_pem_size);
    if (ca_cert == NULL) {
      vectis_set_error(error, VECTIS_ERR_INVALID, "failed to parse CA certificate");
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

vectis_status vectis_cert_generate_private_key(const vectis_private_key_config *config,
                                               vectis_error *error) {
  EVP_PKEY *key;
  vectis_status status;

  if (config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "private key config is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->output_key_path == NULL || config->output_key_path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "private key output_key_path is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->key_bits < 1024u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "private key key_bits must be at least 1024");
    return VECTIS_ERR_INVALID;
  }

  key = vectis_cert_generate_rsa_key(config->key_bits, error);
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
  if (config->subject.common_name == NULL || config->subject.common_name[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "CSR subject common_name is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->output_csr_path == NULL || config->output_csr_path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "CSR output_csr_path is required");
    return VECTIS_ERR_INVALID;
  }

  private_key_source = config->private_key;
  if (private_key_source.path == NULL &&
      private_key_source.memory == NULL &&
      private_key_source.source == NULL &&
      config->private_key_path != NULL) {
    private_key_source = vectis_source_from_path(config->private_key_path);
  }
  key_pem = NULL;
  key_pem_size = 0u;
  key = NULL;
  request = NULL;
  fp = NULL;
  san = NULL;
  status = vectis_read_source_bytes(&private_key_source, &key_pem, &key_pem_size, "private key", error);
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
    status = vectis_cert_add_csr_extension(request, NID_subject_alt_name, san, error);
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
    vectis_set_errorf(error, VECTIS_ERR_INVALID, "failed to open CSR output: %s", config->output_csr_path);
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
  if (status != VECTIS_OK && error != NULL && error->source == VECTIS_ERROR_SOURCE_VECTIS) {
    error->source = VECTIS_ERROR_SOURCE_OPENSSL;
  }
  return status;
}

vectis_status vectis_cert_generate_bundle(const vectis_cert_bundle_config *config,
                                          vectis_error *error) {
  EVP_PKEY *key;
  EVP_PKEY *signing_key;
  X509 *ca_cert;
  EVP_PKEY *ca_key;
  X509 *cert;
  char *san;
  vectis_status status;

  if (config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "certificate bundle config is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->subject.common_name == NULL || config->subject.common_name[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "certificate subject common_name is required");
    return VECTIS_ERR_INVALID;
  }
  if (config->output_bundle_path == NULL &&
      (config->output_cert_path == NULL || config->output_key_path == NULL)) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "certificate output requires output_bundle_path or output_cert_path + output_key_path");
    return VECTIS_ERR_INVALID;
  }
  if (config->key_bits < 1024u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "certificate key_bits must be at least 1024");
    return VECTIS_ERR_INVALID;
  }
  if (config->valid_days <= 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "certificate valid_days must be greater than zero");
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
  key = vectis_cert_generate_rsa_key(config->key_bits, error);
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
      X509_gmtime_adj(X509_get_notAfter(cert), config->valid_days * 24L * 60L * 60L) == NULL ||
      X509_set_pubkey(cert, key) != 1) {
    status = VECTIS_ERR_STATE;
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to initialize certificate");
    goto done;
  }
  status = vectis_cert_set_subject(cert, &config->subject, error);
  if (status != VECTIS_OK) {
    goto done;
  }
  if (X509_set_issuer_name(cert,
                           ca_cert != NULL
                               ? X509_get_subject_name(ca_cert)
                               : X509_get_subject_name(cert)) != 1) {
    status = VECTIS_ERR_STATE;
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to set certificate issuer");
    goto done;
  }
  status = vectis_cert_add_extension(cert,
                                     NID_basic_constraints,
                                     config->is_ca ? "critical,CA:TRUE" : "critical,CA:FALSE",
                                     error);
  if (status != VECTIS_OK) {
    goto done;
  }
  status = vectis_cert_add_extension(cert,
                                     NID_key_usage,
                                     config->is_ca ? "keyCertSign,cRLSign" :
                                         "digitalSignature,keyEncipherment",
                                     error);
  if (status != VECTIS_OK) {
    goto done;
  }
  if (!config->is_ca) {
    status = vectis_cert_add_extension(cert, NID_ext_key_usage, "serverAuth,clientAuth", error);
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
  if (status != VECTIS_OK && error != NULL && error->source == VECTIS_ERROR_SOURCE_VECTIS) {
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
