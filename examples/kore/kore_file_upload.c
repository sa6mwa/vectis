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
  vectis_body_materialize_config body_config;
  vectis_body_materialized body;
  vectis_status status;

  (void)userdata;
  logger = vectis_logger(app);
  name = vectis_request_path_param(request, "name");
  vectis_body_materialize_config_init(&body_config);
  body_config.memory_limit_bytes = 1024u * 1024u;
  body_config.prefix = "vectis-upload";
  status = vectis_request_body_materialize(request, &body_config, &body, error);
  if (status != VECTIS_OK) {
    return status;
  }
  if (logger != NULL) {
    logger->infof(logger,
                  "example.kore_file_upload.accept",
                  "name=%s bytes=%lu storage=%s path=%s",
                  name != NULL ? name : "unnamed",
                  (unsigned long)body.size,
                  body.kind == VECTIS_BODY_MATERIALIZED_FILE ? "file" : "memory",
                  body.path != NULL ? body.path : "");
  }
  if (body.kind == VECTIS_BODY_MATERIALIZED_FILE && body.path != NULL) {
    (void)remove(body.path);
  }
  vectis_body_materialized_cleanup(&body);
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
  config.server.max_request_body_bytes = VECTIS_BODY_DEFAULT_UPLOAD_MAX_BYTES;

  app = vectis_app_new(&config, &error);
  if (app == NULL) {
    logger->destroy(logger);
    return 1;
  }

  route = vectis_upload_route(VECTIS_HTTP_POST, "/files/:name", upload_file, NULL);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    app->close(app);
    logger->destroy(logger);
    return 1;
  }

  logger->infof(logger,
                "example.kore_file_upload.start",
                "body_mode=%s max_body=%lu min_rate=%lu",
                vectis_body_mode_string(route.body.mode),
                (unsigned long)route.body.max_bytes,
                (unsigned long)route.body.min_rate_bytes_per_sec);
  (void)app->start(app, &error);
  app->close(app);
  logger->destroy(logger);
  return 0;
}
