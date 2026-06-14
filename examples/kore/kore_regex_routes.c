#include <stdio.h>

#include <pslog.h>
#include <vectis/vectis.h>

static vectis_status report(vectis_app *app, vectis_request *request,
                            vectis_response *response, void *userdata,
                            vectis_error *error) {
  pslog_logger *logger;

  (void)request;
  (void)userdata;
  logger = app->logger(app);
  if (logger != NULL) {
    logger->infof(logger, "example.kore_regex.report", "route=%s",
                  "^/reports/[0-9]+$");
  }
  return vectis_response_text(response, 200, "text/plain", "report\n", error);
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
  config.app_name = "regex-kore-api";
  config.logger = logger;
  config.tls.cert_key_bundle =
      vectis_source_from_path("/etc/vectis/server.pem");

  app = vectis_app_new(&config, &error);
  if (app == NULL) {
    logger->destroy(logger);
    return 1;
  }

  route =
      vectis_route_regex(VECTIS_HTTP_GET, "^/reports/[0-9]+$", report, NULL);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    app->close(app);
    logger->destroy(logger);
    return 1;
  }

  (void)app->start(app, &error);
  app->close(app);
  logger->destroy(logger);
  return 0;
}
