#include <stdio.h>

#include <pslog.h>
#include <vectis/vectis.h>

static vectis_status upload_file(vectis_app *app,
                                 vectis_request *request,
                                 vectis_response *response,
                                 void *userdata,
                                 vectis_error *error) {
  pslog_logger *logger;
  const char *name;
  vectis_body_spill_config spill_config;
  vectis_body_spill_result spill;
  vectis_status status;

  (void)userdata;
  logger = vectis_logger(app);
  name = vectis_request_path_param(request, "name");
  vectis_body_spill_config_init(&spill_config);
  spill_config.memory_limit_bytes = 1024u * 1024u;
  spill_config.prefix = "vectis-upload";
  status = vectis_request_body_spill(request, &spill_config, &spill, error);
  if (status != VECTIS_OK) {
    return status;
  }
  if (logger != NULL) {
    logger->infof(logger,
                  "example.kore_file_upload.accept",
                  "name=%s bytes=%lu spooled=%d path=%s",
                  name != NULL ? name : "unnamed",
                  (unsigned long)spill.size,
                  spill.spooled_to_disk,
                  spill.path != NULL ? spill.path : "");
  }
  if (spill.spooled_to_disk && spill.path != NULL) {
    (void)remove(spill.path);
  }
  vectis_body_spill_result_cleanup(&spill);
  return vectis_response_text(response, 202, "text/plain", "accepted\n", error);
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
  config.app_name = "upload-api";
  config.logger = logger;
  config.tls.cert_key_bundle = vectis_source_from_path("/etc/vectis/server.pem");

  app = vectis_new(&config, &error);
  if (app == NULL) {
    logger->destroy(logger);
    return 1;
  }

  route = vectis_upload_route(VECTIS_HTTP_POST, "/files/:name", upload_file, NULL);
  if (vectis_register_route(app, &route, &error) != VECTIS_OK) {
    vectis_destroy(app);
    logger->destroy(logger);
    return 1;
  }

  logger->infof(logger,
                "example.kore_file_upload.start",
                "body_mode=%s max_body=%lu min_rate=%lu",
                vectis_body_mode_string(route.body.mode),
                (unsigned long)route.body.max_bytes,
                (unsigned long)route.body.min_rate_bytes_per_sec);
  (void)vectis_start(app, &error);
  vectis_destroy(app);
  logger->destroy(logger);
  return 0;
}
