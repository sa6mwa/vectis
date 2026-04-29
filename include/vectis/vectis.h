#ifndef VECTIS_VECTIS_H
#define VECTIS_VECTIS_H

#include <stddef.h>
#include <curl/curl.h>
#ifndef LONEJSON_WITH_CURL
#define LONEJSON_WITH_CURL 1
#endif
#include <lonejson.h>
#include <pslog.h>

#define VECTIS_ACME_DIRECTORY_LETSENCRYPT_PRODUCTION "https://acme-v02.api.letsencrypt.org/directory"
#define VECTIS_SERVER_DEFAULT_MAX_CONNECTIONS 1024u
#define VECTIS_SERVER_DEFAULT_MAX_REQUEST_HEADER_BYTES 65536u
#define VECTIS_SERVER_DEFAULT_MAX_REQUEST_BODY_BYTES 10485760u
#define VECTIS_SERVER_DEFAULT_REQUEST_HEADER_TIMEOUT_MS 5000L
#define VECTIS_SERVER_DEFAULT_REQUEST_BODY_IDLE_TIMEOUT_MS 15000L
#define VECTIS_SERVER_DEFAULT_RESPONSE_WRITE_IDLE_TIMEOUT_MS 15000L
#define VECTIS_SERVER_DEFAULT_REQUEST_BODY_MIN_RATE_BYTES_PER_SEC 1024u
#define VECTIS_SERVER_DEFAULT_REQUEST_BODY_MIN_RATE_GRACE_MS 10000L
#define VECTIS_SERVER_DEFAULT_IDLE_TIMEOUT_MS 30000L
#define VECTIS_SERVER_DEFAULT_KEEPALIVE_TIMEOUT_MS 5000L
#define VECTIS_SERVER_DEFAULT_KEEPALIVE_MAX_REQUESTS 100u
#define VECTIS_BODY_DEFAULT_UPLOAD_MAX_BYTES ((size_t)3221225472UL)
#define VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES 1048576u
#define VECTIS_BODY_DEFAULT_UPLOAD_MIN_RATE_BYTES_PER_SEC 32768u
#define VECTIS_BODY_DEFAULT_UPLOAD_MIN_RATE_GRACE_MS 30000L

