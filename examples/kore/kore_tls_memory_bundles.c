#include <stdio.h>

#include <pslog.h>
#include <vectis/vectis.h>

static vectis_status health(vectis_app *app, vectis_request *request,
                            vectis_response *response, void *userdata,
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
  const char server_bundle[] = "-----BEGIN CERTIFICATE-----\n"
                               "server certificate pem\n"
                               "-----END CERTIFICATE-----\n"
                               "-----BEGIN PRIVATE KEY-----\n"
                               "server private key pem\n"
                               "-----END PRIVATE KEY-----\n";
  const char client_ca_bundle[] = "-----BEGIN CERTIFICATE-----\n"
                                  "client ca certificate pem\n"
                                  "-----END CERTIFICATE-----\n";
  const char upstream_ca_bundle[] = "-----BEGIN CERTIFICATE-----\n"
                                    "ca certificate pem\n"
                                    "-----END CERTIFICATE-----\n";

  pslog_default_config(&log_config);
  log_config.mode = PSLOG_MODE_JSON;
  log_config.min_level = PSLOG_LEVEL_INFO;
  log_config.output = pslog_output_from_fp(stderr, 0);
  logger = pslog_new(&log_config);
  if (logger == NULL) {
    return 1;
  }

  vectis_app_config_init(&config);
  config.app_name = "memory-tls-api";
  config.logger = logger;
  config.tls.cert_key_bundle =
      vectis_source_from_memory(server_bundle, sizeof(server_bundle) - 1u);
  config.tls.ca_bundle = vectis_source_from_memory(
      upstream_ca_bundle, sizeof(upstream_ca_bundle) - 1u);
  config.tls.client_ca_bundle = vectis_source_from_memory(
      client_ca_bundle, sizeof(client_ca_bundle) - 1u);
  config.tls.require_client_certificate = 1;

  app = vectis_app_new(&config, &error);
  if (app == NULL) {
    logger->destroy(logger);
    return 1;
  }

  route = vectis_route(VECTIS_HTTP_GET, "/health", health, NULL);
  (void)app->route(app, &route, &error);

  logger->infof(logger, "example.kore_tls_memory.start",
                "app=%s require_client_certificate=%d", config.app_name,
                config.tls.require_client_certificate);
  (void)app->run(app, &error);
  app->close(app);
  logger->destroy(logger);
  return 0;
}
