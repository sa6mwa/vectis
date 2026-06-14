#include <stdio.h>
#include <string.h>

#include <lonejson.h>
#include <pslog.h>
#include <vectis/vectis.h>

typedef struct order_request {
  char id[64];
} order_request;

typedef struct order_created {
  char id[64];
  char status[32];
} order_created;

typedef struct api_error {
  char code[32];
  char message[128];
} api_error;

static const lonejson_field order_request_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(order_request, id, "id",
                                    LONEJSON_OVERFLOW_FAIL)};

static const lonejson_field order_created_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(order_created, id, "id",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(order_created, status, "status",
                                    LONEJSON_OVERFLOW_FAIL)};

static const lonejson_field api_error_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(api_error, code, "code",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(api_error, message, "message",
                                    LONEJSON_OVERFLOW_FAIL)};

LONEJSON_MAP_DEFINE(order_request_map, order_request, order_request_fields);
LONEJSON_MAP_DEFINE(order_created_map, order_created, order_created_fields);
LONEJSON_MAP_DEFINE(api_error_map, api_error, api_error_fields);

static vectis_status create_order(vectis_app *app, vectis_request *request,
                                  void *input, vectis_json_response *response,
                                  void *userdata, vectis_error *error) {
  order_request *in;
  order_created created;
  api_error problem;

  (void)app;
  (void)request;
  (void)userdata;
  in = (order_request *)input;
  memset(&created, 0, sizeof(created));
  memset(&problem, 0, sizeof(problem));
  if (in == NULL || in->id[0] == '\0') {
    (void)snprintf(problem.code, sizeof(problem.code), "invalid_order");
    (void)snprintf(problem.message, sizeof(problem.message),
                   "order id is required");
    return vectis_json_reply(response, 422, &api_error_map, &problem, error);
  }
  if (strcmp(in->id, "existing") == 0) {
    (void)snprintf(problem.code, sizeof(problem.code), "order_conflict");
    (void)snprintf(problem.message, sizeof(problem.message),
                   "order already exists");
    return vectis_json_reply(response, 409, &api_error_map, &problem, error);
  }
  (void)snprintf(created.id, sizeof(created.id), "%s", in->id);
  (void)snprintf(created.status, sizeof(created.status), "created");
  return vectis_json_reply(response, 201, &order_created_map, &created, error);
}

int main(void) {
  vectis_app_config config;
  vectis_json_typed_route_config route;
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
  config.app_name = "json-multi-output-api";
  config.logger = logger;
  config.tls.cert_key_bundle =
      vectis_source_from_path("/etc/vectis/server.pem");

  app = vectis_app_new(&config, &error);
  if (app == NULL) {
    logger->destroy(logger);
    return 1;
  }

  route =
      vectis_json_typed_route(VECTIS_HTTP_POST, "/orders", &order_request_map,
                              sizeof(order_request), create_order, NULL);
  logger->infof(logger, "example.kore_json_multi_output.register", "path=%s",
                route.path);
  (void)app->json_typed_route(app, &route, &error);
  app->close(app);
  logger->destroy(logger);
  return 0;
}
