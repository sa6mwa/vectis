#include <assert.h>
#include <string.h>

#include <lonejson.h>
#include <vectis/vectis.h>

typedef struct sample_doc {
  char id[32];
} sample_doc;

static const lonejson_field sample_doc_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(sample_doc, id, "id", LONEJSON_OVERFLOW_FAIL)};

LONEJSON_MAP_DEFINE(sample_doc_map, sample_doc, sample_doc_fields);

static vectis_status sample_json_handler(vectis_app *app,
                                         vectis_request *request,
                                         void *input,
                                         void *output,
                                         void *userdata,
                                         vectis_error *error) {
  (void)app;
  (void)request;
  (void)input;
  (void)output;
  (void)userdata;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status sample_route_handler(vectis_app *app,
                                          vectis_request *request,
                                          vectis_response *response,
                                          void *userdata,
                                          vectis_error *error) {
  (void)app;
  (void)request;
  (void)response;
  (void)userdata;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static void assert_http_surface(void) {
  vectis_http_client_config client;
  vectis_http_client *handle;
  vectis_http_request request;
  vectis_http_response response;
  vectis_error error;
  sample_doc doc;
  vectis_status status;

  handle = NULL;
  memset(&response, 0, sizeof(response));
  memset(&doc, 0, sizeof(doc));
  vectis_http_client_config_init(&client);
  assert(client.timeout_ms == 30000L);
  assert(client.connect_timeout_ms == 10000L);
  assert(client.follow_redirects == 1);

  status = vectis_http_get(&client, NULL, &response, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "url") != NULL);

  client.base_url = "https://api.example.com";
  status = vectis_http_post_json(&client, "/docs", &sample_doc_map, &doc, &response, &error);
  assert(status == VECTIS_ERR_NOT_IMPLEMENTED);
  assert(strstr(error.message, "curl HTTP execution") != NULL);

  client.client_bundle = vectis_source_from_path("/tmp/client.pem");
  status = vectis_http_client_new(&client, &handle, &error);
  assert(status == VECTIS_OK);
  assert(handle != NULL);
  vectis_http_request_init(&request);
  request.url = "/docs";
  status = vectis_http_client_execute(handle, &request, &response, &error);
  assert(status == VECTIS_ERR_NOT_IMPLEMENTED);
  assert(error.source == VECTIS_ERROR_SOURCE_CURL);
  status = vectis_http_client_post_json(handle, "/docs", &sample_doc_map, &doc, &response, &error);
  assert(status == VECTIS_ERR_NOT_IMPLEMENTED);
  assert(error.source == VECTIS_ERROR_SOURCE_CURL);
  vectis_http_client_destroy(handle);

  vectis_http_response_cleanup(&response);
}

static void assert_io_surface(void) {
  vectis_sftp_config sftp;
  vectis_ssh_config ssh;
  vectis_ssh_exec_result result;
  vectis_mqtt_config mqtt;
  vectis_cert_bundle_config certs;
  vectis_error error;
  vectis_status status;
  const char payload[] = "ready";

  vectis_sftp_config_init(&sftp);
  assert(sftp.timeout_ms == 30000L);
  status = vectis_sftp_upload_file(&sftp, "local", "remote", &error);
  assert(status == VECTIS_ERR_INVALID);

  vectis_ssh_config_init(&ssh);
  memset(&result, 0, sizeof(result));
  assert(ssh.port == 22u);
  assert(ssh.timeout_ms == 30000L);
  status = vectis_ssh_exec(&ssh, "uptime", &result, &error);
  assert(status == VECTIS_ERR_INVALID);
  ssh.host = "worker.internal";
  ssh.username = "vectis";
  status = vectis_ssh_sftp_upload_file(&ssh, "local", "remote", &error);
  assert(status == VECTIS_ERR_NOT_IMPLEMENTED);
  vectis_ssh_exec_result_cleanup(&result);

  vectis_mqtt_config_init(&mqtt);
  mqtt.broker_url = "mqtts://broker.example.com";
  status = vectis_mqtt_publish(&mqtt,
                               "workflow/test",
                               payload,
                               sizeof(payload) - 1u,
                               "text/plain",
                               &error);
  assert(status == VECTIS_ERR_NOT_IMPLEMENTED);

  vectis_cert_bundle_config_init(&certs);
  assert(certs.key_bits == 4096u);
  assert(certs.valid_days == 397L);
  status = vectis_cert_generate_bundle(&certs, &error);
  assert(status == VECTIS_ERR_INVALID);
}

static void assert_json_route_surface(void) {
  vectis_app_config config;
  vectis_json_route_config route;
  vectis_route_config raw_route;
  vectis_error error;
  vectis_status status;
  vectis_app *app;

  vectis_app_config_init(&config);
  config.tls.cert_key_bundle_path = "/tmp/server.pem";
  config.lockd.unix_socket_path = "/tmp/lockd.sock";
  app = vectis_new(&config, &error);
  assert(app != NULL);
  assert(vectis_lockd_client(app) == NULL);

  raw_route = vectis_route(VECTIS_HTTP_GET, "/state/:id?", sample_route_handler, NULL);
  assert(raw_route.path_kind == VECTIS_ROUTE_PATH_PARAMS);
  status = vectis_register_route(app, &raw_route, &error);
  assert(status == VECTIS_OK);

  raw_route = vectis_route_regex(VECTIS_HTTP_GET, "^/internal/[0-9]+$", sample_route_handler, NULL);
  assert(raw_route.path_kind == VECTIS_ROUTE_PATH_REGEX);
  status = vectis_register_route(app, &raw_route, &error);
  assert(status == VECTIS_OK);

  route = vectis_json_route(VECTIS_HTTP_POST,
                            "/typed/:id",
                            &sample_doc_map,
                            sizeof(sample_doc),
                            &sample_doc_map,
                            sizeof(sample_doc),
                            sample_json_handler,
                            NULL);
  assert(route.path_kind == VECTIS_ROUTE_PATH_PARAMS);
  status = vectis_register_json_route(app, &route, &error);
  assert(status == VECTIS_ERR_NOT_IMPLEMENTED);
  assert(error.source == VECTIS_ERROR_SOURCE_KORE);
  assert(strstr(error.message, "JSON route auto-wiring") != NULL);

  vectis_destroy(app);
}

static void assert_tls_source_surface(void) {
  vectis_app_config config;
  vectis_error error;
  vectis_status status;
  vectis_app *app;
  const char server_bundle[] =
      "-----BEGIN CERTIFICATE-----\n"
      "server\n"
      "-----END CERTIFICATE-----\n"
      "-----BEGIN PRIVATE KEY-----\n"
      "key\n"
      "-----END PRIVATE KEY-----\n";
  const char client_ca[] =
      "-----BEGIN CERTIFICATE-----\n"
      "ca\n"
      "-----END CERTIFICATE-----\n";

  vectis_app_config_init(&config);
  config.tls.cert_key_bundle = vectis_source_from_memory(server_bundle, sizeof(server_bundle) - 1u);
  config.lockd.unix_socket_path = "/tmp/lockd.sock";
  app = vectis_new(&config, &error);
  assert(app != NULL);
  status = vectis_start(app, &error);
  assert(status == VECTIS_ERR_NOT_IMPLEMENTED);
  vectis_destroy(app);

  vectis_app_config_init(&config);
  config.tls.cert_key_bundle = vectis_source_from_memory(server_bundle, sizeof(server_bundle) - 1u);
  config.tls.require_client_certificate = 1;
  config.lockd.unix_socket_path = "/tmp/lockd.sock";
  app = vectis_new(&config, &error);
  assert(app != NULL);
  status = vectis_start(app, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "client_ca_bundle") != NULL);
  vectis_destroy(app);

  vectis_app_config_init(&config);
  config.tls.cert_key_bundle = vectis_source_from_memory(server_bundle, sizeof(server_bundle) - 1u);
  config.tls.client_ca_bundle = vectis_source_from_memory(client_ca, sizeof(client_ca) - 1u);
  config.tls.require_client_certificate = 1;
  config.lockd.unix_socket_path = "/tmp/lockd.sock";
  app = vectis_new(&config, &error);
  assert(app != NULL);
  status = vectis_start(app, &error);
  assert(status == VECTIS_ERR_NOT_IMPLEMENTED);
  vectis_destroy(app);
}

int main(void) {
  assert_http_surface();
  assert_io_surface();
  assert_json_route_surface();
  assert_tls_source_surface();
  return 0;
}
