#include <stdlib.h>
#include <stdio.h>

#include <lc/lc.h>
#include <pslog.h>
#include <vectis/vectis.h>

typedef struct consumer_context {
  pslog_logger *logger;
} consumer_context;

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

static int handle_order(void *context,
                        lc_consumer_message *message,
                        lc_error *error) {
  consumer_context *ctx;

  (void)error;
  ctx = (consumer_context *)context;
  ctx->logger->infof(ctx->logger,
                     "example.vectis_consumer.delivery",
                     "consumer=%s queue=%s message=%s",
                     message->name != NULL ? message->name : "",
                     message->queue != NULL ? message->queue : "",
                     message->message != NULL && message->message->message_id != NULL
                         ? message->message->message_id
                         : "");
  return LC_OK;
}

int main(void) {
  vectis_app_config app_config;
  vectis_app *app;
  vectis_consumer_service *service;
  vectis_error error;
  lc_consumer_config consumer;
  lc_consumer_service_config service_config;
  pslog_logger *root_logger;
  pslog_logger *logger;
  pslog_logger *lockd_root_logger;
  pslog_logger *lockd_logger;
  consumer_context context;
  const char *endpoints[1];
  const char *socket_path;
  const char *endpoint;
  const char *bundle_path;
  int rc;

  root_logger = NULL;
  lockd_root_logger = NULL;
  logger = new_scoped_logger("consumer", pslog_palette_default(), &root_logger);
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
    fputs("failed to allocate logger\n", stderr);
    return 1;
  }

  vectis_app_config_init(&app_config);
  lc_consumer_config_init(&consumer);
  lc_consumer_service_config_init(&service_config);
  app = NULL;
  service = NULL;
  rc = 1;

  socket_path = getenv("LOCKD_SOCKET");
  endpoint = getenv("LOCKD_ENDPOINT");
  bundle_path = getenv("LOCKD_CLIENT_BUNDLE");
  app_config.app_name = "vectis-consumer";
  app_config.logger = logger;
  app_config.lockd.logger = lockd_logger;
  app_config.lockd.default_namespace = "examples";
  if (socket_path != NULL && socket_path[0] != '\0') {
    app_config.lockd.unix_socket_path = socket_path;
  } else {
    endpoints[0] = endpoint != NULL && endpoint[0] != '\0'
                       ? endpoint
                       : "https://127.0.0.1:8443";
    app_config.lockd.endpoints = endpoints;
    app_config.lockd.endpoint_count = 1u;
    app_config.lockd.client_bundle = vectis_source_from_path(bundle_path);
  }

  app = vectis_new(&app_config, &error);
  if (app == NULL) {
    logger->errorf(logger,
                   "example.vectis_consumer.new_failed",
                   "source=%s message=%s",
                   vectis_error_source_string(error.source),
                   error.message);
    logger->destroy(logger);
    lockd_logger->destroy(lockd_logger);
    lockd_root_logger->destroy(lockd_root_logger);
    root_logger->destroy(root_logger);
    return 1;
  }

  context.logger = logger;
  consumer.name = "orders-worker";
  consumer.request.queue = "orders";
  consumer.request.owner = "orders-worker";
  consumer.request.visibility_timeout_seconds = 30L;
  consumer.worker_count = 2u;
  consumer.handle = handle_order;
  consumer.context = &context;
  service_config.consumers = &consumer;
  service_config.consumer_count = 1u;

  if (vectis_consumer_service_new(app, &service_config, &service, &error) != VECTIS_OK) {
    logger->errorf(logger,
                   "example.vectis_consumer.create_failed",
                   "source=%s code=%d dependency=%ld http_status=%ld message=%s detail=%s",
                   vectis_error_source_string(error.source),
                   (int)error.code,
                   error.dependency_code,
                   error.http_status,
                   error.message,
                   error.detail);
  } else if (vectis_consumer_service_run(service, &error) != VECTIS_OK) {
    logger->errorf(logger,
                   "example.vectis_consumer.run_failed",
                   "source=%s code=%d dependency=%ld http_status=%ld message=%s detail=%s",
                   vectis_error_source_string(error.source),
                   (int)error.code,
                   error.dependency_code,
                   error.http_status,
                   error.message,
                   error.detail);
  } else {
    rc = 0;
  }

  vectis_consumer_service_destroy(service);
  vectis_destroy(app);
  logger->destroy(logger);
  lockd_logger->destroy(lockd_logger);
  lockd_root_logger->destroy(lockd_root_logger);
  root_logger->destroy(root_logger);
  return rc;
}
