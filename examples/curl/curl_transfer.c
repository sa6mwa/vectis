#include <string.h>

#include <vectis/vectis.h>

int main(void) {
  vectis_http_client_config client;
  vectis_http_client *handle;
  vectis_http_request upload;
  vectis_http_request download;
  vectis_http_response response = {0};
  vectis_error error;
  const char *headers[] = {"content-type: application/octet-stream"};

  handle = NULL;
  vectis_http_client_config_init(&client);
  client.client_bundle = vectis_source_from_path("/etc/vectis/downstream-client.pem");
  client.timeout_ms = 60000L;
  if (vectis_http_client_new(&client, &handle, &error) != VECTIS_OK) {
    return 1;
  }

  vectis_http_request_init(&download);
  download.method = VECTIS_HTTP_GET;
  download.url = "https://api.example.com/export/orders.ndjson";
  download.download_path = "var/orders.ndjson";
  (void)vectis_http_client_execute(handle, &download, &response, &error);
  vectis_http_response_cleanup(&response);

  memset(&response, 0, sizeof(response));
  vectis_http_request_init(&upload);
  upload.method = VECTIS_HTTP_PUT;
  upload.url = "https://api.example.com/import/orders.ndjson";
  upload.headers = headers;
  upload.header_count = 1u;
  upload.body_path = "var/orders.ndjson";
  upload.content_type = "application/octet-stream";
  (void)vectis_http_client_execute(handle, &upload, &response, &error);
  vectis_http_response_cleanup(&response);

  vectis_http_client_destroy(handle);
  return 0;
}
