#include "vectis_internal.h"
#include <assert.h>
#include <string.h>
#include <vectis/vectis.h>

static vectis_status sample_handler(vectis_app *app, vectis_request *request,
                                    vectis_response *response, void *userdata,
                                    vectis_error *error) {
  (void)app;
  (void)request;
  (void)response;
  (void)userdata;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status sample_json_handler(vectis_app *app,
                                         vectis_request *request, void *input,
                                         void *output, void *userdata,
                                         vectis_error *error) {
  (void)app;
  (void)request;
  (void)input;
  (void)output;
  (void)userdata;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status sample_json_typed_handler(
    vectis_app *app, vectis_request *request, void *input,
    vectis_json_response *response, void *userdata, vectis_error *error) {
  (void)app;
  (void)request;
  (void)input;
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
  vectis_json_route_config json_route;
  vectis_json_typed_route_config typed_json_route;
  vectis_route_config param_route;
  vectis_status status;
  const char *bad_endpoints[1];

  vectis_app_config_init(&config);
  assert(strcmp(config.app_name, "vectis") == 0);
  assert(config.tls.mode == VECTIS_TLS_MODE_MANUAL);
  assert(config.tls.port == 8443u);
  assert(strcmp(config.tls.domain, "*") == 0);
  assert(strcmp(config.tls.acme_directory_url,
                VECTIS_ACME_DIRECTORY_LETSENCRYPT_PRODUCTION) == 0);
  assert(config.server.max_connections ==
         VECTIS_SERVER_DEFAULT_MAX_CONNECTIONS);
  assert(config.server.max_request_header_bytes ==
         VECTIS_SERVER_DEFAULT_MAX_REQUEST_HEADER_BYTES);
  assert(config.server.max_request_body_bytes == 0u);
  assert(config.server.request_header_timeout_ms ==
         VECTIS_SERVER_DEFAULT_REQUEST_HEADER_TIMEOUT_MS);
  assert(config.server.request_body_idle_timeout_ms ==
         VECTIS_SERVER_DEFAULT_REQUEST_BODY_IDLE_TIMEOUT_MS);
  assert(config.server.response_write_idle_timeout_ms ==
         VECTIS_SERVER_DEFAULT_RESPONSE_WRITE_IDLE_TIMEOUT_MS);
  assert(config.server.request_body_min_rate_bytes_per_sec ==
         VECTIS_SERVER_DEFAULT_REQUEST_BODY_MIN_RATE_BYTES_PER_SEC);
  assert(config.server.request_body_min_rate_grace_ms ==
         VECTIS_SERVER_DEFAULT_REQUEST_BODY_MIN_RATE_GRACE_MS);
  assert(config.server.idle_timeout_ms ==
         VECTIS_SERVER_DEFAULT_IDLE_TIMEOUT_MS);
  assert(config.server.keepalive_disabled == 0);
  assert(config.server.keepalive_timeout_ms ==
         VECTIS_SERVER_DEFAULT_KEEPALIVE_TIMEOUT_MS);
  assert(config.server.keepalive_max_requests ==
         VECTIS_SERVER_DEFAULT_KEEPALIVE_MAX_REQUESTS);
  assert(config.lockd.timeout_ms == 30000L);
  assert(config.lockd.logger == NULL);
  assert(config.lockd.logger_disabled == 0);
  vectis_error_clear(&error);
  assert(error.source == VECTIS_ERROR_SOURCE_NONE);
  assert(strcmp(vectis_error_source_string(VECTIS_ERROR_SOURCE_CURL), "curl") ==
         0);
  assert(strcmp(vectis_http_method_string(VECTIS_HTTP_OPTIONS), "OPTIONS") ==
         0);
  assert(strcmp(vectis_body_mode_string(VECTIS_BODY_STREAMING_UPLOAD),
                "streaming_upload") == 0);

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.port = 0u;
  config.server.max_connections = 0u;
  config.server.max_request_header_bytes = 0u;
  config.server.max_request_body_bytes = 0u;
  config.server.request_header_timeout_ms = 0L;
  config.server.request_body_idle_timeout_ms = 0L;
  config.server.response_write_idle_timeout_ms = 0L;
  config.server.request_body_min_rate_bytes_per_sec = 0u;
  config.server.request_body_min_rate_grace_ms = 0L;
  config.server.idle_timeout_ms = 0L;
  config.server.keepalive_timeout_ms = 0L;
  config.server.keepalive_max_requests = 0u;
  config.lockd.timeout_ms = 0L;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_POST, "/zero-default-body", sample_handler,
                       NULL);
  route.body = vectis_body_buffered_max(0u);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  app->close(app);

  vectis_app_config_init(&config);
  config.lockd.endpoint_count = 1u;
  config.lockd.endpoints = NULL;
  app = vectis_app_new(&config, &error);
  assert(app == NULL);
  assert(strstr(error.message, "lockd endpoints") != NULL);

  bad_endpoints[0] = NULL;
  vectis_app_config_init(&config);
  config.lockd.endpoint_count = 1u;
  config.lockd.endpoints = bad_endpoints;
  app = vectis_app_new(&config, &error);
  assert(app == NULL);
  assert(strstr(error.message, "lockd endpoints") != NULL);

  bad_endpoints[0] = "";
  vectis_app_config_init(&config);
  config.lockd.endpoint_count = 1u;
  config.lockd.endpoints = bad_endpoints;
  app = vectis_app_new(&config, &error);
  assert(app == NULL);
  assert(strstr(error.message, "lockd endpoints") != NULL);

  vectis_app_config_init(&config);
  config.app_name = "orders";
  config.tls.cert_key_bundle = vectis_source_from_path("/tmp/orders.pem");
  config.lockd.unix_socket_path = "/tmp/lockd.sock";

  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  assert(app->start != NULL);
  assert(app->route != NULL);
  assert(app->close != NULL);
  assert(vectis_logger(app) != NULL);
  assert(vectis_internal_lockd_client(app) == NULL);

  route = vectis_route(VECTIS_HTTP_POST, "/orders", sample_handler, NULL);
  assert(route.methods == VECTIS_HTTP_METHODS_POST);
  assert(route.body.mode == VECTIS_BODY_NONE);
  route.body = vectis_body_json_default();
  assert(route.body.mode == VECTIS_BODY_JSON);
  assert(route.body.max_bytes == VECTIS_SERVER_DEFAULT_MAX_REQUEST_BODY_BYTES);
  assert(route.body.memory_buffer_limit_bytes ==
         VECTIS_BODY_DEFAULT_MEMORY_BUFFER_LIMIT_BYTES);
  assert(route.body.disk_spool_disabled == 0);

  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  assert(vectis_route_count(app) == 1u);

  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_CONFLICT);
  assert(strstr(error.message, "duplicate route registration") != NULL);

  route =
      vectis_route_methods(VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD,
                           "/health", sample_handler, NULL);
  assert(route.method == VECTIS_HTTP_GET);
  assert((route.methods & VECTIS_HTTP_METHODS_HEAD) != 0u);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  assert(vectis_route_count(app) == 2u);

  json_route = vectis_json_route_methods(VECTIS_HTTP_METHODS_NONE,
                                         "/json-empty-methods", NULL, 0u, NULL,
                                         0u, sample_json_handler, NULL);
  status = vectis_register_json_route(app, &json_route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "at least one HTTP method") != NULL);
  assert(vectis_route_count(app) == 2u);

  typed_json_route =
      vectis_json_typed_route_methods(VECTIS_HTTP_METHODS_NONE,
                                      "/typed-json-empty-methods", NULL, 0u,
                                      sample_json_typed_handler, NULL);
  status = vectis_register_json_typed_route(app, &typed_json_route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "at least one HTTP method") != NULL);
  assert(vectis_route_count(app) == 2u);

  param_route = vectis_route(VECTIS_HTTP_HEAD, "/orders", sample_handler, NULL);
  status = vectis_register_route(app, &param_route, &error);
  assert(status == VECTIS_OK);
  assert(vectis_route_count(app) == 3u);

  param_route = vectis_route(VECTIS_HTTP_GET, "/orders/:id/items/:item_id",
                             sample_handler, NULL);
  assert(param_route.path_kind == VECTIS_ROUTE_PATH_PARAMS);
  status = vectis_register_route(app, &param_route, &error);
  assert(status == VECTIS_OK);
  assert(vectis_route_count(app) == 4u);

  param_route = vectis_route(VECTIS_HTTP_GET, "/orders/:id?/items/:item_id?",
                             sample_handler, NULL);
  status = vectis_register_route(app, &param_route, &error);
  assert(status == VECTIS_OK);
  assert(vectis_route_count(app) == 5u);

  app->close(app);

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_ACME;
  config.tls.acme_email = "ops@example.com";
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/acme-domain", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "tls.domain") != NULL);
  app->close(app);

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_ACME;
  config.tls.domain = "api.example.com";
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/acme-email", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "acme_email") != NULL);
  app->close(app);

  return 0;
}
