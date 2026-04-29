#include <vectis/vectis.h>
#include <stdio.h>

static vectis_status inspect_chunk(const void *data,
                                   size_t size,
                                   void *userdata,
                                   vectis_error *error) {
  (void)error;
  (void)fprintf((FILE *)userdata, "received %lu bytes\n", (unsigned long)size);
  (void)data;
  return VECTIS_OK;
}

int main(void) {
  vectis_http_client_config client;
  vectis_http_client *handle;
  vectis_http_request request;
  vectis_http_response response = {0};
  vectis_error error;

  handle = NULL;
  vectis_http_client_config_init(&client);
  client.client_bundle = vectis_source_from_path("/etc/vectis/downstream-client.pem");
  client.timeout_ms = 60000L;
  client.low_speed_limit_bytes_per_sec = 1024L;
  client.low_speed_time_seconds = 30L;
  if (vectis_http_client_new(&client, &handle, &error) != VECTIS_OK) {
    return 1;
  }

  (void)vectis_http_client_download_file(handle,
                                         "https://api.example.com/export/orders.ndjson",
                                         "var/orders.ndjson",
                                         &response,
                                         &error);
  vectis_http_response_cleanup(&response);

  vectis_http_request_init(&request);
  request.url = "https://api.example.com/export/orders.ndjson";
  request.response_body = inspect_chunk;
  request.response_body_userdata = stdout;
  (void)vectis_http_client_execute(handle, &request, &response, &error);
  vectis_http_response_cleanup(&response);

  (void)vectis_http_client_upload_file(handle,
                                       VECTIS_HTTP_PUT,
                                       "https://api.example.com/import/orders.ndjson",
                                       "var/orders.ndjson",
                                       "application/octet-stream",
                                       &response,
                                       &error);
  vectis_http_response_cleanup(&response);

  vectis_http_client_destroy(handle);
  return 0;
}
