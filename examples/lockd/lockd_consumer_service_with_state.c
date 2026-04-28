#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <lc/lc.h>
#include <lonejson.h>
#include <pslog.h>

typedef struct stateful_context {
  pslog_logger *logger;
} stateful_context;

typedef struct workflow_state {
  char id[64];
  char phase[32];
} workflow_state;

static const lonejson_field workflow_state_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(workflow_state, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(workflow_state, phase, "phase", LONEJSON_OVERFLOW_FAIL)};

LONEJSON_MAP_DEFINE(workflow_state_map, workflow_state, workflow_state_fields);

static int handle_stateful_order(void *context,
                                 lc_consumer_message *message,
                                 lc_error *error) {
  stateful_context *ctx;
  workflow_state state = {"", ""};

  ctx = (stateful_context *)context;
  if (message->state == NULL) {
    ctx->logger->errorf(ctx->logger, "example.lockd_state_consumer.missing_state",
                        "queue=%s message=%s",
                        message->queue != NULL ? message->queue : "",
                        message->message != NULL && message->message->message_id != NULL
                            ? message->message->message_id
                            : "");
    return LC_ERR_PROTOCOL;
  }
  if (message->state->load(message->state, &workflow_state_map, &state, NULL, NULL, NULL, error) != LC_OK) {
    return LC_ERR_TRANSPORT;
  }
  (void)snprintf(state.phase, sizeof(state.phase), "%s", "processed");
  if (message->state->save(message->state, &workflow_state_map, &state, NULL, error) != LC_OK) {
    return LC_ERR_TRANSPORT;
  }
  ctx->logger->infof(ctx->logger, "example.lockd_state_consumer.processed",
                     "queue=%s message=%s id=%s phase=%s",
                     message->queue != NULL ? message->queue : "",
                     message->message != NULL && message->message->message_id != NULL
                         ? message->message->message_id
                         : "",
                     state.id,
                     state.phase);
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
  stateful_context context;
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

  endpoints[0] = getenv("LOCKD_ENDPOINT") != NULL ? getenv("LOCKD_ENDPOINT") : "https://127.0.0.1:8443";
  client_config.endpoints = endpoints;
  client_config.endpoint_count = 1u;
  client_config.client_bundle_path = getenv("LOCKD_CLIENT_BUNDLE");
  client_config.default_namespace = "examples";
  client_config.disable_mtls = client_config.client_bundle_path == NULL;
  client_config.insecure_skip_verify = client_config.client_bundle_path == NULL;
  client_config.logger = sdk_logger;

  example_logger->infof(example_logger, "example.lockd_state_consumer.start",
                        "endpoint=%s namespace=%s queue=%s",
                        endpoints[0], client_config.default_namespace, "orders-with-state");

  if (lc_client_open(&client_config, &client, &error) != LC_OK) {
    example_logger->errorf(example_logger, "lc_client_open",
                           "code=%d http_status=%ld message=%s detail=%s",
                           error.code, error.http_status,
                           error.message != NULL ? error.message : "",
                           error.detail != NULL ? error.detail : "");
    lc_error_cleanup(&error);
    example_logger->destroy(example_logger);
    sdk_logger->destroy(sdk_logger);
    return 1;
  }

  context.logger = example_logger;
  consumer.name = "stateful-orders-worker";
  consumer.request.queue = "orders-with-state";
  consumer.request.owner = "stateful-orders-worker";
  consumer.request.visibility_timeout_seconds = 60L;
  consumer.with_state = 1;
  consumer.handle = handle_stateful_order;
  consumer.context = &context;
  service_config.consumers = &consumer;
  service_config.consumer_count = 1u;

  if (lc_client_new_consumer_service(client, &service_config, &service, &error) != LC_OK) {
    example_logger->errorf(example_logger, "client->new_consumer_service",
                           "code=%d http_status=%ld message=%s detail=%s",
                           error.code, error.http_status,
                           error.message != NULL ? error.message : "",
                           error.detail != NULL ? error.detail : "");
    lc_client_close(client);
    lc_error_cleanup(&error);
    example_logger->destroy(example_logger);
    sdk_logger->destroy(sdk_logger);
    return 1;
  }

  rc = service->run(service, &error);
  if (rc != LC_OK) {
    example_logger->errorf(example_logger, "service->run",
                           "code=%d http_status=%ld message=%s detail=%s",
                           error.code, error.http_status,
                           error.message != NULL ? error.message : "",
                           error.detail != NULL ? error.detail : "");
  }
  lc_consumer_service_close(service);
  lc_client_close(client);
  lc_error_cleanup(&error);
  example_logger->destroy(example_logger);
  sdk_logger->destroy(sdk_logger);
  return rc == LC_OK ? 0 : 1;
}
