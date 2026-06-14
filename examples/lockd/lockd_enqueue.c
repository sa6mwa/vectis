#include <stdlib.h>

#include <lc/lc.h>

int main(void) {
  lc_client_config config;
  lc_client *client;
  lc_enqueue_req enqueue;
  lc_enqueue_res result;
  lc_source *payload;
  lc_error error;
  const char *endpoints[1];
  const char json[] = "{\"type\":\"order.created\",\"id\":\"1001\"}";

  lc_client_config_init(&config);
  lc_enqueue_req_init(&enqueue);
  lc_error_init(&error);
  client = NULL;
  payload = NULL;

  endpoints[0] = getenv("LOCKD_ENDPOINT") != NULL ? getenv("LOCKD_ENDPOINT")
                                                  : "https://127.0.0.1:8443";
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
  if (lc_source_from_memory(json, sizeof(json) - 1u, &payload, &error) !=
      LC_OK) {
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }

  enqueue.queue = "orders";
  enqueue.visibility_timeout_seconds = 30L;
  enqueue.ttl_seconds = 3600L;
  enqueue.max_attempts = 5;
  enqueue.content_type = "application/json";
  if (lc_enqueue(client, &enqueue, payload, &result, &error) != LC_OK) {
    lc_source_close(payload);
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }

  lc_enqueue_res_cleanup(&result);
  lc_source_close(payload);
  lc_client_close(client);
  lc_error_cleanup(&error);
  return 0;
}
