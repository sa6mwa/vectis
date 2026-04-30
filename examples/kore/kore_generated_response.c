#include <stdio.h>

#include <lonejson.h>
#include <vectis/vectis.h>

typedef struct report_response {
  char id[64];
  char status[32];
} report_response;

static const lonejson_field report_response_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(report_response, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(report_response, status, "status", LONEJSON_OVERFLOW_FAIL)};

LONEJSON_MAP_DEFINE(report_response_map, report_response, report_response_fields);

static vectis_status report(vectis_app *app,
                            vectis_request *request,
                            vectis_response *response,
                            void *userdata,
                            vectis_error *error) {
  report_response body;
  const char *id;

  (void)app;
  (void)userdata;
  id = vectis_request_path_param(request, "id");
  if (id == NULL) {
    return vectis_response_status(response, 404, error);
  }
  (void)snprintf(body.id, sizeof(body.id), "%s", id);
  (void)snprintf(body.status, sizeof(body.status), "ready");
  return vectis_response_json_generated(response, 200, &report_response_map, &body, error);
}

int main(void) {
  vectis_app_config config;
  vectis_route_config route;
  vectis_error error;
  vectis_app *app;

  vectis_app_config_init(&config);
  config.app_name = "generated-response-api";
  config.tls.cert_key_bundle = vectis_source_from_path("/etc/vectis/server.pem");

  app = vectis_app_new(&config, &error);
  if (app == NULL) {
    return 1;
  }
  route = vectis_route(VECTIS_HTTP_GET, "/reports/:id", report, NULL);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    app->close(app);
    return 1;
  }
  app->close(app);
  return 0;
}
