#include <stdio.h>
#include <stdlib.h>

#include <lc/lc.h>
#include <pslog.h>

typedef struct consumer_context {
  pslog_logger *logger;
} consumer_context;

static int fail_with_error(pslog_logger *logger, const char *step,
                           lc_error *error) {
  logger->errorf(logger, step, "code=%d http_status=%ld message=%s detail=%s",
                 error != NULL ? error->code : 0,
                 error != NULL ? error->http_status : 0L,
                 error != NULL && error->message != NULL ? error->message : "",
                 error != NULL && error->detail != NULL ? error->detail : "");
  return 1;
}

static int handle_order(void *context, lc_consumer_message *message,
                        lc_error *error) {
  consumer_context *ctx;
  lc_sink *sink;
  size_t written;

  ctx = (consumer_context *)context;
  written = 0u;
  ctx->logger->infof(ctx->logger, "example.lockd_consumer.delivery",
                     "consumer=%s queue=%s message=%s",
                     message->name != NULL ? message->name : "",
                     message->queue != NULL ? message->queue : "",
                     message->message != NULL &&
                             message->message->message_id != NULL
                         ? message->message->message_id
                         : "");
  if (lc_sink_to_file("consumer-order.json", &sink, error) != LC_OK) {
    return LC_ERR_INVALID;
  }
  if (message->message->write_payload(message->message, sink, &written,
                                      error) != LC_OK) {
    lc_sink_close(sink);
    return LC_ERR_TRANSPORT;
  }
  lc_sink_close(sink);
  return LC_OK;
}

int main(void) {
  lc_client_config client_config;
  lc_client *client;
  lc_consumer_config consumer;
  lc_consumer_service_config service_config;
  lc_consumer_service *service;
  lc_error error;
  pslog_config sdk_log_config;
  pslog_config example_log_config;
  pslog_logger *sdk_logger;
  pslog_logger *example_logger;
  consumer_context context;
  const char *endpoints[1];
  int rc;

  pslog_default_config(&sdk_log_config);
  sdk_log_config.mode = PSLOG_MODE_CONSOLE;
  sdk_log_config.min_level = PSLOG_LEVEL_TRACE;
  sdk_log_config.timestamps = 1;
  sdk_logger = pslog_new(&sdk_log_config);
  if (sdk_logger == NULL) {
    fprintf(stderr, "failed to allocate sdk logger\n");
    return 1;
  }

  pslog_default_config(&example_log_config);
  example_log_config.mode = PSLOG_MODE_CONSOLE;
  example_log_config.min_level = PSLOG_LEVEL_TRACE;
  example_log_config.timestamps = 1;
  example_log_config.palette = &pslog_builtin_palette_horizon;
  example_logger = pslog_new(&example_log_config);
  if (example_logger == NULL) {
    sdk_logger->destroy(sdk_logger);
    fprintf(stderr, "failed to allocate example logger\n");
    return 1;
  }

  lc_client_config_init(&client_config);
  lc_consumer_config_init(&consumer);
  lc_consumer_service_config_init(&service_config);
  lc_error_init(&error);
  client = NULL;
  service = NULL;

  endpoints[0] = getenv("LOCKD_ENDPOINT") != NULL ? getenv("LOCKD_ENDPOINT")
                                                  : "https://127.0.0.1:8443";
  client_config.endpoints = endpoints;
  client_config.endpoint_count = 1u;
  client_config.client_bundle_path = getenv("LOCKD_CLIENT_BUNDLE");
  client_config.default_namespace = "examples";
  client_config.disable_mtls = client_config.client_bundle_path == NULL;
  client_config.insecure_skip_verify = client_config.client_bundle_path == NULL;
  client_config.logger = sdk_logger;

  example_logger->infof(example_logger, "example.lockd_consumer.start",
                        "endpoint=%s namespace=%s queue=%s", endpoints[0],
                        client_config.default_namespace, "orders");

  if (lc_client_open(&client_config, &client, &error) != LC_OK) {
    rc = fail_with_error(example_logger, "lc_client_open", &error);
    lc_error_cleanup(&error);
    example_logger->destroy(example_logger);
    sdk_logger->destroy(sdk_logger);
    return rc;
  }

  context.logger = example_logger;
  consumer.name = "orders-worker";
  consumer.request.queue = "orders";
  consumer.request.owner = "orders-worker";
  consumer.request.visibility_timeout_seconds = 30L;
  consumer.worker_count = 2u;
  consumer.handle = handle_order;
  consumer.context = &context;
  service_config.consumers = &consumer;
  service_config.consumer_count = 1u;

  if (lc_client_new_consumer_service(client, &service_config, &service,
                                     &error) != LC_OK) {
    rc =
        fail_with_error(example_logger, "client->new_consumer_service", &error);
    lc_client_close(client);
    lc_error_cleanup(&error);
    example_logger->destroy(example_logger);
    sdk_logger->destroy(sdk_logger);
    return rc;
  }

  rc = service->run(service, &error);
  if (rc != LC_OK) {
    rc = fail_with_error(example_logger, "service->run", &error);
  }
  lc_consumer_service_close(service);
  lc_client_close(client);
  lc_error_cleanup(&error);
  example_logger->destroy(example_logger);
  sdk_logger->destroy(sdk_logger);
  return rc == LC_OK ? 0 : rc;
}
