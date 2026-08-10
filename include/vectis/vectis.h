#ifndef VECTIS_VECTIS_H
#define VECTIS_VECTIS_H

#include <curl/curl.h>
#include <stddef.h>
#ifndef LONEJSON_WITH_CURL
#define LONEJSON_WITH_CURL 1
#endif
#include <lonejson.h>
#include <pslog.h>

#define VECTIS_ACME_DIRECTORY_LETSENCRYPT_PRODUCTION                           \
  "https://acme-v02.api.letsencrypt.org/directory"
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
#define VECTIS_BODY_DEFAULT_MEMORY_BUFFER_LIMIT_BYTES 262144u
#define VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES                          \
  VECTIS_BODY_DEFAULT_MEMORY_BUFFER_LIMIT_BYTES

#ifdef __cplusplus
extern "C" {
#endif

struct lc_client;
struct lc_lease;
struct lc_sink;
struct lc_source;
struct lc_consumer_service;
struct lc_consumer_service_config;

typedef struct vectis_app vectis_app;
typedef struct vectis_consumer_service vectis_consumer_service;
typedef struct vectis_http_client vectis_http_client;
typedef struct vectis_sftp vectis_sftp;
typedef struct vectis_ssh vectis_ssh;
typedef struct vectis_mqtt vectis_mqtt;
typedef struct vectis_request vectis_request;
typedef struct vectis_response vectis_response;
typedef struct vectis_json_response vectis_json_response;
typedef struct vectis_dsv_rows vectis_dsv_rows;

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
#define VECTIS_HTTP_METHODS_ALL                                                \
  (VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_POST |                        \
   VECTIS_HTTP_METHODS_PUT | VECTIS_HTTP_METHODS_PATCH |                       \
   VECTIS_HTTP_METHODS_DELETE | VECTIS_HTTP_METHODS_HEAD |                     \
   VECTIS_HTTP_METHODS_OPTIONS)

#define VECTIS_HTTP_RETRY_NONE 0u
#define VECTIS_HTTP_RETRY_TRANSPORT 1u
#define VECTIS_HTTP_RETRY_429 2u
#define VECTIS_HTTP_RETRY_5XX 4u
#define VECTIS_HTTP_RETRY_DEFAULT                                              \
  (VECTIS_HTTP_RETRY_TRANSPORT | VECTIS_HTTP_RETRY_429 | VECTIS_HTTP_RETRY_5XX)
#define VECTIS_HTTP_RETRY_INHERIT ((vectis_http_retry_conditions)~0u)

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
 * vectis_*_init() function before use. NULL pointers and zero-valued scalar
 * config fields mean "use the Vectis default" unless the field is a named
 * enum/bitmask selector whose zero value is itself a documented option.
 *
 * Boolean fields default to zero. When Vectis enables behavior by default, the
 * public field is phrased as an opt-out such as *_disabled so zero still means
 * the default enabled behavior.
 */

typedef vectis_status (*vectis_curl_configure_fn)(CURL *curl, void *userdata,
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
  int header_disabled;
  int strict_row_width_disabled;
  int trim_cr_disabled;
  int indented_comments_disabled;
  const char *comment_prefix;
  const char *const *columns;
  size_t column_count;
  size_t max_field_bytes;
} vectis_dsv_config;

typedef struct vectis_xml_config {
  const char *root_element;
  const char *text_key;
  const char *attribute_prefix;
  int trim_text;
  int skip_unknown_disabled;
  size_t max_depth;
  size_t max_text_bytes;
} vectis_xml_config;

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

typedef vectis_status (*vectis_upload_open_fn)(vectis_app *app,
                                               vectis_request *request,
                                               void *userdata, void **state,
                                               vectis_error *error);
typedef vectis_status (*vectis_upload_write_fn)(vectis_app *app,
                                                vectis_request *request,
                                                const void *data, size_t size,
                                                void *state, void *userdata,
                                                vectis_error *error);
typedef vectis_status (*vectis_upload_finish_fn)(vectis_app *app,
                                                 vectis_request *request,
                                                 vectis_response *response,
                                                 void *state, void *userdata,
                                                 vectis_error *error);
typedef void (*vectis_upload_close_fn)(vectis_app *app, vectis_request *request,
                                       void *state, void *userdata);

/**
 * Handler for a live streaming upload reader.
 *
 * `reader` is borrowed and valid only for the duration of the callback. Read
 * from it until EOF; Vectis applies bounded in-memory backpressure while the
 * transport feeds request chunks. Do not close or reset the borrowed reader.
 */
typedef vectis_status (*vectis_upload_reader_handler_fn)(
    vectis_app *app, vectis_request *request, struct lc_source *reader,
    vectis_response *response, void *userdata, vectis_error *error);

/**
 * Callback invoked for each item decoded from a selected JSON array.
 *
 * `index` is zero-based within the selected array. `item` points at
 * caller-owned storage initialized and populated through the supplied
 * `lonejson_map`; the pointer is valid only for the duration of the callback
 * and is reused for the next element. Return `VECTIS_OK` to continue or any
 * other `vectis_status` to stop streaming and propagate that status to the
 * caller.
 */
typedef vectis_status (*vectis_json_array_item_fn)(void *userdata, size_t index,
                                                   void *item,
                                                   vectis_error *error);

typedef struct vectis_body_policy {
  vectis_body_mode mode;
  size_t max_bytes;
  size_t memory_buffer_limit_bytes;
  int disk_spool_disabled;
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

typedef struct vectis_upload_route_config {
  vectis_http_method method;
  vectis_http_methods methods;
  const char *path;
  vectis_route_path_kind path_kind;
  vectis_body_policy body;
  vectis_upload_open_fn open;
  vectis_upload_write_fn write;
  vectis_upload_finish_fn finish;
  vectis_upload_close_fn close;
  void *userdata;
} vectis_upload_route_config;

typedef struct vectis_upload_file_route_config {
  vectis_http_method method;
  vectis_http_methods methods;
  const char *path;
  vectis_route_path_kind path_kind;
  vectis_body_policy body;
  const char *file_path;
  const char *content_type;
} vectis_upload_file_route_config;

typedef struct vectis_upload_reader_route_config {
  vectis_http_method method;
  vectis_http_methods methods;
  const char *path;
  vectis_route_path_kind path_kind;
  vectis_body_policy body;
  size_t buffer_bytes;
  vectis_upload_reader_handler_fn handler;
  void *userdata;
} vectis_upload_reader_route_config;

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
                                                      void *input, void *output,
                                                      void *userdata,
                                                      vectis_error *error);

typedef vectis_status (*vectis_json_typed_route_handler_fn)(
    vectis_app *app, vectis_request *request, void *input,
    vectis_json_response *response, void *userdata, vectis_error *error);

typedef vectis_status (*vectis_xml_route_handler_fn)(
    vectis_app *app, vectis_request *request, void *input,
    vectis_response *response, void *userdata, vectis_error *error);

typedef vectis_status (*vectis_dsv_route_handler_fn)(
    vectis_app *app, vectis_request *request, vectis_dsv_rows *rows,
    vectis_response *response, void *userdata, vectis_error *error);

typedef vectis_status (*vectis_lockd_state_update_fn)(struct lc_lease *lease,
                                                      void *state, int *save,
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

typedef struct vectis_xml_route_config {
  vectis_http_method method;
  vectis_http_methods methods;
  const char *path;
  vectis_route_path_kind path_kind;
  vectis_body_policy body;
  size_t buffer_bytes;
  const lonejson_map *input_map;
  size_t input_size;
  vectis_xml_config config;
  vectis_xml_route_handler_fn handler;
  void *userdata;
} vectis_xml_route_config;

typedef struct vectis_dsv_route_config {
  vectis_http_method method;
  vectis_http_methods methods;
  const char *path;
  vectis_route_path_kind path_kind;
  vectis_body_policy body;
  size_t buffer_bytes;
  const lonejson_map *row_map;
  size_t row_size;
  vectis_dsv_config config;
  vectis_dsv_route_handler_fn handler;
  void *userdata;
} vectis_dsv_route_config;

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

/*
 * Stateful Vectis SDK handles expose direct function-pointer method semantics
 * with an explicit `self` argument, mirroring liblockdc.
 *
 * The handle methods are the primary DX for stateful/resource-owning APIs. The
 * free functions below remain public as constructors, stateless builders,
 * cleanup helpers, and lower-level levers for callers that need them.
 *
 * `impl` is private and must not be read or written by applications. Public
 * config fields on non-app handles are shallow effective config copies:
 * borrowed strings, sources, loggers, maps, and callback contexts must outlive
 * the handle unless a specific API documents a deep copy.
 *
 * Constructors clear `*out` before validation so failed construction never
 * leaves a stale handle pointer behind. Close functions accept NULL.
 */
struct vectis_app {
  /* Start or stop the app runtime. Apps with routes start Kore; lockd-only apps
   * open the configured lockd client without starting Kore.
   */
  vectis_status (*start)(vectis_app *self, vectis_error *error);
  vectis_status (*stop)(vectis_app *self, vectis_error *error);

  /* Register HTTP routes. Builder helpers such as vectis_route(),
   * vectis_json_route(), and vectis_upload_route() are intentionally still free
   * functions because they produce value configs, not owned handles.
   */
  vectis_status (*route)(vectis_app *self, const vectis_route_config *route,
                         vectis_error *error);
  vectis_status (*json_route)(vectis_app *self,
                              const vectis_json_route_config *route,
                              vectis_error *error);
  vectis_status (*json_typed_route)(vectis_app *self,
                                    const vectis_json_typed_route_config *route,
                                    vectis_error *error);
  vectis_status (*xml_route)(vectis_app *self,
                             const vectis_xml_route_config *route,
                             vectis_error *error);
  vectis_status (*dsv_route)(vectis_app *self,
                             const vectis_dsv_route_config *route,
                             vectis_error *error);
  vectis_status (*upload_stream)(vectis_app *self,
                                 const vectis_upload_route_config *route,
                                 vectis_error *error);
  vectis_status (*upload_file)(vectis_app *self,
                               const vectis_upload_file_route_config *route,
                               vectis_error *error);
  vectis_status (*upload_reader)(vectis_app *self,
                                 const vectis_upload_reader_route_config *route,
                                 vectis_error *error);
  vectis_status (*prefixed_route)(vectis_app *self, const char *prefix,
                                  const vectis_route_config *route,
                                  vectis_error *error);
  vectis_status (*prefixed_json_route)(vectis_app *self, const char *prefix,
                                       const vectis_json_route_config *route,
                                       vectis_error *error);
  vectis_status (*prefixed_json_typed_route)(
      vectis_app *self, const char *prefix,
      const vectis_json_typed_route_config *route, vectis_error *error);
  vectis_status (*prefixed_xml_route)(vectis_app *self, const char *prefix,
                                      const vectis_xml_route_config *route,
                                      vectis_error *error);
  vectis_status (*prefixed_dsv_route)(vectis_app *self, const char *prefix,
                                      const vectis_dsv_route_config *route,
                                      vectis_error *error);
  vectis_status (*static_file)(vectis_app *self,
                               const vectis_static_file_config *config,
                               vectis_error *error);
  vectis_status (*static_directory)(
      vectis_app *self, const vectis_static_directory_config *config,
      vectis_error *error);

  /* Attach per-route OpenAPI metadata or generate an OpenAPI document from the
   * current route registry. Generation writes to `out`; callers clean it with
   * vectis_mutable_bytes_cleanup().
   */
  vectis_status (*openapi_doc)(vectis_app *self, vectis_http_methods methods,
                               const char *path,
                               const vectis_openapi_route_doc *doc,
                               vectis_error *error);
  vectis_status (*openapi)(vectis_app *self,
                           const vectis_openapi_document *document,
                           vectis_openapi_format format,
                           vectis_mutable_bytes *out, vectis_error *error);
  size_t (*route_count)(const vectis_app *self);
  pslog_logger *(*logger)(vectis_app *self);

  /* Returns the app-owned process-local lockd client when lockd is configured
   * and open in the current process. Route handlers on a started
   * lockd-configured app can treat this as present.
   */
  struct lc_client *(*lockd_client)(vectis_app *self);

  /* Create a Vectis-owned wrapper around a liblockdc consumer service. The
   * lc_consumer_service_config and its callbacks remain caller-owned.
   */
  vectis_status (*consumer_service)(
      vectis_app *self, const struct lc_consumer_service_config *config,
      vectis_consumer_service **out, vectis_error *error);
  void (*close)(vectis_app *self);
  void *impl;
};

struct vectis_consumer_service {
  /* Escape hatch for APIs not covered by the Vectis facade. The returned raw
   * service remains owned by this handle.
   */
  struct lc_consumer_service *(*raw)(vectis_consumer_service *self);
  vectis_status (*run)(vectis_consumer_service *self, vectis_error *error);
  vectis_status (*start)(vectis_consumer_service *self, vectis_error *error);
  vectis_status (*stop)(vectis_consumer_service *self, vectis_error *error);
  vectis_status (*wait)(vectis_consumer_service *self, vectis_error *error);
  vectis_status (*run_until)(vectis_consumer_service *self,
                             const volatile int *done, long timeout_ms,
                             vectis_error *error);
  void (*close)(vectis_consumer_service *self);
  void *impl;
};

struct vectis_http_client {
  /* Execute a fully specified request. Convenience methods below construct a
   * request and delegate here.
   */
  vectis_status (*execute)(vectis_http_client *self,
                           const vectis_http_request *request,
                           vectis_http_response *response, vectis_error *error);
  vectis_status (*get)(vectis_http_client *self, const char *url,
                       vectis_http_response *response, vectis_error *error);
  /* Stream a selected JSON array from an HTTP GET response body. The response
   * body is consumed incrementally and is not materialized in `response->body`.
   */
  vectis_status (*get_json_array)(
      vectis_http_client *self, const char *url, const char *array_path,
      const lonejson_map *map, void *item, vectis_json_array_item_fn callback,
      void *userdata, vectis_http_response *response, vectis_error *error);
  /* HTTP DELETE. Named `del` so vectis.h remains usable from C++ translation
   * units where `delete` is a keyword.
   */
  vectis_status (*del)(vectis_http_client *self, const char *url,
                       vectis_http_response *response, vectis_error *error);
  vectis_status (*head)(vectis_http_client *self, const char *url,
                        vectis_http_response *response, vectis_error *error);
  vectis_status (*options)(vectis_http_client *self, const char *url,
                           vectis_http_response *response, vectis_error *error);
  vectis_status (*download_file)(vectis_http_client *self, const char *url,
                                 const char *local_path,
                                 vectis_http_response *response,
                                 vectis_error *error);
  vectis_status (*upload_file)(vectis_http_client *self,
                               vectis_http_method method, const char *url,
                               const char *local_path, const char *content_type,
                               vectis_http_response *response,
                               vectis_error *error);
  vectis_status (*post_json)(vectis_http_client *self, const char *url,
                             const lonejson_map *map, const void *value,
                             vectis_http_response *response,
                             vectis_error *error);
  vectis_status (*put_json)(vectis_http_client *self, const char *url,
                            const lonejson_map *map, const void *value,
                            vectis_http_response *response,
                            vectis_error *error);
  vectis_status (*patch_json)(vectis_http_client *self, const char *url,
                              const lonejson_map *map, const void *value,
                              vectis_http_response *response,
                              vectis_error *error);
  void (*close)(vectis_http_client *self);

  /* Shallow effective config copy used by the methods above. */
  vectis_http_client_config config;
  void *impl;
};

struct vectis_sftp {
  vectis_status (*upload_file)(vectis_sftp *self, const char *local_path,
                               const char *remote_path, vectis_error *error);
  vectis_status (*download_file)(vectis_sftp *self, const char *remote_path,
                                 const char *local_path, vectis_error *error);
  void (*close)(vectis_sftp *self);

  /* Shallow effective config copy used by the methods above. */
  vectis_sftp_config config;
  void *impl;
};

struct vectis_ssh {
  vectis_status (*exec)(vectis_ssh *self, const char *command,
                        vectis_ssh_exec_result *result, vectis_error *error);
  vectis_status (*sftp_upload_file)(vectis_ssh *self, const char *local_path,
                                    const char *remote_path,
                                    vectis_error *error);
  vectis_status (*sftp_download_file)(vectis_ssh *self, const char *remote_path,
                                      const char *local_path,
                                      vectis_error *error);
  void (*close)(vectis_ssh *self);

  /* Shallow effective config copy used by the methods above. */
  vectis_ssh_config config;
  void *impl;
};

struct vectis_mqtt {
  vectis_status (*publish)(vectis_mqtt *self, const char *topic,
                           const void *payload, size_t payload_size,
                           const char *content_type, vectis_error *error);
  vectis_status (*publish_json)(vectis_mqtt *self, const char *topic,
                                const lonejson_map *map, const void *value,
                                vectis_error *error);
  void (*close)(vectis_mqtt *self);

  /* Shallow effective config copy used by the methods above. */
  vectis_mqtt_config config;
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
void vectis_upload_route_config_init(vectis_upload_route_config *config);
void vectis_upload_file_route_config_init(
    vectis_upload_file_route_config *config);
void vectis_upload_reader_route_config_init(
    vectis_upload_reader_route_config *config);
void vectis_json_route_config_init(vectis_json_route_config *config);
void vectis_json_typed_route_config_init(
    vectis_json_typed_route_config *config);
void vectis_xml_route_config_init(vectis_xml_route_config *config);
void vectis_dsv_route_config_init(vectis_dsv_route_config *config);
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
vectis_route_config vectis_route(vectis_http_method method, const char *path,
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
vectis_route_config
vectis_json_body_route_methods(vectis_http_methods methods, const char *path,
                               vectis_route_handler_fn handler, void *userdata);
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
                                            const char *path, size_t max_bytes,
                                            vectis_route_handler_fn handler,
                                            void *userdata);
vectis_route_config vectis_upload_route_max_methods(
    vectis_http_methods methods, const char *path, size_t max_bytes,
    vectis_route_handler_fn handler, void *userdata);
vectis_upload_route_config vectis_stream_upload_route(
    vectis_http_method method, const char *path, vectis_upload_open_fn open,
    vectis_upload_write_fn write, vectis_upload_finish_fn finish,
    vectis_upload_close_fn close, void *userdata);
vectis_upload_route_config vectis_stream_upload_route_methods(
    vectis_http_methods methods, const char *path, vectis_upload_open_fn open,
    vectis_upload_write_fn write, vectis_upload_finish_fn finish,
    vectis_upload_close_fn close, void *userdata);
vectis_upload_file_route_config
vectis_upload_file_route(vectis_http_method method, const char *path,
                         const char *file_path, const char *content_type);
vectis_upload_reader_route_config
vectis_upload_reader_route(vectis_http_method method, const char *path,
                           vectis_upload_reader_handler_fn handler,
                           void *userdata);
vectis_upload_reader_route_config vectis_upload_reader_route_methods(
    vectis_http_methods methods, const char *path,
    vectis_upload_reader_handler_fn handler, void *userdata);
vectis_json_route_config
vectis_json_route(vectis_http_method method, const char *path,
                  const lonejson_map *input_map, size_t input_size,
                  const lonejson_map *output_map, size_t output_size,
                  vectis_json_route_handler_fn handler, void *userdata);
vectis_json_route_config
vectis_json_route_methods(vectis_http_methods methods, const char *path,
                          const lonejson_map *input_map, size_t input_size,
                          const lonejson_map *output_map, size_t output_size,
                          vectis_json_route_handler_fn handler, void *userdata);
vectis_json_typed_route_config
vectis_json_typed_route(vectis_http_method method, const char *path,
                        const lonejson_map *input_map, size_t input_size,
                        vectis_json_typed_route_handler_fn handler,
                        void *userdata);
vectis_json_typed_route_config vectis_json_typed_route_methods(
    vectis_http_methods methods, const char *path,
    const lonejson_map *input_map, size_t input_size,
    vectis_json_typed_route_handler_fn handler, void *userdata);
vectis_xml_route_config
vectis_xml_route(vectis_http_method method, const char *path,
                 const lonejson_map *input_map, size_t input_size,
                 const vectis_xml_config *config,
                 vectis_xml_route_handler_fn handler, void *userdata);
vectis_xml_route_config
vectis_xml_route_methods(vectis_http_methods methods, const char *path,
                         const lonejson_map *input_map, size_t input_size,
                         const vectis_xml_config *config,
                         vectis_xml_route_handler_fn handler, void *userdata);
vectis_dsv_route_config
vectis_dsv_route(vectis_http_method method, const char *path,
                 const lonejson_map *row_map, size_t row_size,
                 const vectis_dsv_config *config,
                 vectis_dsv_route_handler_fn handler, void *userdata);
vectis_dsv_route_config
vectis_dsv_route_methods(vectis_http_methods methods, const char *path,
                         const lonejson_map *row_map, size_t row_size,
                         const vectis_dsv_config *config,
                         vectis_dsv_route_handler_fn handler, void *userdata);
void vectis_static_file_config_init(vectis_static_file_config *config);
void vectis_static_directory_config_init(
    vectis_static_directory_config *config);

vectis_app *vectis_app_new(const vectis_app_config *config,
                           vectis_error *error);
vectis_app *vectis_new(const vectis_app_config *config, vectis_error *error);
void vectis_app_close(vectis_app *app);
void vectis_destroy(vectis_app *app);
vectis_status vectis_start(vectis_app *app, vectis_error *error);
vectis_status vectis_stop(vectis_app *app, vectis_error *error);
vectis_status vectis_register_route(vectis_app *app,
                                    const vectis_route_config *route,
                                    vectis_error *error);
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
vectis_status vectis_register_json_route(vectis_app *app,
                                         const vectis_json_route_config *route,
                                         vectis_error *error);
vectis_status
vectis_register_json_typed_route(vectis_app *app,
                                 const vectis_json_typed_route_config *route,
                                 vectis_error *error);
vectis_status vectis_register_xml_route(vectis_app *app,
                                        const vectis_xml_route_config *route,
                                        vectis_error *error);
vectis_status vectis_register_dsv_route(vectis_app *app,
                                        const vectis_dsv_route_config *route,
                                        vectis_error *error);
vectis_status vectis_register_prefixed_route(vectis_app *app,
                                             const char *prefix,
                                             const vectis_route_config *route,
                                             vectis_error *error);
vectis_status
vectis_register_prefixed_json_route(vectis_app *app, const char *prefix,
                                    const vectis_json_route_config *route,
                                    vectis_error *error);
vectis_status vectis_register_prefixed_json_typed_route(
    vectis_app *app, const char *prefix,
    const vectis_json_typed_route_config *route, vectis_error *error);
vectis_status
vectis_register_prefixed_xml_route(vectis_app *app, const char *prefix,
                                   const vectis_xml_route_config *route,
                                   vectis_error *error);
vectis_status
vectis_register_prefixed_dsv_route(vectis_app *app, const char *prefix,
                                   const vectis_dsv_route_config *route,
                                   vectis_error *error);
vectis_status
vectis_register_static_file(vectis_app *app,
                            const vectis_static_file_config *config,
                            vectis_error *error);
vectis_status
vectis_register_static_directory(vectis_app *app,
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
vectis_status
vectis_consumer_service_new(vectis_app *app,
                            const struct lc_consumer_service_config *config,
                            vectis_consumer_service **out, vectis_error *error);
struct lc_consumer_service *
vectis_consumer_service_raw(vectis_consumer_service *service);
vectis_status vectis_consumer_service_run(vectis_consumer_service *service,
                                          vectis_error *error);
vectis_status vectis_consumer_service_start(vectis_consumer_service *service,
                                            vectis_error *error);
vectis_status vectis_consumer_service_stop(vectis_consumer_service *service,
                                           vectis_error *error);
vectis_status vectis_consumer_service_wait(vectis_consumer_service *service,
                                           vectis_error *error);
vectis_status
vectis_consumer_service_run_until(vectis_consumer_service *service,
                                  const volatile int *done, long timeout_ms,
                                  vectis_error *error);
void vectis_consumer_service_destroy(vectis_consumer_service *service);
vectis_status vectis_json_validate_cstr(const char *json, vectis_error *error);
void vectis_dsv_config_init(vectis_dsv_config *config);
vectis_dsv_config vectis_dsv_csv(void);
vectis_dsv_config vectis_dsv_tsv(void);
vectis_dsv_config vectis_dsv_csv_rows(void);
vectis_dsv_config vectis_dsv_tsv_rows(void);
vectis_status vectis_dsv_parse_lonejson(struct lc_source *source,
                                        const lonejson_map *map,
                                        const vectis_dsv_config *config,
                                        vectis_dsv_lonejson_row_fn row,
                                        void *userdata, vectis_error *error);
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
vectis_status vectis_dsv_source_to_lonejson_array(
    const vectis_source *source, const lonejson_map *map,
    const vectis_dsv_config *config, vectis_mutable_bytes *out,
    vectis_error *error);
vectis_status vectis_dsv_to_json_array_spill(
    struct lc_source *source, const vectis_dsv_config *config,
    const vectis_body_spill_config *spill, vectis_body_spill_result *out,
    vectis_error *error);
vectis_status vectis_dsv_source_to_json_array_spill(
    const vectis_source *source, const vectis_dsv_config *config,
    const vectis_body_spill_config *spill, vectis_body_spill_result *out,
    vectis_error *error);
vectis_status vectis_dsv_to_lonejson_array_spill(
    struct lc_source *source, const lonejson_map *map,
    const vectis_dsv_config *config, const vectis_body_spill_config *spill,
    vectis_body_spill_result *out, vectis_error *error);
vectis_status vectis_dsv_source_to_lonejson_array_spill(
    const vectis_source *source, const lonejson_map *map,
    const vectis_dsv_config *config, const vectis_body_spill_config *spill,
    vectis_body_spill_result *out, vectis_error *error);
vectis_status vectis_dsv_write_lonejson_rows(struct lc_sink *sink,
                                             const lonejson_map *map,
                                             const vectis_dsv_config *config,
                                             const void *rows, size_t row_count,
                                             size_t row_stride,
                                             vectis_error *error);
vectis_status vectis_dsv_lonejson_rows_to_bytes(
    const lonejson_map *map, const vectis_dsv_config *config, const void *rows,
    size_t row_count, size_t row_stride, vectis_mutable_bytes *out,
    vectis_error *error);
/**
 * Pull the next row from a streaming DSV route.
 *
 * On success, `*has_row` is set to 1 with `*row` pointing at borrowed storage
 * valid until the next call or until the route handler returns. EOF is reported
 * as `VECTIS_OK` with `*has_row == 0`.
 */
vectis_status vectis_dsv_rows_next(vectis_dsv_rows *rows, int *has_row,
                                   size_t *row_number, const void **row,
                                   vectis_error *error);
/**
 * Stream a selected JSON array from a Vectis source.
 *
 * `array_path` is a LoneJSON selected-array path; pass `NULL` or an empty
 * string for a root JSON array. Each element is decoded into `item` using
 * `map`, passed to `callback`, then cleaned up before the next element is read.
 * The full JSON document and selected array are not materialized.
 */
vectis_status vectis_json_array_each_source(const vectis_source *source,
                                            const char *array_path,
                                            const lonejson_map *map, void *item,
                                            vectis_json_array_item_fn callback,
                                            void *userdata,
                                            vectis_error *error);
/**
 * Rewrite a selected JSON array from a Vectis source into an `lc_sink`.
 *
 * The source and sink are processed incrementally through LoneJSON's array
 * rewriter. `selector` follows LoneJSON's selected-array syntax; pass `NULL` or
 * an empty string for a root JSON array. `options` owns the item map, item
 * storage, and rewrite callback policy.
 */
vectis_status vectis_json_array_rewrite_source(
    const vectis_source *source, const char *selector, struct lc_sink *sink,
    const lonejson_array_rewrite_options *options, vectis_error *error);
void vectis_xml_config_init(vectis_xml_config *config);
vectis_xml_config vectis_xml_default(void);
vectis_status vectis_xml_parse_lonejson(struct lc_source *source,
                                        const lonejson_map *map,
                                        const vectis_xml_config *config,
                                        void *out, vectis_error *error);
vectis_status vectis_xml_parse_lonejson_source(const vectis_source *source,
                                               const lonejson_map *map,
                                               const vectis_xml_config *config,
                                               void *out, vectis_error *error);
vectis_status vectis_format_key(char *out, size_t out_size, vectis_error *error,
                                const char *format, ...);
vectis_status vectis_lockd_state_load(struct lc_client *client, const char *key,
                                      const char *owner, long ttl_seconds,
                                      const lonejson_map *map, void *out,
                                      vectis_error *error);
vectis_status vectis_lockd_state_save(struct lc_client *client, const char *key,
                                      const char *owner, long ttl_seconds,
                                      const lonejson_map *map,
                                      const void *value, vectis_error *error);
vectis_status vectis_lockd_state_update(struct lc_client *client,
                                        const char *key, const char *owner,
                                        long ttl_seconds,
                                        const lonejson_map *map, void *state,
                                        vectis_lockd_state_update_fn update,
                                        void *userdata, vectis_error *error);

vectis_status vectis_request_json_into(vectis_request *request,
                                       const lonejson_map *map, void *out,
                                       vectis_error *error);
/**
 * Stream a selected JSON array from a request body.
 *
 * The request body reader is reset before parsing. This is a true streaming
 * parse of the request body; use `vectis_request_json_into()` only when the
 * complete JSON object is the desired API contract.
 */
vectis_status
vectis_request_json_array_each(vectis_request *request, const char *array_path,
                               const lonejson_map *map, void *item,
                               vectis_json_array_item_fn callback,
                               void *userdata, vectis_error *error);
vectis_http_method vectis_request_method(vectis_request *request);
const char *vectis_request_path(vectis_request *request);
const char *vectis_request_path_param(vectis_request *request,
                                      const char *name);
const char *vectis_request_query(vectis_request *request, const char *name);
const char *vectis_request_header(vectis_request *request, const char *name);
struct http_request *vectis_request_kore(vectis_request *request);
struct lc_source *vectis_request_body_reader(vectis_request *request);
void vectis_body_materialize_config_init(
    vectis_body_materialize_config *config);
void vectis_body_materialized_cleanup(vectis_body_materialized *body);
vectis_status
vectis_body_materialized_open_reader(const vectis_body_materialized *body,
                                     struct lc_source **out,
                                     vectis_error *error);
vectis_status vectis_request_body_materialize(
    vectis_request *request, const vectis_body_materialize_config *config,
    vectis_body_materialized *out, vectis_error *error);
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
                                        vectis_bytes *out, vectis_error *error);
vectis_status vectis_request_body_copy(vectis_request *request,
                                       vectis_mutable_bytes *out,
                                       vectis_error *error);
void vectis_mutable_bytes_cleanup(vectis_mutable_bytes *bytes);
const char *vectis_request_body_path(vectis_request *request);
int vectis_request_body_is_spooled(vectis_request *request);
vectis_status vectis_response_status(vectis_response *response, int status_code,
                                     vectis_error *error);
vectis_status vectis_response_header(vectis_response *response,
                                     const char *name, const char *value,
                                     vectis_error *error);
vectis_status vectis_response_text(vectis_response *response, int status_code,
                                   const char *content_type, const char *text,
                                   vectis_error *error);
vectis_status vectis_response_bytes(vectis_response *response, int status_code,
                                    const char *content_type, vectis_bytes body,
                                    vectis_error *error);
vectis_status vectis_response_file(vectis_response *response, int status_code,
                                   const char *content_type, const char *path,
                                   vectis_error *error);
vectis_status vectis_response_source(vectis_response *response, int status_code,
                                     const char *content_type,
                                     struct lc_source *source,
                                     vectis_error *error);
vectis_status vectis_response_json(vectis_response *response, int status_code,
                                   const lonejson_map *map, const void *value,
                                   vectis_error *error);
vectis_status vectis_response_json_generated(vectis_response *response,
                                             int status_code,
                                             const lonejson_map *map,
                                             const void *value,
                                             vectis_error *error);
vectis_status vectis_json_reply(vectis_json_response *response, int status_code,
                                const lonejson_map *map, const void *value,
                                vectis_error *error);
vectis_status vectis_json_reply_status(vectis_json_response *response,
                                       int status_code, vectis_error *error);
vectis_status vectis_response_error_json(vectis_response *response,
                                         int status_code, const char *code,
                                         const char *message,
                                         const char *detail,
                                         vectis_error *error);

void vectis_http_client_config_init(vectis_http_client_config *config);
vectis_status vectis_http_client_new(const vectis_http_client_config *config,
                                     vectis_http_client **out,
                                     vectis_error *error);
vectis_status
vectis_http_client_from_app(vectis_app *app,
                            const vectis_http_client_config *config,
                            vectis_http_client **out, vectis_error *error);
void vectis_http_client_destroy(vectis_http_client *client);
void vectis_http_client_close(vectis_http_client *client);
void vectis_http_request_init(vectis_http_request *request);
void vectis_http_response_cleanup(vectis_http_response *response);
vectis_status
vectis_http_response_json_into(const vectis_http_response *response,
                               const lonejson_map *map, void *out,
                               vectis_error *error);
/**
 * Iterate a selected JSON array from an already-buffered HTTP response.
 *
 * This helper streams the selected array out of `response->body`, but the HTTP
 * transport has already materialized the response. Use
 * `vectis_http_get_json_array()` when the body itself must be consumed
 * incrementally from the network.
 */
vectis_status vectis_http_response_json_array_each(
    const vectis_http_response *response, const char *array_path,
    const lonejson_map *map, void *item, vectis_json_array_item_fn callback,
    void *userdata, vectis_error *error);
vectis_status vectis_http_client_execute(vectis_http_client *client,
                                         const vectis_http_request *request,
                                         vectis_http_response *response,
                                         vectis_error *error);
vectis_status vectis_http_client_get(vectis_http_client *client,
                                     const char *url,
                                     vectis_http_response *response,
                                     vectis_error *error);
/**
 * Stream a selected JSON array from an HTTP GET response through a client
 * handle.
 *
 * The response metadata is still populated, but the body bytes are consumed by
 * LoneJSON and are not copied into `response->body`.
 */
vectis_status vectis_http_client_get_json_array(
    vectis_http_client *client, const char *url, const char *array_path,
    const lonejson_map *map, void *item, vectis_json_array_item_fn callback,
    void *userdata, vectis_http_response *response, vectis_error *error);
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
vectis_status vectis_http_client_upload_file(
    vectis_http_client *client, vectis_http_method method, const char *url,
    const char *local_path, const char *content_type,
    vectis_http_response *response, vectis_error *error);
vectis_status vectis_http_client_post_json(
    vectis_http_client *client, const char *url, const lonejson_map *map,
    const void *value, vectis_http_response *response, vectis_error *error);
vectis_status vectis_http_client_put_json(
    vectis_http_client *client, const char *url, const lonejson_map *map,
    const void *value, vectis_http_response *response, vectis_error *error);
vectis_status vectis_http_client_patch_json(
    vectis_http_client *client, const char *url, const lonejson_map *map,
    const void *value, vectis_http_response *response, vectis_error *error);
vectis_status vectis_http_execute(const vectis_http_client_config *client,
                                  const vectis_http_request *request,
                                  vectis_http_response *response,
                                  vectis_error *error);
vectis_status vectis_http_get(const vectis_http_client_config *client,
                              const char *url, vectis_http_response *response,
                              vectis_error *error);
/**
 * Stream a selected JSON array from an HTTP GET response.
 *
 * This is the transport-level streaming variant for large JSON array payloads.
 * `response->body` remains `NULL`; callers observe elements through `callback`
 * as they arrive and can abort by returning a non-OK status.
 */
vectis_status
vectis_http_get_json_array(const vectis_http_client_config *client,
                           const char *url, const char *array_path,
                           const lonejson_map *map, void *item,
                           vectis_json_array_item_fn callback, void *userdata,
                           vectis_http_response *response, vectis_error *error);
vectis_status vectis_http_delete(const vectis_http_client_config *client,
                                 const char *url,
                                 vectis_http_response *response,
                                 vectis_error *error);
vectis_status vectis_http_head(const vectis_http_client_config *client,
                               const char *url, vectis_http_response *response,
                               vectis_error *error);
vectis_status vectis_http_options(const vectis_http_client_config *client,
                                  const char *url,
                                  vectis_http_response *response,
                                  vectis_error *error);
vectis_status vectis_http_download_file(const vectis_http_client_config *client,
                                        const char *url, const char *local_path,
                                        vectis_http_response *response,
                                        vectis_error *error);
vectis_status vectis_http_upload_file(const vectis_http_client_config *client,
                                      vectis_http_method method,
                                      const char *url, const char *local_path,
                                      const char *content_type,
                                      vectis_http_response *response,
                                      vectis_error *error);
vectis_status vectis_http_post_json(const vectis_http_client_config *client,
                                    const char *url, const lonejson_map *map,
                                    const void *value,
                                    vectis_http_response *response,
                                    vectis_error *error);
vectis_status vectis_http_put_json(const vectis_http_client_config *client,
                                   const char *url, const lonejson_map *map,
                                   const void *value,
                                   vectis_http_response *response,
                                   vectis_error *error);
vectis_status vectis_http_patch_json(const vectis_http_client_config *client,
                                     const char *url, const lonejson_map *map,
                                     const void *value,
                                     vectis_http_response *response,
                                     vectis_error *error);

void vectis_sftp_config_init(vectis_sftp_config *config);
vectis_status vectis_sftp_new(const vectis_sftp_config *config,
                              vectis_sftp **out, vectis_error *error);
void vectis_sftp_close(vectis_sftp *sftp);
vectis_status vectis_sftp_upload_file(const vectis_sftp_config *config,
                                      const char *local_path,
                                      const char *remote_path,
                                      vectis_error *error);
vectis_status vectis_sftp_download_file(const vectis_sftp_config *config,
                                        const char *remote_path,
                                        const char *local_path,
                                        vectis_error *error);

void vectis_ssh_config_init(vectis_ssh_config *config);
vectis_status vectis_ssh_new(const vectis_ssh_config *config, vectis_ssh **out,
                             vectis_error *error);
void vectis_ssh_close(vectis_ssh *ssh);
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
vectis_status vectis_mqtt_new(const vectis_mqtt_config *config,
                              vectis_mqtt **out, vectis_error *error);
void vectis_mqtt_close(vectis_mqtt *mqtt);
vectis_status vectis_mqtt_publish(const vectis_mqtt_config *config,
                                  const char *topic, const void *payload,
                                  size_t payload_size, const char *content_type,
                                  vectis_error *error);
vectis_status vectis_mqtt_publish_json(const vectis_mqtt_config *config,
                                       const char *topic,
                                       const lonejson_map *map,
                                       const void *value, vectis_error *error);

void vectis_cert_subject_init(vectis_cert_subject *subject);
void vectis_cert_bundle_config_init(vectis_cert_bundle_config *config);
void vectis_private_key_config_init(vectis_private_key_config *config);
void vectis_csr_config_init(vectis_csr_config *config);
vectis_status
vectis_cert_generate_private_key(const vectis_private_key_config *config,
                                 vectis_error *error);
vectis_status vectis_cert_generate_csr(const vectis_csr_config *config,
                                       vectis_error *error);
vectis_status
vectis_cert_generate_bundle(const vectis_cert_bundle_config *config,
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
