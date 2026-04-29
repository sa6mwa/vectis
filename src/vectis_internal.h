#ifndef VECTIS_INTERNAL_H
#define VECTIS_INTERNAL_H

#include <vectis/vectis.h>

struct http_request;

typedef struct vectis_kore_runtime_config {
  vectis_app *app;
  const char *app_name;
  const char *bind;
  unsigned short port;
  const char *domain;
  vectis_tls_mode tls_mode;
  const char *acme_email;
  const char *acme_directory_url;
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
  pslog_logger *logger;
  char *runtime_certfile;
  char *runtime_certkey;
  char *runtime_client_ca_file;
  int runtime_certfile_temporary;
  int runtime_certkey_temporary;
  int runtime_client_ca_temporary;
} vectis_kore_runtime_config;

void vectis_set_error(vectis_error *error, vectis_status code, const char *message);
struct lc_client *vectis_internal_lockd_client(vectis_app *app);
vectis_status vectis_internal_kore_start(const vectis_kore_runtime_config *config,
                                         vectis_error *error);
vectis_status vectis_internal_kore_stop(vectis_app *app, vectis_error *error);
vectis_status vectis_internal_invoke_route(vectis_app *app,
                                           size_t index,
                                           vectis_request *request,
                                           vectis_response *response,
                                           vectis_error *error);
vectis_status vectis_internal_dispatch_route(vectis_app *app,
                                             vectis_http_method method,
                                             const char *path,
                                             vectis_request *request,
                                             vectis_response *response,
                                             vectis_error *error);
vectis_status vectis_internal_route_body_policy(vectis_app *app,
                                                vectis_http_method method,
                                                const char *path,
                                                vectis_body_policy *policy,
                                                vectis_error *error);

vectis_request *vectis_internal_request_new(vectis_error *error);
void vectis_internal_request_init(vectis_request *request);
void vectis_internal_request_cleanup(vectis_request *request);
void vectis_internal_request_free(vectis_request *request);
void vectis_internal_request_set_method(vectis_request *request,
                                        vectis_http_method method);
vectis_status vectis_internal_request_set_body(vectis_request *request,
                                               const void *body,
                                               size_t body_size,
                                               vectis_error *error);
vectis_status vectis_internal_request_set_body_path(vectis_request *request,
                                                    const char *body_path,
                                                    size_t body_size,
                                                    vectis_error *error);
vectis_status vectis_internal_request_set_body_reader(vectis_request *request,
                                                      struct lc_source *source,
                                                      size_t body_size,
                                                      int owned,
                                                      const vectis_body_policy *policy,
                                                      vectis_error *error);
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
const char *vectis_internal_response_content_type(const vectis_response *response);
vectis_bytes vectis_internal_response_body(const vectis_response *response);
const char *vectis_internal_response_file_path(const vectis_response *response);
int vectis_internal_response_file_temporary(const vectis_response *response);
size_t vectis_internal_response_header_count(const vectis_response *response);
const char *vectis_internal_response_header_name(const vectis_response *response,
                                                 size_t index);
const char *vectis_internal_response_header_value(const vectis_response *response,
                                                  size_t index);

#endif