#ifdef __cplusplus
extern "C" {
#endif

struct lc_client;
struct lc_lease;
struct lc_source;
struct lc_consumer_service;
struct lc_consumer_service_config;

typedef struct vectis_app vectis_app;
typedef struct vectis_consumer_service vectis_consumer_service;
typedef struct vectis_methods vectis_methods;
typedef struct vectis_request vectis_request;
typedef struct vectis_response vectis_response;
typedef struct vectis_json_response vectis_json_response;

typedef enum vectis_status {
  VECTIS_OK = 0,
  VECTIS_ERR_INVALID = 1,
  VECTIS_ERR_NOMEM = 2,
  VECTIS_ERR_STATE = 3,
  VECTIS_ERR_CONFLICT = 4,
  VECTIS_ERR_NOT_IMPLEMENTED = 5,
  VECTIS_ERR_TIMEOUT = 6
} vectis_status;

typedef enum vectis_tls_mode {
  VECTIS_TLS_MODE_DISABLED = 0,
  VECTIS_TLS_MODE_MANUAL = 1,
  VECTIS_TLS_MODE_ACME = 2
} vectis_tls_mode;

typedef enum vectis_http_method {
  VECTIS_HTTP_ANY = 0,
  VECTIS_HTTP_GET,
  VECTIS_HTTP_POST,
  VECTIS_HTTP_PUT,
  VECTIS_HTTP_PATCH,
  VECTIS_HTTP_DELETE,
  VECTIS_HTTP_HEAD,
  VECTIS_HTTP_OPTIONS
} vectis_http_method;

typedef unsigned int vectis_http_methods;
typedef unsigned int vectis_http_retry_conditions;

#define VECTIS_HTTP_METHOD_MASK(method) (1u << (unsigned)(method))
#define VECTIS_HTTP_METHODS_NONE 0u
#define VECTIS_HTTP_METHODS_GET VECTIS_HTTP_METHOD_MASK(VECTIS_HTTP_GET)
#define VECTIS_HTTP_METHODS_POST VECTIS_HTTP_METHOD_MASK(VECTIS_HTTP_POST)
#define VECTIS_HTTP_METHODS_PUT VECTIS_HTTP_METHOD_MASK(VECTIS_HTTP_PUT)
#define VECTIS_HTTP_METHODS_PATCH VECTIS_HTTP_METHOD_MASK(VECTIS_HTTP_PATCH)
#define VECTIS_HTTP_METHODS_DELETE VECTIS_HTTP_METHOD_MASK(VECTIS_HTTP_DELETE)
#define VECTIS_HTTP_METHODS_HEAD VECTIS_HTTP_METHOD_MASK(VECTIS_HTTP_HEAD)
#define VECTIS_HTTP_METHODS_OPTIONS VECTIS_HTTP_METHOD_MASK(VECTIS_HTTP_OPTIONS)
#define VECTIS_HTTP_METHODS_ALL \
  (VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_POST | VECTIS_HTTP_METHODS_PUT | \
   VECTIS_HTTP_METHODS_PATCH | VECTIS_HTTP_METHODS_DELETE | VECTIS_HTTP_METHODS_HEAD | \
   VECTIS_HTTP_METHODS_OPTIONS)

#define VECTIS_HTTP_RETRY_NONE 0u
#define VECTIS_HTTP_RETRY_TRANSPORT 1u
#define VECTIS_HTTP_RETRY_429 2u
#define VECTIS_HTTP_RETRY_5XX 4u
#define VECTIS_HTTP_RETRY_DEFAULT \
  (VECTIS_HTTP_RETRY_TRANSPORT | VECTIS_HTTP_RETRY_429 | VECTIS_HTTP_RETRY_5XX)

typedef enum vectis_route_path_kind {
  VECTIS_ROUTE_PATH_LITERAL = 0,
  VECTIS_ROUTE_PATH_PARAMS = 1,
  VECTIS_ROUTE_PATH_REGEX = 2
} vectis_route_path_kind;

typedef enum vectis_body_mode {
  VECTIS_BODY_NONE = 0,
  VECTIS_BODY_JSON = 1,
  VECTIS_BODY_BUFFERED = 2,
  VECTIS_BODY_STREAMING_UPLOAD = 3
} vectis_body_mode;

typedef enum vectis_error_source {
  VECTIS_ERROR_SOURCE_NONE = 0,
  VECTIS_ERROR_SOURCE_VECTIS = 1,
  VECTIS_ERROR_SOURCE_KORE = 2,
  VECTIS_ERROR_SOURCE_LOCKDC = 3,
  VECTIS_ERROR_SOURCE_LONEJSON = 4,
  VECTIS_ERROR_SOURCE_PSLOG = 5,
  VECTIS_ERROR_SOURCE_CURL = 6,
  VECTIS_ERROR_SOURCE_OPENSSL = 7,
  VECTIS_ERROR_SOURCE_LIBSSH2 = 8
} vectis_error_source;

typedef struct vectis_error {
  vectis_status code;
  vectis_error_source source;
  long dependency_code;
  long http_status;
  char message[256];
  char detail[256];
} vectis_error;

/*
 * Public configuration structs must be initialized with their matching
 * vectis_*_init() function before use. Boolean fields default to zero; opt-out
 * fields such as *_disabled are used when Vectis enables behavior by default.
 */

typedef vectis_status (*vectis_curl_configure_fn)(CURL *curl,
                                                  void *userdata,
                                                  vectis_error *error);
typedef vectis_status (*vectis_http_response_body_fn)(const void *data,
                                                      size_t size,
                                                      void *userdata,
                                                      vectis_error *error);

typedef struct vectis_bytes {
  const void *data;
  size_t size;
} vectis_bytes;

typedef struct vectis_mutable_bytes {
  void *data;
  size_t size;
} vectis_mutable_bytes;

typedef enum vectis_body_materialized_kind {
  VECTIS_BODY_MATERIALIZED_NONE = 0,
  VECTIS_BODY_MATERIALIZED_MEMORY = 1,
  VECTIS_BODY_MATERIALIZED_FILE = 2
} vectis_body_materialized_kind;

typedef struct vectis_body_materialize_config {
  void *buffer;
  size_t buffer_size;
  size_t memory_limit_bytes;
  const char *directory;
  const char *prefix;
} vectis_body_materialize_config;

typedef struct vectis_body_materialized {
  vectis_body_materialized_kind kind;
  vectis_mutable_bytes memory;
  char *path;
  size_t size;
  int owns_memory;
} vectis_body_materialized;

typedef struct vectis_body_spill_config {
  size_t memory_limit_bytes;
  const char *directory;
  const char *prefix;
} vectis_body_spill_config;

typedef struct vectis_body_spill_result {
  vectis_mutable_bytes memory;
  char *path;
  size_t size;
  int spooled_to_disk;
} vectis_body_spill_result;

typedef struct vectis_dsv_config {
  int delimiter;
  int quote;
  int escape;
  int has_header;
  int strict_row_width;
  int trim_cr;
  const char *const *columns;
  size_t column_count;
  size_t max_field_bytes;
} vectis_dsv_config;

typedef struct vectis_source {
  const char *path;
  struct lc_source *source;
  const void *memory;
  size_t memory_size;
} vectis_source;

typedef struct vectis_lockd_config {
  const char *const *endpoints;
  size_t endpoint_count;
  const char *unix_socket_path;
  vectis_source client_bundle;
  struct lc_source *client_bundle_source;
  const void *client_bundle_pem;
  size_t client_bundle_pem_size;
  const char *client_bundle_path;
  const char *default_namespace;
  long timeout_ms;
  pslog_logger *logger;
  int logger_disabled;
} vectis_lockd_config;

typedef struct vectis_tls_config {
  vectis_tls_mode mode;
  const char *bind;
  unsigned short port;
  const char *domain;
  const char *cert_key_bundle_path;
  vectis_source cert_key_bundle;
  struct lc_source *cert_key_bundle_source;
  const void *cert_key_bundle_pem;
  size_t cert_key_bundle_pem_size;
  const char *certificate_path;
  vectis_source certificate;
  struct lc_source *certificate_source;
  const void *certificate_pem;
  size_t certificate_pem_size;
  const char *private_key_path;
  vectis_source private_key;
  struct lc_source *private_key_source;
  const void *private_key_pem;
  size_t private_key_pem_size;
  const char *ca_bundle_path;
  vectis_source ca_bundle;
  struct lc_source *ca_bundle_source;
  const void *ca_bundle_pem;
  size_t ca_bundle_pem_size;
  const char *client_ca_bundle_path;
  vectis_source client_ca_bundle;
  struct lc_source *client_ca_bundle_source;
  const void *client_ca_bundle_pem;
  size_t client_ca_bundle_pem_size;
  int require_client_certificate;
  const char *acme_email;
  const char *acme_directory_url;
} vectis_tls_config;

typedef vectis_status (*vectis_route_handler_fn)(vectis_app *app,
                                                 vectis_request *request,
                                                 vectis_response *response,
                                                 void *userdata,
                                                 vectis_error *error);

typedef struct vectis_body_policy {
  vectis_body_mode mode;
  size_t max_bytes;
  size_t memory_buffer_limit_bytes;
  int disk_spool_disabled;
  long idle_timeout_ms;
  size_t min_rate_bytes_per_sec;
  long min_rate_grace_ms;
} vectis_body_policy;

typedef struct vectis_route_config {
  vectis_http_method method;
  vectis_http_methods methods;
  const char *path;
  vectis_route_path_kind path_kind;
  vectis_body_policy body;
  vectis_route_handler_fn handler;
  void *userdata;
} vectis_route_config;

typedef struct vectis_static_file_config {
  const char *path;
  const char *file_path;
  const char *content_type;
  vectis_http_methods methods;
} vectis_static_file_config;

typedef struct vectis_static_directory_config {
  const char *path_prefix;
  const char *root_dir;
  const char *content_type;
  const char *index_file;
  vectis_http_methods methods;
} vectis_static_directory_config;

typedef vectis_status (*vectis_json_route_handler_fn)(vectis_app *app,
                                                      vectis_request *request,
                                                      void *input,
                                                      void *output,
                                                      void *userdata,
                                                      vectis_error *error);

typedef vectis_status (*vectis_json_typed_route_handler_fn)(vectis_app *app,
                                                            vectis_request *request,
                                                            void *input,
                                                            vectis_json_response *response,
                                                            void *userdata,
                                                            vectis_error *error);

typedef vectis_status (*vectis_lockd_state_update_fn)(struct lc_lease *lease,
                                                      void *state,
                                                      int *save,
                                                      void *userdata,
                                                      vectis_error *error);

typedef vectis_status (*vectis_dsv_lonejson_row_fn)(void *userdata,
                                                    size_t row_number,
                                                    void *row,
                                                    vectis_error *error);

typedef struct vectis_json_route_config {
  vectis_http_method method;
  vectis_http_methods methods;
  const char *path;
  vectis_route_path_kind path_kind;
  vectis_body_policy body;
  const lonejson_map *input_map;
  size_t input_size;
  const lonejson_map *output_map;
  size_t output_size;
  vectis_json_route_handler_fn handler;
  void *userdata;
} vectis_json_route_config;

typedef struct vectis_json_typed_route_config {
  vectis_http_method method;
  vectis_http_methods methods;
  const char *path;
  vectis_route_path_kind path_kind;
  vectis_body_policy body;
  const lonejson_map *input_map;
  size_t input_size;
  vectis_json_typed_route_handler_fn handler;
  void *userdata;
} vectis_json_typed_route_config;

typedef enum vectis_openapi_format {
  VECTIS_OPENAPI_JSON = 0,
  VECTIS_OPENAPI_YAML = 1
} vectis_openapi_format;

typedef struct vectis_openapi_schema {
  const char *name;
  const lonejson_map *map;
} vectis_openapi_schema;

typedef struct vectis_openapi_response {
  int status_code;
  const char *description;
  vectis_openapi_schema schema;
} vectis_openapi_response;

typedef struct vectis_openapi_route_doc {
  const char *summary;
  const char *operation_id;
  const char *const *tags;
  size_t tag_count;
  vectis_openapi_schema request_schema;
  vectis_openapi_response *responses;
  size_t response_count;
  size_t response_capacity;
} vectis_openapi_route_doc;

typedef struct vectis_openapi_document {
  const char *title;
  const char *version;
} vectis_openapi_document;

typedef struct vectis_server_config {
  size_t max_connections;
  size_t max_request_header_bytes;
  size_t max_request_body_bytes;
  long request_header_timeout_ms;
  long request_body_idle_timeout_ms;
  long response_write_idle_timeout_ms;
  size_t request_body_min_rate_bytes_per_sec;
  long request_body_min_rate_grace_ms;
  long idle_timeout_ms;
  int keepalive_disabled;
  long keepalive_timeout_ms;
  unsigned keepalive_max_requests;
} vectis_server_config;

typedef struct vectis_app_config {
  const char *app_name;
  pslog_logger *logger;
  pslog_mode log_mode;
  pslog_level min_log_level;
  vectis_server_config server;
  vectis_tls_config tls;
  vectis_lockd_config lockd;
} vectis_app_config;

typedef struct vectis_http_client_config {
  const char *base_url;
  vectis_source client_bundle;
  const char *client_bundle_path;
  const void *client_bundle_pem;
  size_t client_bundle_pem_size;
  vectis_source ca_bundle;
  const char *ca_bundle_path;
  long timeout_ms;
  long connect_timeout_ms;
  int follow_redirects_disabled;
  const char *proxy_url;
  long low_speed_limit_bytes_per_sec;
  long low_speed_time_seconds;
  unsigned retry_max_attempts;
  long retry_initial_delay_ms;
  long retry_max_delay_ms;
  vectis_http_retry_conditions retry_conditions;
  pslog_logger *logger;
  vectis_curl_configure_fn configure_curl;
  void *configure_curl_userdata;
} vectis_http_client_config;

typedef struct vectis_http_client vectis_http_client;
struct http_request;

typedef struct vectis_http_request {
  vectis_http_method method;
  const char *url;
  const char *const *headers;
  size_t header_count;
  const void *body;
  size_t body_size;
  const char *body_path;
  const char *content_type;
  const lonejson_map *json_map;
  const void *json_value;
  const char *download_path;
  long timeout_ms;
  const char *proxy_url;
  long low_speed_limit_bytes_per_sec;
  long low_speed_time_seconds;
  unsigned retry_max_attempts;
  long retry_initial_delay_ms;
  long retry_max_delay_ms;
  vectis_http_retry_conditions retry_conditions;
  vectis_http_response_body_fn response_body;
  void *response_body_userdata;
  vectis_curl_configure_fn configure_curl;
  void *configure_curl_userdata;
} vectis_http_request;

typedef struct vectis_http_response {
  long status_code;
  char *content_type;
  void *body;
  size_t body_size;
} vectis_http_response;

typedef struct vectis_sftp_config {
  const char *url;
  const char *username;
  const char *password;
  vectis_source private_key;
  const char *private_key_path;
  const char *known_hosts_path;
  long timeout_ms;
  pslog_logger *logger;
} vectis_sftp_config;

typedef struct vectis_ssh_config {
  const char *host;
  unsigned short port;
  const char *username;
  const char *password;
  vectis_source private_key;
  const char *private_key_path;
  const char *known_hosts_path;
  long timeout_ms;
  pslog_logger *logger;
} vectis_ssh_config;

typedef struct vectis_ssh_exec_result {
  int exit_status;
  char *stdout_data;
  size_t stdout_size;
  char *stderr_data;
  size_t stderr_size;
} vectis_ssh_exec_result;

typedef struct vectis_mqtt_config {
  const char *broker_url;
  const char *username;
  const char *password;
  vectis_source client_bundle;
  const char *client_bundle_path;
  const void *client_bundle_pem;
  size_t client_bundle_pem_size;
  vectis_source ca_bundle;
  const char *ca_bundle_path;
  long timeout_ms;
  pslog_logger *logger;
} vectis_mqtt_config;

typedef struct vectis_cert_subject {
  const char *common_name;
  const char *organization;
  const char *organizational_unit;
  const char *country;
  const char *state;
  const char *locality;
} vectis_cert_subject;

typedef struct vectis_cert_bundle_config {
  vectis_cert_subject subject;
  const char *dns_names;
  const char *ip_addresses;
  const char *ca_cert_path;
  const char *ca_key_path;
  int is_ca;
  const char *output_bundle_path;
  const char *output_cert_path;
  const char *output_key_path;
  unsigned key_bits;
  long valid_days;
} vectis_cert_bundle_config;

typedef struct vectis_private_key_config {
  const char *output_key_path;
  unsigned key_bits;
} vectis_private_key_config;

typedef struct vectis_csr_config {
  vectis_cert_subject subject;
  const char *dns_names;
  const char *ip_addresses;
  vectis_source private_key;
  const char *private_key_path;
  const char *output_csr_path;
} vectis_csr_config;

struct vectis_methods {
  void (*destroy)(vectis_app *app);
  vectis_status (*start)(vectis_app *app, vectis_error *error);
  vectis_status (*stop)(vectis_app *app, vectis_error *error);
  vectis_status (*register_route)(vectis_app *app,
                                  const vectis_route_config *route,
                                  vectis_error *error);
  size_t (*route_count)(const vectis_app *app);
  pslog_logger *(*logger)(vectis_app *app);
};

struct vectis_app {
  const vectis_methods *vt;
  void *impl;
};

void vectis_error_clear(vectis_error *error);
const char *vectis_status_string(vectis_status status);
const char *vectis_error_source_string(vectis_error_source source);
const char *vectis_http_method_string(vectis_http_method method);
const char *vectis_body_mode_string(vectis_body_mode mode);
void vectis_source_init(vectis_source *source);
vectis_source vectis_source_from_path(const char *path);
vectis_source vectis_source_from_memory(const void *memory, size_t memory_size);
vectis_source vectis_source_from_lc(struct lc_source *source);
void vectis_app_config_init(vectis_app_config *config);
void vectis_server_config_init(vectis_server_config *config);
void vectis_tls_config_init(vectis_tls_config *config);
void vectis_lockd_config_init(vectis_lockd_config *config);
void vectis_body_policy_init(vectis_body_policy *policy);
vectis_body_policy vectis_body_none(void);
vectis_body_policy vectis_body_json_default(void);
vectis_body_policy vectis_body_buffered_max(size_t max_bytes);
vectis_body_policy vectis_body_upload(void);
vectis_body_policy vectis_body_upload_max(size_t max_bytes);
void vectis_route_config_init(vectis_route_config *config);
void vectis_json_route_config_init(vectis_json_route_config *config);
void vectis_json_typed_route_config_init(vectis_json_typed_route_config *config);
void vectis_openapi_document_init(vectis_openapi_document *document);
void vectis_openapi_route_doc_init(vectis_openapi_route_doc *doc);
void vectis_openapi_route_doc_cleanup(vectis_openapi_route_doc *doc);
vectis_openapi_schema vectis_openapi_lonejson_schema(const char *name,
                                                     const lonejson_map *map);
vectis_status vectis_openapi_request_json(vectis_openapi_route_doc *doc,
                                          vectis_openapi_schema schema,
                                          vectis_error *error);
vectis_status vectis_openapi_response_json(vectis_openapi_route_doc *doc,
                                           int status_code,
                                           const char *description,
                                           vectis_openapi_schema schema,
                                           vectis_error *error);
vectis_route_config vectis_route(vectis_http_method method,
                                 const char *path,
                                 vectis_route_handler_fn handler,
                                 void *userdata);
vectis_route_config vectis_route_methods(vectis_http_methods methods,
                                         const char *path,
                                         vectis_route_handler_fn handler,
                                         void *userdata);
vectis_route_config vectis_json_body_route(vectis_http_method method,
                                           const char *path,
                                           vectis_route_handler_fn handler,
                                           void *userdata);
vectis_route_config vectis_json_body_route_methods(vectis_http_methods methods,
                                                   const char *path,
                                                   vectis_route_handler_fn handler,
                                                   void *userdata);
vectis_route_config vectis_route_regex(vectis_http_method method,
                                       const char *pattern,
                                       vectis_route_handler_fn handler,
                                       void *userdata);
vectis_route_config vectis_route_regex_methods(vectis_http_methods methods,
                                               const char *pattern,
                                               vectis_route_handler_fn handler,
                                               void *userdata);
vectis_route_config vectis_upload_route(vectis_http_method method,
                                        const char *path,
                                        vectis_route_handler_fn handler,
                                        void *userdata);
vectis_route_config vectis_upload_route_methods(vectis_http_methods methods,
                                                const char *path,
                                                vectis_route_handler_fn handler,
                                                void *userdata);
vectis_route_config vectis_upload_route_max(vectis_http_method method,
                                            const char *path,
                                            size_t max_bytes,
                                            vectis_route_handler_fn handler,
                                            void *userdata);
vectis_route_config vectis_upload_route_max_methods(vectis_http_methods methods,
                                                    const char *path,
                                                    size_t max_bytes,
                                                    vectis_route_handler_fn handler,
                                                    void *userdata);
vectis_json_route_config vectis_json_route(vectis_http_method method,
                                           const char *path,
                                           const lonejson_map *input_map,
                                           size_t input_size,
                                           const lonejson_map *output_map,
                                           size_t output_size,
                                           vectis_json_route_handler_fn handler,
                                           void *userdata);
vectis_json_route_config vectis_json_route_methods(vectis_http_methods methods,
                                                   const char *path,
                                                   const lonejson_map *input_map,
                                                   size_t input_size,
                                                   const lonejson_map *output_map,
                                                   size_t output_size,
                                                   vectis_json_route_handler_fn handler,
                                                   void *userdata);
vectis_json_typed_route_config vectis_json_typed_route(vectis_http_method method,
                                                       const char *path,
                                                       const lonejson_map *input_map,
                                                       size_t input_size,
                                                       vectis_json_typed_route_handler_fn handler,
                                                       void *userdata);
vectis_json_typed_route_config vectis_json_typed_route_methods(vectis_http_methods methods,
                                                               const char *path,
                                                               const lonejson_map *input_map,
                                                               size_t input_size,
                                                               vectis_json_typed_route_handler_fn handler,
                                                               void *userdata);
void vectis_static_file_config_init(vectis_static_file_config *config);
void vectis_static_directory_config_init(vectis_static_directory_config *config);

vectis_app *vectis_new(const vectis_app_config *config, vectis_error *error);
void vectis_destroy(vectis_app *app);
vectis_status vectis_start(vectis_app *app, vectis_error *error);
vectis_status vectis_stop(vectis_app *app, vectis_error *error);
vectis_status vectis_register_route(vectis_app *app,
                                    const vectis_route_config *route,
                                    vectis_error *error);
vectis_status vectis_register_json_route(vectis_app *app,
                                         const vectis_json_route_config *route,
                                         vectis_error *error);
vectis_status vectis_register_json_typed_route(vectis_app *app,
                                               const vectis_json_typed_route_config *route,
                                               vectis_error *error);
vectis_status vectis_register_prefixed_route(vectis_app *app,
                                             const char *prefix,
                                             const vectis_route_config *route,
                                             vectis_error *error);
vectis_status vectis_register_prefixed_json_route(vectis_app *app,
                                                  const char *prefix,
                                                  const vectis_json_route_config *route,
                                                  vectis_error *error);
vectis_status vectis_register_prefixed_json_typed_route(vectis_app *app,
                                                        const char *prefix,
                                                        const vectis_json_typed_route_config *route,
                                                        vectis_error *error);
vectis_status vectis_register_static_file(vectis_app *app,
                                          const vectis_static_file_config *config,
                                          vectis_error *error);
vectis_status vectis_register_static_directory(vectis_app *app,
                                               const vectis_static_directory_config *config,
                                               vectis_error *error);
vectis_status vectis_attach_openapi_doc(vectis_app *app,
                                        vectis_http_methods methods,
                                        const char *path,
                                        const vectis_openapi_route_doc *doc,
                                        vectis_error *error);
vectis_status vectis_generate_openapi(vectis_app *app,
                                      const vectis_openapi_document *document,
                                      vectis_openapi_format format,
                                      vectis_mutable_bytes *out,
                                      vectis_error *error);
size_t vectis_route_count(const vectis_app *app);
pslog_logger *vectis_logger(vectis_app *app);
/* Returns the app-owned lockd client after successful runtime startup.
 * Route handlers on a started lockd-configured app can treat this as present.
 * NULL is reserved for invalid apps or pre-start lifecycle inspection.
 */
struct lc_client *vectis_lockd_client(vectis_app *app);
vectis_status vectis_consumer_service_new(vectis_app *app,
                                          const struct lc_consumer_service_config *config,
                                          vectis_consumer_service **out,
                                          vectis_error *error);
struct lc_consumer_service *vectis_consumer_service_raw(vectis_consumer_service *service);
vectis_status vectis_consumer_service_run(vectis_consumer_service *service,
                                          vectis_error *error);
vectis_status vectis_consumer_service_start(vectis_consumer_service *service,
                                            vectis_error *error);
vectis_status vectis_consumer_service_stop(vectis_consumer_service *service,
                                           vectis_error *error);
vectis_status vectis_consumer_service_wait(vectis_consumer_service *service,
                                           vectis_error *error);
vectis_status vectis_consumer_service_run_until(vectis_consumer_service *service,
                                                const volatile int *done,
                                                long timeout_ms,
                                                vectis_error *error);
void vectis_consumer_service_destroy(vectis_consumer_service *service);
vectis_status vectis_json_validate_cstr(const char *json, vectis_error *error);
void vectis_dsv_config_init(vectis_dsv_config *config);
vectis_dsv_config vectis_dsv_csv(void);
vectis_dsv_config vectis_dsv_tsv(void);
vectis_status vectis_dsv_parse_lonejson(struct lc_source *source,
                                        const lonejson_map *map,
                                        const vectis_dsv_config *config,
                                        vectis_dsv_lonejson_row_fn row,
                                        void *userdata,
                                        vectis_error *error);
vectis_status vectis_dsv_parse_lonejson_source(const vectis_source *source,
                                               const lonejson_map *map,
                                               const vectis_dsv_config *config,
                                               vectis_dsv_lonejson_row_fn row,
                                               void *userdata,
                                               vectis_error *error);
vectis_status vectis_dsv_to_json_array(struct lc_source *source,
                                       const vectis_dsv_config *config,
                                       vectis_mutable_bytes *out,
                                       vectis_error *error);
vectis_status vectis_dsv_source_to_json_array(const vectis_source *source,
                                             const vectis_dsv_config *config,
                                             vectis_mutable_bytes *out,
                                             vectis_error *error);
vectis_status vectis_dsv_to_lonejson_array(struct lc_source *source,
                                           const lonejson_map *map,
                                           const vectis_dsv_config *config,
                                           vectis_mutable_bytes *out,
                                           vectis_error *error);
vectis_status vectis_dsv_source_to_lonejson_array(const vectis_source *source,
                                                 const lonejson_map *map,
                                                 const vectis_dsv_config *config,
                                                 vectis_mutable_bytes *out,
                                                 vectis_error *error);
vectis_status vectis_format_key(char *out,
                                size_t out_size,
                                vectis_error *error,
                                const char *format,
                                ...);
vectis_status vectis_lockd_state_load(struct lc_client *client,
                                      const char *key,
                                      const char *owner,
                                      long ttl_seconds,
                                      const lonejson_map *map,
                                      void *out,
                                      vectis_error *error);
vectis_status vectis_lockd_state_save(struct lc_client *client,
                                      const char *key,
                                      const char *owner,
                                      long ttl_seconds,
                                      const lonejson_map *map,
                                      const void *value,
                                      vectis_error *error);
vectis_status vectis_lockd_state_update(struct lc_client *client,
                                        const char *key,
                                        const char *owner,
                                        long ttl_seconds,
                                        const lonejson_map *map,
                                        void *state,
                                        vectis_lockd_state_update_fn update,
                                        void *userdata,
                                        vectis_error *error);

vectis_status vectis_request_json_into(vectis_request *request,
                                       const lonejson_map *map,
                                       void *out,
                                       vectis_error *error);
vectis_http_method vectis_request_method(vectis_request *request);
const char *vectis_request_path(vectis_request *request);
const char *vectis_request_path_param(vectis_request *request,
                                      const char *name);
const char *vectis_request_query(vectis_request *request,
                                 const char *name);
const char *vectis_request_header(vectis_request *request,
                                  const char *name);
struct http_request *vectis_request_kore(vectis_request *request);
struct lc_source *vectis_request_body_reader(vectis_request *request);
void vectis_body_materialize_config_init(vectis_body_materialize_config *config);
void vectis_body_materialized_cleanup(vectis_body_materialized *body);
vectis_status vectis_body_materialized_open_reader(const vectis_body_materialized *body,
                                                   struct lc_source **out,
                                                   vectis_error *error);
vectis_status vectis_request_body_materialize(vectis_request *request,
                                              const vectis_body_materialize_config *config,
                                              vectis_body_materialized *out,
                                              vectis_error *error);
void vectis_body_spill_config_init(vectis_body_spill_config *config);
void vectis_body_spill_result_cleanup(vectis_body_spill_result *result);
vectis_status vectis_request_body_read_all(vectis_request *request,
                                           vectis_mutable_bytes *out,
                                           vectis_error *error);
vectis_status vectis_request_body_spill(vectis_request *request,
                                        const vectis_body_spill_config *config,
                                        vectis_body_spill_result *out,
                                        vectis_error *error);
vectis_status vectis_request_body_bytes(vectis_request *request,
                                        vectis_bytes *out,
                                        vectis_error *error);
vectis_status vectis_request_body_copy(vectis_request *request,
                                       vectis_mutable_bytes *out,
                                       vectis_error *error);
void vectis_mutable_bytes_cleanup(vectis_mutable_bytes *bytes);
const char *vectis_request_body_path(vectis_request *request);
int vectis_request_body_is_spooled(vectis_request *request);
vectis_status vectis_response_status(vectis_response *response,
                                     int status_code,
                                     vectis_error *error);
vectis_status vectis_response_header(vectis_response *response,
                                     const char *name,
                                     const char *value,
                                     vectis_error *error);
vectis_status vectis_response_text(vectis_response *response,
                                   int status_code,
                                   const char *content_type,
                                   const char *text,
                                   vectis_error *error);
vectis_status vectis_response_bytes(vectis_response *response,
                                    int status_code,
                                    const char *content_type,
                                    vectis_bytes body,
                                    vectis_error *error);
vectis_status vectis_response_file(vectis_response *response,
                                   int status_code,
                                   const char *content_type,
                                   const char *path,
                                   vectis_error *error);
vectis_status vectis_response_source(vectis_response *response,
                                     int status_code,
                                     const char *content_type,
                                     struct lc_source *source,
                                     vectis_error *error);
vectis_status vectis_response_json(vectis_response *response,
                                   int status_code,
                                   const lonejson_map *map,
                                   const void *value,
                                   vectis_error *error);
vectis_status vectis_response_json_generated(vectis_response *response,
                                             int status_code,
                                             const lonejson_map *map,
                                             const void *value,
                                             vectis_error *error);
vectis_status vectis_json_reply(vectis_json_response *response,
                                int status_code,
                                const lonejson_map *map,
                                const void *value,
                                vectis_error *error);
vectis_status vectis_json_reply_status(vectis_json_response *response,
                                       int status_code,
                                       vectis_error *error);
vectis_status vectis_response_error_json(vectis_response *response,
                                         int status_code,
                                         const char *code,
                                         const char *message,
                                         const char *detail,
                                         vectis_error *error);

void vectis_http_client_config_init(vectis_http_client_config *config);
vectis_status vectis_http_client_new(const vectis_http_client_config *config,
                                     vectis_http_client **out,
                                     vectis_error *error);
vectis_status vectis_http_client_from_app(vectis_app *app,
                                          const vectis_http_client_config *config,
                                          vectis_http_client **out,
                                          vectis_error *error);
void vectis_http_client_destroy(vectis_http_client *client);
void vectis_http_request_init(vectis_http_request *request);
void vectis_http_response_cleanup(vectis_http_response *response);
vectis_status vectis_http_response_json_into(const vectis_http_response *response,
                                             const lonejson_map *map,
                                             void *out,
                                             vectis_error *error);
vectis_status vectis_http_client_execute(vectis_http_client *client,
                                         const vectis_http_request *request,
                                         vectis_http_response *response,
                                         vectis_error *error);
vectis_status vectis_http_client_get(vectis_http_client *client,
                                     const char *url,
                                     vectis_http_response *response,
                                     vectis_error *error);
vectis_status vectis_http_client_delete(vectis_http_client *client,
                                        const char *url,
                                        vectis_http_response *response,
                                        vectis_error *error);
vectis_status vectis_http_client_head(vectis_http_client *client,
                                      const char *url,
                                      vectis_http_response *response,
                                      vectis_error *error);
vectis_status vectis_http_client_options(vectis_http_client *client,
                                         const char *url,
                                         vectis_http_response *response,
                                         vectis_error *error);
vectis_status vectis_http_client_download_file(vectis_http_client *client,
                                               const char *url,
                                               const char *local_path,
                                               vectis_http_response *response,
                                               vectis_error *error);
vectis_status vectis_http_client_upload_file(vectis_http_client *client,
                                             vectis_http_method method,
                                             const char *url,
                                             const char *local_path,
                                             const char *content_type,
                                             vectis_http_response *response,
                                             vectis_error *error);
vectis_status vectis_http_client_post_json(vectis_http_client *client,
                                           const char *url,
                                           const lonejson_map *map,
                                           const void *value,
                                           vectis_http_response *response,
                                           vectis_error *error);
vectis_status vectis_http_client_put_json(vectis_http_client *client,
                                          const char *url,
                                          const lonejson_map *map,
                                          const void *value,
                                          vectis_http_response *response,
                                          vectis_error *error);
vectis_status vectis_http_client_patch_json(vectis_http_client *client,
                                            const char *url,
                                            const lonejson_map *map,
                                            const void *value,
                                            vectis_http_response *response,
                                            vectis_error *error);
vectis_status vectis_http_execute(const vectis_http_client_config *client,
                                  const vectis_http_request *request,
                                  vectis_http_response *response,
                                  vectis_error *error);
vectis_status vectis_http_get(const vectis_http_client_config *client,
                              const char *url,
                              vectis_http_response *response,
                              vectis_error *error);
vectis_status vectis_http_delete(const vectis_http_client_config *client,
                                 const char *url,
                                 vectis_http_response *response,
                                 vectis_error *error);
vectis_status vectis_http_head(const vectis_http_client_config *client,
                               const char *url,
                               vectis_http_response *response,
                               vectis_error *error);
vectis_status vectis_http_options(const vectis_http_client_config *client,
                                  const char *url,
                                  vectis_http_response *response,
                                  vectis_error *error);
vectis_status vectis_http_download_file(const vectis_http_client_config *client,
                                        const char *url,
                                        const char *local_path,
                                        vectis_http_response *response,
                                        vectis_error *error);
vectis_status vectis_http_upload_file(const vectis_http_client_config *client,
                                      vectis_http_method method,
                                      const char *url,
                                      const char *local_path,
                                      const char *content_type,
                                      vectis_http_response *response,
                                      vectis_error *error);
vectis_status vectis_http_post_json(const vectis_http_client_config *client,
                                    const char *url,
                                    const lonejson_map *map,
                                    const void *value,
                                    vectis_http_response *response,
                                    vectis_error *error);
vectis_status vectis_http_put_json(const vectis_http_client_config *client,
                                   const char *url,
                                   const lonejson_map *map,
                                   const void *value,
                                   vectis_http_response *response,
                                   vectis_error *error);
vectis_status vectis_http_patch_json(const vectis_http_client_config *client,
                                     const char *url,
                                     const lonejson_map *map,
                                     const void *value,
                                     vectis_http_response *response,
                                     vectis_error *error);

void vectis_sftp_config_init(vectis_sftp_config *config);
vectis_status vectis_sftp_upload_file(const vectis_sftp_config *config,
                                      const char *local_path,
                                      const char *remote_path,
                                      vectis_error *error);
vectis_status vectis_sftp_download_file(const vectis_sftp_config *config,
                                        const char *remote_path,
                                        const char *local_path,
                                        vectis_error *error);

void vectis_ssh_config_init(vectis_ssh_config *config);
void vectis_ssh_exec_result_cleanup(vectis_ssh_exec_result *result);
vectis_status vectis_ssh_exec(const vectis_ssh_config *config,
                              const char *command,
                              vectis_ssh_exec_result *result,
                              vectis_error *error);
vectis_status vectis_ssh_sftp_upload_file(const vectis_ssh_config *config,
                                          const char *local_path,
                                          const char *remote_path,
                                          vectis_error *error);
vectis_status vectis_ssh_sftp_download_file(const vectis_ssh_config *config,
                                            const char *remote_path,
                                            const char *local_path,
                                            vectis_error *error);

void vectis_mqtt_config_init(vectis_mqtt_config *config);
vectis_status vectis_mqtt_publish(const vectis_mqtt_config *config,
                                  const char *topic,
                                  const void *payload,
                                  size_t payload_size,
                                  const char *content_type,
                                  vectis_error *error);
vectis_status vectis_mqtt_publish_json(const vectis_mqtt_config *config,
                                       const char *topic,
                                       const lonejson_map *map,
                                       const void *value,
                                       vectis_error *error);

void vectis_cert_subject_init(vectis_cert_subject *subject);
void vectis_cert_bundle_config_init(vectis_cert_bundle_config *config);
void vectis_private_key_config_init(vectis_private_key_config *config);
void vectis_csr_config_init(vectis_csr_config *config);
vectis_status vectis_cert_generate_private_key(const vectis_private_key_config *config,
                                               vectis_error *error);
vectis_status vectis_cert_generate_csr(const vectis_csr_config *config,
                                       vectis_error *error);
vectis_status vectis_cert_generate_bundle(const vectis_cert_bundle_config *config,
                                          vectis_error *error);
vectis_status vectis_cert_validate_bundle(const vectis_source *bundle,
                                          vectis_error *error);
vectis_status vectis_cert_validate_pair(const vectis_source *certificate,
                                        const vectis_source *private_key,
                                        const vectis_source *ca_bundle,
                                        vectis_error *error);

#ifdef __cplusplus
}
#endif

#endif
