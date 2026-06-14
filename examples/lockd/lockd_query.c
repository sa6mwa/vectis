#include <stdlib.h>

#include <lc/lc.h>

int main(void) {
  lc_client_config config;
  lc_client *client;
  lc_query_req query;
  lc_query_res result;
  lc_sink *sink;
  lc_error error;
  const char *endpoints[1];

  lc_client_config_init(&config);
  lc_query_req_init(&query);
  lc_error_init(&error);
  client = NULL;
  sink = NULL;

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
  if (lc_sink_to_file("query-results.jsonl", &sink, &error) != LC_OK) {
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }

  query.selector_json = "{\"status\":\"active\"}";
  query.limit = 100L;
  query.return_mode = "jsonl";
  if (lc_query(client, &query, sink, &result, &error) != LC_OK) {
    lc_sink_close(sink);
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }

  lc_query_res_cleanup(&result);
  lc_sink_close(sink);
  lc_client_close(client);
  lc_error_cleanup(&error);
  return 0;
}
