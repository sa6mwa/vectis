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
  vectis_route_config route;
  vectis_route_config param_route;
  vectis_status status;

  vectis_app_config_init(&config);
  assert(strcmp(config.app_name, "vectis") == 0);
  assert(config.tls.mode == VECTIS_TLS_MODE_MANUAL);
  assert(config.tls.port == 8443u);
  assert(strcmp(config.tls.acme_directory_url, VECTIS_ACME_DIRECTORY_LETSENCRYPT_PRODUCTION) == 0);
  assert(config.server.max_connections == VECTIS_SERVER_DEFAULT_MAX_CONNECTIONS);
  assert(config.server.max_request_header_bytes == VECTIS_SERVER_DEFAULT_MAX_REQUEST_HEADER_BYTES);
  assert(config.server.max_request_body_bytes == VECTIS_SERVER_DEFAULT_MAX_REQUEST_BODY_BYTES);
  assert(config.server.request_header_timeout_ms == VECTIS_SERVER_DEFAULT_REQUEST_HEADER_TIMEOUT_MS);
  assert(config.server.request_body_idle_timeout_ms ==
         VECTIS_SERVER_DEFAULT_REQUEST_BODY_IDLE_TIMEOUT_MS);
  assert(config.server.response_write_idle_timeout_ms ==
         VECTIS_SERVER_DEFAULT_RESPONSE_WRITE_IDLE_TIMEOUT_MS);
  assert(config.server.request_body_min_rate_bytes_per_sec ==
         VECTIS_SERVER_DEFAULT_REQUEST_BODY_MIN_RATE_BYTES_PER_SEC);
  assert(config.server.request_body_min_rate_grace_ms ==
         VECTIS_SERVER_DEFAULT_REQUEST_BODY_MIN_RATE_GRACE_MS);
  assert(config.server.idle_timeout_ms == VECTIS_SERVER_DEFAULT_IDLE_TIMEOUT_MS);
  assert(config.server.keepalive_enabled == 1);
  assert(config.server.keepalive_timeout_ms == VECTIS_SERVER_DEFAULT_KEEPALIVE_TIMEOUT_MS);
  assert(config.server.keepalive_max_requests == VECTIS_SERVER_DEFAULT_KEEPALIVE_MAX_REQUESTS);
  assert(config.lockd.timeout_ms == 30000L);
  vectis_error_clear(&error);
  assert(error.source == VECTIS_ERROR_SOURCE_NONE);
  assert(strcmp(vectis_error_source_string(VECTIS_ERROR_SOURCE_CURL), "curl") == 0);

  config.app_name = "orders";
  config.tls.cert_key_bundle = vectis_source_from_path("/tmp/orders.pem");
  config.lockd.unix_socket_path = "/tmp/lockd.sock";

  app = vectis_new(&config, &error);
  assert(app != NULL);
  assert(app->vt != NULL);
  assert(vectis_logger(app) != NULL);
  assert(vectis_internal_lockd_client(app) == NULL);

  route = vectis_route(VECTIS_HTTP_POST, "/orders", sample_handler, NULL);
  assert(route.methods == VECTIS_HTTP_METHODS_POST);
  assert(route.body.mode == VECTIS_BODY_NONE);
  route.body = vectis_body_json_default();
  assert(route.body.mode == VECTIS_BODY_JSON);
  assert(route.body.max_bytes == VECTIS_SERVER_DEFAULT_MAX_REQUEST_BODY_BYTES);

  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  assert(vectis_route_count(app) == 1u);

  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_CONFLICT);
  assert(strstr(error.message, "duplicate route registration") != NULL);

  route = vectis_route_methods(VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD,
                               "/health",
                               sample_handler,
                               NULL);
  assert(route.method == VECTIS_HTTP_GET);
  assert((route.methods & VECTIS_HTTP_METHODS_HEAD) != 0u);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  assert(vectis_route_count(app) == 2u);

  param_route = vectis_route(VECTIS_HTTP_HEAD, "/orders", sample_handler, NULL);
  status = vectis_register_route(app, &param_route, &error);
  assert(status == VECTIS_OK);
  assert(vectis_route_count(app) == 3u);

  param_route = vectis_route(VECTIS_HTTP_GET,
                             "/orders/:id/items/:item_id",
                             sample_handler,
                             NULL);
  assert(param_route.path_kind == VECTIS_ROUTE_PATH_PARAMS);
  status = vectis_register_route(app, &param_route, &error);
  assert(status == VECTIS_OK);
  assert(vectis_route_count(app) == 4u);

  param_route = vectis_route(VECTIS_HTTP_GET,
                             "/orders/:id?/items/:item_id?",
                             sample_handler,
                             NULL);
  status = vectis_register_route(app, &param_route, &error);
  assert(status == VECTIS_OK);
  assert(vectis_route_count(app) == 5u);

  vectis_destroy(app);
  return 0;
}
