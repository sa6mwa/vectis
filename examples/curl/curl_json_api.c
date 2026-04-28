#include <stddef.h>

#include <lonejson.h>
#include <vectis/vectis.h>

typedef struct downstream_request {
  char event[64];
  lonejson_int64 count;
} downstream_request;

static const lonejson_field downstream_request_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(downstream_request, event, "event", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64(downstream_request, count, "count")};

LONEJSON_MAP_DEFINE(downstream_request_map, downstream_request, downstream_request_fields);

int main(void) {
  vectis_http_client_config client;
  vectis_http_client *handle;
  vectis_http_response response = {0};
  vectis_error error;
  downstream_request payload;

  handle = NULL;
  vectis_http_client_config_init(&client);
  vectis_http_response_cleanup(&response);

  client.base_url = "https://api.example.com";
  client.client_bundle = vectis_source_from_path("/etc/vectis/downstream-client.pem");
  client.timeout_ms = 5000L;

  payload.event[0] = 'o';
  payload.event[1] = 'r';
  payload.event[2] = 'd';
  payload.event[3] = 'e';
  payload.event[4] = 'r';
  payload.event[5] = '\0';
  payload.count = 1;

  if (vectis_http_client_new(&client, &handle, &error) != VECTIS_OK) {
    return 1;
  }

  (void)vectis_http_client_get(handle, "/health", &response, &error);
  vectis_http_response_cleanup(&response);

  (void)vectis_http_client_head(handle, "/health", &response, &error);
  vectis_http_response_cleanup(&response);

  (void)vectis_http_client_options(handle, "/events", &response, &error);
  vectis_http_response_cleanup(&response);

  (void)vectis_http_client_post_json(handle, "/events", &downstream_request_map, &payload, &response, &error);
  vectis_http_response_cleanup(&response);

  (void)vectis_http_client_put_json(handle, "/events/order", &downstream_request_map, &payload, &response, &error);
  vectis_http_response_cleanup(&response);

  (void)vectis_http_client_patch_json(handle, "/events/order", &downstream_request_map, &payload, &response, &error);
  vectis_http_response_cleanup(&response);

  (void)vectis_http_client_delete(handle, "/events/order", &response, &error);
  vectis_http_response_cleanup(&response);

  vectis_http_client_destroy(handle);
  return 0;
}
