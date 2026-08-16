#ifndef VECTIS_VECTIS_H
#define VECTIS_VECTIS_H

#ifndef LONEJSON_WITH_CURL
#define LONEJSON_WITH_CURL 1
#endif
#include <cai/cai.h>
#include <cai/mcp.h>
#include <cpkt/opcua.h>
#include <curl/curl.h>
#include <stddef.h>
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
#define VECTIS_AUTOBLOCK_MAX_STATUS_RULES 16u
#define VECTIS_AUTOBLOCK_MAX_EVENT_RULES 16u
#define VECTIS_AUTOBLOCK_MAX_TRUSTED_PROXIES 16u
#define VECTIS_BODY_DEFAULT_UPLOAD_MAX_BYTES ((size_t)3221225472UL)
#define VECTIS_BODY_DEFAULT_MEMORY_BUFFER_LIMIT_BYTES 262144u
#define VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES                          \
  VECTIS_BODY_DEFAULT_MEMORY_BUFFER_LIMIT_BYTES
#define VECTIS_MAILBOX_DEFAULT_CAPACITY 1024u
#define VECTIS_MAILBOX_DEFAULT_MAX_PAYLOAD_BYTES 1048576u
#define VECTIS_MAILBOX_BROKER_DEFAULT_MAX_PENDING 64u
#define VECTIS_ROUTE_EVENT_DEFAULT_MAX_BODY_BYTES 65536u
#define VECTIS_LOCKD_CONSUMER_EVENT_DEFAULT_MAX_PAYLOAD_BYTES 65536u
#define VECTIS_OPCUA_MONITOR_EVENT_DEFAULT_MAX_PAYLOAD_BYTES 65536u
#define VECTIS_SSH_SFTP_OPEN_READ 0x01u
#define VECTIS_SSH_SFTP_OPEN_WRITE 0x02u
#define VECTIS_SSH_SFTP_OPEN_CREATE 0x04u
#define VECTIS_SSH_SFTP_OPEN_TRUNCATE 0x08u
#define VECTIS_SSH_SFTP_OPEN_APPEND 0x10u

#ifdef __cplusplus
extern "C" {
#endif

struct lc_client;
struct lc_lease;
struct lc_sink;
struct lc_source;
struct lc_enqueue_req;
struct lc_enqueue_res;
struct lc_consumer_service;
struct lc_consumer_service_config;
struct lc_consumer_message;
struct lc_error;

typedef struct vectis_app vectis_app;
typedef struct vectis_consumer_service vectis_consumer_service;
typedef struct vectis_http_client vectis_http_client;
typedef struct vectis_mailbox vectis_mailbox;
typedef struct vectis_mailbox_broker vectis_mailbox_broker;
typedef struct vectis_opcua_monitor_mailbox vectis_opcua_monitor_mailbox;
typedef struct vectis_embedded_fs vectis_embedded_fs;
typedef struct vectis_sftp vectis_sftp;
typedef struct vectis_ssh vectis_ssh;
typedef struct vectis_ssh_sftp_session vectis_ssh_sftp_session;
typedef struct vectis_ssh_sftp_file vectis_ssh_sftp_file;
typedef struct vectis_ssh_sftp_dir vectis_ssh_sftp_dir;
typedef struct vectis_mqtt vectis_mqtt;
typedef struct vectis_request vectis_request;
typedef struct vectis_response vectis_response;
typedef struct vectis_json_response vectis_json_response;
typedef struct vectis_dsv_rows vectis_dsv_rows;
typedef struct vectis_auth_routes_config vectis_auth_routes_config;
typedef struct vectis_webdav_mount_config vectis_webdav_mount_config;
typedef struct vectis_webdav_embedded_site_config
    vectis_webdav_embedded_site_config;
typedef struct vectis_webdav_embedded_mount_config
    vectis_webdav_embedded_mount_config;

#define VECTIS_CAI_DEFAULT_OUTPUT_CONTENT_TYPE "text/plain; charset=utf-8"

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
  VECTIS_HTTP_OPTIONS,
  VECTIS_HTTP_PROPFIND,
  VECTIS_HTTP_MKCOL,
  VECTIS_HTTP_COPY,
  VECTIS_HTTP_MOVE
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
#define VECTIS_HTTP_METHODS_PROPFIND                                           \
  VECTIS_HTTP_METHOD_MASK(VECTIS_HTTP_PROPFIND)
#define VECTIS_HTTP_METHODS_MKCOL VECTIS_HTTP_METHOD_MASK(VECTIS_HTTP_MKCOL)
#define VECTIS_HTTP_METHODS_COPY VECTIS_HTTP_METHOD_MASK(VECTIS_HTTP_COPY)
#define VECTIS_HTTP_METHODS_MOVE VECTIS_HTTP_METHOD_MASK(VECTIS_HTTP_MOVE)
#define VECTIS_HTTP_METHODS_ALL                                                \
  (VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_POST |                        \
   VECTIS_HTTP_METHODS_PUT | VECTIS_HTTP_METHODS_PATCH |                       \
   VECTIS_HTTP_METHODS_DELETE | VECTIS_HTTP_METHODS_HEAD |                     \
   VECTIS_HTTP_METHODS_OPTIONS | VECTIS_HTTP_METHODS_PROPFIND |                \
   VECTIS_HTTP_METHODS_MKCOL | VECTIS_HTTP_METHODS_COPY |                      \
   VECTIS_HTTP_METHODS_MOVE)

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
  VECTIS_ERROR_SOURCE_LIBSSH2 = 8,
  VECTIS_ERROR_SOURCE_CPKT = 9,
  VECTIS_ERROR_SOURCE_CAI = 10
} vectis_error_source;

