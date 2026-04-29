#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <lc/lc.h>
#include <lonejson.h>
#include "vectis_internal.h"
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
  sample_doc *in_doc;
  sample_doc *out_doc;

  (void)app;
  (void)request;
  (void)userdata;
  in_doc = (sample_doc *)input;
  out_doc = (sample_doc *)output;
  if (in_doc != NULL && out_doc != NULL) {
    memcpy(out_doc->id, in_doc->id, strlen(in_doc->id) + 1u);
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status sample_route_handler(vectis_app *app,
                                          vectis_request *request,
                                          vectis_response *response,
                                          void *userdata,
                                          vectis_error *error) {
  const char *id;

  (void)app;
  (void)userdata;
  id = vectis_request_path_param(request, "id");
  if (id != NULL) {
    return vectis_response_text(response, 200, "text/plain", id, error);
  }
  return vectis_response_status(response, 204, error);
}

static int sample_consumer_handler(void *context,
                                   lc_consumer_message *message,
                                   lc_error *error) {
  (void)context;
  (void)message;
  (void)error;
  return LC_OK;
}

static vectis_status curl_config_ok(CURL *curl, void *userdata, vectis_error *error) {
  int *count;

  (void)error;
  count = (int *)userdata;
  if (count != NULL) {
    (*count)++;
  }
  (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, "vectis-test");
  return VECTIS_OK;
}

static vectis_status curl_config_fail(CURL *curl, void *userdata, vectis_error *error) {
  (void)curl;
  (void)userdata;
  vectis_set_error(error, VECTIS_ERR_STATE, "raw curl configuration failed");
  return VECTIS_ERR_STATE;
}

static vectis_status response_stream_ok(const void *data,
                                        size_t size,
                                        void *userdata,
                                        vectis_error *error) {
  sample_doc *doc;

  (void)error;
  doc = (sample_doc *)userdata;
  assert(size < sizeof(doc->id));
  memcpy(doc->id, data, size);
  doc->id[size] = '\0';
  return VECTIS_OK;
}

static vectis_status response_stream_fail(const void *data,
                                          size_t size,
                                          void *userdata,
                                          vectis_error *error) {
  (void)data;
  (void)size;
  (void)userdata;
  vectis_set_error(error, VECTIS_ERR_STATE, "stream callback failed");
  return VECTIS_ERR_STATE;
}

static void assert_http_surface(void) {
  vectis_http_client_config client;
  vectis_http_client *handle;
  vectis_http_request request;
  vectis_http_response response;
  vectis_error error;
  sample_doc doc;
  vectis_status status;
  FILE *fp;
  const char source_path[] = "/tmp/vectis_http_source.txt";
  const char upload_path[] = "/tmp/vectis_http_upload.txt";
  const char helper_download_path[] = "/tmp/vectis_http_download_helper.txt";
  const char helper_upload_path[] = "/tmp/vectis_http_upload_helper.txt";
  const char missing_upload_path[] = "/tmp/vectis_http_missing_upload.txt";
  const char source_body[] = "vectis file body";
  const char upload_body[] = "upload body";
  int curl_config_count;

  assert(strcmp(vectis_status_string(VECTIS_ERR_TIMEOUT), "timeout") == 0);
  curl_config_count = 0;
  handle = NULL;
  memset(&response, 0, sizeof(response));
  memset(&doc, 0, sizeof(doc));
  vectis_http_client_config_init(&client);
  assert(client.timeout_ms == 30000L);
  assert(client.connect_timeout_ms == 10000L);
  assert(client.follow_redirects == 1);
  assert(client.proxy_url == NULL);
  assert(client.low_speed_limit_bytes_per_sec == 0L);
  assert(client.low_speed_time_seconds == 0L);
  client.low_speed_limit_bytes_per_sec = -1L;
  status = vectis_http_client_new(&client, &handle, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "non-negative") != NULL);
  client.low_speed_limit_bytes_per_sec = 0L;
  client.configure_curl = curl_config_ok;
  client.configure_curl_userdata = &curl_config_count;

  status = vectis_http_get(&client, NULL, &response, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "url") != NULL);

  fp = fopen(source_path, "wb");
  assert(fp != NULL);
  assert(fwrite(source_body, 1u, sizeof(source_body) - 1u, fp) == sizeof(source_body) - 1u);
  assert(fclose(fp) == 0);

  client.base_url = "file:///tmp";
  status = vectis_http_get(&client, "/vectis_http_source.txt", &response, &error);
  assert(status == VECTIS_OK);
  assert(curl_config_count == 1);
  assert(response.body_size == sizeof(source_body) - 1u);
  assert(memcmp(response.body, source_body, sizeof(source_body) - 1u) == 0);
  vectis_http_response_cleanup(&response);

  status = vectis_http_client_new(&client, &handle, &error);
  assert(status == VECTIS_OK);
  assert(handle != NULL);
  vectis_http_request_init(&request);
  request.url = "/vectis_http_source.txt";
  status = vectis_http_client_execute(handle, &request, &response, &error);
  assert(status == VECTIS_OK);
  assert(curl_config_count == 2);
  assert(response.body_size == sizeof(source_body) - 1u);
  vectis_http_response_cleanup(&response);

  vectis_http_request_init(&request);
  request.url = "/vectis_http_source.txt";
  request.configure_curl = curl_config_fail;
  status = vectis_http_client_execute(handle, &request, &response, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "raw curl") != NULL);

  status = vectis_http_client_download_file(handle,
                                            "/vectis_http_source.txt",
                                            helper_download_path,
                                            &response,
                                            &error);
  assert(status == VECTIS_OK);
  vectis_http_response_cleanup(&response);
  fp = fopen(helper_download_path, "rb");
  assert(fp != NULL);
  memset(&doc, 0, sizeof(doc));
  assert(fread(doc.id, 1u, sizeof(source_body) - 1u, fp) == sizeof(source_body) - 1u);
  assert(fclose(fp) == 0);
  assert(memcmp(doc.id, source_body, sizeof(source_body) - 1u) == 0);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_PUT;
  request.url = "file:///tmp/vectis_http_upload.txt";
  request.body = upload_body;
  request.body_size = sizeof(upload_body) - 1u;
  status = vectis_http_client_execute(handle, &request, &response, &error);
  assert(status == VECTIS_OK);
  vectis_http_response_cleanup(&response);
  fp = fopen(upload_path, "rb");
  assert(fp != NULL);
  memset(&doc, 0, sizeof(doc));
  assert(fread(doc.id, 1u, sizeof(upload_body) - 1u, fp) == sizeof(upload_body) - 1u);
  assert(fclose(fp) == 0);
  assert(memcmp(doc.id, upload_body, sizeof(upload_body) - 1u) == 0);

  status = vectis_http_upload_file(&client,
                                   VECTIS_HTTP_PUT,
                                   "file:///tmp/vectis_http_upload_helper.txt",
                                   source_path,
                                   "text/plain",
                                   &response,
                                   &error);
  assert(status == VECTIS_OK);
  vectis_http_response_cleanup(&response);
  fp = fopen(helper_upload_path, "rb");
  assert(fp != NULL);
  memset(&doc, 0, sizeof(doc));
  assert(fread(doc.id, 1u, sizeof(source_body) - 1u, fp) == sizeof(source_body) - 1u);
  assert(fclose(fp) == 0);
  assert(memcmp(doc.id, source_body, sizeof(source_body) - 1u) == 0);

  vectis_http_request_init(&request);
  request.url = "/vectis_http_source.txt";
  request.low_speed_limit_bytes_per_sec = 1L;
  request.low_speed_time_seconds = 10L;
  request.response_body = response_stream_ok;
  request.response_body_userdata = &doc;
  memset(&doc, 0, sizeof(doc));
  status = vectis_http_client_execute(handle, &request, &response, &error);
  assert(status == VECTIS_OK);
  assert(response.body == NULL);
  assert(response.body_size == 0u);
  assert(memcmp(doc.id, source_body, sizeof(source_body) - 1u) == 0);
  vectis_http_response_cleanup(&response);

  vectis_http_request_init(&request);
  request.url = "/vectis_http_source.txt";
  request.response_body = response_stream_fail;
  status = vectis_http_client_execute(handle, &request, &response, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "stream callback") != NULL);

  vectis_http_request_init(&request);
  request.url = "/vectis_http_source.txt";
  request.download_path = helper_download_path;
  request.response_body = response_stream_ok;
  status = vectis_http_client_execute(handle, &request, &response, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "download_path") != NULL);

  status = vectis_http_client_upload_file(handle,
                                          VECTIS_HTTP_GET,
                                          "file:///tmp/vectis_http_missing_upload.txt",
                                          source_path,
                                          "text/plain",
                                          &response,
                                          &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "upload-capable") != NULL);

  status = vectis_http_client_head(handle, "/vectis_http_source.txt", &response, &error);
  assert(status == VECTIS_OK);
  assert(response.body_size == 0u);
  status = vectis_http_client_options(handle, "/vectis_http_source.txt", &response, &error);
  assert(status != VECTIS_ERR_NOT_IMPLEMENTED);
  vectis_http_client_destroy(handle);

  (void)remove(source_path);
  (void)remove(upload_path);
  (void)remove(helper_download_path);
  (void)remove(helper_upload_path);
  (void)remove(missing_upload_path);
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
  const char bundle_path[] = "/tmp/vectis-test-bundle.pem";
  const char cert_path[] = "/tmp/vectis-test-cert.pem";
  const char key_path[] = "/tmp/vectis-test-key.pem";
  char line[128];
  FILE *fp;

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
  ssh.host = "127.0.0.1";
  ssh.port = 1u;
  ssh.username = "vectis";
  ssh.password = "secret";
  status = vectis_ssh_sftp_upload_file(&ssh, "local", "remote", &error);
  assert(status == VECTIS_ERR_INVALID || status == VECTIS_ERR_STATE);
  status = vectis_ssh_exec(&ssh, "true", &result, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(error.source == VECTIS_ERROR_SOURCE_LIBSSH2);
  vectis_ssh_exec_result_cleanup(&result);

  vectis_mqtt_config_init(&mqtt);
  status = vectis_mqtt_publish(&mqtt,
                               "workflow/test",
                               payload,
                               sizeof(payload) - 1u,
                               "text/plain",
                               &error);
  assert(status == VECTIS_ERR_INVALID);

  vectis_cert_bundle_config_init(&certs);
  assert(certs.key_bits == 4096u);
  assert(certs.valid_days == 397L);
  status = vectis_cert_generate_bundle(&certs, &error);
  assert(status == VECTIS_ERR_INVALID);

  certs.subject.common_name = "localhost";
  certs.dns_names = "localhost";
  certs.output_bundle_path = bundle_path;
  certs.output_cert_path = cert_path;
  certs.output_key_path = key_path;
  certs.key_bits = 1024u;
  certs.valid_days = 1L;
  status = vectis_cert_generate_bundle(&certs, &error);
  assert(status == VECTIS_OK);
  fp = fopen(bundle_path, "rb");
  assert(fp != NULL);
  assert(fgets(line, sizeof(line), fp) != NULL);
  assert(strstr(line, "BEGIN CERTIFICATE") != NULL);
  assert(fclose(fp) == 0);
  fp = fopen(key_path, "rb");
  assert(fp != NULL);
  assert(fgets(line, sizeof(line), fp) != NULL);
  assert(strstr(line, "BEGIN") != NULL);
  assert(fclose(fp) == 0);
  (void)remove(bundle_path);
  (void)remove(cert_path);
  (void)remove(key_path);
}

