#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lc/lc.h>
#include <lonejson.h>
#include "vectis_internal.h"
#include <vectis/vectis.h>

typedef struct sample_doc {
  char id[32];
} sample_doc;

typedef struct sample_error_doc {
  char code[32];
  char message[64];
} sample_error_doc;

typedef struct sample_dsv_doc {
  char id[32];
  lonejson_int64 count;
  int active;
} sample_dsv_doc;

typedef struct sample_dsv_rows {
  size_t count;
  lonejson_int64 total;
  int active_count;
  char last_id[32];
} sample_dsv_rows;

typedef struct sample_xml_line {
  char sku[32];
  lonejson_int64 quantity;
} sample_xml_line;

typedef struct sample_xml_amount {
  char currency[8];
  double text;
} sample_xml_amount;

typedef struct sample_xml_doc {
  char id[32];
  sample_xml_amount amount;
  lonejson_object_array line;
  lonejson_string_array tag;
  int active;
} sample_xml_doc;

static const lonejson_field sample_doc_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(sample_doc, id, "id", LONEJSON_OVERFLOW_FAIL)};

static const lonejson_field sample_error_doc_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(sample_error_doc, code, "code", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(sample_error_doc, message, "message", LONEJSON_OVERFLOW_FAIL)};

static const lonejson_field sample_dsv_doc_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(sample_dsv_doc, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64_REQ(sample_dsv_doc, count, "count"),
    LONEJSON_FIELD_BOOL_REQ(sample_dsv_doc, active, "active")};

static const lonejson_field sample_xml_line_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(sample_xml_line, sku, "sku", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64_REQ(sample_xml_line, quantity, "quantity")};

static const lonejson_field sample_xml_amount_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(sample_xml_amount, currency, "currency", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_F64_REQ(sample_xml_amount, text, "text")};

LONEJSON_MAP_DEFINE(sample_xml_line_map, sample_xml_line, sample_xml_line_fields);
LONEJSON_MAP_DEFINE(sample_xml_amount_map, sample_xml_amount, sample_xml_amount_fields);

