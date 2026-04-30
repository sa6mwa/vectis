#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static const char *env_or_default(const char *name, const char *fallback) {
  const char *value;

  value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return fallback;
  }
  return value;
}

static unsigned short env_port_or_default(const char *name, unsigned short fallback) {
  const char *value;
  long port;

  value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return fallback;
  }
  port = strtol(value, NULL, 10);
  if (port <= 0L || port > 65535L) {
    return fallback;
  }
  return (unsigned short)port;
}

static void serve_forever(void) {
  for (;;) {
    (void)sleep(3600u);
  }
}

static int print_error(const char *operation, const vectis_error *error) {
  fprintf(stderr, "%s failed", operation);
  if (error != NULL && error->message[0] != '\0') {
    fprintf(stderr, ": %s", error->message);
  }
  if (error != NULL && error->detail[0] != '\0') {
    fprintf(stderr, " (%s)", error->detail);
  }
  fprintf(stderr, "\n");
  return 1;
}

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

static vectis_status get_state(vectis_app *app,
                               vectis_request *request,
                               vectis_response *response,
                               void *userdata,
                               vectis_error *error) {
  const char *id;
  struct lc_client *lockd;
  pslog_logger *logger;
  state_response state = {"", ""};
  state_response loaded = {"", ""};
  char key[128];
  vectis_status status;

  (void)userdata;
  id = vectis_request_path_param(request, "id");
  if (id == NULL) {
    id = "1001";
  }
  lockd = app->lockd_client(app);
  logger = app->logger(app);
  if (logger != NULL) {
    logger->infof(logger, "example.kore_lockd.get_state", "id=%s", id);
  }
  if (lockd == NULL) {
    return vectis_response_error_json(response,
                                      503,
                                      "lockd_unavailable",
                                      "lockd client is not configured",
                                      "",
                                      error);
  }
  (void)snprintf(state.id, sizeof(state.id), "%s", id);
  (void)snprintf(state.status, sizeof(state.status), "%s", "loaded");
  status = vectis_format_key(key, sizeof(key), error, "state/%s", id);
  if (status != VECTIS_OK) {
    return vectis_response_error_json(response,
                                      400,
                                      "invalid_state_id",
                                      "state id cannot be used as a lockd key",
                                      error != NULL ? error->message : "",
                                      error);
  }

  status = vectis_lockd_state_save(lockd,
                                   key,
                                   "vectis-kore-lockd-example",
                                   30L,
                                   &state_response_map,
                                   &state,
                                   error);
  if (status == VECTIS_OK) {
    status = vectis_lockd_state_load(lockd,
                                     key,
                                     "vectis-kore-lockd-example",
                                     30L,
                                     &state_response_map,
                                     &loaded,
                                     error);
  }
  if (status != VECTIS_OK) {
    return vectis_response_error_json(response,
                                      503,
                                      "lockd_state_failed",
                                      "failed to save or load lockd state",
                                      error != NULL ? error->message : "",
                                      error);
  }
  return vectis_response_json(response, 200, &state_response_map, &loaded, error);
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
  const char *endpoints[1];

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

  vectis_app_config_init(&config);
  config.app_name = "lockd-api";
  config.logger = logger;
  config.tls.bind = env_or_default("VECTIS_KORE_BIND", "127.0.0.1");
  config.tls.port = env_port_or_default("VECTIS_KORE_PORT", 28081u);
  if (env_or_default("VECTIS_KORE_TLS", "manual")[0] == 'd') {
    config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  } else {
    config.tls.cert_key_bundle = vectis_source_from_path(
        env_or_default("VECTIS_KORE_CERT_BUNDLE", "/etc/vectis/server.pem"));
  }
  endpoints[0] = env_or_default("LOCKD_ENDPOINT", "https://127.0.0.1:8443");
  config.lockd.endpoints = endpoints;
  config.lockd.endpoint_count = 1u;
  config.lockd.client_bundle = vectis_source_from_path(
      env_or_default("LOCKD_CLIENT_BUNDLE", "/etc/vectis/lockd-client.pem"));
  config.lockd.default_namespace = "examples";
  config.lockd.logger = lockd_logger;

  app = vectis_app_new(&config, &error);
  if (app == NULL) {
    (void)print_error("vectis_app_new", &error);
    logger->destroy(logger);
    lockd_logger->destroy(lockd_logger);
    lockd_root_logger->destroy(lockd_root_logger);
    root_logger->destroy(root_logger);
    return 1;
  }
  route = vectis_route(VECTIS_HTTP_GET, "/state/:id?", get_state, NULL);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    (void)print_error("app->route", &error);
    app->close(app);
    logger->destroy(logger);
    lockd_logger->destroy(lockd_logger);
    lockd_root_logger->destroy(lockd_root_logger);
    root_logger->destroy(root_logger);
    return 1;
  }
  logger->infof(logger, "example.kore_lockd.start",
                "endpoint=%s namespace=%s", endpoints[0],
                config.lockd.default_namespace);
  if (app->start(app, &error) != VECTIS_OK) {
    (void)print_error("app->start", &error);
    app->close(app);
    logger->destroy(logger);
    lockd_logger->destroy(lockd_logger);
    lockd_root_logger->destroy(lockd_root_logger);
    root_logger->destroy(root_logger);
    return 1;
  }
  serve_forever();
  app->close(app);
  logger->destroy(logger);
  lockd_logger->destroy(lockd_logger);
  lockd_root_logger->destroy(lockd_root_logger);
  root_logger->destroy(root_logger);
  return 0;
}
