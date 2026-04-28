#include "vectis_internal.h"

#include <lc/lc.h>
#include <lonejson.h>
#include <pthread.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct vectis_route_entry {
  vectis_http_method method;
  vectis_http_methods methods;
  char *path;
  vectis_route_path_kind path_kind;
  vectis_body_policy body;
  vectis_route_handler_fn handler;
  void *userdata;
} vectis_route_entry;

typedef struct vectis_app_impl {
  pthread_mutex_t mutex;
  int started;
  int owns_logger;
  char *app_name;
  char *bind;
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
  long timeout_ms;
  unsigned short port;
  vectis_tls_mode tls_mode;
  int require_client_certificate;
  vectis_server_config server;
  pslog_logger *logger;
  vectis_route_entry *routes;
  size_t route_count;
  size_t route_capacity;
  struct lc_client *lockd_client;
} vectis_app_impl;

struct vectis_http_client {
  vectis_http_client_config config;
  pslog_logger *logger;
};

static vectis_status vectis_app_start_impl(vectis_app *app, vectis_error *error);
static vectis_status vectis_app_stop_impl(vectis_app *app, vectis_error *error);
static vectis_status vectis_app_register_route_impl(vectis_app *app,
                                                    const vectis_route_config *route,
                                                    vectis_error *error);
static size_t vectis_app_route_count_impl(const vectis_app *app);
static pslog_logger *vectis_app_logger_impl(vectis_app *app);

static const vectis_methods vectis_default_methods = {
    vectis_destroy,
    vectis_app_start_impl,
    vectis_app_stop_impl,
    vectis_app_register_route_impl,
    vectis_app_route_count_impl,
    vectis_app_logger_impl};

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

static vectis_status vectis_not_implemented(vectis_error *error,
                                            const char *feature) {
  vectis_set_errorf(error,
                    VECTIS_ERR_NOT_IMPLEMENTED,
                    "%s is not implemented yet",
                    feature != NULL ? feature : "requested Vectis feature");
  return VECTIS_ERR_NOT_IMPLEMENTED;
}

static vectis_status vectis_not_implemented_from(vectis_error *error,
                                                 vectis_error_source source,
                                                 const char *feature) {
  vectis_status status;

  status = vectis_not_implemented(error, feature);
  if (error != NULL) {
    error->source = source;
  }
  return status;
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

vectis_source vectis_source_from_lc(struct lc_source *lc_source) {
  vectis_source source;

  vectis_source_init(&source);
  source.source = lc_source;
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
  config->keepalive_enabled = 1;
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
  policy.spool_to_disk = 0;
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
  policy.spool_to_disk = 1;
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
  } else if (!policy->spool_to_disk && policy->memory_buffer_limit_bytes < policy->max_bytes) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "streaming upload body policy requires spool_to_disk unless fully buffered");
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
  }
  free(impl->routes);
  impl->routes = NULL;
  impl->route_count = 0u;
  impl->route_capacity = 0u;
}