static const lonejson_field sample_xml_doc_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(sample_xml_doc, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_OBJECT_REQ(sample_xml_doc, amount, "amount", &sample_xml_amount_map),
    LONEJSON_FIELD_OBJECT_ARRAY(sample_xml_doc,
                                line,
                                "line",
                                sample_xml_line,
                                &sample_xml_line_map,
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_ARRAY(sample_xml_doc, tag, "tag", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_BOOL_REQ(sample_xml_doc, active, "active")};

LONEJSON_MAP_DEFINE(sample_doc_map, sample_doc, sample_doc_fields);
LONEJSON_MAP_DEFINE(sample_error_doc_map, sample_error_doc, sample_error_doc_fields);
LONEJSON_MAP_DEFINE(sample_dsv_doc_map, sample_dsv_doc, sample_dsv_doc_fields);
LONEJSON_MAP_DEFINE(sample_xml_doc_map, sample_xml_doc, sample_xml_doc_fields);

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

static vectis_status sample_json_typed_handler(vectis_app *app,
                                               vectis_request *request,
                                               void *input,
                                               vectis_json_response *response,
                                               void *userdata,
                                               vectis_error *error) {
  sample_doc *in_doc;
  sample_doc out_doc;
  sample_error_doc error_doc;

  (void)app;
  (void)request;
  (void)userdata;
  in_doc = (sample_doc *)input;
  memset(&out_doc, 0, sizeof(out_doc));
  memset(&error_doc, 0, sizeof(error_doc));
  if (in_doc != NULL && strcmp(in_doc->id, "conflict") == 0) {
    (void)snprintf(error_doc.code, sizeof(error_doc.code), "conflict");
    (void)snprintf(error_doc.message, sizeof(error_doc.message), "document already exists");
    return vectis_json_reply(response, 409, &sample_error_doc_map, &error_doc, error);
  }
  if (in_doc != NULL) {
    (void)snprintf(out_doc.id, sizeof(out_doc.id), "%s", in_doc->id);
  }
  return vectis_json_reply(response, 201, &sample_doc_map, &out_doc, error);
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

static vectis_status sample_dsv_row(void *userdata,
                                    size_t row_number,
                                    void *row,
                                    vectis_error *error) {
  sample_dsv_rows *rows;
  sample_dsv_doc *doc;

  (void)error;
  rows = (sample_dsv_rows *)userdata;
  doc = (sample_dsv_doc *)row;
  assert(row_number == rows->count + 1u);
  rows->count++;
  rows->total += doc->count;
  if (doc->active) {
    rows->active_count++;
  }
  (void)snprintf(rows->last_id, sizeof(rows->last_id), "%s", doc->id);
  return VECTIS_OK;
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

static vectis_status curl_config_fail_first_transfer(CURL *curl,
                                                     void *userdata,
                                                     vectis_error *error) {
  int *count;

  (void)error;
  count = (int *)userdata;
  if (count != NULL) {
    (*count)++;
    if (*count == 1) {
      (void)curl_easy_setopt(curl, CURLOPT_URL, "file:///tmp/vectis_http_missing_retry.txt");
    }
  }
  return VECTIS_OK;
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
  const char json_path[] = "/tmp/vectis_http_doc.json";
  const char source_body[] = "vectis file body";
  const char upload_body[] = "upload body";
  const char json_body[] = "{\"id\":\"downstream\"}";
  int curl_config_count;
  int retry_config_count;

  assert(strcmp(vectis_status_string(VECTIS_ERR_TIMEOUT), "timeout") == 0);
  curl_config_count = 0;
  retry_config_count = 0;
  handle = NULL;
  memset(&response, 0, sizeof(response));
  memset(&doc, 0, sizeof(doc));
  vectis_http_client_config_init(&client);
  assert(client.timeout_ms == 30000L);
  assert(client.connect_timeout_ms == 10000L);
  assert(client.follow_redirects_disabled == 0);
  assert(client.proxy_url == NULL);
  assert(client.low_speed_limit_bytes_per_sec == 0L);
  assert(client.low_speed_time_seconds == 0L);
  assert(client.retry_max_attempts == 1u);
  assert(client.retry_initial_delay_ms == 250L);
  assert(client.retry_max_delay_ms == 2000L);
  assert(client.retry_conditions == VECTIS_HTTP_RETRY_DEFAULT);
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
  fp = fopen(json_path, "wb");
  assert(fp != NULL);
  assert(fwrite(json_body, 1u, sizeof(json_body) - 1u, fp) == sizeof(json_body) - 1u);
  assert(fclose(fp) == 0);

  client.base_url = "file:///tmp";
  status = vectis_http_get(&client, "/vectis_http_source.txt", &response, &error);
  assert(status == VECTIS_OK);
  assert(curl_config_count == 1);
  assert(response.body_size == sizeof(source_body) - 1u);
  assert(memcmp(response.body, source_body, sizeof(source_body) - 1u) == 0);
  vectis_http_response_cleanup(&response);

  status = vectis_http_get(&client, "/vectis_http_doc.json", &response, &error);
  assert(status == VECTIS_OK);
  memset(&doc, 0, sizeof(doc));
  status = vectis_http_response_json_into(&response, &sample_doc_map, &doc, &error);
  assert(status == VECTIS_OK);
  assert(strcmp(doc.id, "downstream") == 0);
  vectis_http_response_cleanup(&response);

  status = vectis_http_client_new(&client, &handle, &error);
  assert(status == VECTIS_OK);
  assert(handle != NULL);
  vectis_http_request_init(&request);
  request.url = "/vectis_http_source.txt";
  status = vectis_http_client_execute(handle, &request, &response, &error);
  assert(status == VECTIS_OK);
  assert(curl_config_count == 3);
  assert(response.body_size == sizeof(source_body) - 1u);
  vectis_http_response_cleanup(&response);

  vectis_http_request_init(&request);
  request.url = "/vectis_http_source.txt";
  request.retry_max_attempts = 2u;
  request.retry_initial_delay_ms = 0L;
  request.retry_max_delay_ms = 0L;
  request.configure_curl = curl_config_fail_first_transfer;
  retry_config_count = 0;
  request.configure_curl_userdata = &retry_config_count;
  status = vectis_http_client_execute(handle, &request, &response, &error);
  assert(status == VECTIS_OK);
  assert(retry_config_count == 2);
  assert(response.body_size == sizeof(source_body) - 1u);
  vectis_http_response_cleanup(&response);

  vectis_http_request_init(&request);
  request.url = "/vectis_http_source.txt";
  request.retry_max_attempts = 2u;
  request.response_body = response_stream_ok;
  status = vectis_http_client_execute(handle, &request, &response, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "streaming") != NULL);

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
  (void)remove(json_path);
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
  lc_source *source;
  vectis_bytes body;
  vectis_mutable_bytes body_copy;
  vectis_body_materialize_config materialize_config;
  vectis_body_materialized materialized;
  char small_body_buffer[64];
  char tiny_body_buffer[4];
  sample_doc doc;
  const char json[] = "{\"id\":\"abc\"}";
  const char text[] = "created";

  request = vectis_internal_request_new(&error);
  response = vectis_internal_response_new(&error);
  assert(request != NULL);
  assert(response != NULL);

  status = vectis_internal_request_set_body(request, json, sizeof(json) - 1u, &error);
  assert(status == VECTIS_OK);
  assert(vectis_request_body_reader(request) != NULL);
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
  assert(vectis_request_kore(request) == NULL);
  assert(vectis_request_method(request) == VECTIS_HTTP_ANY);
  vectis_internal_request_set_method(request, VECTIS_HTTP_PATCH);
  assert(vectis_request_method(request) == VECTIS_HTTP_PATCH);

  memset(&doc, 0, sizeof(doc));
  status = vectis_request_body_bytes(request, &body, &error);
  assert(status == VECTIS_OK);
  assert(body.size == sizeof(json) - 1u);
  assert(!vectis_request_body_is_spooled(request));
  assert(vectis_request_body_path(request) == NULL);
  status = vectis_request_json_into(request, &sample_doc_map, &doc, &error);
  assert(status == VECTIS_OK);
  assert(strcmp(doc.id, "abc") == 0);
  vectis_body_materialize_config_init(&materialize_config);
  materialize_config.buffer = small_body_buffer;
  materialize_config.buffer_size = sizeof(small_body_buffer);
  status = vectis_request_body_materialize(request, &materialize_config, &materialized, &error);
  assert(status == VECTIS_OK);
  assert(materialized.kind == VECTIS_BODY_MATERIALIZED_MEMORY);
  assert(materialized.memory.data == small_body_buffer);
  assert(materialized.memory.size == sizeof(json) - 1u);
  assert(memcmp(materialized.memory.data, json, sizeof(json) - 1u) == 0);
  vectis_body_materialized_cleanup(&materialized);
  vectis_body_materialize_config_init(&materialize_config);
  materialize_config.buffer = tiny_body_buffer;
  materialize_config.buffer_size = sizeof(tiny_body_buffer);
  materialize_config.prefix = "vectis-sdk-body";
  status = vectis_request_body_materialize(request, &materialize_config, &materialized, &error);
  assert(status == VECTIS_OK);
  assert(materialized.kind == VECTIS_BODY_MATERIALIZED_FILE);
  assert(materialized.path != NULL);
  assert(materialized.size == sizeof(json) - 1u);
  remove(materialized.path);
  vectis_body_materialized_cleanup(&materialized);

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
  status = vectis_response_json_generated(response, 200, &sample_doc_map, &doc, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_body(response).data == NULL);
  assert(vectis_internal_response_file_path(response) != NULL);
  assert(vectis_internal_response_file_temporary(response));
  source = NULL;
  assert(lc_source_from_memory(text, sizeof(text) - 1u, &source, NULL) == LC_OK);
  status = vectis_response_source(response, 200, "text/plain", source, &error);
  lc_source_close(source);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_body(response).data == NULL);
  assert(vectis_internal_response_file_path(response) != NULL);
  assert(vectis_internal_response_file_temporary(response));
  status = vectis_response_header(response, "x-vectis-test", "ok", &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_header_count(response) == 1u);
  assert(strcmp(vectis_internal_response_header_name(response, 0u), "x-vectis-test") == 0);
  assert(strcmp(vectis_internal_response_header_value(response, 0u), "ok") == 0);
  status = vectis_response_header(response, "bad:name", "ok", &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_response_header(response, "x-bad", "bad\nvalue", &error);
  assert(status == VECTIS_ERR_INVALID);

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
  vectis_json_typed_route_config typed_route;
  vectis_route_config raw_route;
  vectis_static_file_config static_file;
  vectis_static_directory_config static_dir;
  vectis_body_policy policy;
  vectis_error error;
  vectis_status status;
  vectis_app *app;
  vectis_request *request;
  vectis_response *response;
  vectis_bytes body;
  vectis_mutable_bytes body_copy;
  sample_doc output;
  sample_error_doc error_output;
  char key[64];
  const char json[] = "{\"id\":\"abc\"}";
  const char spool_path[] = "/tmp/vectis-spooled-body";
  const char static_file_path[] = "/tmp/vectis-static-file.txt";
  const char static_dir_path[] = "/tmp/vectis-static-dir";
  const char static_dir_file_path[] = "/tmp/vectis-static-dir/app.js";
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

  fp = fopen(static_file_path, "wb");
  assert(fp != NULL);
  assert(fwrite("static-file", 1u, 11u, fp) == 11u);
  assert(fclose(fp) == 0);
  (void)remove(static_dir_file_path);
  (void)rmdir(static_dir_path);
  assert(mkdir(static_dir_path, 0700) == 0);
  fp = fopen(static_dir_file_path, "wb");
  assert(fp != NULL);
  assert(fwrite("static-dir", 1u, 10u, fp) == 10u);
  assert(fclose(fp) == 0);
  vectis_static_file_config_init(&static_file);
  static_file.path = "/static-file";
  static_file.file_path = static_file_path;
  static_file.content_type = "text/plain";
  status = vectis_register_static_file(app, &static_file, &error);
  assert(status == VECTIS_OK);
  vectis_static_directory_config_init(&static_dir);
  static_dir.path_prefix = "/assets";
  static_dir.root_dir = static_dir_path;
  static_dir.content_type = "application/javascript";
  status = vectis_register_static_directory(app, &static_dir, &error);
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

  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_GET, "/static-file", request, response, &error);
  assert(status == VECTIS_OK);
  body = vectis_internal_response_body(response);
  assert(vectis_request_path(request) != NULL);
  assert(strcmp(vectis_request_path(request), "/static-file") == 0);
  assert(vectis_internal_response_status_code(response) == 200);
  assert(strcmp(vectis_internal_response_file_path(response), static_file_path) == 0);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_HEAD, "/static-file", request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 200);
  body = vectis_internal_response_body(response);
  assert(body.data == NULL);
  assert(body.size == 0u);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_GET, "/assets/app.js", request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 200);
  assert(strcmp(vectis_internal_response_file_path(response), static_dir_file_path) == 0);
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
  assert(vectis_route_count(app) == 6u);
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

  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);
  typed_route = vectis_json_typed_route(VECTIS_HTTP_POST,
                                        "/typed-response",
                                        &sample_doc_map,
                                        sizeof(sample_doc),
                                        sample_json_typed_handler,
                                        NULL);
  status = vectis_register_prefixed_json_typed_route(app, "/api/v1", &typed_route, &error);
  assert(status == VECTIS_OK);
  assert(vectis_route_count(app) == 7u);
  status = vectis_internal_request_set_body(request, json, sizeof(json) - 1u, &error);
  assert(status == VECTIS_OK);
  status = vectis_internal_dispatch_route(app,
                                          VECTIS_HTTP_POST,
                                          "/api/v1/typed-response",
                                          request,
                                          response,
                                          &error);
  assert(status == VECTIS_OK);
  body = vectis_internal_response_body(response);
  assert(vectis_internal_response_status_code(response) == 201);
  memset(&output, 0, sizeof(output));
  assert(lonejson_parse_buffer(&sample_doc_map, &output, body.data, body.size, NULL, NULL) ==
         LONEJSON_STATUS_OK);
  assert(strcmp(output.id, "abc") == 0);

  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);
  status = vectis_internal_request_set_body(request,
                                            "{\"id\":\"conflict\"}",
                                            sizeof("{\"id\":\"conflict\"}") - 1u,
                                            &error);
  assert(status == VECTIS_OK);
  status = vectis_internal_dispatch_route(app,
                                          VECTIS_HTTP_POST,
                                          "/api/v1/typed-response",
                                          request,
                                          response,
                                          &error);
  assert(status == VECTIS_OK);
  body = vectis_internal_response_body(response);
  assert(vectis_internal_response_status_code(response) == 409);
  memset(&error_output, 0, sizeof(error_output));
  assert(lonejson_parse_buffer(&sample_error_doc_map,
                               &error_output,
                               body.data,
                               body.size,
                               NULL,
                               NULL) == LONEJSON_STATUS_OK);
  assert(strcmp(error_output.code, "conflict") == 0);

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
  assert(strstr(error.message, "reader-backed") != NULL);
  remove(spool_path);

  vectis_internal_response_free(response);
  vectis_internal_request_free(request);
  vectis_destroy(app);
  (void)remove(static_file_path);
  (void)remove(static_dir_file_path);
  (void)rmdir(static_dir_path);
}

