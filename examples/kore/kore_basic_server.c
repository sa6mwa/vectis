#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <pslog.h>
#include <vectis/vectis.h>

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

static vectis_status health(vectis_app *app,
                            vectis_request *request,
                            vectis_response *response,
                            void *userdata,
                            vectis_error *error) {
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 200, "text/plain", "ok\n", error);
}

int main(void) {
  vectis_app_config config;
  vectis_route_config route;
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
  config.app_name = "basic-kore-api";
  config.logger = logger;
  config.tls.bind = env_or_default("VECTIS_KORE_BIND", "127.0.0.1");
  config.tls.port = env_port_or_default("VECTIS_KORE_PORT", 28080u);
  if (env_or_default("VECTIS_KORE_TLS", "manual")[0] == 'd') {
    config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  } else {
    config.tls.cert_key_bundle = vectis_source_from_path(
        env_or_default("VECTIS_KORE_CERT_BUNDLE", "/etc/vectis/server.pem"));
  }

  app = vectis_new(&config, &error);
  if (app == NULL) {
    (void)print_error("vectis_new", &error);
    logger->destroy(logger);
    return 1;
  }

  route = vectis_route_methods(VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD,
                               "/health",
                               health,
                               NULL);
  if (vectis_register_route(app, &route, &error) != VECTIS_OK) {
    (void)print_error("vectis_register_route", &error);
    vectis_destroy(app);
    logger->destroy(logger);
    return 1;
  }

  logger->infof(logger, "example.kore_basic.start", "bind=%s port=%u",
                config.tls.bind, (unsigned)config.tls.port);
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