static void vectis_destroy_impl(vectis_app_impl *impl) {
  if (impl == NULL) {
    return;
  }

  if (impl->lockd_client != NULL) {
    lc_client_close(impl->lockd_client);
    impl->lockd_client = NULL;
  }

  vectis_free_routes(impl);
  vectis_free_endpoints(impl);

  free(impl->app_name);
  free(impl->bind);
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
  if (config->keepalive_enabled) {
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
  int has_lockd_transport;
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

  if (app == NULL) {
    return;
  }
  impl = (vectis_app_impl *)app->impl;
  vectis_destroy_impl(impl);
  free(app);
}

static vectis_status vectis_app_start_impl(vectis_app *app, vectis_error *error) {
  vectis_app_impl *impl;
  vectis_status status;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  impl = (vectis_app_impl *)app->impl;

  status = vectis_validate_startable(impl, error);
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

  vectis_set_error(error,
                   VECTIS_ERR_NOT_IMPLEMENTED,
                   "Kore runtime bootstrap and lockd client startup are not implemented yet");
  return VECTIS_ERR_NOT_IMPLEMENTED;
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
  (void)pthread_mutex_unlock(&impl->mutex);

  vectis_set_error(error,
                   VECTIS_ERR_NOT_IMPLEMENTED,
                   "Kore runtime shutdown and consumer teardown are not implemented yet");
  return VECTIS_ERR_NOT_IMPLEMENTED;
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

vectis_status vectis_register_json_route(vectis_app *app,
                                         const vectis_json_route_config *route,
                                         vectis_error *error) {
  vectis_app_impl *impl;

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
  if (route->output_map != NULL && (route->output_size == 0u || route->output_size > 10485760u)) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json route output_size is invalid");
    return VECTIS_ERR_INVALID;
  }
  impl = (vectis_app_impl *)app->impl;
  if (vectis_validate_body_policy(&route->body,
                                  impl != NULL ? &impl->server : NULL,
                                  error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  return vectis_not_implemented_from(error, VECTIS_ERROR_SOURCE_KORE, "JSON route auto-wiring");
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

size_t vectis_route_count(const vectis_app *app) {
  if (app == NULL || app->vt == NULL || app->vt->route_count == NULL) {
    return 0u;
  }
  return app->vt->route_count(app);
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

  if (app == NULL || app->impl == NULL) {
    return NULL;
  }
  impl = (vectis_app_impl *)app->impl;
  return impl->lockd_client;
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

vectis_status vectis_request_json_into(vectis_request *request,
                                       const lonejson_map *map,
                                       void *out,
                                       vectis_error *error) {
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
  return vectis_not_implemented_from(error, VECTIS_ERROR_SOURCE_KORE, "request JSON parsing");
}

const char *vectis_request_path_param(vectis_request *request,
                                      const char *name) {
  (void)request;
  (void)name;
  return NULL;
}

const char *vectis_request_query(vectis_request *request,
                                 const char *name) {
  (void)request;
  (void)name;
  return NULL;
}

const char *vectis_request_header(vectis_request *request,
                                  const char *name) {
  (void)request;
  (void)name;
  return NULL;
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
  return vectis_not_implemented_from(error, VECTIS_ERROR_SOURCE_KORE, "request body access");
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
  return vectis_not_implemented_from(error, VECTIS_ERROR_SOURCE_KORE, "response status writing");
}

vectis_status vectis_response_text(vectis_response *response,
                                   int status_code,
                                   const char *content_type,
                                   const char *text,
                                   vectis_error *error) {
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
  return vectis_not_implemented_from(error, VECTIS_ERROR_SOURCE_KORE, "text response writing");
}

vectis_status vectis_response_bytes(vectis_response *response,
                                    int status_code,
                                    const char *content_type,
                                    vectis_bytes body,
                                    vectis_error *error) {
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
  return vectis_not_implemented_from(error, VECTIS_ERROR_SOURCE_KORE, "byte response writing");
}

vectis_status vectis_response_json(vectis_response *response,
                                   int status_code,
                                   const lonejson_map *map,
                                   const void *value,
                                   vectis_error *error) {
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
  return vectis_not_implemented_from(error, VECTIS_ERROR_SOURCE_KORE, "response JSON serialization");
}

void vectis_http_client_config_init(vectis_http_client_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->timeout_ms = 30000L;
  config->connect_timeout_ms = 10000L;
  config->follow_redirects = 1;
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
  if (effective->timeout_ms < 0L || effective->connect_timeout_ms < 0L) {
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

vectis_status vectis_http_execute(const vectis_http_client_config *client,
                                  const vectis_http_request *request,
                                  vectis_http_response *response,
                                  vectis_error *error) {
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
  return vectis_not_implemented_from(error, VECTIS_ERROR_SOURCE_CURL, "curl HTTP execution");
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

vectis_status vectis_sftp_upload_file(const vectis_sftp_config *config,
                                      const char *local_path,
                                      const char *remote_path,
                                      vectis_error *error) {
  if (config == NULL || config->url == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SFTP config with url is required");
    return VECTIS_ERR_INVALID;
  }
  if (local_path == NULL || remote_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SFTP upload requires local_path and remote_path");
    return VECTIS_ERR_INVALID;
  }
  return vectis_not_implemented_from(error, VECTIS_ERROR_SOURCE_CURL, "curl SFTP upload");
}

vectis_status vectis_sftp_download_file(const vectis_sftp_config *config,
                                        const char *remote_path,
                                        const char *local_path,
                                        vectis_error *error) {
  if (config == NULL || config->url == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SFTP config with url is required");
    return VECTIS_ERR_INVALID;
  }
  if (remote_path == NULL || local_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SFTP download requires remote_path and local_path");
    return VECTIS_ERR_INVALID;
  }
  return vectis_not_implemented_from(error, VECTIS_ERROR_SOURCE_CURL, "curl SFTP download");
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

vectis_status vectis_ssh_exec(const vectis_ssh_config *config,
                              const char *command,
                              vectis_ssh_exec_result *result,
                              vectis_error *error) {
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
  return vectis_not_implemented_from(error, VECTIS_ERROR_SOURCE_LIBSSH2, "libssh2 command execution");
}

vectis_status vectis_ssh_sftp_upload_file(const vectis_ssh_config *config,
                                          const char *local_path,
                                          const char *remote_path,
                                          vectis_error *error) {
  if (config == NULL || config->host == NULL || config->username == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SSH config requires host and username");
    return VECTIS_ERR_INVALID;
  }
  if (local_path == NULL || remote_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SSH SFTP upload requires local_path and remote_path");
    return VECTIS_ERR_INVALID;
  }
  return vectis_not_implemented_from(error, VECTIS_ERROR_SOURCE_LIBSSH2, "libssh2 SFTP upload");
}

vectis_status vectis_ssh_sftp_download_file(const vectis_ssh_config *config,
                                            const char *remote_path,
                                            const char *local_path,
                                            vectis_error *error) {
  if (config == NULL || config->host == NULL || config->username == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SSH config requires host and username");
    return VECTIS_ERR_INVALID;
  }
  if (remote_path == NULL || local_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "SSH SFTP download requires remote_path and local_path");
    return VECTIS_ERR_INVALID;
  }
  return vectis_not_implemented_from(error, VECTIS_ERROR_SOURCE_LIBSSH2, "libssh2 SFTP download");
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
  return vectis_not_implemented_from(error, VECTIS_ERROR_SOURCE_CURL, "curl MQTT publish");
}

vectis_status vectis_mqtt_publish_json(const vectis_mqtt_config *config,
                                       const char *topic,
                                       const lonejson_map *map,
                                       const void *value,
                                       vectis_error *error) {
  if (map == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lonejson map is required");
    return VECTIS_ERR_INVALID;
  }
  if (value == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json value is required");
    return VECTIS_ERR_INVALID;
  }
  (void)config;
  (void)topic;
  return vectis_not_implemented_from(error, VECTIS_ERROR_SOURCE_CURL, "curl MQTT JSON publish");
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

vectis_status vectis_cert_generate_bundle(const vectis_cert_bundle_config *config,
                                          vectis_error *error) {
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
  return vectis_not_implemented_from(error,
                                     VECTIS_ERROR_SOURCE_OPENSSL,
                                     "OpenSSL certificate bundle generation");
}

struct lc_client *vectis_internal_lockd_client(vectis_app *app) {
  vectis_app_impl *impl;

  if (app == NULL || app->impl == NULL) {
    return NULL;
  }

  impl = (vectis_app_impl *)app->impl;
  return impl->lockd_client;
}