static void assert_openapi_surface(void) {
  vectis_app_config config;
  vectis_openapi_route_doc doc;
  vectis_openapi_document document;
  vectis_mutable_bytes json;
  vectis_mutable_bytes yaml;
  vectis_error error;
  vectis_status status;
  vectis_app *app;
  const char *tags[] = {"orders"};

  vectis_app_config_init(&config);
  config.tls.cert_key_bundle_path = "/tmp/server.pem";
  app = vectis_new(&config, &error);
  assert(app != NULL);

  vectis_openapi_route_doc_init(&doc);
  doc.summary = "Create order";
  doc.operation_id = "createOrder";
  doc.tags = tags;
  doc.tag_count = 1u;
  status = vectis_openapi_request_json(&doc,
                                       vectis_openapi_lonejson_schema("OrderRequest",
                                                                      &sample_doc_map),
                                       &error);
  assert(status == VECTIS_OK);
  status = vectis_openapi_response_json(&doc,
                                        201,
                                        "Created",
                                        vectis_openapi_lonejson_schema("OrderCreated",
                                                                       &sample_doc_map),
                                        &error);
  assert(status == VECTIS_OK);
  status = vectis_openapi_response_json(&doc,
                                        409,
                                        "Conflict",
                                        vectis_openapi_lonejson_schema("ApiError",
                                                                       &sample_error_doc_map),
                                        &error);
  assert(status == VECTIS_OK);
  status = vectis_attach_openapi_doc(app,
                                     VECTIS_HTTP_METHODS_POST,
                                     "/orders/:id?",
                                     &doc,
                                     &error);
  assert(status == VECTIS_OK);

  vectis_openapi_document_init(&document);
  document.title = "Orders API";
  document.version = "1.2.3";
  status = vectis_generate_openapi(app, &document, VECTIS_OPENAPI_JSON, &json, &error);
  assert(status == VECTIS_OK);
  assert(json.data != NULL);
  assert(strstr((const char *)json.data, "\"openapi\":\"3.1.0\"") != NULL);
  assert(strstr((const char *)json.data, "\"/orders/{id}\"") != NULL);
  assert(strstr((const char *)json.data, "\"operationId\":\"createOrder\"") != NULL);
  assert(strstr((const char *)json.data, "\"201\"") != NULL);
  assert(strstr((const char *)json.data, "\"409\"") != NULL);
  assert(strstr((const char *)json.data, "\"OrderRequest\"") != NULL);
  assert(strstr((const char *)json.data, "\"ApiError\"") != NULL);
  assert(strstr((const char *)json.data, "\"required\":[\"id\"]") != NULL);
  vectis_mutable_bytes_cleanup(&json);

  status = vectis_generate_openapi(app, &document, VECTIS_OPENAPI_YAML, &yaml, &error);
  assert(status == VECTIS_OK);
  assert(yaml.data != NULL);
  assert(strstr((const char *)yaml.data, "openapi: 3.1.0") != NULL);
  assert(strstr((const char *)yaml.data, "\"/orders/{id}\":") != NULL);
  assert(strstr((const char *)yaml.data, "operationId: \"createOrder\"") != NULL);
  assert(strstr((const char *)yaml.data, "\"201\":") != NULL);
  assert(strstr((const char *)yaml.data, "OrderRequest:") != NULL);
  assert(strstr((const char *)yaml.data, "ApiError:") != NULL);
  vectis_mutable_bytes_cleanup(&yaml);

  vectis_openapi_route_doc_cleanup(&doc);
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

static void assert_dsv_surface(void) {
  const char csv[] = "id,count,active\r\nalpha,2,true\r\n\"beta,quoted\",3,false\r\n";
  const char commented_csv[] =
      "  # export metadata\n"
      "id,count,active\n"
      "epsilon,23,true\n"
      "# skipped row,999,false\n"
      "\"#literal\",29,false\n";
  const char csv_with_alt_header[] = "external_id,total,enabled\nnamed,17,true\n";
  const char headerless_csv[] = "gamma,13,true\n";
  const char headerless_commented_csv[] =
      "# row-only export\n"
      "zeta,31,true\n"
      "  # skipped row,999,false\n"
      "eta,37,false\n";
  const char tsv[] = "id\tcount\tactive\none\t7\ttrue\n";
  const char headerless_tsv[] = "delta\t19\tfalse\n";
  const char short_row_csv[] = "id,count,active\nbad,1\n";
  const char oversized_field_csv[] = "id,count,active\nabc,1,true\n";
  const char unterminated_csv[] = "id,count,active\n\"unterminated,1,true\n";
  const char *columns[] = {"id", "count", "active"};
  vectis_dsv_config config;
  vectis_source dsv_source;
  vectis_mutable_bytes json;
  sample_dsv_rows rows;
  lc_source *source;
  vectis_error error;
  vectis_status status;

  memset(&rows, 0, sizeof(rows));
  config = vectis_dsv_csv();
  vectis_source_init(&dsv_source);
  status = vectis_dsv_parse_lonejson_source(&dsv_source,
                                            &sample_dsv_doc_map,
                                            &config,
                                            sample_dsv_row,
                                            &rows,
                                            &error);
  assert(status == VECTIS_ERR_INVALID);
  vectis_error_clear(&error);

  dsv_source = vectis_source_from_memory(csv, sizeof(csv) - 1u);
  status = vectis_dsv_parse_lonejson_source(&dsv_source,
                                            &sample_dsv_doc_map,
                                            &config,
                                            sample_dsv_row,
                                            &rows,
                                            &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 2u);
  assert(rows.total == 5);
  assert(rows.active_count == 1);
  assert(strcmp(rows.last_id, "beta,quoted") == 0);

  dsv_source = vectis_source_from_memory(commented_csv, sizeof(commented_csv) - 1u);
  config = vectis_dsv_csv();
  config.comment_prefix = "#";
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson_source(&dsv_source,
                                            &sample_dsv_doc_map,
                                            &config,
                                            sample_dsv_row,
                                            &rows,
                                            &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 2u);
  assert(rows.total == 52);
  assert(rows.active_count == 1);
  assert(strcmp(rows.last_id, "#literal") == 0);

  dsv_source = vectis_source_from_memory(csv_with_alt_header, sizeof(csv_with_alt_header) - 1u);
  config = vectis_dsv_csv();
  config.columns = columns;
  config.column_count = 3u;
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson_source(&dsv_source,
                                            &sample_dsv_doc_map,
                                            &config,
                                            sample_dsv_row,
                                            &rows,
                                            &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 1u);
  assert(rows.total == 17);
  assert(rows.active_count == 1);
  assert(strcmp(rows.last_id, "named") == 0);

  memset(&json, 0, sizeof(json));
  dsv_source = vectis_source_from_memory(csv, sizeof(csv) - 1u);
  status = vectis_dsv_source_to_json_array(&dsv_source,
                                           &config,
                                           &json,
                                           &error);
  assert(status == VECTIS_OK);
  assert(json.data != NULL);
  assert(strstr((const char *)json.data, "\"id\":\"alpha\"") != NULL);
  assert(strstr((const char *)json.data, "\"id\":\"beta,quoted\"") != NULL);
  assert(strstr((const char *)json.data, "\"count\":\"3\"") != NULL);
  vectis_mutable_bytes_cleanup(&json);

  memset(&json, 0, sizeof(json));
  dsv_source = vectis_source_from_memory(csv, sizeof(csv) - 1u);
  status = vectis_dsv_source_to_lonejson_array(&dsv_source,
                                               &sample_dsv_doc_map,
                                               &config,
                                               &json,
                                               &error);
  assert(status == VECTIS_OK);
  assert(json.data != NULL);
  assert(strstr((const char *)json.data, "\"id\":\"alpha\"") != NULL);
  assert(strstr((const char *)json.data, "\"count\":3") != NULL);
  assert(strstr((const char *)json.data, "\"active\":false") != NULL);
  vectis_mutable_bytes_cleanup(&json);

  memset(&rows, 0, sizeof(rows));
  assert(lc_source_from_memory(tsv, sizeof(tsv) - 1u, &source, NULL) == LC_OK);
  config = vectis_dsv_tsv();
  status = vectis_dsv_parse_lonejson(source,
                                     &sample_dsv_doc_map,
                                     &config,
                                     sample_dsv_row,
                                     &rows,
                                     &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 1u);
  assert(rows.total == 7);
  assert(rows.active_count == 1);
  lc_source_close(source);

  assert(lc_source_from_memory("two|11|false\n", 13u, &source, NULL) == LC_OK);
  vectis_dsv_config_init(&config);
  config.delimiter = '|';
  config.has_header = 0;
  config.columns = columns;
  config.column_count = 3u;
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson(source,
                                     &sample_dsv_doc_map,
                                     &config,
                                     sample_dsv_row,
                                     &rows,
                                     &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 1u);
  assert(rows.total == 11);
  assert(rows.active_count == 0);
  lc_source_close(source);

  assert(lc_source_from_memory(headerless_csv, sizeof(headerless_csv) - 1u, &source, NULL) == LC_OK);
  config = vectis_dsv_csv_rows();
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson(source,
                                     &sample_dsv_doc_map,
                                     &config,
                                     sample_dsv_row,
                                     &rows,
                                     &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 1u);
  assert(rows.total == 13);
  assert(rows.active_count == 1);
  assert(strcmp(rows.last_id, "gamma") == 0);
  lc_source_close(source);

  dsv_source = vectis_source_from_memory(headerless_commented_csv,
                                         sizeof(headerless_commented_csv) - 1u);
  config = vectis_dsv_csv_rows();
  config.comment_prefix = "#";
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson_source(&dsv_source,
                                            &sample_dsv_doc_map,
                                            &config,
                                            sample_dsv_row,
                                            &rows,
                                            &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 2u);
  assert(rows.total == 68);
  assert(rows.active_count == 1);
  assert(strcmp(rows.last_id, "eta") == 0);

  assert(lc_source_from_memory(headerless_tsv, sizeof(headerless_tsv) - 1u, &source, NULL) == LC_OK);
  config = vectis_dsv_tsv_rows();
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson(source,
                                     &sample_dsv_doc_map,
                                     &config,
                                     sample_dsv_row,
                                     &rows,
                                     &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 1u);
  assert(rows.total == 19);
  assert(rows.active_count == 0);
  assert(strcmp(rows.last_id, "delta") == 0);
  lc_source_close(source);

  memset(&json, 0, sizeof(json));
  config = vectis_dsv_csv_rows();
  dsv_source = vectis_source_from_memory(headerless_csv, sizeof(headerless_csv) - 1u);
  status = vectis_dsv_source_to_lonejson_array(&dsv_source,
                                               &sample_dsv_doc_map,
                                               &config,
                                               &json,
                                               &error);
  assert(status == VECTIS_OK);
  assert(strstr((const char *)json.data, "\"id\":\"gamma\"") != NULL);
  assert(strstr((const char *)json.data, "\"count\":13") != NULL);
  assert(strstr((const char *)json.data, "\"active\":true") != NULL);
  vectis_mutable_bytes_cleanup(&json);

  memset(&json, 0, sizeof(json));
  dsv_source = vectis_source_from_memory(headerless_csv, sizeof(headerless_csv) - 1u);
  status = vectis_dsv_source_to_json_array(&dsv_source, &config, &json, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "columns") != NULL);
  vectis_error_clear(&error);

  assert(lc_source_from_memory(short_row_csv, sizeof(short_row_csv) - 1u, &source, NULL) == LC_OK);
  config = vectis_dsv_csv();
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson(source,
                                     &sample_dsv_doc_map,
                                     &config,
                                     sample_dsv_row,
                                     &rows,
                                     &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "row width") != NULL);
  lc_source_close(source);
  vectis_error_clear(&error);

  assert(lc_source_from_memory(oversized_field_csv,
                               sizeof(oversized_field_csv) - 1u,
                               &source,
                               NULL) == LC_OK);
  config = vectis_dsv_csv();
  config.max_field_bytes = 2u;
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson(source,
                                     &sample_dsv_doc_map,
                                     &config,
                                     sample_dsv_row,
                                     &rows,
                                     &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "max_field_bytes") != NULL);
  lc_source_close(source);
  vectis_error_clear(&error);

  assert(lc_source_from_memory(unterminated_csv,
                               sizeof(unterminated_csv) - 1u,
                               &source,
                               NULL) == LC_OK);
  config = vectis_dsv_csv();
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson(source,
                                     &sample_dsv_doc_map,
                                     &config,
                                     sample_dsv_row,
                                     &rows,
                                     &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "unterminated") != NULL);
  lc_source_close(source);
  vectis_error_clear(&error);
}

