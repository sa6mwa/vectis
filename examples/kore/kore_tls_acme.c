#include <stdio.h>

#include <pslog.h>
#include <vectis/vectis.h>

int main(void) {
  vectis_app_config manual;
  vectis_app_config acme;
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

  vectis_app_config_init(&manual);
  manual.app_name = "manual-tls-api";
  manual.logger = logger;
  manual.tls.mode = VECTIS_TLS_MODE_MANUAL;
  manual.tls.bind = "0.0.0.0";
  manual.tls.port = 8443u;
  manual.tls.certificate_path = "/etc/vectis/server.crt";
  manual.tls.private_key_path = "/etc/vectis/server.key";
  manual.lockd.unix_socket_path = "/run/lockd.sock";

  vectis_app_config_init(&acme);
  acme.app_name = "acme-api";
  acme.logger = logger;
  acme.tls.mode = VECTIS_TLS_MODE_ACME;
  acme.tls.bind = "0.0.0.0";
  acme.tls.port = 443u;
  acme.tls.acme_email = "ops@example.com";
  acme.lockd.unix_socket_path = "/run/lockd.sock";

  logger->infof(logger, "example.kore_tls.manual",
                "app=%s cert=%s key=%s", manual.app_name,
                manual.tls.certificate_path, manual.tls.private_key_path);
  logger->infof(logger, "example.kore_tls.acme",
                "app=%s email=%s directory=%s", acme.app_name,
                acme.tls.acme_email, acme.tls.acme_directory_url);
  logger->destroy(logger);
  return 0;
}