static void assert_request_response_surface(void) {
  vectis_request *request;
  vectis_response *response;
  vectis_error error;
  vectis_status status;
  vectis_bytes body;
  vectis_mutable_bytes body_copy;
  sample_doc doc;
  const char json[] = "{\"id\":\"abc\"}";
  const char text[] = "created";

  request = vectis_internal_request_new(&error);
  response = vectis_internal_response_new(&error);
  assert(request != NULL);
  assert(response != NULL);

  status = vectis_internal_request_set_body(request, json, sizeof(json) - 1u, &error);
  assert(status == VECTIS_OK);
  memset(&body_copy, 0, sizeof(body_copy));
  status = vectis_request_body_copy(request, &body_copy, &error);
  assert(status == VECTIS_OK);
  assert(body_copy.size == sizeof(json) - 1u);
  assert(memcmp(body_copy.data, json, sizeof(json) - 1u) == 0);
  vectis_mutable_bytes_cleanup(&body_copy);
  assert(body_copy.data == NULL);
  assert(body_copy.size == 0u);
  status = vectis_internal_request_add_path_param(request, "id", "abc", &error);
  assert(status == VECTIS_OK);
  status = vectis_internal_request_add_query(request, "expand", "items", &error);
  assert(status == VECTIS_OK);
  status = vectis_internal_request_add_header(request, "content-type", "application/json", &error);
  assert(status == VECTIS_OK);

  assert(strcmp(vectis_request_path_param(request, "id"), "abc") == 0);
  assert(strcmp(vectis_request_query(request, "expand"), "items") == 0);
  assert(strcmp(vectis_request_header(request, "content-type"), "application/json") == 0);
  assert(vectis_request_header(request, "missing") == NULL);

  memset(&doc, 0, sizeof(doc));
  status = vectis_request_body_bytes(request, &body, &error);
  assert(status == VECTIS_OK);
  assert(body.size == sizeof(json) - 1u);
  assert(!vectis_request_body_is_spooled(request));
  assert(vectis_request_body_path(request) == NULL);
  status = vectis_request_json_into(request, &sample_doc_map, &doc, &error);
  assert(status == VECTIS_OK);
  assert(strcmp(doc.id, "abc") == 0);

  status = vectis_response_text(response, 201, "text/plain", text, &error);
  assert(status == VECTIS_OK);
  body = vectis_internal_response_body(response);
  assert(vectis_internal_response_status_code(response) == 201);
  assert(strcmp(vectis_internal_response_content_type(response), "text/plain") == 0);
  assert(body.size == sizeof(text) - 1u);
  assert(memcmp(body.data, text, sizeof(text) - 1u) == 0);

  status = vectis_response_json(response, 200, &sample_doc_map, &doc, &error);
  assert(status == VECTIS_OK);
  body = vectis_internal_response_body(response);
  assert(vectis_internal_response_status_code(response) == 200);
  assert(strcmp(vectis_internal_response_content_type(response), "application/json") == 0);
  assert(body.size > 0u);
  assert(strstr((const char *)body.data, "\"abc\"") != NULL);

  status = vectis_response_error_json(response,
                                      422,
                                      "invalid_order",
                                      "order is invalid",
                                      "missing id",
                                      &error);
  assert(status == VECTIS_OK);
  body = vectis_internal_response_body(response);
  assert(vectis_internal_response_status_code(response) == 422);
  assert(strcmp(vectis_internal_response_content_type(response), "application/json") == 0);
  assert(strstr((const char *)body.data, "\"code\":\"invalid_order\"") != NULL);
  assert(strstr((const char *)body.data, "\"message\":\"order is invalid\"") != NULL);
  assert(strstr((const char *)body.data, "\"detail\":\"missing id\"") != NULL);

  vectis_internal_request_free(request);
  vectis_internal_response_free(response);
}

