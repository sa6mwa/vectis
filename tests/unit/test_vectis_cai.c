#include "vectis_internal.h"
#include <assert.h>
#include <cai/cai.h>
#include <lc/lc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vectis/vectis.h>

static void expect_cai_source_bytes(cai_source *source, const char *expected) {
  cai_error error;
  char buffer[64];
  size_t nread;

  cai_error_init(&error);
  nread = cai_source_read(source, buffer, sizeof(buffer), &error);
  assert(nread == strlen(expected));
  assert(memcmp(buffer, expected, nread) == 0);
  assert(cai_source_read(source, buffer, sizeof(buffer), &error) == 0u);
  assert(cai_source_reset(source, &error) == CAI_OK);
  nread = cai_source_read(source, buffer, sizeof(buffer), &error);
  assert(nread == strlen(expected));
  assert(memcmp(buffer, expected, nread) == 0);
  cai_error_cleanup(&error);
}

static void test_source_from_memory(void) {
  vectis_source source;
  vectis_error error;
  cai_source *cai_source;
  const char payload[] = "vectis cai source";

  vectis_error_clear(&error);
  source = vectis_source_from_memory(payload, sizeof(payload) - 1u);
  cai_source = NULL;
  assert(vectis_cai_source_from_source(&source, &cai_source, &error) ==
         VECTIS_OK);
  assert(cai_source != NULL);
  expect_cai_source_bytes(cai_source, payload);
  cai_source_close(cai_source);
}

static void test_source_from_request(void) {
  vectis_request *request;
  vectis_error error;
  cai_source *source;
  const char payload[] = "request body";

  vectis_error_clear(&error);
  request = vectis_internal_request_new(&error);
  assert(request != NULL);
  assert(vectis_internal_request_set_body(
             request, payload, sizeof(payload) - 1u, &error) == VECTIS_OK);
  source = NULL;
  assert(vectis_cai_source_from_request(request, &source, &error) == VECTIS_OK);
  assert(source != NULL);
  expect_cai_source_bytes(source, payload);
  cai_source_close(source);
  vectis_internal_request_free(request);
}

typedef struct single_pass_source {
  const char *data;
  size_t size;
  size_t offset;
} single_pass_source;

static size_t single_pass_source_read(void *context, void *buffer, size_t count,
                                      lc_error *error) {
  single_pass_source *source;
  size_t remaining;

  source = (single_pass_source *)context;
  assert(source != NULL);
  if (source->offset >= source->size) {
    return 0u;
  }
  remaining = source->size - source->offset;
  if (count > remaining) {
    count = remaining;
  }
  memcpy(buffer, source->data + source->offset, count);
  source->offset += count;
  (void)error;
  return count;
}

static size_t single_pass_lc_source_read(lc_source *self, void *buffer,
                                         size_t count, lc_error *error) {
  assert(self != NULL);
  return single_pass_source_read(self->impl, buffer, count, error);
}

static void test_source_from_single_pass_lc_source_has_no_reset(void) {
  single_pass_source single_pass;
  vectis_source source;
  vectis_error error;
  lc_source lcsrc;
  cai_source *caisrc;
  const char payload[] = "single pass";
  char buffer[32];
  cai_error caierr;
  size_t nread;

  memset(&single_pass, 0, sizeof(single_pass));
  single_pass.data = payload;
  single_pass.size = sizeof(payload) - 1u;
  memset(&lcsrc, 0, sizeof(lcsrc));
  lcsrc.read = single_pass_lc_source_read;
  lcsrc.reset = NULL;
  lcsrc.close = NULL;
  lcsrc.impl = &single_pass;

  source = vectis_source_from_lc(&lcsrc);
  caisrc = NULL;
  assert(vectis_cai_source_from_source(&source, &caisrc, &error) == VECTIS_OK);
  assert(caisrc != NULL);
  assert(caisrc->callbacks.reset == NULL);
  cai_error_init(&caierr);
  assert(cai_source_reset(caisrc, &caierr) != CAI_OK);
  nread = cai_source_read(caisrc, buffer, sizeof(buffer), &caierr);
  assert(nread == sizeof(payload) - 1u);
  assert(memcmp(buffer, payload, nread) == 0);
  cai_error_cleanup(&caierr);
  cai_source_close(caisrc);
}

