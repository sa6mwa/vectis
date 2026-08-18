#ifndef VECTIS_INTERNAL_H
#define VECTIS_INTERNAL_H

#include <sys/types.h>

#include <vectis/auth.h>
#include <vectis/vectis.h>

struct http_request;

typedef struct vectis_kore_runtime_config {
  vectis_app *app;
  const char *app_name;
  const char *bind;
  unsigned short port;
  int http_redirect_enabled;
  const char *http_redirect_bind;
  unsigned short http_redirect_port;
  const char *domain;
  const char *const *domains;
  size_t domain_count;
  vectis_tls_mode tls_mode;
  vectis_tls_version tls_version;
  const char *tls_cipher_list;
  const char *acme_email;
  const char *acme_directory_url;
  const char *acme_storage_endpoint;
  const char *acme_storage_namespace;
  const char *acme_storage_key;
  const char *acme_state_dir;
  const char *lockd_client_bundle_path;
  const void *lockd_client_bundle_pem;
  size_t lockd_client_bundle_pem_size;
  const char *pouch_crypto_key;
  const char *pouch_crypto_key_file;
  int pouch_crypto_generate_key_file;
  int pouch_crypto_generate_key_file_set;
  const char *pouch_compression;
  const char *cert_key_bundle_path;
  const void *cert_key_bundle_pem;
  size_t cert_key_bundle_pem_size;
  struct lc_source *cert_key_bundle_source;
  const char *certificate_path;
  const void *certificate_pem;
  size_t certificate_pem_size;
  struct lc_source *certificate_source;
  const char *private_key_path;
  const void *private_key_pem;
  size_t private_key_pem_size;
  struct lc_source *private_key_source;
  const char *ca_bundle_path;
  const void *ca_bundle_pem;
  size_t ca_bundle_pem_size;
  struct lc_source *ca_bundle_source;
  const char *client_ca_bundle_path;
  const void *client_ca_bundle_pem;
  size_t client_ca_bundle_pem_size;
  struct lc_source *client_ca_bundle_source;
  int require_client_certificate;
  vectis_server_config server;
  size_t body_disk_offload_bytes;
  int body_disk_offload_configured;
  pslog_logger *logger;
  char *runtime_certfile;
  char *runtime_certkey;
  char *runtime_client_ca_file;
  int runtime_certfile_temporary;
  int runtime_certkey_temporary;
  int runtime_client_ca_temporary;
  /* Private supervisor control channel. The child writes READY and listens for
   * STOP frames; the supervisor owns the other end.
   */
  int control_fd;
  int *app_ready;
} vectis_kore_runtime_config;

typedef enum vectis_runtime_control_type {
  VECTIS_RUNTIME_CONTROL_READY = 1,
  VECTIS_RUNTIME_CONTROL_STOP = 2,
  VECTIS_RUNTIME_CONTROL_SERVICE_FAILURE = 3,
  VECTIS_RUNTIME_CONTROL_METRICS = 4
} vectis_runtime_control_type;

typedef struct vectis_upload_stream_runtime {
  vectis_body_policy policy;
  vectis_upload_write_fn write;
  vectis_upload_finish_fn finish;
  vectis_upload_close_fn close;
  void *userdata;
  void *state;
  int opened;
} vectis_upload_stream_runtime;

typedef struct vectis_internal_websocket_match {
  vectis_websocket_connect_fn connect;
  vectis_websocket_message_fn message;
  vectis_websocket_disconnect_fn disconnect;
  void *userdata;
} vectis_internal_websocket_match;

typedef struct vectis_internal_runtime_observation {
  unsigned long child_ready_sequence;
  unsigned long metrics_start_sequence;
  unsigned long metrics_stop_sequence;
  unsigned long managed_start_sequence;
  unsigned long managed_stop_sequence;
  unsigned long consumer_start_sequence;
  unsigned long consumer_stop_sequence;
} vectis_internal_runtime_observation;

void vectis_set_error(vectis_error *error, vectis_status code,
                      const char *message);
struct lc_client *vectis_internal_lockd_client(vectis_app *app);
int vectis_internal_kore_autoblock_mutex_recovers_worker_death(void);
pid_t vectis_internal_kore_child_pid(vectis_app *app);
void vectis_internal_runtime_observe(
    vectis_app *app, vectis_internal_runtime_observation *observation);
size_t vectis_internal_worker_count(vectis_app *app);
size_t vectis_internal_request_limit(vectis_app *app);
size_t vectis_internal_worker_accept_threshold(vectis_app *app);
size_t vectis_internal_worker_rlimit_nofiles(vectis_app *app);
unsigned vectis_internal_kore_curl_timeout_seconds(vectis_app *app);
size_t vectis_internal_kore_curl_recv_max_bytes(vectis_app *app);
int vectis_internal_kore_quiet(vectis_app *app);
int vectis_internal_worker_set_affinity_disabled(vectis_app *app);
long vectis_internal_worker_shutdown_timeout_ms(vectis_app *app);
vectis_worker_death_policy vectis_internal_worker_death_policy(vectis_app *app);
size_t vectis_internal_max_request_body_bytes(vectis_app *app);
size_t vectis_internal_body_disk_offload_bytes(vectis_app *app,
                                               int *configured);
