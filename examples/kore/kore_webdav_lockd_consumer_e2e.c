#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lc/lc.h>
#include <pslog.h>
#include <vectis/vectis.h>
#include <vectis/webdav.h>

typedef struct combined_config {
  const char *endpoint;
  const char *bundle_path;
  const char *namespace_name;
  const char *queue;
  const char *bind;
  const char *webdav_cache_dir;
  unsigned short port;
} combined_config;

typedef struct combined_context {
  combined_config config;
  vectis_webdav_config webdav_storage;
  lc_consumer_config consumer;
  lc_consumer_service_config service_config;
  vectis_consumer_service *service;
  int service_started;
} combined_context;

static const lc_enqueue_res lc_enqueue_res_zero;

static const char *env_or_default(const char *name, const char *fallback) {
  const char *value;

  value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return fallback;
  }
  return value;
}

static unsigned short env_port_or_default(const char *name,
                                          unsigned short fallback) {
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

static void load_config(combined_config *config) {
  config->endpoint = env_or_default("LOCKD_ENDPOINT", "https://127.0.0.1:8443");
  config->bundle_path =
      env_or_default("LOCKD_CLIENT_BUNDLE", "/etc/vectis/lockd-client.pem");
  config->namespace_name =
      env_or_default("VECTIS_E2E_COMBINED_NAMESPACE", "examples");
  config->queue =
      env_or_default("VECTIS_E2E_COMBINED_QUEUE", "vectis-e2e-combined");
  config->bind = env_or_default("VECTIS_KORE_BIND", "127.0.0.1");
  config->webdav_cache_dir = env_or_default("VECTIS_E2E_COMBINED_WEBDAV_CACHE",
                                            "/tmp/vectis-e2e-combined-webdav");
  config->port = env_port_or_default("VECTIS_KORE_PORT", 28085u);
}

static void serve_forever(void) {
  for (;;) {
    (void)sleep(3600u);
  }
}

static int print_vectis_error(const char *operation,
                              const vectis_error *error) {
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

static vectis_status allow_webdav(const vectis_webdav_auth_request *request,
                                  vectis_webdav_auth_response *response,
                                  void *userdata, vectis_error *error) {
  (void)request;
  (void)userdata;
  (void)error;
  vectis_webdav_auth_response_init(response);
  response->action = VECTIS_WEBDAV_AUTH_ALLOW;
  response->principal[0] = 'e';
  response->principal[1] = '2';
  response->principal[2] = 'e';
  response->principal[3] = '\0';
  return VECTIS_OK;
}

static vectis_status health(vectis_app *app, vectis_request *request,
                            vectis_response *response, void *userdata,
                            vectis_error *error) {
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 200, "text/plain", "ok\n", error);
}

static int write_marker(combined_context *context, const char *path,
                        const char *body) {
  vectis_webdav_status status;

  status = vectis_webdav_put(&context->webdav_storage, path,
                             (const unsigned char *)body, strlen(body));
  return status == VECTIS_WEBDAV_OK ? LC_OK : LC_ERR_PROTOCOL;
}

static int handle_message(void *userdata, lc_consumer_message *delivery,
                          lc_error *error) {
  combined_context *context;
  int rc;

  context = (combined_context *)userdata;
  rc = write_marker(context, "/consumer-processing.txt", "processing\n");
  if (rc == LC_OK) {
    (void)sleep(3u);
    rc = delivery->message->ack(delivery->message, error);
  }
  if (rc == LC_OK) {
    rc = write_marker(context, "/consumer-done.txt", "handled\n");
  }
  return rc;
}

static vectis_status ensure_consumer(vectis_app *app, combined_context *context,
                                     vectis_error *error) {
  if (context->service_started) {
    return VECTIS_OK;
  }
  if (context->service == NULL &&
      app->consumer_service(app, &context->service_config, &context->service,
                            error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  if (context->service->start(context->service, error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  context->service_started = 1;
  return VECTIS_OK;
}

static vectis_status enqueue_message(vectis_app *app, vectis_request *request,
                                     vectis_response *response, void *userdata,
                                     vectis_error *error) {
  combined_context *context;
  const char *id;
  lc_client *client;
  lc_enqueue_req enqueue;
  lc_enqueue_res result;
  lc_source *source;
  lc_error lcerr;
  int rc;

  context = (combined_context *)userdata;
  id = vectis_request_path_param(request, "id");
  if (id == NULL || id[0] == '\0') {
    return vectis_response_error_json(response, 400, "missing_id",
                                      "message id is required", NULL, error);
  }
  client = app->lockd_client(app);
  if (client == NULL) {
    return vectis_response_error_json(response, 503, "lockd_unavailable",
                                      "lockd client is not configured", NULL,
                                      error);
  }

  lc_error_init(&lcerr);
  lc_enqueue_req_init(&enqueue);
  result = lc_enqueue_res_zero;
  source = NULL;
  (void)vectis_webdav_delete(&context->webdav_storage,
                             "/consumer-processing.txt");
  (void)vectis_webdav_delete(&context->webdav_storage, "/consumer-done.txt");
  if (ensure_consumer(app, context, error) != VECTIS_OK) {
    return vectis_response_error_json(response, 503, "consumer_unavailable",
                                      "failed to start lockd consumer service",
                                      error != NULL ? error->message : "",
                                      error);
  }
  rc = lc_source_from_memory(id, strlen(id), &source, &lcerr);
  if (rc == LC_OK) {
    enqueue.queue = context->config.queue;
    enqueue.visibility_timeout_seconds = 30L;
    enqueue.ttl_seconds = 3600L;
    enqueue.max_attempts = 5;
    enqueue.content_type = "text/plain";
    rc = lc_enqueue(client, &enqueue, source, &result, &lcerr);
  }
  if (source != NULL) {
    lc_source_close(source);
  }
  lc_enqueue_res_cleanup(&result);
  if (rc != LC_OK) {
    lc_error_cleanup(&lcerr);
    return vectis_response_error_json(response, 503, "lockd_enqueue_failed",
                                      "failed to enqueue lockd message", NULL,
                                      error);
  }
  lc_error_cleanup(&lcerr);
  return vectis_response_text(response, 202, "text/plain", "queued\n", error);
}

int main(void) {
  combined_context context;
  vectis_app_config app_config;
  vectis_route_config route;
  vectis_webdav_mount_config webdav;
  vectis_error error;
  vectis_app *app;
  pslog_logger *root_logger;
  pslog_logger *logger;
  pslog_logger *lockd_root_logger;
  pslog_logger *lockd_logger;
  const char *endpoints[1];

  memset(&context, 0, sizeof(context));
  root_logger = NULL;
  lockd_root_logger = NULL;
  load_config(&context.config);
  logger = new_scoped_logger("kore", pslog_palette_default(), &root_logger);
  lockd_logger = new_scoped_logger("lockd", &pslog_builtin_palette_horizon,
                                   &lockd_root_logger);
  if (logger == NULL || lockd_logger == NULL) {
    if (logger != NULL) {
      logger->destroy(logger);
    }
    if (root_logger != NULL) {
      root_logger->destroy(root_logger);
    }
    if (lockd_logger != NULL) {
      lockd_logger->destroy(lockd_logger);
    }
    if (lockd_root_logger != NULL) {
      lockd_root_logger->destroy(lockd_root_logger);
    }
    return 1;
  }

  vectis_app_config_init(&app_config);
  vectis_webdav_config_init(&context.webdav_storage);
  lc_consumer_config_init(&context.consumer);
  lc_consumer_service_config_init(&context.service_config);
  context.webdav_storage.cache_dir = context.config.webdav_cache_dir;
  context.webdav_storage.site_id = "combined";
  context.consumer.name = "combined-worker";
  context.consumer.request.queue = context.config.queue;
  context.consumer.request.owner = "combined-worker";
  context.consumer.request.visibility_timeout_seconds = 30L;
  context.consumer.request.wait_seconds = 1L;
  context.consumer.worker_count = 1u;
  context.consumer.handle = handle_message;
  context.consumer.context = &context;
  context.service_config.consumers = &context.consumer;
  context.service_config.consumer_count = 1u;
  app_config.app_name = "vectis-e2e-combined-server-consumer";
  app_config.logger = logger;
  app_config.tls.bind = context.config.bind;
  app_config.tls.port = context.config.port;
  app_config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  endpoints[0] = context.config.endpoint;
  app_config.lockd.endpoints = endpoints;
  app_config.lockd.endpoint_count = 1u;
  app_config.lockd.client_bundle =
      vectis_source_from_path(context.config.bundle_path);
  app_config.lockd.default_namespace = context.config.namespace_name;
  app_config.lockd.logger = lockd_logger;

  app = vectis_app_new(&app_config, &error);
  if (app == NULL) {
    (void)print_vectis_error("vectis_app_new", &error);
    logger->destroy(logger);
    root_logger->destroy(root_logger);
    lockd_logger->destroy(lockd_logger);
    lockd_root_logger->destroy(lockd_root_logger);
    return 1;
  }

  route = vectis_route(VECTIS_HTTP_GET, "/health", health, NULL);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    (void)print_vectis_error("app->route(/health)", &error);
    app->close(app);
    logger->destroy(logger);
    root_logger->destroy(root_logger);
    lockd_logger->destroy(lockd_logger);
    lockd_root_logger->destroy(lockd_root_logger);
    return 1;
  }
  route =
      vectis_route(VECTIS_HTTP_POST, "/enqueue/:id", enqueue_message, &context);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    (void)print_vectis_error("app->route(/enqueue)", &error);
    app->close(app);
    logger->destroy(logger);
    root_logger->destroy(root_logger);
    lockd_logger->destroy(lockd_logger);
    lockd_root_logger->destroy(lockd_root_logger);
    return 1;
  }

  vectis_webdav_mount_config_init(&webdav);
  webdav.path_prefix = "/dav";
  webdav.storage = context.webdav_storage;
  webdav.auth_required = 1;
  webdav.auth = allow_webdav;
  if (app->webdav(app, &webdav, &error) != VECTIS_OK) {
    (void)print_vectis_error("app->webdav", &error);
    app->close(app);
    logger->destroy(logger);
    root_logger->destroy(root_logger);
    lockd_logger->destroy(lockd_logger);
    lockd_root_logger->destroy(lockd_root_logger);
    return 1;
  }

  if (app->start(app, &error) != VECTIS_OK) {
    (void)print_vectis_error("app->start", &error);
    app->close(app);
    logger->destroy(logger);
    root_logger->destroy(root_logger);
    lockd_logger->destroy(lockd_logger);
    lockd_root_logger->destroy(lockd_root_logger);
    return 1;
  }

  serve_forever();
  if (context.service != NULL) {
    (void)context.service->stop(context.service, &error);
    (void)context.service->wait(context.service, &error);
    context.service->close(context.service);
  }
  app->close(app);
  logger->destroy(logger);
  root_logger->destroy(root_logger);
  lockd_logger->destroy(lockd_logger);
  lockd_root_logger->destroy(lockd_root_logger);
  return 0;
}