static void test_borrowed_client_contract(void) {
  vectis_app_config config;
  vectis_error error;
  vectis_app *app;
  cai_client borrowed;
  cai_client *client;

  memset(&borrowed, 0, sizeof(borrowed));
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.cai.client = &borrowed;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  client = NULL;
  assert(app->cai_client(app, &client, &error) == VECTIS_OK);
  assert(client == &borrowed);
  client = NULL;
  assert(vectis_app_cai_client(app, &client, &error) == VECTIS_OK);
  assert(client == &borrowed);
  vectis_destroy(app);
}

static void test_borrowed_client_rejected_after_fork(void) {
  vectis_app_config config;
  vectis_error error;
  vectis_app *app;
  cai_client borrowed;
  cai_client *client;
  pid_t child;
  int status;

  memset(&borrowed, 0, sizeof(borrowed));
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.cai.client = &borrowed;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  child = fork();
  assert(child >= 0);
  if (child == 0) {
    client = NULL;
    if (app->cai_client(app, &client, &error) == VECTIS_ERR_STATE &&
        client == NULL) {
      _exit(0);
    }
    _exit(1);
  }
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status));
  assert(WEXITSTATUS(status) == 0);
  vectis_destroy(app);
}

static void test_error_mapping(void) {
  vectis_error error;
  cai_error caierr;
  char message[] = "server rejected request";
  char detail[] = "quota exhausted";

  cai_error_init(&caierr);
  caierr.code = CAI_ERR_SERVER;
  caierr.http_status = 429;
  caierr.message = message;
  caierr.detail = detail;
  assert(vectis_cai_error(&error, &caierr, "fallback") == VECTIS_ERR_STATE);
  assert(error.source == VECTIS_ERROR_SOURCE_CAI);
  assert(error.dependency_code == CAI_ERR_SERVER);
  assert(error.http_status == 429);
  assert(strcmp(error.message, message) == 0);
  assert(strcmp(error.detail, detail) == 0);
  caierr.message = NULL;
  caierr.detail = NULL;
  caierr.code = CAI_ERR_LIMIT;
  assert(vectis_cai_error(&error, &caierr, "limit") == VECTIS_ERR_CONFLICT);
  assert(error.source == VECTIS_ERROR_SOURCE_CAI);
  assert(strcmp(error.message, "limit") == 0);
}

static void test_invalid_output_adapters(void) {
  vectis_error error;
  size_t written;

  assert(vectis_cai_output_file(NULL, "/tmp/vectis-cai-unit.out", &written,
                                &error) == VECTIS_ERR_INVALID);
  assert(error.source == VECTIS_ERROR_SOURCE_VECTIS);
  assert(vectis_cai_output_response(NULL, NULL, 200, NULL, &error) ==
         VECTIS_ERR_INVALID);
  assert(vectis_cai_output_enqueue(NULL, NULL, NULL, NULL, &error) ==
         VECTIS_ERR_INVALID);
}

static int test_mcp_tool(void *context, const char *arguments_json,
                         cai_sink *output, cai_error *error) {
  const char result[] = "{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}";
  (void)context;
  (void)arguments_json;
  return cai_sink_write(output, result, sizeof(result) - 1u, error);
}

static void test_mcp_route_config(void) {
  vectis_cai_mcp_route_config config;

  vectis_cai_mcp_route_config_init(&config);
  assert(config.method == VECTIS_HTTP_ANY);
  assert((config.methods & VECTIS_HTTP_METHODS_GET) != 0u);
  assert((config.methods & VECTIS_HTTP_METHODS_POST) != 0u);
  assert((config.methods & VECTIS_HTTP_METHODS_DELETE) != 0u);
  assert(config.path == NULL);
  assert(config.handler == NULL);
  assert(config.handler_config.tools == NULL);
  assert(config.buffer_bytes == VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES);
}