static void assert_xml_surface(void) {
  const char xml[] =
      "<invoice ignored=\"yes\">"
      "<id> inv-001 </id>"
      "<amount currency=\"SEK\">123.50</amount>"
      "<line><sku>A-1</sku><quantity>2</quantity></line>"
      "<line><sku>B-2</sku><quantity>3</quantity></line>"
      "<tag>finance</tag>"
      "<tag>xml</tag>"
      "<active>1</active>"
      "<unknown>ignored</unknown>"
      "</invoice>";
  const char bad_root[] = "<statement><id>x</id></statement>";
  const char duplicate_scalar[] = "<invoice><id>a</id><id>b</id></invoice>";
  const char non_contiguous_array[] =
      "<invoice>"
      "<id>a</id>"
      "<line><sku>A-1</sku><quantity>2</quantity></line>"
      "<active>true</active>"
      "<line><sku>B-2</sku><quantity>3</quantity></line>"
      "</invoice>";
  vectis_xml_config config;
  vectis_source xml_source;
  vectis_error error;
  vectis_status status;
  sample_xml_doc doc;
  sample_xml_line *lines;

  config = vectis_xml_default();
  config.root_element = "invoice";
  memset(&doc, 0, sizeof(doc));
  xml_source = vectis_source_from_memory(xml, sizeof(xml) - 1u);
  status = vectis_xml_parse_lonejson_source(&xml_source,
                                            &sample_xml_doc_map,
                                            &config,
                                            &doc,
                                            &error);
  assert(status == VECTIS_OK);
  assert(strcmp(doc.id, "inv-001") == 0);
  assert(strcmp(doc.amount.currency, "SEK") == 0);
  assert(doc.amount.text > 123.49 && doc.amount.text < 123.51);
  assert(doc.line.count == 2u);
  lines = (sample_xml_line *)doc.line.items;
  assert(strcmp(lines[0].sku, "A-1") == 0);
  assert(lines[0].quantity == 2);
  assert(strcmp(lines[1].sku, "B-2") == 0);
  assert(lines[1].quantity == 3);
  assert(doc.tag.count == 2u);
  assert(strcmp(doc.tag.items[0], "finance") == 0);
  assert(strcmp(doc.tag.items[1], "xml") == 0);
  assert(doc.active == 1);
  lonejson_cleanup(&sample_xml_doc_map, &doc);

  xml_source = vectis_source_from_memory(bad_root, sizeof(bad_root) - 1u);
  status = vectis_xml_parse_lonejson_source(&xml_source,
                                            &sample_xml_doc_map,
                                            &config,
                                            &doc,
                                            &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "root element") != NULL);
  vectis_error_clear(&error);

  xml_source = vectis_source_from_memory(duplicate_scalar, sizeof(duplicate_scalar) - 1u);
  status = vectis_xml_parse_lonejson_source(&xml_source,
                                            &sample_xml_doc_map,
                                            &config,
                                            &doc,
                                            &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "duplicate") != NULL);
  vectis_error_clear(&error);

  xml_source = vectis_source_from_memory(non_contiguous_array, sizeof(non_contiguous_array) - 1u);
  status = vectis_xml_parse_lonejson_source(&xml_source,
                                            &sample_xml_doc_map,
                                            &config,
                                            &doc,
                                            &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "non-contiguous") != NULL);
  vectis_error_clear(&error);

  xml_source = vectis_source_from_memory(xml, sizeof(xml) - 1u);
  config = vectis_xml_default();
  config.root_element = "invoice";
  config.skip_unknown = 0;
  status = vectis_xml_parse_lonejson_source(&xml_source,
                                            &sample_xml_doc_map,
                                            &config,
                                            &doc,
                                            &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "unknown") != NULL);
  vectis_error_clear(&error);
}

int main(void) {
  assert_http_surface();
  assert_io_surface();
  assert_request_response_surface();
  assert_json_route_surface();
  assert_openapi_surface();
  assert_tls_source_surface();
  assert_consumer_service_surface();
  assert_dsv_surface();
  assert_xml_surface();
  return 0;
}
