#include <stdlib.h>

#include <lc/lc.h>

int main(void) {
  lc_client_config config;
  lc_client *client;
  lc_lease *lease;
  lc_source *upload;
  lc_sink *download;
  lc_acquire_req acquire;
  lc_release_req release;
  lc_attach_req attach;
  lc_attach_res attach_result;
  lc_attachment_get_req get;
  lc_attachment_get_res get_result;
  lc_error error;
  const char *endpoints[1];

  lc_client_config_init(&config);
  lc_acquire_req_init(&acquire);
  lc_release_req_init(&release);
  lc_attach_req_init(&attach);
  lc_attachment_get_req_init(&get);
  lc_error_init(&error);
  client = NULL;
  lease = NULL;
  upload = NULL;
  download = NULL;

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

  acquire.key = "accounts/1001";
  acquire.owner = "vectis-attachment-example";
  acquire.ttl_seconds = 30L;
  if (lc_acquire(client, &acquire, &lease, &error) != LC_OK) {
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }

  if (lc_source_from_file("invoice.pdf", &upload, &error) != LC_OK) {
    lc_lease_close(lease);
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }
  attach.name = "invoice.pdf";
  attach.content_type = "application/pdf";
  if (lease->attach(lease, &attach, upload, &attach_result, &error) != LC_OK) {
    lc_source_close(upload);
    lc_lease_close(lease);
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }
  lc_attach_res_cleanup(&attach_result);
  lc_source_close(upload);

  get.selector.name = "invoice.pdf";
  if (lc_sink_to_file("downloaded-invoice.pdf", &download, &error) != LC_OK) {
    lc_lease_close(lease);
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }
  if (lease->get_attachment(lease, &get, download, &get_result, &error) !=
      LC_OK) {
    lc_sink_close(download);
    lc_lease_close(lease);
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }

  lc_attachment_get_res_cleanup(&get_result);
  lc_sink_close(download);
  if (lease->release(lease, &release, &error) != LC_OK) {
    lc_lease_close(lease);
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }
  lc_client_close(client);
  lc_error_cleanup(&error);
  return 0;
}