const char *vectis_internal_request_body_spool_dir(vectis_app *app);
vectis_status vectis_internal_kore_run(const vectis_kore_runtime_config *config,
                                       vectis_error *error);
vectis_status
vectis_internal_kore_validate(const vectis_kore_runtime_config *config,
                              vectis_error *error);
vectis_status vectis_internal_kore_stop(vectis_app *app, vectis_error *error);
int vectis_internal_kore_signal_requested(void);
int vectis_internal_kore_signal_number(void);
vectis_status
vectis_internal_runtime_control_write(int fd, vectis_runtime_control_type type,
                                      const void *payload, size_t payload_size,
                                      vectis_error *error);
vectis_status vectis_internal_runtime_control_read(
    int fd, vectis_runtime_control_type *type_out,
    vectis_mutable_bytes *payload, vectis_error *error);
#if defined(VECTIS_BUILD_FUZZERS)
void vectis_internal_kore_fuzzer_set_app(vectis_app *app);
#endif
vectis_status vectis_internal_invoke_route(vectis_app *app, size_t index,
                                           vectis_request *request,
                                           vectis_response *response,
                                           vectis_error *error);
vectis_status
vectis_internal_dispatch_route(vectis_app *app, vectis_http_method method,
                               const char *path, vectis_request *request,
                               vectis_response *response, vectis_error *error);
vectis_status
vectis_internal_match_websocket(vectis_app *app, vectis_http_method method,
                                const char *path, vectis_request *request,
                                vectis_internal_websocket_match *match,
                                vectis_error *error);
void vectis_internal_metrics_note_http_status(vectis_app *app, int status);
void vectis_internal_metrics_note_route_miss(vectis_app *app);
void vectis_internal_metrics_note_body_reject(vectis_app *app, int status);
void vectis_internal_metrics_note_auth(vectis_app *app,
                                       vectis_auth_action action);
vectis_status vectis_internal_route_body_policy(vectis_app *app,
                                                vectis_http_method method,
                                                const char *path,
                                                vectis_body_policy *policy,
                                                vectis_error *error);
vectis_status
vectis_internal_upload_stream_open(vectis_app *app, vectis_http_method method,
                                   const char *path, vectis_request *request,
                                   vectis_upload_stream_runtime *stream,
                                   vectis_error *error);
vectis_status
vectis_internal_upload_stream_write(vectis_app *app, vectis_request *request,
                                    vectis_upload_stream_runtime *stream,
                                    const void *data, size_t size,
                                    vectis_error *error);
vectis_status vectis_internal_upload_stream_finish(
    vectis_app *app, vectis_request *request, vectis_response *response,
    vectis_upload_stream_runtime *stream, vectis_error *error);
void vectis_internal_upload_stream_close(vectis_app *app,
                                         vectis_request *request,
                                         vectis_upload_stream_runtime *stream);

vectis_request *vectis_internal_request_new(vectis_error *error);
void vectis_internal_request_init(vectis_request *request);
void vectis_internal_request_cleanup(vectis_request *request);
void vectis_internal_request_free(vectis_request *request);
void vectis_internal_request_set_method(vectis_request *request,
                                        vectis_http_method method);
vectis_status vectis_internal_request_set_path(vectis_request *request,
                                               const char *path,
                                               vectis_error *error);
vectis_status vectis_internal_request_set_body(vectis_request *request,
                                               const void *body,
                                               size_t body_size,
                                               vectis_error *error);
vectis_status vectis_internal_request_set_body_path(vectis_request *request,
                                                    const char *body_path,
                                                    size_t body_size,
                                                    vectis_error *error);
vectis_status vectis_internal_request_set_body_reader(
    vectis_request *request, struct lc_source *source, size_t body_size,
    int owned, const vectis_body_policy *policy, vectis_error *error);
void vectis_internal_request_set_streaming_upload(
    vectis_request *request, const vectis_body_policy *policy);
void vectis_internal_request_set_kore(vectis_request *request,
                                      struct http_request *kore_request);
vectis_status vectis_internal_request_add_path_param(vectis_request *request,
                                                     const char *name,
                                                     const char *value,
                                                     vectis_error *error);
vectis_status vectis_internal_request_add_query(vectis_request *request,
                                                const char *name,
                                                const char *value,
                                                vectis_error *error);
vectis_status vectis_internal_request_add_header(vectis_request *request,
                                                 const char *name,
                                                 const char *value,
                                                 vectis_error *error);

vectis_response *vectis_internal_response_new(vectis_error *error);
void vectis_internal_response_init(vectis_response *response);
void vectis_internal_response_cleanup(vectis_response *response);
void vectis_internal_response_free(vectis_response *response);
int vectis_internal_response_status_code(const vectis_response *response);
const char *
vectis_internal_response_content_type(const vectis_response *response);
vectis_bytes vectis_internal_response_body(const vectis_response *response);
const char *vectis_internal_response_file_path(const vectis_response *response);
int vectis_internal_response_file_temporary(const vectis_response *response);
struct lc_source *
vectis_internal_response_take_stream_source(vectis_response *response);
size_t vectis_internal_response_header_count(const vectis_response *response);
const char *
vectis_internal_response_header_name(const vectis_response *response,
                                     size_t index);
const char *
vectis_internal_response_header_value(const vectis_response *response,
                                      size_t index);

#endif
