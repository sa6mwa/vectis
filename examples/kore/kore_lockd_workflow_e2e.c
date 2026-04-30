#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <lc/lc.h>
#include <lonejson.h>
#include <pslog.h>
#include <vectis/vectis.h>

typedef struct workflow_input {
  char content[128];
} workflow_input;

typedef struct workflow_content {
  char id[64];
  char content[128];
} workflow_content;

typedef struct workflow_counter {
  char id[64];
  lonejson_int64 counter;
} workflow_counter;

typedef struct workflow_message {
  char id[64];
} workflow_message;

typedef struct workflow_response {
  char id[64];
  char queue[96];
  lonejson_int64 counter;
} workflow_response;

typedef struct workflow_config {
  const char *endpoint;
  const char *bundle_path;
  const char *queue;
  const char *namespace_name;
  const char *expected_content;
  const char *bind;
  unsigned short port;
} workflow_config;

typedef struct workflow_server_context {
  workflow_config config;
} workflow_server_context;

typedef struct workflow_consumer_context {
  workflow_config config;
  lonejson_int64 expected_counter;
  lonejson_int64 next_counter;
  int ack_message;
  volatile int done;
  volatile int handled;
  volatile int failed;
} workflow_consumer_context;

typedef struct workflow_counter_update {
  lonejson_int64 expected;
  lonejson_int64 next;
  int matched;
} workflow_counter_update;

static const lonejson_field workflow_input_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(workflow_input,
                                    content,
                                    "content",
                                    LONEJSON_OVERFLOW_FAIL)};

static const lonejson_field workflow_content_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(workflow_content,
                                    id,
                                    "id",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(workflow_content,
                                    content,
                                    "content",
                                    LONEJSON_OVERFLOW_FAIL)};

static const lonejson_field workflow_counter_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(workflow_counter,
                                    id,
                                    "id",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64_REQ(workflow_counter, counter, "counter")};

static const lonejson_field workflow_message_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(workflow_message,
                                    id,
                                    "id",
                                    LONEJSON_OVERFLOW_FAIL)};

