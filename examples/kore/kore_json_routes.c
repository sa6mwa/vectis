#include <stdio.h>
#include <string.h>

#include <lonejson.h>
#include <pslog.h>
#include <vectis/vectis.h>

typedef struct echo_request {
  char message[128];
} echo_request;

typedef struct echo_response {
  char message[128];
} echo_response;

static const lonejson_field echo_request_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(echo_request, message, "message", LONEJSON_OVERFLOW_FAIL)};

static const lonejson_field echo_response_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(echo_response, message, "message", LONEJSON_OVERFLOW_FAIL)};

LONEJSON_MAP_DEFINE(echo_request_map, echo_request, echo_request_fields);
LONEJSON_MAP_DEFINE(echo_response_map, echo_response, echo_response_fields);

static vectis_status echo(vectis_app *app,
                          vectis_request *request,
                          void *input,
                          void *output,
                          void *userdata,
                          vectis_error *error) {
  echo_request *in;
  echo_response *out;

  (void)app;
  (void)request;
  (void)userdata;
  (void)error;
  in = (echo_request *)input;
  out = (echo_response *)output;
  (void)snprintf(out->message, sizeof(out->message), "%s", in->message);
  return VECTIS_OK;
}

int main(void) {
  vectis_app_config config;
  vectis_json_route_config route;
  vectis_error error;
  vectis_app *app;
  pslog_config log_config;
  pslog_logger *logger;

  pslog_default_config(&log_config);
  log_config.mode = PSLOG_MODE_JSON;
  log_config.min_level = PSLOG_LEVEL_INFO;
  log_config.output = pslog_output_from_fp(stderr, 0);
  logger = pslog_new(&log_config);
  if (logger == NULL) {
    return 1;
  }

  vectis_app_config_init(&config);
  config.app_name = "json-kore-api";
  config.logger = logger;
  config.tls.cert_key_bundle = vectis_source_from_path("/etc/vectis/server.pem");

  app = vectis_new(&config, &error);
  if (app == NULL) {
    logger->destroy(logger);
    return 1;
  }

  route = vectis_json_route(VECTIS_HTTP_POST,
                            "/echo",
                            &echo_request_map,
                            sizeof(echo_request),
                            &echo_response_map,
                            sizeof(echo_response),
                            echo,
                            NULL);

  logger->infof(logger, "example.kore_json.register", "method=%s path=%s",
                "POST", route.path);
  (void)vectis_register_json_route(app, &route, &error);
  vectis_destroy(app);
  logger->destroy(logger);
  return 0;
}
