#include <assert.h>
#include <string.h>
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
  const char *endpoints[] = {"https://lockd.example.test:9341"};
  vectis_route_config bad_route;

  vectis_app_config_init(&config);

  app = vectis_new(&config, &error);
  assert(app != NULL);

  status = vectis_start(app, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "manual TLS requires") != NULL);
  vectis_destroy(app);

  config.tls.cert_key_bundle_path = "/tmp/server.pem";
  config.lockd.endpoints = endpoints;
  config.lockd.endpoint_count = 1u;
  config.lockd.client_bundle_path = "/tmp/client.pem";
  app = vectis_new(&config, &error);
  assert(app != NULL);

  status = vectis_start(app, &error);
  assert(status == VECTIS_ERR_NOT_IMPLEMENTED);
  assert(strstr(error.message, "not implemented") != NULL);

  bad_route.method = VECTIS_HTTP_GET;
  bad_route.path = "missing-slash";
  bad_route.handler = sample_handler;
  bad_route.userdata = NULL;
  status = vectis_register_route(app, &bad_route, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "route path must start with '/'") != NULL);

  status = vectis_json_validate_cstr("{\"ok\":true}", &error);
  assert(status == VECTIS_OK);

  status = vectis_json_validate_cstr("{\"ok\":", &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "invalid json") != NULL);

  status = vectis_stop(app, &error);
  assert(status == VECTIS_ERR_STATE);

  vectis_destroy(app);
  return 0;
}