static const lonejson_field workflow_response_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(workflow_response,
                                    id,
                                    "id",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(workflow_response,
                                    queue,
                                    "queue",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64_REQ(workflow_response, counter, "counter")};

LONEJSON_MAP_DEFINE(workflow_input_map, workflow_input, workflow_input_fields);
LONEJSON_MAP_DEFINE(workflow_content_map, workflow_content, workflow_content_fields);
LONEJSON_MAP_DEFINE(workflow_counter_map, workflow_counter, workflow_counter_fields);
LONEJSON_MAP_DEFINE(workflow_message_map, workflow_message, workflow_message_fields);
LONEJSON_MAP_DEFINE(workflow_response_map, workflow_response, workflow_response_fields);

static const workflow_input workflow_input_zero;
static const workflow_content workflow_content_zero;
static const workflow_counter workflow_counter_zero;
static const workflow_message workflow_message_zero;
static const workflow_response workflow_response_zero;
static const workflow_consumer_context workflow_consumer_context_zero;
static const lc_enqueue_res lc_enqueue_res_zero;

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

static void load_config(workflow_config *config) {
  config->endpoint = env_or_default("LOCKD_ENDPOINT", "https://127.0.0.1:8443");
  config->bundle_path = env_or_default("LOCKD_CLIENT_BUNDLE",
                                       "/etc/vectis/lockd-client.pem");
  config->queue = env_or_default("VECTIS_E2E_WORKFLOW_QUEUE",
                                 "vectis-e2e-workflow");
  config->namespace_name = env_or_default("VECTIS_E2E_WORKFLOW_NAMESPACE",
                                          "examples");
  config->expected_content = env_or_default("VECTIS_E2E_WORKFLOW_CONTENT",
                                            "vectis e2e content");
  config->bind = env_or_default("VECTIS_KORE_BIND", "127.0.0.1");
  config->port = env_port_or_default("VECTIS_KORE_PORT", 28082u);
}

static void serve_forever(void) {
  for (;;) {
    (void)sleep(3600u);
  }
}

static int print_vectis_error(const char *operation, const vectis_error *error) {
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

static int print_lockd_error(const char *operation, const lc_error *error) {
  fprintf(stderr, "%s failed", operation);
  if (error != NULL && error->message != NULL && error->message[0] != '\0') {
    fprintf(stderr, ": %s", error->message);
  }
  fprintf(stderr, "\n");
  return 1;
}

static vectis_status save_content_state(lc_client *client,
                                        const char *id,
                                        const char *content,
                                        vectis_error *error) {
  workflow_content doc;
  char key[160];
  vectis_status status;

  doc = workflow_content_zero;
  status = vectis_format_key(key, sizeof(key), error, "workflow/%s/content", id);
  if (status != VECTIS_OK) {
    return status;
  }
  (void)snprintf(doc.id, sizeof(doc.id), "%s", id);
  (void)snprintf(doc.content, sizeof(doc.content), "%s", content);
  return vectis_lockd_state_save(client,
                                 key,
                                 "vectis-e2e-kore-producer",
                                 30L,
                                 &workflow_content_map,
                                 &doc,
                                 error);
}

static vectis_status save_counter_state(lc_client *client,
                                        const char *id,
                                        lonejson_int64 counter,
                                        const char *owner,
                                        vectis_error *error) {
  workflow_counter doc;
  char key[160];
  vectis_status status;

  doc = workflow_counter_zero;
  status = vectis_format_key(key, sizeof(key), error, "workflow/%s/counter", id);
  if (status != VECTIS_OK) {
    return status;
  }
  (void)snprintf(doc.id, sizeof(doc.id), "%s", id);
  doc.counter = counter;
  return vectis_lockd_state_save(client,
                                 key,
                                 owner,
                                 30L,
                                 &workflow_counter_map,
                                 &doc,
                                 error);
}

static vectis_status load_content_state(lc_client *client,
                                        const char *id,
                                        workflow_content *doc,
                                        vectis_error *error) {
  char key[160];
  vectis_status status;

  *doc = workflow_content_zero;
  status = vectis_format_key(key, sizeof(key), error, "workflow/%s/content", id);
  if (status != VECTIS_OK) {
    return status;
  }
  return vectis_lockd_state_load(client,
                                 key,
                                 "vectis-e2e-content-reader",
                                 30L,
                                 &workflow_content_map,
                                 doc,
                                 error);
}

static vectis_status load_counter_state(lc_client *client,
                                        const char *id,
                                        workflow_counter *doc,
                                        vectis_error *error) {
  char key[160];
  vectis_status status;

  *doc = workflow_counter_zero;
  status = vectis_format_key(key, sizeof(key), error, "workflow/%s/counter", id);
  if (status != VECTIS_OK) {
    return status;
  }
  return vectis_lockd_state_load(client,
                                 key,
                                 "vectis-e2e-counter-reader",
                                 30L,
                                 &workflow_counter_map,
                                 doc,
                                 error);
}

static vectis_status update_counter_value(struct lc_lease *lease,
                                          void *state,
                                          int *save,
                                          void *userdata,
                                          vectis_error *error) {
  workflow_counter *doc;
  workflow_counter_update *update;

  (void)lease;
  (void)error;
  doc = (workflow_counter *)state;
  update = (workflow_counter_update *)userdata;
  if (doc->counter != update->expected) {
    update->matched = 0;
    *save = 0;
    return VECTIS_OK;
  }
  doc->counter = update->next;
  update->matched = 1;
  *save = 1;
  return VECTIS_OK;
}

static vectis_status update_counter_state(lc_client *client,
                                          const char *id,
                                          lonejson_int64 expected,
                                          lonejson_int64 next,
                                          const char *owner,
                                          int *matched,
                                          vectis_error *error) {
  workflow_counter doc;
  workflow_counter_update update;
  char key[160];
  vectis_status status;

  doc = workflow_counter_zero;
  update.expected = expected;
  update.next = next;
  update.matched = 0;
  if (matched != NULL) {
    *matched = 0;
  }
  status = vectis_format_key(key, sizeof(key), error, "workflow/%s/counter", id);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_lockd_state_update(client,
                                     key,
                                     owner,
                                     30L,
                                     &workflow_counter_map,
                                     &doc,
                                     update_counter_value,
                                     &update,
                                     error);
  if (status == VECTIS_OK && matched != NULL) {
    *matched = update.matched;
  }
  return status;
}

static int enqueue_workflow_message(lc_client *client,
                                    const workflow_config *config,
                                    const char *id,
                                    lc_error *error) {
  workflow_message message;
  lc_enqueue_req enqueue;
  lc_enqueue_res result;
  lc_source *source;
  lonejson_error json_error;
  char *json;
  size_t json_size;
  int rc;

  message = workflow_message_zero;
  result = lc_enqueue_res_zero;
  lc_enqueue_req_init(&enqueue);
  source = NULL;
  (void)snprintf(message.id, sizeof(message.id), "%s", id);
  json = lonejson_serialize_alloc(&workflow_message_map,
                                  &message,
                                  &json_size,
                                  NULL,
                                  &json_error);
  if (json == NULL) {
    fprintf(stderr, "serialize workflow message failed: %s\n", json_error.message);
    return LC_ERR_INVALID;
  }
  rc = lc_source_from_memory(json, json_size, &source, error);
  if (rc == LC_OK) {
    enqueue.queue = config->queue;
    enqueue.visibility_timeout_seconds = 30L;
    enqueue.ttl_seconds = 3600L;
    enqueue.max_attempts = 5;
    enqueue.content_type = "application/json";
    rc = lc_enqueue(client, &enqueue, source, &result, error);
    lc_enqueue_res_cleanup(&result);
  }
  if (source != NULL) {
    lc_source_close(source);
  }
  free(json);
  return rc;
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

static vectis_status start_workflow(vectis_app *app,
                                    vectis_request *request,
                                    vectis_response *response,
                                    void *userdata,
                                    vectis_error *error) {
  workflow_server_context *context;
  workflow_input input;
  workflow_response output;
  lc_client *client;
  lc_error lcerr;
  const char *id;
  vectis_status status;
  int rc;

  context = (workflow_server_context *)userdata;
  input = workflow_input_zero;
  output = workflow_response_zero;
  lc_error_init(&lcerr);
  id = vectis_request_path_param(request, "id");
  if (id == NULL || id[0] == '\0') {
    return vectis_response_error_json(response,
                                      400,
                                      "missing_id",
                                      "workflow id is required",
                                      NULL,
                                      error);
  }
  if (vectis_request_json_into(request, &workflow_input_map, &input, error) != VECTIS_OK) {
    return VECTIS_ERR_INVALID;
  }
  client = vectis_lockd_client(app);
  if (client == NULL) {
    return VECTIS_ERR_STATE;
  }
  status = save_content_state(client, id, input.content, error);
  if (status == VECTIS_OK) {
    status = save_counter_state(client, id, 1, "vectis-e2e-kore-producer", error);
  }
  if (status != VECTIS_OK) {
    lc_error_cleanup(&lcerr);
    return status;
  }
  rc = enqueue_workflow_message(client, &context->config, id, &lcerr);
  if (rc != LC_OK) {
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }
  (void)snprintf(output.id, sizeof(output.id), "%s", id);
  (void)snprintf(output.queue, sizeof(output.queue), "%s", context->config.queue);
  output.counter = 1;
  lc_error_cleanup(&lcerr);
  return vectis_response_json(response, 202, &workflow_response_map, &output, error);
}

static int parse_delivery_id(lc_message *message,
                             workflow_message *workflow,
                             lc_error *error) {
  lc_sink *sink;
  const void *bytes;
  size_t length;
  lonejson_error json_error;
  lonejson_status json_status;
  int rc;

  sink = NULL;
  bytes = NULL;
  length = 0u;
  *workflow = workflow_message_zero;
  rc = lc_sink_to_memory(&sink, error);
  if (rc != LC_OK) {
    return rc;
  }
  rc = message->write_payload(message, sink, NULL, error);
  if (rc == LC_OK) {
    rc = lc_sink_memory_bytes(sink, &bytes, &length, error);
  }
  if (rc == LC_OK) {
    json_status = lonejson_parse_buffer(&workflow_message_map,
                                        workflow,
                                        (const char *)bytes,
                                        length,
                                        NULL,
                                        &json_error);
    if (json_status != LONEJSON_STATUS_OK) {
      fprintf(stderr, "parse workflow message failed: %s\n", json_error.message);
      rc = LC_ERR_INVALID;
    }
  }
  lc_sink_close(sink);
  return rc;
}

static int handle_workflow_message(void *userdata,
                                   lc_consumer_message *delivery,
                                   lc_error *error) {
  workflow_consumer_context *context;
  workflow_message message;
  workflow_content content;
  lc_nack_req nack;
  vectis_error verror;
  vectis_status status;
  int rc;
  int matched;

  context = (workflow_consumer_context *)userdata;
  matched = 0;
  vectis_error_clear(&verror);
  rc = parse_delivery_id(delivery->message, &message, error);
  if (rc == LC_OK) {
    status = load_content_state(delivery->client, message.id, &content, &verror);
    if (status != VECTIS_OK) {
      fprintf(stderr, "load content failed: %s\n", verror.message);
      rc = LC_ERR_PROTOCOL;
    }
  }
  if (rc == LC_OK &&
      (strcmp(content.id, message.id) != 0 ||
       strcmp(content.content, context->config.expected_content) != 0)) {
    fprintf(stderr,
            "unexpected content for %s: id=%s content=%s expected_content=%s\n",
            message.id,
            content.id,
            content.content,
            context->config.expected_content);
    rc = LC_ERR_PROTOCOL;
  }
  if (rc == LC_OK) {
    status = update_counter_state(delivery->client,
                                  message.id,
                                  context->expected_counter,
                                  context->next_counter,
                                  context->ack_message
                                      ? "vectis-e2e-consumer-second"
                                      : "vectis-e2e-consumer-first",
                                  &matched,
                                  &verror);
    if (status != VECTIS_OK) {
      fprintf(stderr, "update counter failed: %s\n", verror.message);
      rc = LC_ERR_PROTOCOL;
    }
  }
  if (rc == LC_OK) {
    if (!matched) {
      fprintf(stderr,
              "workflow.consumer.not_ready id=%s expected=%ld ack=%d\n",
              message.id,
              (long)context->expected_counter,
              context->ack_message);
      lc_nack_req_init(&nack);
      nack.delay_seconds = 0L;
      nack.intent = LC_NACK_INTENT_DEFER;
      rc = delivery->message->nack(delivery->message, &nack, error);
    } else if (context->ack_message) {
      fprintf(stderr,
              "workflow.consumer.ack id=%s expected=%ld next=%ld\n",
              message.id,
              (long)context->expected_counter,
              (long)context->next_counter);
      rc = delivery->message->ack(delivery->message, error);
    } else {
      fprintf(stderr,
              "workflow.consumer.defer id=%s expected=%ld next=%ld\n",
              message.id,
              (long)context->expected_counter,
              (long)context->next_counter);
      lc_nack_req_init(&nack);
      nack.delay_seconds = 0L;
      nack.intent = LC_NACK_INTENT_DEFER;
      rc = delivery->message->nack(delivery->message, &nack, error);
    }
  }
  if (rc == LC_OK) {
    if (matched) {
      context->handled = 1;
      context->done = 1;
    }
  } else {
    context->failed = 1;
    context->done = 1;
  }
  return rc;
}

static int new_logger(const char *component,
                      const pslog_palette *palette,
                      pslog_logger **root_out,
                      pslog_logger **out) {
  pslog_config log_config;
  pslog_logger *root;
  pslog_logger *scoped;

  if (root_out != NULL) {
    *root_out = NULL;
  }
  if (out != NULL) {
    *out = NULL;
  }
  pslog_default_config(&log_config);
  log_config.mode = PSLOG_MODE_CONSOLE;
  log_config.min_level = PSLOG_LEVEL_INFO;
  log_config.output = pslog_output_from_fp(stderr, 0);
  log_config.palette = palette;
  root = pslog_new(&log_config);
  if (root == NULL) {
    return 1;
  }
  scoped = root->withf(root, "component=%s", component);
  if (scoped == NULL) {
    root->destroy(root);
    return 1;
  }
  if (root_out != NULL) {
    *root_out = root;
  }
  if (out != NULL) {
    *out = scoped;
  }
  return 0;
}

static int run_server(void) {
  workflow_server_context context;
  vectis_app_config app_config;
  vectis_route_config route;
  vectis_error error;
  vectis_app *app;
  pslog_logger *root_logger;
  pslog_logger *logger;
  pslog_logger *lockd_root_logger;
  pslog_logger *lockd_logger;
  const char *endpoints[1];

  root_logger = NULL;
  lockd_root_logger = NULL;
  load_config(&context.config);
  if (new_logger("kore", pslog_palette_default(), &root_logger, &logger) != 0) {
    return 1;
  }
  if (new_logger("lockd", &pslog_builtin_palette_horizon, &lockd_root_logger, &lockd_logger) != 0) {
    logger->destroy(logger);
    root_logger->destroy(root_logger);
    return 1;
  }
  vectis_app_config_init(&app_config);
  app_config.app_name = "vectis-e2e-workflow-api";
  app_config.logger = logger;
  app_config.tls.bind = context.config.bind;
  app_config.tls.port = context.config.port;
  app_config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  endpoints[0] = context.config.endpoint;
  app_config.lockd.endpoints = endpoints;
  app_config.lockd.endpoint_count = 1u;
  app_config.lockd.client_bundle = vectis_source_from_path(context.config.bundle_path);
  app_config.lockd.default_namespace = context.config.namespace_name;
  app_config.lockd.logger = lockd_logger;
  app = vectis_app_new(&app_config, &error);
  if (app == NULL) {
    (void)print_vectis_error("vectis_app_new", &error);
    logger->destroy(logger);
    lockd_logger->destroy(lockd_logger);
    lockd_root_logger->destroy(lockd_root_logger);
    root_logger->destroy(root_logger);
    return 1;
  }
  route = vectis_route_methods(VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD,
                               "/health",
                               health,
                               NULL);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    (void)print_vectis_error("app->route", &error);
    app->close(app);
    logger->destroy(logger);
    lockd_logger->destroy(lockd_logger);
    lockd_root_logger->destroy(lockd_root_logger);
    root_logger->destroy(root_logger);
    return 1;
  }
  route = vectis_json_body_route(VECTIS_HTTP_POST, "/workflow/:id", start_workflow, &context);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    (void)print_vectis_error("app->route", &error);
    app->close(app);
    logger->destroy(logger);
    lockd_logger->destroy(lockd_logger);
    lockd_root_logger->destroy(lockd_root_logger);
    root_logger->destroy(root_logger);
    return 1;
  }
  if (app->start(app, &error) != VECTIS_OK) {
    (void)print_vectis_error("app->start", &error);
    app->close(app);
    logger->destroy(logger);
    lockd_logger->destroy(lockd_logger);
    lockd_root_logger->destroy(lockd_root_logger);
    root_logger->destroy(root_logger);
    return 1;
  }
  serve_forever();
  app->close(app);
  logger->destroy(logger);
  lockd_logger->destroy(lockd_logger);
  lockd_root_logger->destroy(lockd_root_logger);
  root_logger->destroy(root_logger);
  return 0;
}

static int run_consumer(lonejson_int64 expected_counter,
                        lonejson_int64 next_counter,
                        int ack_message) {
  workflow_consumer_context context;
  vectis_app_config app_config;
  lc_consumer_config consumer;
  lc_consumer_service_config service_config;
  vectis_error error;
  vectis_app *app;
  vectis_consumer_service *service;
  pslog_logger *root_logger;
  pslog_logger *logger;
  pslog_logger *lockd_root_logger;
  pslog_logger *lockd_logger;
  const char *endpoints[1];
  int rc;
  vectis_status status;

  context = workflow_consumer_context_zero;
  root_logger = NULL;
  lockd_root_logger = NULL;
  service = NULL;
  load_config(&context.config);
  context.expected_counter = expected_counter;
  context.next_counter = next_counter;
  context.ack_message = ack_message;
  if (new_logger("consumer", pslog_palette_default(), &root_logger, &logger) != 0) {
    return 1;
  }
  if (new_logger("lockd", &pslog_builtin_palette_horizon, &lockd_root_logger, &lockd_logger) != 0) {
    logger->destroy(logger);
    root_logger->destroy(root_logger);
    return 1;
  }
  vectis_app_config_init(&app_config);
  lc_consumer_config_init(&consumer);
  lc_consumer_service_config_init(&service_config);
  app_config.app_name = ack_message ? "vectis-e2e-consumer-second"
                                    : "vectis-e2e-consumer-first";
  app_config.logger = logger;
  endpoints[0] = context.config.endpoint;
  app_config.lockd.endpoints = endpoints;
  app_config.lockd.endpoint_count = 1u;
  app_config.lockd.client_bundle = vectis_source_from_path(context.config.bundle_path);
  app_config.lockd.default_namespace = context.config.namespace_name;
  app_config.lockd.logger = lockd_logger;
  app = vectis_app_new(&app_config, &error);
  if (app == NULL) {
    (void)print_vectis_error("vectis_app_new", &error);
    logger->destroy(logger);
    lockd_logger->destroy(lockd_logger);
    lockd_root_logger->destroy(lockd_root_logger);
    root_logger->destroy(root_logger);
    return 1;
  }
  consumer.name = ack_message ? "workflow-second" : "workflow-first";
  consumer.request.queue = context.config.queue;
  consumer.request.owner = consumer.name;
  consumer.request.visibility_timeout_seconds = 30L;
  consumer.request.wait_seconds = 1L;
  consumer.worker_count = 1u;
  consumer.handle = handle_workflow_message;
  consumer.context = &context;
  service_config.consumers = &consumer;
  service_config.consumer_count = 1u;
  rc = 1;
  status = app->consumer_service(app, &service_config, &service, &error);
  if (status != VECTIS_OK) {
    (void)print_vectis_error("app->consumer_service", &error);
  } else {
    status = service->run_until(service, &context.done, 30000L, &error);
    if (status != VECTIS_OK) {
      (void)print_vectis_error("service->run_until", &error);
    } else if (context.handled && !context.failed) {
      rc = 0;
    } else {
      fprintf(stderr, "workflow consumer stopped without handling expected phase\n");
    }
  }
  service->close(service);
  app->close(app);
  logger->destroy(logger);
  lockd_logger->destroy(lockd_logger);
  lockd_root_logger->destroy(lockd_root_logger);
  root_logger->destroy(root_logger);
  return rc;
}

static int run_verify(void) {
  workflow_config config;
  lc_client_config client_config;
  lc_client *client;
  lc_error error;
  const char *endpoints[1];
  workflow_content content;
  workflow_counter counter;
  lc_queue_stats_req stats_req;
  lc_queue_stats_res stats;
  vectis_error verror;
  vectis_status status;
  int rc;

  load_config(&config);
  vectis_error_clear(&verror);
  lc_client_config_init(&client_config);
  lc_error_init(&error);
  client = NULL;
  endpoints[0] = config.endpoint;
  client_config.endpoints = endpoints;
  client_config.endpoint_count = 1u;
  client_config.client_bundle_path = config.bundle_path;
  client_config.default_namespace = config.namespace_name;
  rc = lc_client_open(&client_config, &client, &error);
  if (rc == LC_OK) {
    status = load_content_state(client,
                                env_or_default("VECTIS_E2E_WORKFLOW_ID", "e2e"),
                                &content,
                                &verror);
    if (status != VECTIS_OK) {
      fprintf(stderr, "verify content failed: %s\n", verror.message);
      rc = LC_ERR_PROTOCOL;
    }
  }
  if (rc == LC_OK) {
    status = load_counter_state(client,
                                env_or_default("VECTIS_E2E_WORKFLOW_ID", "e2e"),
                                &counter,
                                &verror);
    if (status != VECTIS_OK) {
      fprintf(stderr, "verify counter failed: %s\n", verror.message);
      rc = LC_ERR_PROTOCOL;
    }
  }
  if (rc == LC_OK &&
      (strcmp(content.content, config.expected_content) != 0 || counter.counter != 3)) {
    fprintf(stderr,
            "verify failed: content=%s counter=%ld\n",
            content.content,
            (long)counter.counter);
    rc = LC_ERR_PROTOCOL;
  }
  if (rc == LC_OK) {
    lc_queue_stats_req_init(&stats_req);
    memset(&stats, 0, sizeof(stats));
    stats_req.queue = config.queue;
    if (lc_queue_stats(client, &stats_req, &stats, &error) != LC_OK) {
      rc = LC_ERR_PROTOCOL;
    } else {
      if (stats.available != 0 || stats.pending_candidates != 0) {
        fprintf(stderr,
                "verify queue not drained: available=%d pending=%d\n",
                stats.available,
                stats.pending_candidates);
        rc = LC_ERR_PROTOCOL;
      }
      lc_queue_stats_res_cleanup(&stats);
    }
  }
  if (client != NULL) {
    lc_client_close(client);
  }
  if (rc != LC_OK) {
    (void)print_lockd_error("workflow verify", &error);
  }
  lc_error_cleanup(&error);
  return rc == LC_OK ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fputs("usage: vectis_example_workflow_e2e server|consumer-first|consumer-second|verify\n",
          stderr);
    return 2;
  }
  if (strcmp(argv[1], "server") == 0) {
    return run_server();
  }
  if (strcmp(argv[1], "consumer-first") == 0) {
    return run_consumer(1, 2, 0);
  }
  if (strcmp(argv[1], "consumer-second") == 0) {
    return run_consumer(2, 3, 1);
  }
  if (strcmp(argv[1], "verify") == 0) {
    return run_verify();
  }
  fputs("unknown mode\n", stderr);
  return 2;
}
