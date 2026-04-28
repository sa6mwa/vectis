#include <stdlib.h>

#include <lc/lc.h>

int main(void) {
  lc_client_config config;
  lc_client *client;
  lc_source *bundle;
  lc_error error;
  const char *endpoint;
  const char *bundle_path;
  const char *endpoints[1];

  lc_client_config_init(&config);
  lc_error_init(&error);
  client = NULL;
  bundle = NULL;

  endpoint = getenv("LOCKD_ENDPOINT");
  bundle_path = getenv("LOCKD_CLIENT_BUNDLE");
  if (endpoint == NULL) {
    endpoint = "https://127.0.0.1:8443";
  }
  endpoints[0] = endpoint;
  config.endpoints = endpoints;
  config.endpoint_count = 1u;
  config.default_namespace = "examples";
  config.timeout_ms = 30000L;

  if (bundle_path != NULL) {
    if (lc_source_from_file(bundle_path, &bundle, &error) != LC_OK) {
      lc_error_cleanup(&error);
      return 1;
    }
    config.client_bundle_source = bundle;
  } else {
    config.disable_mtls = 1;
    config.insecure_skip_verify = 1;
  }

  if (lc_client_open(&config, &client, &error) != LC_OK) {
    lc_source_close(bundle);
    lc_error_cleanup(&error);
    return 1;
  }

  lc_client_close(client);
  lc_source_close(bundle);
  lc_error_cleanup(&error);
  return 0;
}
