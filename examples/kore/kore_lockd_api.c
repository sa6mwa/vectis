#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static vectis_status get_state(vectis_app *app,
                               vectis_request *request,
                               vectis_response *response,
                               void *userdata,
                               vectis_error *error) {
  const char *id;
  lc_client *lockd;
  pslog_logger *logger;
  state_response state;
  state_response loaded;
  lc_acquire_req acquire;
  lc_release_req release;
  lc_get_res get_response;
  lc_lease *lease;
  char key[128];
  lc_error lockd_error;

  (void)userdata;
  memset(&state, 0, sizeof(state));
  memset(&loaded, 0, sizeof(loaded));
  memset(&get_response, 0, sizeof(get_response));
  lc_acquire_req_init(&acquire);
  lc_release_req_init(&release);
  lc_error_init(&lockd_error);
  lease = NULL;

  id = vectis_request_path_param(request, "id");
  if (id == NULL) {
    id = "1001";
  }
  lockd = vectis_lockd_client(app);
  logger = vectis_logger(app);
  if (logger != NULL) {
    logger->infof(logger, "example.kore_lockd.get_state", "id=%s", id);
  }
  if (lockd == NULL) {
    return VECTIS_ERR_STATE;
  }
  (void)snprintf(state.id, sizeof(state.id), "%s", id);
  (void)snprintf(state.status, sizeof(state.status), "%s", "loaded");
  (void)snprintf(key, sizeof(key), "state/%s", id);

  acquire.key = key;
  acquire.owner = "vectis-kore-lockd-example";
  acquire.ttl_seconds = 30L;
  if (lc_acquire(lockd, &acquire, &lease, &lockd_error) != LC_OK) {
    lc_error_cleanup(&lockd_error);
    return VECTIS_ERR_STATE;
  }
  if (lease->save(lease, &state_response_map, &state, NULL, &lockd_error) != LC_OK) {
    lc_lease_close(lease);
    lc_error_cleanup(&lockd_error);
    return VECTIS_ERR_STATE;
  }
  if (lease->load(lease, &state_response_map, &loaded, NULL, NULL, &get_response, &lockd_error) != LC_OK) {
    lc_lease_close(lease);
    lc_get_res_cleanup(&get_response);
    lc_error_cleanup(&lockd_error);
    return VECTIS_ERR_STATE;
  }
  lc_get_res_cleanup(&get_response);
  if (lease->release(lease, &release, &lockd_error) != LC_OK) {
    lc_lease_close(lease);
    lc_error_cleanup(&lockd_error);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lockd_error);
  return vectis_response_json(response, 200, &state_response_map, &loaded, error);
}

int main(void) {
  vectis_app_config config;
  vectis_route_config route;
  vectis_error error;
  vectis_app *app;
  pslog_config log_config;
  pslog_logger *logger;
  const char *endpoints[1];

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

  app = vectis_new(&config, &error);
  if (app == NULL) {
    (void)print_error("vectis_new", &error);
    logger->destroy(logger);
    return 1;
  }
  route = vectis_route(VECTIS_HTTP_GET, "/state/:id?", get_state, NULL);
  if (vectis_register_route(app, &route, &error) != VECTIS_OK) {
    (void)print_error("vectis_register_route", &error);
    vectis_destroy(app);
    logger->destroy(logger);
    return 1;
  }
  logger->infof(logger, "example.kore_lockd.start",
                "endpoint=%s namespace=%s", endpoints[0],
                config.lockd.default_namespace);
  if (vectis_start(app, &error) != VECTIS_OK) {
    (void)print_error("vectis_start", &error);
    vectis_destroy(app);
    logger->destroy(logger);
    return 1;
  }
  serve_forever();
  vectis_destroy(app);
  logger->destroy(logger);
  return 0;
}
