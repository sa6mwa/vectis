#include <assert.h>
#include <string.h>
#include "vectis_internal.h"
#include <vectis/vectis.h>

static vectis_status sample_handler(vectis_app *app,
                                    vectis_request *request,
                                    vectis_response *response,
                                    void *userdata,
                                    vectis_error *error) {
  (void)app;
  (void)request;
  (void)response;
  (void)userdata;
  vectis_error_clear(error);
  return VECTIS_OK;
}

int main(void) {
  vectis_app_config config;
  vectis_error error;
  vectis_app *app;
  vectis_status status;
  vectis_route_config bad_route;
  vectis_route_config route;

  vectis_app_config_init(&config);

  app = vectis_new(&config, &error);
  assert(app != NULL);
  assert(vectis_internal_lockd_client(app) == NULL);

  status = vectis_start(app, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "manual TLS requires") != NULL);
  vectis_destroy(app);

  config.tls.cert_key_bundle = vectis_source_from_path("/tmp/server.pem");
  app = vectis_new(&config, &error);
  assert(app != NULL);
  assert(vectis_internal_lockd_client(app) == NULL);

  status = vectis_start(app, &error);
  assert(status == VECTIS_ERR_NOT_IMPLEMENTED);
  assert(strstr(error.message, "not implemented") != NULL);

  vectis_destroy(app);
  config.server.request_header_timeout_ms = 0L;
  app = vectis_new(&config, &error);
  assert(app == NULL);
  assert(strstr(error.message, "request_header_timeout_ms") != NULL);
  vectis_app_config_init(&config);
  config.tls.cert_key_bundle = vectis_source_from_path("/tmp/server.pem");
  config.lockd.unix_socket_path = "/tmp/lockd.sock";
  config.server.keepalive_max_requests = 0u;
  app = vectis_new(&config, &error);
  assert(app == NULL);
  assert(strstr(error.message, "keepalive_max_requests") != NULL);
  vectis_app_config_init(&config);
  config.tls.cert_key_bundle = vectis_source_from_path("/tmp/server.pem");
  config.lockd.unix_socket_path = "/tmp/lockd.sock";
  config.server.keepalive_enabled = 0;
  config.server.keepalive_timeout_ms = 0L;
  config.server.keepalive_max_requests = 0u;
  app = vectis_new(&config, &error);
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

  route = vectis_route_methods(VECTIS_HTTP_METHODS_NONE, "/bad-method", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "HTTP method") != NULL);

  route = vectis_route_methods(VECTIS_HTTP_METHODS_OPTIONS | VECTIS_HTTP_METHODS_HEAD,
                               "/multi",
                               sample_handler,
                               NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_OPTIONS, "/multi", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_CONFLICT);

  route = vectis_upload_route(VECTIS_HTTP_POST, "/upload", sample_handler, NULL);
  assert(route.body.mode == VECTIS_BODY_STREAMING_UPLOAD);
  assert(route.body.max_bytes == VECTIS_BODY_DEFAULT_UPLOAD_MAX_BYTES);
  assert(route.body.spool_to_disk == 1);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);

  route = vectis_upload_route_max(VECTIS_HTTP_POST,
                                  "/bad-upload",
                                  (size_t)1024u,
                                  sample_handler,
                                  NULL);
  route.body.spool_to_disk = 0;
  route.body.memory_buffer_limit_bytes = 512u;
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "spool_to_disk") != NULL);

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

  vectis_destroy(app);
  return 0;
}
