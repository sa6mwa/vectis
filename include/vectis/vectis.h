#ifndef VECTIS_VECTIS_H
#define VECTIS_VECTIS_H

#include <stddef.h>
#include <pslog.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lc_client;

typedef struct vectis_app vectis_app;
typedef struct vectis_methods vectis_methods;
typedef struct vectis_request vectis_request;
typedef struct vectis_response vectis_response;

typedef enum vectis_status {
  VECTIS_OK = 0,
  VECTIS_ERR_INVALID = 1,
  VECTIS_ERR_NOMEM = 2,
  VECTIS_ERR_STATE = 3,
  VECTIS_ERR_CONFLICT = 4,
  VECTIS_ERR_NOT_IMPLEMENTED = 5
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
  VECTIS_HTTP_DELETE
} vectis_http_method;

typedef struct vectis_error {
  vectis_status code;
  char message[256];
} vectis_error;

typedef struct vectis_lockd_config {
  const char *const *endpoints;
  size_t endpoint_count;
  const char *unix_socket_path;
  const char *client_bundle_path;
  const char *default_namespace;
  long timeout_ms;
} vectis_lockd_config;

typedef struct vectis_tls_config {
  vectis_tls_mode mode;
  const char *bind;
  unsigned short port;
  const char *cert_key_bundle_path;
  const char *certificate_path;
  const char *private_key_path;
  const char *acme_email;
  const char *acme_directory_url;
} vectis_tls_config;

typedef vectis_status (*vectis_route_handler_fn)(vectis_app *app,
                                                 vectis_request *request,
                                                 vectis_response *response,
                                                 void *userdata,
                                                 vectis_error *error);

typedef struct vectis_route_config {
  vectis_http_method method;
  const char *path;
  vectis_route_handler_fn handler;
  void *userdata;
} vectis_route_config;

typedef struct vectis_app_config {
  const char *app_name;
  pslog_logger *logger;
  pslog_mode log_mode;
  pslog_level min_log_level;
  vectis_tls_config tls;
  vectis_lockd_config lockd;
} vectis_app_config;

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
void vectis_app_config_init(vectis_app_config *config);
void vectis_tls_config_init(vectis_tls_config *config);
void vectis_lockd_config_init(vectis_lockd_config *config);

vectis_app *vectis_new(const vectis_app_config *config, vectis_error *error);
void vectis_destroy(vectis_app *app);
vectis_status vectis_start(vectis_app *app, vectis_error *error);
vectis_status vectis_stop(vectis_app *app, vectis_error *error);
vectis_status vectis_register_route(vectis_app *app,
                                    const vectis_route_config *route,
                                    vectis_error *error);
size_t vectis_route_count(const vectis_app *app);
pslog_logger *vectis_logger(vectis_app *app);
vectis_status vectis_json_validate_cstr(const char *json, vectis_error *error);

#ifdef __cplusplus
}
#endif

#endif