static void test_mcp_route_registration(void) {
  vectis_app_config app_config;
  vectis_cai_mcp_route_config route;
  vectis_error error;
  vectis_app *app;
  cai_tool_registry *registry;
  cai_error caierr;
  const char schema[] = "{\"type\":\"object\",\"properties\":{}}";

  vectis_error_clear(&error);
  vectis_app_config_init(&app_config);
  app_config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  app = vectis_app_new(&app_config, &error);
  assert(app != NULL);

  route = vectis_cai_mcp_route_configured("/mcp", NULL);
  assert(app->cai_mcp_route(app, &route, &error) == VECTIS_ERR_INVALID);
  assert(error.source == VECTIS_ERROR_SOURCE_CAI);

  registry = NULL;
  cai_error_init(&caierr);
  assert(cai_tool_registry_new(&registry, &caierr) == CAI_OK);
  assert(cai_tool_registry_register_raw(registry, "ping", "ping tool", schema,
                                        1, test_mcp_tool, NULL,
                                        &caierr) == CAI_OK);
  route = vectis_cai_mcp_route_configured("/mcp-ready", NULL);
  route.handler_config.tools = registry;
  vectis_error_clear(&error);
  assert(vectis_register_cai_mcp_route(app, &route, &error) == VECTIS_OK);

  vectis_destroy(app);
  cai_tool_registry_destroy(registry);
  cai_error_cleanup(&caierr);
}

static void test_worker_envelope(void) {
  vectis_cai_worker_request request;
  vectis_cai_worker_event event;
  vectis_cai_worker_response response;
  vectis_mailbox_event reply_event;
  vectis_error error;
  char reply_json[] = "{\"status\":0,\"dependency_code\":0,\"http_status\":0,"
                      "\"text\":\"worker ok\"}";

  vectis_cai_worker_request_init(&request);
  assert(request.output_mode == VECTIS_CAI_WORKER_OUTPUT_TEXT);
  assert(request.max_response_bytes ==
         VECTIS_CAI_WORKER_DEFAULT_MAX_RESPONSE_BYTES);
  request.provider = "openai";
  request.model = "gpt-test";
  request.input = "hello";
  request.instructions = "be brief";
  request.max_output_tokens = 8;
  assert(vectis_cai_worker_event_build(&request, &event, &error) == VECTIS_OK);
  assert(strcmp(event.message.kind, VECTIS_CAI_WORKER_REQUEST_KIND) == 0);
  assert(event.message.expects_reply);
  assert(event.message.payload_size > 0u);
  assert(strstr((const char *)event.message.payload,
                "\"model\":\"gpt-test\"") != NULL);
  assert(strstr((const char *)event.message.payload, "\"input\":\"hello\"") !=
         NULL);
  vectis_cai_worker_event_cleanup(&event);

  vectis_cai_worker_response_init(&response);
  vectis_mailbox_event_init(&reply_event);
  reply_event.kind = VECTIS_CAI_WORKER_REPLY_KIND;
  reply_event.payload = reply_json;
  reply_event.payload_size = sizeof(reply_json) - 1u;
  assert(vectis_cai_worker_response_decode(&reply_event, &response, &error) ==
         VECTIS_OK);
  assert(response.status == VECTIS_OK);
  assert(response.source == VECTIS_ERROR_SOURCE_NONE);
  assert(response.text != NULL);
  assert(strcmp(response.text, "worker ok") == 0);
  vectis_cai_worker_response_cleanup(&response);
}

static void test_worker_rejects_unrepresentable_size_limits(void) {
#if SIZE_MAX > INT64_MAX
  vectis_cai_worker_request request;
  vectis_cai_worker_event event;
  vectis_error error;

  vectis_cai_worker_request_init(&request);
  request.provider = "test";
  request.model = "test-model";
  request.input = "hello";
  request.max_response_bytes = (size_t)INT64_MAX + (size_t)1u;
  assert(vectis_cai_worker_event_build(&request, &event, &error) ==
         VECTIS_ERR_INVALID);
  assert(event.message.payload == NULL);
  assert(strstr(error.message, "max_response_bytes") != NULL);
#endif
}