typedef struct vectis_error {
  vectis_status code;
  vectis_error_source source;
  long dependency_code;
  long http_status;
  char message[256];
  char detail[256];
} vectis_error;

typedef struct vectis_mailbox_config {
  /* Zero means VECTIS_MAILBOX_DEFAULT_CAPACITY. */
  size_t capacity;
  /* Zero means VECTIS_MAILBOX_DEFAULT_MAX_PAYLOAD_BYTES. */
  size_t max_payload_bytes;
} vectis_mailbox_config;

/* Borrowed input message. publish functions copy kind and payload bytes. */
typedef struct vectis_mailbox_message {
  const char *kind;
  const void *payload;
  size_t payload_size;
  unsigned long correlation_id;
  int expects_reply;
} vectis_mailbox_message;

/* Owned output event. Release with vectis_mailbox_event_cleanup(). */
typedef struct vectis_mailbox_event {
  char *kind;
  void *payload;
  size_t payload_size;
  unsigned long correlation_id;
  int expects_reply;
} vectis_mailbox_event;

/* Snapshot counters for mailbox diagnostics. Counters are monotonic. */
typedef struct vectis_mailbox_stats {
  size_t capacity;
  size_t max_payload_bytes;
  size_t current_depth;
  size_t high_water_depth;
  unsigned long published;
  unsigned long publish_failures;
  unsigned long full_failures;
  unsigned long closed_failures;
  unsigned long timeout_failures;
  unsigned long drained;
  unsigned long requests_published;
  unsigned long replies_published;
  unsigned long correlation_ids_issued;
} vectis_mailbox_stats;

typedef struct vectis_mailbox_broker_config {
  /*
   * Borrowed mailbox that receives worker requests. The broker does not close
   * or destroy it.
   */
  vectis_mailbox *request_mailbox;
  /*
   * Per-request reply mailbox config. Zero capacity means one queued reply;
   * zero max_payload_bytes means VECTIS_MAILBOX_DEFAULT_MAX_PAYLOAD_BYTES.
   */
  vectis_mailbox_config reply_mailbox;
  /* Zero means VECTIS_MAILBOX_BROKER_DEFAULT_MAX_PENDING. */
  size_t max_pending;
} vectis_mailbox_broker_config;

typedef struct vectis_route_event_config {
  /* Defaults to "vectis.route". */
  const char *kind;
  const char *const *path_params;
  size_t path_param_count;
  const char *const *query;
  size_t query_count;
  const char *const *headers;
  size_t header_count;
  /*
   * Body bytes are copied only when include_body is nonzero and the request
   * body is already buffered. Reader-backed or streaming bodies fail closed.
   */
  int include_body;
  /* Zero means VECTIS_ROUTE_EVENT_DEFAULT_MAX_BODY_BYTES. */
  size_t max_body_bytes;
  /* Success response mapping for vectis_route_mailbox_request(). */
  int reply_status_code;
  const char *reply_content_type;
  /* Timeout response mapping for vectis_route_mailbox_request(). */
  int timeout_status_code;
  const char *timeout_content_type;
  const char *timeout_body;
  /* Non-timeout broker error response mapping. */
  int error_status_code;
} vectis_route_event_config;

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

typedef struct vectis_route_event {
  vectis_mailbox_message message;
  vectis_mutable_bytes payload;
} vectis_route_event;

typedef struct vectis_lockd_consumer_event_config {
  /* Defaults to "vectis.lockd.consumer". */
  const char *kind;
  /*
   * Payload bytes are copied only when include_payload is nonzero. This is a
   * bounded materialized projection of the lockd message payload, not a
   * streaming bridge.
   */
  int include_payload;
  /* Zero means VECTIS_LOCKD_CONSUMER_EVENT_DEFAULT_MAX_PAYLOAD_BYTES. */
  size_t max_payload_bytes;
} vectis_lockd_consumer_event_config;

typedef struct vectis_lockd_consumer_event {
  vectis_mailbox_message message;
  vectis_mutable_bytes payload;
} vectis_lockd_consumer_event;

typedef struct vectis_lockd_consumer_mailbox_receiver_config {
  /*
   * Fire-and-forget target. The receiver returns success after the event has
   * been copied into this mailbox.
   */
  vectis_mailbox *mailbox;
  /*
   * Request/reply target. When set, this takes precedence over mailbox and the
   * receiver returns success only when the broker receives a reply before the
   * timeout.
   */
  vectis_mailbox_broker *broker;
  /* Negative waits indefinitely; zero polls once. */
  long reply_timeout_ms;
  vectis_lockd_consumer_event_config event;
} vectis_lockd_consumer_mailbox_receiver_config;

typedef struct vectis_opcua_monitor_event_config {
  /* Defaults to "vectis.opcua.data_change". */
  const char *data_change_kind;
  /* Defaults to "vectis.opcua.event". */
  const char *event_kind;
  /* Defaults to "vectis.opcua.event_fields". */
  const char *event_fields_kind;
  /* Zero means VECTIS_OPCUA_MONITOR_EVENT_DEFAULT_MAX_PAYLOAD_BYTES. */
  size_t max_payload_bytes;
} vectis_opcua_monitor_event_config;

typedef struct vectis_opcua_monitor_mailbox_config {
  /*
   * Fire-and-forget target. The adapter copies each callback event into this
   * mailbox and records publish failures in adapter stats.
   */
  vectis_mailbox *mailbox;
  /*
   * Request/reply target. When set, this takes precedence over mailbox and the
   * callback waits for a broker reply using reply_timeout_ms.
   */
  vectis_mailbox_broker *broker;
  /* Negative waits indefinitely; zero polls once. */
  long reply_timeout_ms;
  vectis_opcua_monitor_event_config event;
} vectis_opcua_monitor_mailbox_config;

