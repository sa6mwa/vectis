#include <stdlib.h>

#include <lc/lc.h>

int main(void) {
  lc_client_config config;
  lc_client *client;
  lc_dequeue_req dequeue;
  lc_nack_req nack;
  lc_extend_req extend;
  lc_message *message;
  lc_sink *payload;
  lc_error error;
  size_t written;
  const char *endpoints[1];

  lc_client_config_init(&config);
  lc_dequeue_req_init(&dequeue);
  lc_nack_req_init(&nack);
  lc_extend_req_init(&extend);
  lc_error_init(&error);
  client = NULL;
  message = NULL;
  payload = NULL;
  written = 0u;

  endpoints[0] = getenv("LOCKD_ENDPOINT") != NULL ? getenv("LOCKD_ENDPOINT") : "https://127.0.0.1:8443";
  config.endpoints = endpoints;
  config.endpoint_count = 1u;
  config.client_bundle_path = getenv("LOCKD_CLIENT_BUNDLE");
  config.default_namespace = "examples";
  config.disable_mtls = config.client_bundle_path == NULL;
  config.insecure_skip_verify = config.client_bundle_path == NULL;

  if (lc_client_open(&config, &client, &error) != LC_OK) {
    lc_error_cleanup(&error);
    return 1;
  }

  dequeue.queue = "orders";
  dequeue.owner = "manual-worker";
  dequeue.visibility_timeout_seconds = 30L;
  dequeue.wait_seconds = 5L;
  if (lc_dequeue(client, &dequeue, &message, &error) != LC_OK) {
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }
  if (message == NULL) {
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 0;
  }

  if (lc_sink_to_file("dequeued-payload.json", &payload, &error) != LC_OK) {
    message->close(message);
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }
  if (message->write_payload(message, payload, &written, &error) != LC_OK) {
    nack.delay_seconds = 30L;
    nack.intent = LC_NACK_INTENT_FAILURE;
    nack.last_error_json = "{\"error\":\"payload write failed\"}";
    (void)message->nack(message, &nack, &error);
    lc_sink_close(payload);
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }

  extend.extend_by_seconds = 30L;
  (void)message->extend(message, &extend, &error);
  if (message->ack(message, &error) != LC_OK) {
    message->close(message);
    lc_sink_close(payload);
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }

  lc_sink_close(payload);
  lc_client_close(client);
  lc_error_cleanup(&error);
  return 0;
}