static void test_worker_service_registration_and_error_reply(void) {
  vectis_app_config app_config;
  vectis_app *app;
  vectis_error error;
  vectis_mailbox_config mailbox_config;
  vectis_mailbox *mailbox;
  vectis_mailbox_broker_config broker_config;
  vectis_mailbox_broker *broker;
  vectis_cai_worker_service_config worker_config;
  vectis_managed_service *service;
  vectis_managed_service_state service_state;
  vectis_mailbox_message bad_message;
  vectis_mailbox_message late_message;
  vectis_mailbox_event reply_event;
  vectis_cai_worker_response response;
  vectis_status status;
  unsigned long correlation_id;

  vectis_mailbox_config_init(&mailbox_config);
  mailbox_config.capacity = 4u;
  mailbox = NULL;
  assert(vectis_mailbox_new(&mailbox_config, &mailbox, &error) == VECTIS_OK);
  vectis_mailbox_broker_config_init(&broker_config);
  broker_config.request_mailbox = mailbox;
  broker = NULL;
  assert(vectis_mailbox_broker_new(&broker_config, &broker, &error) ==
         VECTIS_OK);

  vectis_app_config_init(&app_config);
  app = vectis_app_new(&app_config, &error);
  assert(app != NULL);
  assert(app->cai_worker_service != NULL);
  vectis_cai_worker_service_config_init(&worker_config);
  assert(worker_config.start_with_app == 1);
  assert(worker_config.poll_timeout_ms ==
         VECTIS_CAI_WORKER_DEFAULT_POLL_TIMEOUT_MS);
  worker_config.name = "cai-worker-test";
  worker_config.request_mailbox = mailbox;
  worker_config.reply_broker = broker;
  worker_config.client.api_key = "test-key";
  worker_config.poll_timeout_ms = 10L;
  service = NULL;
  worker_config.client.chatgpt_auth = (cai_chatgpt_auth *)0x1;
  status = app->cai_worker_service(app, &worker_config, &service, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(service == NULL);
  assert(strstr(error.message, "chatgpt_auth") != NULL);
  worker_config.client.chatgpt_auth = NULL;
  status = app->cai_worker_service(app, &worker_config, &service, &error);
  assert(status == VECTIS_OK);
  assert(service != NULL);
  assert(service->state(service, &service_state, &error) == VECTIS_OK);
  assert(service_state.declared);
  assert(service_state.start_requested);
  assert(!service_state.materialized);

  assert(app->start(app, &error) == VECTIS_OK);
  assert(service->state(service, &service_state, &error) == VECTIS_OK);
  assert(service_state.materialized);
  assert(service_state.started);
  assert(service_state.monitor_active);

  memset(&bad_message, 0, sizeof(bad_message));
  bad_message.kind = "vectis.cai.unsupported";
  bad_message.expects_reply = 1;
  vectis_mailbox_event_init(&reply_event);
  status = broker->request(broker, &bad_message, 3000L, &reply_event,
                           &correlation_id, &error);
  assert(status == VECTIS_OK);
  assert(correlation_id != 0u);
  vectis_cai_worker_response_init(&response);
  assert(vectis_cai_worker_response_decode(&reply_event, &response, &error) ==
         VECTIS_OK);
  assert(response.status == VECTIS_ERR_INVALID);
  assert(response.source == VECTIS_ERROR_SOURCE_VECTIS);
  assert(strstr(response.message, "event kind") != NULL);
  vectis_cai_worker_response_cleanup(&response);
  vectis_mailbox_event_cleanup(&reply_event);

  broker->close(broker);
  vectis_mailbox_message_init(&late_message);
  late_message.kind = "vectis.cai.unsupported";
  late_message.expects_reply = 1;
  status =
      mailbox->publish_request(mailbox, &late_message, &correlation_id, &error);
  assert(status == VECTIS_OK);
  usleep(100000u);
  assert(service->state(service, &service_state, &error) == VECTIS_OK);
  assert(service_state.started);
  assert(!service_state.failed);

  assert(app->stop(app, &error) == VECTIS_OK);
  assert(service->state(service, &service_state, &error) == VECTIS_OK);
  assert(service_state.stop_requested);
  assert(!service_state.started);
  service->close(service);
  app->close(app);
  broker->destroy(broker);
  mailbox->destroy(mailbox);
}

int main(void) {
  vectis_cai_config config;

  vectis_cai_config_init(&config);
  assert(config.client == NULL);
  assert(config.logger == NULL);
  assert(config.logger_disabled == 0);
  assert(strcmp(vectis_error_source_string(VECTIS_ERROR_SOURCE_CAI), "cai") ==
         0);

  test_source_from_memory();
  test_source_from_request();
  test_source_from_single_pass_lc_source_has_no_reset();
  test_borrowed_client_contract();
  test_borrowed_client_rejected_after_fork();
  test_error_mapping();
  test_invalid_output_adapters();
  test_mcp_route_config();
  test_mcp_route_registration();
  test_worker_envelope();
  test_worker_rejects_unrepresentable_size_limits();
  test_worker_service_registration_and_error_reply();

  return 0;
}
