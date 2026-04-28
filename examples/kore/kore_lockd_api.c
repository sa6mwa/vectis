#include <stdio.h>

#include <lc/lc.h>
#include <lonejson.h>
#include <pslog.h>
#include <vectis/vectis.h>

typedef struct state_response {
  char id[64];
  char status[32];
} state_response;

static const lonejson_field state_response_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(state_response, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(state_response, status, "status", LONEJSON_OVERFLOW_FAIL)};

LONEJSON_MAP_DEFINE(state_response_map, state_response, state_response_fields);

static vectis_status get_state(vectis_app *app,
                               vectis_request *request,
                               vectis_response *response,
                               void *userdata,
                               vectis_error *error) {
  const char *id;
  lc_client *lockd;
  pslog_logger *logger;
  state_response state;

  (void)userdata;
  id = vectis_request_path_param(request, "id");
  if (id == NULL) {
    id = "1001";
  }
  lockd = vectis_lockd_client(app);
  logger = vectis_logger(app);
  if (logger != NULL) {
    logger->infof(logger, "example.kore_lockd.get_state", "id=%s", id);
  }
  (void)lockd;
  (void)snprintf(state.id, sizeof(state.id), "%s", id);
  (void)snprintf(state.status, sizeof(state.status), "%s", "loaded");
  return vectis_response_json(response, 200, &state_response_map, &state, error);
}

int main(void) {
  vectis_app_config config;
  vectis_route_config route;
  vectis_error error;
  vectis_app *app;
  pslog_config log_config;
  pslog_logger *logger;
  const char *endpoints[] = {"https://127.0.0.1:8443"};

  pslog_default_config(&log_config);
  log_config.mode = PSLOG_MODE_JSON;
  log_config.min_level = PSLOG_LEVEL_INFO;
  log_config.output = pslog_output_from_fp(stderr, 0);
  logger = pslog_new(&log_config);
  if (logger == NULL) {
    return 1;
  }

  vectis_app_config_init(&config);
  config.app_name = "lockd-api";
  config.logger = logger;
  config.tls.cert_key_bundle = vectis_source_from_path("/etc/vectis/server.pem");
  config.lockd.endpoints = endpoints;
  config.lockd.endpoint_count = 1u;
  config.lockd.client_bundle = vectis_source_from_path("/etc/vectis/lockd-client.pem");
  config.lockd.default_namespace = "examples";

  app = vectis_new(&config, &error);
  if (app == NULL) {
    logger->destroy(logger);
    return 1;
  }
  route = vectis_route(VECTIS_HTTP_GET, "/state/:id?", get_state, NULL);
  (void)vectis_register_route(app, &route, &error);
  logger->infof(logger, "example.kore_lockd.start",
                "endpoint=%s namespace=%s", endpoints[0],
                config.lockd.default_namespace);
  (void)vectis_start(app, &error);
  vectis_destroy(app);
  logger->destroy(logger);
  return 0;
}
