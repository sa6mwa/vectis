#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <lonejson.h>
#include <pslog.h>
#include <vectis/vectis.h>

typedef struct order_request {
  char id[64];
  char status[32];
} order_request;

typedef struct order_response {
  char id[64];
  char saved[8];
} order_response;

static const lonejson_field order_request_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(order_request, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(order_request, status, "status", LONEJSON_OVERFLOW_FAIL)};

static const lonejson_field order_response_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(order_response, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(order_response, saved, "saved", LONEJSON_OVERFLOW_FAIL)};

LONEJSON_MAP_DEFINE(order_request_map, order_request, order_request_fields);
LONEJSON_MAP_DEFINE(order_response_map, order_response, order_response_fields);

static pslog_logger *new_scoped_logger(const char *component,
                                       const pslog_palette *palette,
                                       pslog_logger **root_out) {
  pslog_config log_config;
  pslog_logger *root;
  pslog_logger *scoped;

  if (root_out != NULL) {
    *root_out = NULL;
  }
  pslog_default_config(&log_config);
  log_config.mode = PSLOG_MODE_CONSOLE;
  log_config.min_level = PSLOG_LEVEL_INFO;
  log_config.output = pslog_output_from_fp(stderr, 0);
  log_config.palette = palette;
  root = pslog_new(&log_config);
  if (root == NULL) {
    return NULL;
  }
  scoped = root->withf(root, "component=%s", component);
  if (scoped == NULL) {
    root->destroy(root);
    return NULL;
  }
  if (root_out != NULL) {
    *root_out = root;
  }
  return scoped;
}

static vectis_status save_order(vectis_app *app,
                                vectis_request *request,
                                vectis_response *response,
                                void *userdata,
                                vectis_error *error) {
  order_request input;
  order_response output;
  struct lc_client *lockd;
  pslog_logger *logger;
  vectis_status status;
  char key[128];

  (void)userdata;
  input.id[0] = '\0';
  input.status[0] = '\0';
  output.id[0] = '\0';
  output.saved[0] = '\0';

  status = vectis_request_json_into(request, &order_request_map, &input, error);
  if (status != VECTIS_OK) {
    return status;
  }

  lockd = app->lockd_client(app);
  logger = app->logger(app);
  if (logger != NULL) {
    logger->infof(logger, "example.rest_lockd.save_order", "id=%s", input.id);
  }

  if (lockd == NULL) {
    return vectis_response_error_json(response,
                                      503,
                                      "lockd_unavailable",
                                      "lockd client is not configured",
                                      "",
                                      error);
  }
  status = vectis_format_key(key, sizeof(key), error, "orders/%s", input.id);
  if (status != VECTIS_OK) {
    return vectis_response_error_json(response,
                                      400,
                                      "invalid_order_id",
                                      "order id cannot be used as a lockd key",
                                      error != NULL ? error->message : "",
                                      error);
  }
  status = vectis_lockd_state_save(lockd,
                                   key,
                                   "orders-api",
                                   30L,
                                   &order_request_map,
                                   &input,
                                   error);
  if (status != VECTIS_OK) {
    return vectis_response_error_json(response,
                                      503,
                                      "order_save_failed",
                                      "failed to save order state",
                                      error != NULL ? error->message : "",
                                      error);
  }

  (void)snprintf(output.id, sizeof(output.id), "%s", input.id);
  (void)snprintf(output.saved, sizeof(output.saved), "%s", "true");
  return vectis_response_json(response, 200, &order_response_map, &output, error);
}

int main(void) {
  vectis_app_config config;
  vectis_route_config route;
  vectis_error error;
  vectis_app *app;
  pslog_logger *root_logger;
  pslog_logger *logger;
  pslog_logger *lockd_root_logger;
  pslog_logger *lockd_logger;
  const char *endpoints[] = {"https://lockd.internal:8443"};
  const char bundle[] =
      "-----BEGIN CERTIFICATE-----\n"
      "example\n"
      "-----END CERTIFICATE-----\n";

  vectis_app_config_init(&config);
  root_logger = NULL;
  lockd_root_logger = NULL;
  logger = new_scoped_logger("kore", pslog_palette_default(), &root_logger);
  lockd_logger = new_scoped_logger("lockd", &pslog_builtin_palette_horizon, &lockd_root_logger);
  if (logger == NULL || lockd_logger == NULL) {
    if (lockd_logger != NULL) {
      lockd_logger->destroy(lockd_logger);
    }
    if (lockd_root_logger != NULL) {
      lockd_root_logger->destroy(lockd_root_logger);
    }
    if (logger != NULL) {
      logger->destroy(logger);
    }
    if (root_logger != NULL) {
      root_logger->destroy(root_logger);
    }
    return 1;
  }

  config.app_name = "orders-api";
  config.logger = logger;
  config.tls.cert_key_bundle = vectis_source_from_path("/etc/vectis/server.pem");
  config.lockd.endpoints = endpoints;
  config.lockd.endpoint_count = 1u;
  config.lockd.client_bundle = vectis_source_from_memory(bundle, sizeof(bundle) - 1u);
  config.lockd.default_namespace = "orders";
  config.lockd.logger = lockd_logger;

  app = vectis_app_new(&config, &error);
  if (app == NULL) {
    logger->destroy(logger);
    lockd_logger->destroy(lockd_logger);
    lockd_root_logger->destroy(lockd_root_logger);
    root_logger->destroy(root_logger);
    return 1;
  }

  route = vectis_json_body_route(VECTIS_HTTP_POST, "/orders", save_order, NULL);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    app->close(app);
    logger->destroy(logger);
    lockd_logger->destroy(lockd_logger);
    lockd_root_logger->destroy(lockd_root_logger);
    root_logger->destroy(root_logger);
    return 1;
  }

  app->close(app);
  logger->destroy(logger);
  lockd_logger->destroy(lockd_logger);
  lockd_root_logger->destroy(lockd_root_logger);
  root_logger->destroy(root_logger);
  return 0;
}
