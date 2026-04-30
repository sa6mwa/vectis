#include <stdio.h>

#include <lonejson.h>
#include <vectis/vectis.h>

typedef struct order_request {
  char id[64];
} order_request;

typedef struct order_created {
  char id[64];
  char status[32];
} order_created;

typedef struct api_error {
  char code[32];
  char message[128];
} api_error;

static const lonejson_field order_request_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(order_request, id, "id", LONEJSON_OVERFLOW_FAIL)};

static const lonejson_field order_created_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(order_created, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(order_created, status, "status", LONEJSON_OVERFLOW_FAIL)};

static const lonejson_field api_error_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(api_error, code, "code", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(api_error, message, "message", LONEJSON_OVERFLOW_FAIL)};

LONEJSON_MAP_DEFINE(order_request_map, order_request, order_request_fields);
LONEJSON_MAP_DEFINE(order_created_map, order_created, order_created_fields);
LONEJSON_MAP_DEFINE(api_error_map, api_error, api_error_fields);

int main(void) {
  vectis_app_config config;
  vectis_openapi_route_doc route_doc;
  vectis_openapi_document document;
  vectis_mutable_bytes yaml;
  vectis_error error;
  vectis_app *app;
  const char *tags[] = {"orders"};

  vectis_app_config_init(&config);
  config.tls.cert_key_bundle_path = "/tmp/server.pem";
  app = vectis_app_new(&config, &error);
  if (app == NULL) {
    return 1;
  }

  vectis_openapi_route_doc_init(&route_doc);
  route_doc.summary = "Create order";
  route_doc.operation_id = "createOrder";
  route_doc.tags = tags;
  route_doc.tag_count = 1u;
  if (vectis_openapi_request_json(&route_doc,
                                  vectis_openapi_lonejson_schema("OrderRequest",
                                                                 &order_request_map),
                                  &error) != VECTIS_OK ||
      vectis_openapi_response_json(&route_doc,
                                   201,
                                   "Created",
                                   vectis_openapi_lonejson_schema("OrderCreated",
                                                                  &order_created_map),
                                   &error) != VECTIS_OK ||
      vectis_openapi_response_json(&route_doc,
                                   409,
                                   "Conflict",
                                   vectis_openapi_lonejson_schema("ApiError", &api_error_map),
                                   &error) != VECTIS_OK ||
      vectis_attach_openapi_doc(app,
                                VECTIS_HTTP_METHODS_POST,
                                "/orders/:id?",
                                &route_doc,
                                &error) != VECTIS_OK) {
    vectis_openapi_route_doc_cleanup(&route_doc);
    app->close(app);
    return 1;
  }

  vectis_openapi_document_init(&document);
  document.title = "Orders API";
  document.version = "1.0.0";
  if (vectis_generate_openapi(app, &document, VECTIS_OPENAPI_YAML, &yaml, &error) != VECTIS_OK) {
    vectis_openapi_route_doc_cleanup(&route_doc);
    app->close(app);
    return 1;
  }
  (void)fwrite(yaml.data, 1u, yaml.size, stdout);
  vectis_mutable_bytes_cleanup(&yaml);
  vectis_openapi_route_doc_cleanup(&route_doc);
  app->close(app);
  return 0;
}