static void assert_json_route_surface(void) {
  vectis_app_config config;
  vectis_json_route_config route;
  vectis_route_config raw_route;
  vectis_body_policy policy;
  vectis_error error;
  vectis_status status;
  vectis_app *app;
  vectis_request *request;
  vectis_response *response;
  vectis_bytes body;
  vectis_mutable_bytes body_copy;
  sample_doc output;
  char key[64];
  const char json[] = "{\"id\":\"abc\"}";
  const char spool_path[] = "/tmp/vectis-spooled-body";
  FILE *fp;

  vectis_app_config_init(&config);
  config.tls.cert_key_bundle_path = "/tmp/server.pem";
  config.lockd.unix_socket_path = "/tmp/lockd.sock";
  app = vectis_new(&config, &error);
  assert(app != NULL);
  assert(vectis_lockd_client(app) == NULL);
  request = vectis_internal_request_new(&error);
  response = vectis_internal_response_new(&error);
  assert(request != NULL);
  assert(response != NULL);

  raw_route = vectis_route(VECTIS_HTTP_GET, "/state/:id?", sample_route_handler, NULL);
  assert(raw_route.path_kind == VECTIS_ROUTE_PATH_PARAMS);
  status = vectis_register_route(app, &raw_route, &error);
  assert(status == VECTIS_OK);

  raw_route = vectis_route_regex(VECTIS_HTTP_GET, "^/internal/[0-9]+$", sample_route_handler, NULL);
  assert(raw_route.path_kind == VECTIS_ROUTE_PATH_REGEX);
  status = vectis_register_route(app, &raw_route, &error);
  assert(status == VECTIS_OK);

  raw_route = vectis_json_body_route(VECTIS_HTTP_POST, "/raw-json", sample_route_handler, NULL);
  assert(raw_route.body.mode == VECTIS_BODY_JSON);
  assert(raw_route.path_kind == VECTIS_ROUTE_PATH_LITERAL);
  status = vectis_register_prefixed_route(app, "/api/v1", &raw_route, &error);
  assert(status == VECTIS_OK);

  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_GET, "/state/abc", request, response, &error);
  assert(status == VECTIS_OK);
  body = vectis_internal_response_body(response);
  assert(vectis_internal_response_status_code(response) == 200);
  assert(body.size == 3u);
  assert(memcmp(body.data, "abc", 3u) == 0);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_GET, "/state", request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 204);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_GET, "/internal/123", request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 204);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_GET, "/state/..", request, response, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "dot segments") != NULL);

  route = vectis_json_route(VECTIS_HTTP_POST,
                            "/typed/:id",
                            &sample_doc_map,
                            sizeof(sample_doc),
                            &sample_doc_map,
                            sizeof(sample_doc),
                            sample_json_handler,
                            NULL);
  assert(route.path_kind == VECTIS_ROUTE_PATH_PARAMS);
  assert(route.body.mode == VECTIS_BODY_JSON);
  status = vectis_register_prefixed_json_route(app, "/api/v1", &route, &error);
  assert(status == VECTIS_OK);
  assert(vectis_route_count(app) == 4u);
  status = vectis_internal_route_body_policy(app, VECTIS_HTTP_POST, "/api/v1/typed/abc", &policy, &error);
  assert(status == VECTIS_OK);
  assert(policy.mode == VECTIS_BODY_JSON);
  status = vectis_internal_route_body_policy(app, VECTIS_HTTP_DELETE, "/api/v1/typed/abc", &policy, &error);
  assert(status == VECTIS_ERR_STATE);

  status = vectis_internal_request_set_body(request, json, sizeof(json) - 1u, &error);
  assert(status == VECTIS_OK);
  status = vectis_internal_dispatch_route(app,
                                          VECTIS_HTTP_POST,
                                          "/api/v1/typed/abc",
                                          request,
                                          response,
                                          &error);
  assert(status == VECTIS_OK);
  body = vectis_internal_response_body(response);
  assert(vectis_internal_response_status_code(response) == 200);
  assert(strcmp(vectis_internal_response_content_type(response), "application/json") == 0);
  assert(body.data != NULL);
  memset(&output, 0, sizeof(output));
  assert(lonejson_parse_buffer(&sample_doc_map, &output, body.data, body.size, NULL, NULL) ==
         LONEJSON_STATUS_OK);
  assert(strcmp(output.id, "abc") == 0);

  status = vectis_format_key(key, sizeof(key), &error, "state/%s/%s", "orders", "1001");
  assert(status == VECTIS_OK);
  assert(strcmp(key, "state/orders/1001") == 0);
  status = vectis_format_key(key, sizeof(key), &error, "state/%s", "../secret");
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_lockd_state_load(NULL,
                                   key,
                                   "owner",
                                   30L,
                                   &sample_doc_map,
                                   &output,
                                   &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_consumer_service_run_until(NULL, NULL, 1L, &error);
  assert(status == VECTIS_ERR_INVALID);

  fp = fopen(spool_path, "wb");
  assert(fp != NULL);
  assert(fwrite(json, 1u, sizeof(json) - 1u, fp) == sizeof(json) - 1u);
  assert(fclose(fp) == 0);
  memset(&output, 0, sizeof(output));
  status = vectis_internal_request_set_body_path(request, spool_path, sizeof(json) - 1u, &error);
  assert(status == VECTIS_OK);
  assert(vectis_request_body_is_spooled(request));
  assert(strcmp(vectis_request_body_path(request), spool_path) == 0);
  status = vectis_request_json_into(request, &sample_doc_map, &output, &error);
  assert(status == VECTIS_OK);
  assert(strcmp(output.id, "abc") == 0);
  status = vectis_request_body_copy(request, &body_copy, &error);
  assert(status == VECTIS_OK);
  assert(body_copy.size == sizeof(json) - 1u);
  assert(memcmp(body_copy.data, json, sizeof(json) - 1u) == 0);
  vectis_mutable_bytes_cleanup(&body_copy);
  status = vectis_request_body_bytes(request, &body, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "spooled") != NULL);
  remove(spool_path);

  vectis_internal_response_free(response);
  vectis_internal_request_free(request);
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
  assert(status == VECTIS_ERR_STATE);
  assert(error.source == VECTIS_ERROR_SOURCE_LOCKDC);
  assert(vectis_lockd_client(app) == NULL);
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
  assert(status == VECTIS_ERR_STATE);
  assert(error.source == VECTIS_ERROR_SOURCE_LOCKDC);
  vectis_destroy(app);
}

