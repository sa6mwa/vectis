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
  vectis_status status;

  vectis_app_config_init(&config);
  assert(strcmp(config.app_name, "vectis") == 0);
  assert(config.tls.mode == VECTIS_TLS_MODE_MANUAL);
  assert(config.tls.port == 8443u);
  assert(config.lockd.timeout_ms == 30000L);

  config.app_name = "orders";
  config.tls.cert_key_bundle_path = "/tmp/orders.pem";
  config.lockd.unix_socket_path = "/tmp/lockd.sock";

  app = vectis_new(&config, &error);
  assert(app != NULL);
  assert(app->vt != NULL);
  assert(vectis_logger(app) != NULL);
  assert(vectis_internal_lockd_client(app) != NULL);

  route.method = VECTIS_HTTP_POST;
  route.path = "/orders";
  route.handler = sample_handler;
  route.userdata = NULL;

  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  assert(vectis_route_count(app) == 1u);

  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_ERR_CONFLICT);
  assert(strstr(error.message, "duplicate route registration") != NULL);

  vectis_destroy(app);
  return 0;
}