typedef struct vectis_opcua_monitor_mailbox_stats {
  unsigned long data_changes;
  unsigned long events;
  unsigned long event_fields;
  unsigned long publish_failures;
  unsigned long request_failures;
  vectis_error last_error;
} vectis_opcua_monitor_mailbox_stats;

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

typedef struct vectis_cai_config {
  /*
   * Borrowed CAI client. When set, Vectis returns this client and never closes
   * it. When NULL, vectis_app_cai_client() lazily opens an app-owned CAI client
   * from client_config.
   */
  cai_client *client;
  /*
   * Configuration for a Vectis-owned CAI client. String fields are copied into
   * the app at construction time; chatgpt_auth, logger, allocator callbacks,
   * and other handle/context pointers remain borrowed.
   */
  cai_client_config client_config;
  /*
   * Dedicated CAI logger override. When NULL and logging is not disabled, CAI
   * inherits the Vectis app logger.
   */
  pslog_logger *logger;
  int logger_disabled;
} vectis_cai_config;

typedef struct vectis_tls_config {
  vectis_tls_mode mode;
  const char *bind;
  unsigned short port;
  const char *const *domains;
  size_t domain_count;
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
  const char *acme_state_dir;
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

typedef struct vectis_metrics_config {
  /* HTML dashboard path. Defaults to "/.metrics". */
  const char *path;
  /* JSON snapshot path. Defaults to path + ".json". */
  const char *json_path;
  /* Optional dashboard title. When NULL, Vectis derives a request host/listen
   * title and falls back to the app name or "vectis".
   */
  const char *title;
  /* Optional route auth provider. When set, both HTML and JSON metrics routes
   * are guarded by the same Vectis auth provider contract used by WebDAV and
   * Lua routes.
   */
  const struct vectis_auth_provider *auth_provider;
  const char *auth_purpose;
  unsigned allowed_auth_modes;
  /*
   * Snapshot persistence is opt-in. When enabled, a background worker writes
   * JSON snapshots through liblockdc. storage_endpoint may be any lockdc
   * endpoint, including pouch://. When NULL, Vectis uses a local pouch under
   * ${XDG_STATE_HOME:-$HOME/.local/state}/vectis/storage.
   */
  int persistence_enabled;
  const char *storage_endpoint;
  const char *storage_namespace;
  const char *storage_owner;
  unsigned snapshot_interval_seconds;
} vectis_metrics_config;

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

typedef struct vectis_cai_mcp_route_config {
  /** Single-method fallback; VECTIS_HTTP_ANY means use methods. */
  vectis_http_method method;
  /** Allowed MCP HTTP methods; defaults to GET, POST, and DELETE. */
  vectis_http_methods methods;
  /** Literal or regex route path. Defaults are supplied by factory helpers. */
  const char *path;
  /** Path interpretation for path. */
  vectis_route_path_kind path_kind;
  /** Request body policy used before CAI receives the body source. */
  vectis_body_policy body;
  /** Bounded bridge buffer for streaming the request body into CAI. */
  size_t buffer_bytes;
  /** Borrowed handler. If NULL, Vectis creates and owns one from
   * handler_config for the route lifetime. Responses are file-backed through
   * the current Vectis route model, not live response streaming.
   */
  cai_mcp_handler *handler;
  /** CAI handler config used when handler is NULL; nested pointers are
   * borrowed and must outlive the registered route.
   */
  cai_mcp_handler_config handler_config;
} vectis_cai_mcp_route_config;

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

/* Read-only HTTP mount over a borrowed vectis_embedded_fs handle. The caller
 * must keep fs alive until the app is closed or the route is no longer used.
 * NULL content_type defaults to application/octet-stream for entries without a
 * manifest content type. NULL cache_control defaults to no-cache. NULL
 * not_found_body defaults to "not found\n"; NULL not_found_content_type
 * defaults to text/plain; charset=utf-8. methods may contain only GET and HEAD
 * and defaults to both.
 */
typedef struct vectis_static_embedded_config {
  const char *path_prefix;
  const vectis_embedded_fs *fs;
  const char *content_type;
  const char *cache_control;
  const char *not_found_body;
  const char *not_found_content_type;
  vectis_http_methods methods;
} vectis_static_embedded_config;

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

typedef int (*vectis_consumer_receiver_handle_fn)(
    void *context, struct lc_consumer_message *message, struct lc_error *error);
typedef void (*vectis_consumer_receiver_cleanup_fn)(void *context);

typedef struct vectis_consumer_receiver {
  vectis_consumer_receiver_handle_fn handle;
  void *context;
  vectis_consumer_receiver_cleanup_fn cleanup;
} vectis_consumer_receiver;

typedef vectis_status (*vectis_consumer_receiver_create_fn)(
    void *adapter_context, const void *receiver_config,
    vectis_consumer_receiver *out, vectis_error *error);

typedef struct vectis_consumer_receiver_adapter {
  const char *kind;
  vectis_consumer_receiver_create_fn create;
  void *context;
} vectis_consumer_receiver_adapter;

typedef struct vectis_consumer_service_receiver_config {
  const char *name;
  const char *queue;
  const char *owner;
  const char *namespace_name;
  long visibility_timeout_seconds;
  long wait_seconds;
  size_t worker_count;
  int with_state;
  const char *receiver_kind;
  const void *receiver_config;
} vectis_consumer_service_receiver_config;

typedef struct vectis_webdav_marker_receiver_config {
  const char *cache_dir;
  const char *site_id;
  const char *processing_path;
  const char *done_path;
  const char *processing_body;
  const char *done_body;
  size_t max_file_bytes;
  size_t max_total_bytes;
  size_t max_resources;
  long processing_delay_seconds;
} vectis_webdav_marker_receiver_config;

typedef vectis_status (*vectis_dsv_lonejson_row_fn)(void *userdata,
                                                    size_t row_number,
                                                    void *row,
                                                    vectis_error *error);
typedef vectis_status (*vectis_dsv_lonejson_storage_fn)(void *userdata,
                                                        size_t row_number,
                                                        void **row_storage,
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

typedef struct vectis_autoblock_status_rule {
  unsigned int status;
  unsigned int threshold;
} vectis_autoblock_status_rule;

typedef struct vectis_autoblock_event_rule {
  const char *name;
  unsigned int threshold;
} vectis_autoblock_event_rule;

typedef struct vectis_autoblock_config {
  int enabled;
  unsigned int window_seconds;
  unsigned int block_seconds;
  unsigned int max_entries;
  unsigned int tcp_stall_threshold;
  unsigned int tls_failure_threshold;
  vectis_autoblock_status_rule status_rules[VECTIS_AUTOBLOCK_MAX_STATUS_RULES];
  size_t status_rule_count;
  vectis_autoblock_event_rule event_rules[VECTIS_AUTOBLOCK_MAX_EVENT_RULES];
  size_t event_rule_count;
  int proxy_enabled;
  const char *const *trusted_proxies;
  size_t trusted_proxy_count;
} vectis_autoblock_config;

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
  vectis_autoblock_config autoblock;
} vectis_server_config;

typedef struct vectis_app_config {
  const char *app_name;
  pslog_logger *logger;
  pslog_mode log_mode;
  pslog_level min_log_level;
  vectis_server_config server;
  vectis_tls_config tls;
  vectis_lockd_config lockd;
  vectis_cai_config cai;
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

typedef struct vectis_http_header {
  /* Lower/upper-case preserved response header name. Owned by response. */
  char *name;
  /* Header value with surrounding linear whitespace trimmed. Owned by response.
   */
  char *value;
} vectis_http_header;

typedef struct vectis_http_response {
  long status_code;
  char *content_type;
  /* Owned response headers from the final HTTP response. */
  vectis_http_header *headers;
  size_t header_count;
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

typedef struct vectis_ssh_sftp_stat_result {
  unsigned long flags;
  int has_size;
  size_t size;
  int size_overflow;
  int has_uid_gid;
  unsigned long uid;
  unsigned long gid;
  int has_permissions;
  unsigned long permissions;
  int has_atime;
  unsigned long atime;
  int has_mtime;
  unsigned long mtime;
} vectis_ssh_sftp_stat_result;

typedef struct vectis_ssh_sftp_dir_entry {
  int eof;
  char *name;
  char *long_name;
  vectis_ssh_sftp_stat_result stat;
} vectis_ssh_sftp_dir_entry;

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
  /* Start/stop a managed app runtime. Route-backed apps that need the caller
   * to continue start Kore in a child process; app-owned background services
   * materialize only after the route-backed fork boundary is closed.
   */
  vectis_status (*start)(vectis_app *self, vectis_error *error);
  vectis_status (*stop)(vectis_app *self, vectis_error *error);
  /* Run the app until SIGINT, SIGTERM, or SIGQUIT. Route-backed apps without
   * app-owned background services enter Kore directly. Route-backed apps with
   * app-owned services use the supervised runtime so Kore starts from a
   * thread-clean process and services materialize in the supervisor.
   */
  vectis_status (*run)(vectis_app *self, vectis_error *error);
  vectis_status (*wait)(vectis_app *self, vectis_error *error);

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
  vectis_status (*cai_mcp_route)(vectis_app *self,
                                 const vectis_cai_mcp_route_config *route,
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
  vectis_status (*static_embedded)(vectis_app *self,
                                   const vectis_static_embedded_config *config,
                                   vectis_error *error);
  vectis_status (*webdav)(vectis_app *self,
                          const vectis_webdav_mount_config *config,
                          vectis_error *error);
  vectis_status (*webdav_embedded_site)(
      vectis_app *self, const vectis_webdav_embedded_site_config *config,
      vectis_error *error);
  vectis_status (*webdav_embedded)(
      vectis_app *self, const vectis_webdav_embedded_mount_config *config,
      vectis_error *error);
  vectis_status (*auth_routes)(vectis_app *self,
                               const vectis_auth_routes_config *config,
                               vectis_error *error);
  vectis_status (*metrics)(vectis_app *self,
                           const vectis_metrics_config *config,
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

  /* Return the borrowed or app-owned CAI client, opening the app-owned client
   * lazily when necessary. The returned client remains owned by its configured
   * owner and must not be closed by the caller.
   */
  vectis_status (*cai_client)(vectis_app *self, cai_client **out,
                              vectis_error *error);

  /* Returns the app-owned process-local lockd client when lockd is configured
   * and open in the current process. Route handlers on a started
   * lockd-configured app can treat this as present.
   */
  struct lc_client *(*lockd_client)(vectis_app *self);

  /* Declare a Vectis-owned liblockdc consumer service. Vectis copies the
   * consumer config strings needed to materialize the service later; callback
   * function pointers and callback contexts remain borrowed process-local
   * values. The native liblockdc service is created only when the selected app
   * runtime materializes the declaration.
   */
  vectis_status (*consumer_service)(
      vectis_app *self, const struct lc_consumer_service_config *config,
      vectis_consumer_service **out, vectis_error *error);
  vectis_status (*register_consumer_receiver)(
      vectis_app *self, const vectis_consumer_receiver_adapter *adapter,
      vectis_error *error);
  vectis_status (*consumer_service_receiver)(
      vectis_app *self, const vectis_consumer_service_receiver_config *config,
      vectis_consumer_service **out, vectis_error *error);
  void (*close)(vectis_app *self);
  void *impl;
};

struct vectis_consumer_service {
  /* Native liblockdc service for APIs not covered by the Vectis facade. Before
   * materialization, or from a different process than the materialized service,
   * this returns NULL. The returned service remains owned by this handle.
   */
  struct lc_consumer_service *(*native)(vectis_consumer_service *self);
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
  vectis_status (*scp_upload_file)(vectis_ssh *self, const char *local_path,
                                   const char *remote_path,
                                   vectis_error *error);
  vectis_status (*scp_download_file)(vectis_ssh *self, const char *remote_path,
                                     const char *local_path,
                                     vectis_error *error);
  vectis_status (*sftp_stat)(vectis_ssh *self, const char *remote_path,
                             vectis_ssh_sftp_stat_result *result,
                             vectis_error *error);
  vectis_status (*sftp_mkdir)(vectis_ssh *self, const char *remote_path,
                              unsigned long permissions, vectis_error *error);
  vectis_status (*sftp_remove)(vectis_ssh *self, const char *remote_path,
                               vectis_error *error);
  vectis_status (*sftp_rmdir)(vectis_ssh *self, const char *remote_path,
                              vectis_error *error);
  vectis_status (*sftp_rename)(vectis_ssh *self, const char *old_path,
                               const char *new_path, vectis_error *error);
  vectis_status (*sftp_chmod)(vectis_ssh *self, const char *remote_path,
                              unsigned long permissions, vectis_error *error);
  vectis_status (*sftp_open)(vectis_ssh *self, vectis_ssh_sftp_session **out,
                             vectis_error *error);
  void (*close)(vectis_ssh *self);

  /* Shallow effective config copy used by the methods above. */
  vectis_ssh_config config;
  void *impl;
};

struct vectis_ssh_sftp_session {
  vectis_status (*open_file)(vectis_ssh_sftp_session *self,
                             const char *remote_path, unsigned flags,
                             unsigned long permissions,
                             vectis_ssh_sftp_file **out, vectis_error *error);
  vectis_status (*open_dir)(vectis_ssh_sftp_session *self,
                            const char *remote_path, vectis_ssh_sftp_dir **out,
                            vectis_error *error);
  vectis_status (*stat)(vectis_ssh_sftp_session *self, const char *remote_path,
                        vectis_ssh_sftp_stat_result *result,
                        vectis_error *error);
  vectis_status (*mkdir)(vectis_ssh_sftp_session *self, const char *remote_path,
                         unsigned long permissions, vectis_error *error);
  vectis_status (*remove)(vectis_ssh_sftp_session *self,
                          const char *remote_path, vectis_error *error);
  vectis_status (*rmdir)(vectis_ssh_sftp_session *self, const char *remote_path,
                         vectis_error *error);
  vectis_status (*rename)(vectis_ssh_sftp_session *self, const char *old_path,
                          const char *new_path, vectis_error *error);
  vectis_status (*chmod)(vectis_ssh_sftp_session *self, const char *remote_path,
                         unsigned long permissions, vectis_error *error);
  void (*close)(vectis_ssh_sftp_session *self);

  /* Shallow effective config copy used by the methods above. */
  vectis_ssh_config config;
  void *impl;
};

struct vectis_ssh_sftp_file {
  vectis_status (*read)(vectis_ssh_sftp_file *self, void *buffer,
                        size_t capacity, size_t *out_size, vectis_error *error);
  vectis_status (*write)(vectis_ssh_sftp_file *self, const void *data,
                         size_t size, size_t *out_size, vectis_error *error);
  vectis_status (*stat)(vectis_ssh_sftp_file *self,
                        vectis_ssh_sftp_stat_result *result,
                        vectis_error *error);
  void (*close)(vectis_ssh_sftp_file *self);
  void *impl;
};

struct vectis_ssh_sftp_dir {
  vectis_status (*read)(vectis_ssh_sftp_dir *self,
                        vectis_ssh_sftp_dir_entry *entry, vectis_error *error);
  void (*close)(vectis_ssh_sftp_dir *self);
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

/* Bounded FIFO for in-process service handoff. Methods are thread-safe. */
struct vectis_mailbox {
  /* Copy a message into the queue or fail with VECTIS_ERR_CONFLICT when full.
   */
  vectis_status (*publish)(vectis_mailbox *self,
                           const vectis_mailbox_message *message,
                           vectis_error *error);
  /* Publish a request event, assigning a nonzero correlation id when omitted.
   */
  vectis_status (*publish_request)(vectis_mailbox *self,
                                   const vectis_mailbox_message *message,
                                   unsigned long *correlation_id,
                                   vectis_error *error);
  /* Publish a reply event with the supplied nonzero correlation id. */
  vectis_status (*reply)(vectis_mailbox *self, unsigned long correlation_id,
                         const vectis_mailbox_message *message,
                         vectis_error *error);
  /* Non-blocking drain. Returns VECTIS_ERR_TIMEOUT when no event is available.
   */
  vectis_status (*next)(vectis_mailbox *self, vectis_mailbox_event *out,
                        vectis_error *error);
  /* Drain with timeout_ms; negative waits indefinitely, zero polls. */
  vectis_status (*wait_next)(vectis_mailbox *self, vectis_mailbox_event *out,
                             long timeout_ms, vectis_error *error);
  vectis_status (*issue_correlation_id)(vectis_mailbox *self,
                                        unsigned long *out,
                                        vectis_error *error);
  size_t (*depth)(const vectis_mailbox *self);
  vectis_status (*stats)(const vectis_mailbox *self, vectis_mailbox_stats *out,
                         vectis_error *error);
  /* Close rejects new publishes and wakes blocked waiters. */
  void (*close)(vectis_mailbox *self);
  void (*destroy)(vectis_mailbox *self);
  void *impl;
};

/*
 * Request/reply broker over a worker request mailbox. Methods are thread-safe.
 *
 * request() publishes a correlated request, waits on an internal per-request
 * reply mailbox, and always removes that reply mailbox before returning.
 * reply() is the worker-side entry point; it routes by correlation id and
 * returns VECTIS_ERR_TIMEOUT when the request is no longer pending.
 */
struct vectis_mailbox_broker {
  vectis_status (*request)(vectis_mailbox_broker *self,
                           const vectis_mailbox_message *message,
                           long timeout_ms, vectis_mailbox_event *reply,
                           unsigned long *correlation_id, vectis_error *error);
  vectis_status (*reply)(vectis_mailbox_broker *self,
                         unsigned long correlation_id,
                         const vectis_mailbox_message *message,
                         vectis_error *error);
  /* Close wakes pending request waiters; the borrowed request mailbox is left
   * open. */
  void (*close)(vectis_mailbox_broker *self);
  void (*destroy)(vectis_mailbox_broker *self);
  void *impl;
};

/*
 * C-owned OPC UA monitor callback adapter. Pass the callback fields and `user`
 * directly to cpkt_opcua_client_monitor_value*(), monitor_events(), or
 * monitor_event_fields(). The adapter owns no OPC UA client or subscription.
 */
struct vectis_opcua_monitor_mailbox {
  cpkt_opcua_data_change_fn data_change;
  cpkt_opcua_event_fn event;
  cpkt_opcua_event_fields_fn event_fields;
  void *user;
  vectis_status (*stats)(const vectis_opcua_monitor_mailbox *self,
                         vectis_opcua_monitor_mailbox_stats *out,
                         vectis_error *error);
  void (*destroy)(vectis_opcua_monitor_mailbox *self);
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
void vectis_mailbox_config_init(vectis_mailbox_config *config);
void vectis_mailbox_message_init(vectis_mailbox_message *message);
void vectis_mailbox_event_init(vectis_mailbox_event *event);
void vectis_mailbox_event_cleanup(vectis_mailbox_event *event);
void vectis_mailbox_stats_init(vectis_mailbox_stats *stats);
void vectis_mailbox_broker_config_init(vectis_mailbox_broker_config *config);
void vectis_route_event_config_init(vectis_route_event_config *config);
void vectis_route_event_init(vectis_route_event *event);
void vectis_route_event_cleanup(vectis_route_event *event);
vectis_status vectis_mailbox_new(const vectis_mailbox_config *config,
                                 vectis_mailbox **out, vectis_error *error);
vectis_status vectis_mailbox_publish(vectis_mailbox *mailbox,
                                     const vectis_mailbox_message *message,
                                     vectis_error *error);
vectis_status vectis_mailbox_publish_request(
    vectis_mailbox *mailbox, const vectis_mailbox_message *message,
    unsigned long *correlation_id, vectis_error *error);
vectis_status vectis_mailbox_reply(vectis_mailbox *mailbox,
                                   unsigned long correlation_id,
                                   const vectis_mailbox_message *message,
                                   vectis_error *error);
vectis_status vectis_mailbox_next(vectis_mailbox *mailbox,
                                  vectis_mailbox_event *out,
                                  vectis_error *error);
vectis_status vectis_mailbox_wait_next(vectis_mailbox *mailbox,
                                       vectis_mailbox_event *out,
                                       long timeout_ms, vectis_error *error);
vectis_status vectis_mailbox_issue_correlation_id(vectis_mailbox *mailbox,
                                                  unsigned long *out,
                                                  vectis_error *error);
size_t vectis_mailbox_depth(const vectis_mailbox *mailbox);
vectis_status vectis_mailbox_stats_get(const vectis_mailbox *mailbox,
                                       vectis_mailbox_stats *out,
                                       vectis_error *error);
void vectis_mailbox_close(vectis_mailbox *mailbox);
void vectis_mailbox_destroy(vectis_mailbox *mailbox);
vectis_status
vectis_mailbox_broker_new(const vectis_mailbox_broker_config *config,
                          vectis_mailbox_broker **out, vectis_error *error);
vectis_status vectis_mailbox_broker_request(
    vectis_mailbox_broker *broker, const vectis_mailbox_message *message,
    long timeout_ms, vectis_mailbox_event *reply, unsigned long *correlation_id,
    vectis_error *error);
vectis_status vectis_mailbox_broker_reply(vectis_mailbox_broker *broker,
                                          unsigned long correlation_id,
                                          const vectis_mailbox_message *message,
                                          vectis_error *error);
void vectis_mailbox_broker_close(vectis_mailbox_broker *broker);
void vectis_mailbox_broker_destroy(vectis_mailbox_broker *broker);
vectis_status
vectis_route_event_from_request(vectis_request *request,
                                const vectis_route_event_config *config,
                                vectis_route_event *out, vectis_error *error);
vectis_status vectis_route_mailbox_request(
    vectis_mailbox_broker *broker, vectis_request *request,
    vectis_response *response, const vectis_route_event_config *config,
    long timeout_ms, unsigned long *correlation_id, vectis_error *error);
void vectis_lockd_consumer_event_config_init(
    vectis_lockd_consumer_event_config *config);
void vectis_lockd_consumer_event_init(vectis_lockd_consumer_event *event);
void vectis_lockd_consumer_event_cleanup(vectis_lockd_consumer_event *event);
vectis_status vectis_lockd_consumer_event_from_message(
    struct lc_consumer_message *message,
    const vectis_lockd_consumer_event_config *config,
    vectis_lockd_consumer_event *out, vectis_error *error);
void vectis_lockd_consumer_mailbox_receiver_config_init(
    vectis_lockd_consumer_mailbox_receiver_config *config);
vectis_status vectis_lockd_consumer_mailbox_receiver_adapter(
    vectis_consumer_receiver_adapter *out, vectis_error *error);
void vectis_opcua_monitor_event_config_init(
    vectis_opcua_monitor_event_config *config);
void vectis_opcua_monitor_mailbox_config_init(
    vectis_opcua_monitor_mailbox_config *config);
void vectis_opcua_monitor_mailbox_stats_init(
    vectis_opcua_monitor_mailbox_stats *stats);
vectis_status vectis_opcua_monitor_mailbox_new(
    const vectis_opcua_monitor_mailbox_config *config,
    vectis_opcua_monitor_mailbox **out, vectis_error *error);
vectis_status vectis_opcua_monitor_mailbox_stats_get(
    const vectis_opcua_monitor_mailbox *adapter,
    vectis_opcua_monitor_mailbox_stats *out, vectis_error *error);
void vectis_opcua_monitor_mailbox_destroy(
    vectis_opcua_monitor_mailbox *adapter);
void vectis_app_config_init(vectis_app_config *config);
void vectis_server_config_init(vectis_server_config *config);
void vectis_autoblock_config_init(vectis_autoblock_config *config);
void vectis_tls_config_init(vectis_tls_config *config);
void vectis_lockd_config_init(vectis_lockd_config *config);
void vectis_cai_config_init(vectis_cai_config *config);
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
void vectis_cai_mcp_route_config_init(vectis_cai_mcp_route_config *config);
void vectis_json_route_config_init(vectis_json_route_config *config);
void vectis_json_typed_route_config_init(
    vectis_json_typed_route_config *config);
void vectis_xml_route_config_init(vectis_xml_route_config *config);
void vectis_dsv_route_config_init(vectis_dsv_route_config *config);
void vectis_metrics_config_init(vectis_metrics_config *config);
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
vectis_cai_mcp_route_config vectis_cai_mcp_route(const char *path,
                                                 cai_mcp_handler *handler);
vectis_cai_mcp_route_config
vectis_cai_mcp_route_configured(const char *path,
                                const cai_mcp_handler_config *handler_config);
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
void vectis_static_embedded_config_init(vectis_static_embedded_config *config);

vectis_app *vectis_app_new(const vectis_app_config *config,
                           vectis_error *error);
vectis_app *vectis_new(const vectis_app_config *config, vectis_error *error);
void vectis_app_close(vectis_app *app);
void vectis_destroy(vectis_app *app);
vectis_status vectis_start(vectis_app *app, vectis_error *error);
vectis_status vectis_stop(vectis_app *app, vectis_error *error);
vectis_status vectis_run(vectis_app *app, vectis_error *error);
vectis_status vectis_app_wait(vectis_app *app, vectis_error *error);
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
vectis_status
vectis_register_cai_mcp_route(vectis_app *app,
                              const vectis_cai_mcp_route_config *route,
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
vectis_status
vectis_register_static_embedded(vectis_app *app,
                                const vectis_static_embedded_config *config,
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
vectis_status vectis_app_cai_client(vectis_app *app, cai_client **out,
                                    vectis_error *error);
/* Returns the app-owned lockd client after successful runtime startup.
 * Route handlers on a started lockd-configured app can treat this as present.
 * NULL is reserved for invalid apps or pre-start lifecycle inspection.
 */
struct lc_client *vectis_lockd_client(vectis_app *app);
vectis_status
vectis_consumer_service_new(vectis_app *app,
                            const struct lc_consumer_service_config *config,
                            vectis_consumer_service **out, vectis_error *error);
void vectis_consumer_service_receiver_config_init(
    vectis_consumer_service_receiver_config *config);
void vectis_webdav_marker_receiver_config_init(
    vectis_webdav_marker_receiver_config *config);
vectis_status vectis_register_consumer_receiver(
    vectis_app *app, const vectis_consumer_receiver_adapter *adapter,
    vectis_error *error);
vectis_status vectis_consumer_service_new_receiver(
    vectis_app *app, const vectis_consumer_service_receiver_config *config,
    vectis_consumer_service **out, vectis_error *error);
struct lc_consumer_service *
vectis_consumer_service_native(vectis_consumer_service *service);
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
/**
 * Parse DSV rows into caller-owned LoneJSON record storage.
 *
 * This variant is intended for facade/interpreter integrations that can provide
 * correctly prepared record storage, such as Lua-owned LoneJSON records.
 * `schema` must be an ABI-stamped LoneJSON schema view. `storage` is called for
 * each row before fields are assigned. The returned storage is valid only for
 * the subsequent row callback.
 */
vectis_status vectis_dsv_parse_lonejson_view(
    struct lc_source *source, const lonejson_schema_view *schema,
    const vectis_dsv_config *config, vectis_dsv_lonejson_storage_fn storage,
    vectis_dsv_lonejson_row_fn row, void *userdata, vectis_error *error);
vectis_status vectis_dsv_parse_lonejson_view_source(
    const vectis_source *source, const lonejson_schema_view *schema,
    const vectis_dsv_config *config, vectis_dsv_lonejson_storage_fn storage,
    vectis_dsv_lonejson_row_fn row, void *userdata, vectis_error *error);
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
/* Send a live source-backed response. Vectis takes ownership of `source` on
 * success and closes it after the transport finishes or aborts streaming.
 * Unlike vectis_response_source(), this does not spool to a temporary file.
 */
vectis_status vectis_response_stream_source(vectis_response *response,
                                            int status_code,
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
vectis_status vectis_metrics_snapshot_json(vectis_app *app,
                                           vectis_mutable_bytes *out,
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

vectis_status vectis_cai_error(vectis_error *error, const cai_error *cai,
                               const char *fallback_message);
vectis_status vectis_cai_source_from_source(const vectis_source *source,
                                            cai_source **out,
                                            vectis_error *error);
vectis_status vectis_cai_source_from_request(vectis_request *request,
                                             cai_source **out,
                                             vectis_error *error);
vectis_status vectis_cai_output_response(cai_output *output,
                                         vectis_response *response,
                                         int status_code,
                                         const char *content_type,
                                         vectis_error *error);
vectis_status vectis_cai_output_file(cai_output *output, const char *path,
                                     size_t *written, vectis_error *error);
vectis_status vectis_cai_output_enqueue(
    cai_output *output, struct lc_client *client,
    const struct lc_enqueue_req *request, struct lc_enqueue_res *out,
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
/* Return a borrowed header value from response, matched case-insensitively.
 * When a header appears more than once, the last captured value is returned.
 */
const char *vectis_http_response_header(const vectis_http_response *response,
                                        const char *name);
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
void vectis_ssh_sftp_stat_result_init(vectis_ssh_sftp_stat_result *result);
void vectis_ssh_sftp_dir_entry_init(vectis_ssh_sftp_dir_entry *entry);
void vectis_ssh_sftp_dir_entry_cleanup(vectis_ssh_sftp_dir_entry *entry);
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
vectis_status vectis_ssh_scp_upload_file(const vectis_ssh_config *config,
                                         const char *local_path,
                                         const char *remote_path,
                                         vectis_error *error);
vectis_status vectis_ssh_scp_download_file(const vectis_ssh_config *config,
                                           const char *remote_path,
                                           const char *local_path,
                                           vectis_error *error);
vectis_status vectis_ssh_sftp_stat(const vectis_ssh_config *config,
                                   const char *remote_path,
                                   vectis_ssh_sftp_stat_result *result,
                                   vectis_error *error);
vectis_status vectis_ssh_sftp_mkdir(const vectis_ssh_config *config,
                                    const char *remote_path,
                                    unsigned long permissions,
                                    vectis_error *error);
vectis_status vectis_ssh_sftp_remove(const vectis_ssh_config *config,
                                     const char *remote_path,
                                     vectis_error *error);
vectis_status vectis_ssh_sftp_rmdir(const vectis_ssh_config *config,
                                    const char *remote_path,
                                    vectis_error *error);
vectis_status vectis_ssh_sftp_rename(const vectis_ssh_config *config,
                                     const char *old_path, const char *new_path,
                                     vectis_error *error);
vectis_status vectis_ssh_sftp_chmod(const vectis_ssh_config *config,
                                    const char *remote_path,
                                    unsigned long permissions,
                                    vectis_error *error);
vectis_status vectis_ssh_sftp_session_new(const vectis_ssh_config *config,
                                          vectis_ssh_sftp_session **out,
                                          vectis_error *error);
void vectis_ssh_sftp_session_close(vectis_ssh_sftp_session *session);
vectis_status vectis_ssh_sftp_session_open_file(
    vectis_ssh_sftp_session *session, const char *remote_path, unsigned flags,
    unsigned long permissions, vectis_ssh_sftp_file **out, vectis_error *error);
vectis_status vectis_ssh_sftp_session_open_dir(vectis_ssh_sftp_session *session,
                                               const char *remote_path,
                                               vectis_ssh_sftp_dir **out,
                                               vectis_error *error);
vectis_status vectis_ssh_sftp_session_stat(vectis_ssh_sftp_session *session,
                                           const char *remote_path,
                                           vectis_ssh_sftp_stat_result *result,
                                           vectis_error *error);
vectis_status vectis_ssh_sftp_session_mkdir(vectis_ssh_sftp_session *session,
                                            const char *remote_path,
                                            unsigned long permissions,
                                            vectis_error *error);
vectis_status vectis_ssh_sftp_session_remove(vectis_ssh_sftp_session *session,
                                             const char *remote_path,
                                             vectis_error *error);
vectis_status vectis_ssh_sftp_session_rmdir(vectis_ssh_sftp_session *session,
                                            const char *remote_path,
                                            vectis_error *error);
vectis_status vectis_ssh_sftp_session_rename(vectis_ssh_sftp_session *session,
                                             const char *old_path,
                                             const char *new_path,
                                             vectis_error *error);
vectis_status vectis_ssh_sftp_session_chmod(vectis_ssh_sftp_session *session,
                                            const char *remote_path,
                                            unsigned long permissions,
                                            vectis_error *error);
vectis_status vectis_ssh_sftp_file_read(vectis_ssh_sftp_file *file,
                                        void *buffer, size_t capacity,
                                        size_t *out_size, vectis_error *error);
vectis_status vectis_ssh_sftp_file_write(vectis_ssh_sftp_file *file,
                                         const void *data, size_t size,
                                         size_t *out_size, vectis_error *error);
vectis_status vectis_ssh_sftp_file_stat(vectis_ssh_sftp_file *file,
                                        vectis_ssh_sftp_stat_result *result,
                                        vectis_error *error);
void vectis_ssh_sftp_file_close(vectis_ssh_sftp_file *file);
vectis_status vectis_ssh_sftp_dir_read(vectis_ssh_sftp_dir *dir,
                                       vectis_ssh_sftp_dir_entry *entry,
                                       vectis_error *error);
void vectis_ssh_sftp_dir_close(vectis_ssh_sftp_dir *dir);

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