static void assert_consumer_service_surface(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  lc_consumer_config consumer;
  lc_consumer_service_config service_config;
  vectis_consumer_service *service;

  service = NULL;
  status = vectis_consumer_service_new(NULL, NULL, &service, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(service == NULL);
  status = vectis_consumer_service_run(NULL, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_consumer_service_start(NULL, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_consumer_service_stop(NULL, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_consumer_service_wait(NULL, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(vectis_consumer_service_raw(NULL) == NULL);

  lc_consumer_config_init(&consumer);
  lc_consumer_service_config_init(&service_config);
  consumer.name = "orders-worker";
  consumer.request.queue = "orders";
  consumer.handle = sample_consumer_handler;
  service_config.consumers = &consumer;
  service_config.consumer_count = 1u;

  vectis_app_config_init(&config);
  app = vectis_new(&config, &error);
  assert(app != NULL);
  status = vectis_consumer_service_new(app, &service_config, &service, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "lockd") != NULL);
  assert(service == NULL);
  vectis_destroy(app);

  vectis_app_config_init(&config);
  config.lockd.unix_socket_path = "/tmp/vectis-missing-lockd.sock";
  app = vectis_new(&config, &error);
  assert(app != NULL);
  status = vectis_consumer_service_new(app, &service_config, &service, &error);
  assert(status == VECTIS_OK || status == VECTIS_ERR_STATE);
  if (status == VECTIS_OK) {
    assert(service != NULL);
    assert(vectis_consumer_service_raw(service) != NULL);
    vectis_consumer_service_destroy(service);
  } else {
    assert(error.source == VECTIS_ERROR_SOURCE_LOCKDC);
    assert(service == NULL);
  }
  vectis_destroy(app);
}

int main(void) {
  assert_http_surface();
  assert_io_surface();
  assert_request_response_surface();
  assert_json_route_surface();
  assert_tls_source_surface();
  assert_consumer_service_surface();
  return 0;
}
