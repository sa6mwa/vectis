#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vectis_internal.h"
#include <lc/lc.h>
#include <lonejson.h>
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

typedef struct sample_dsv_view_rows {
  sample_dsv_rows rows;
  sample_dsv_doc item;
} sample_dsv_view_rows;

typedef struct sample_xml_line {
  char sku[32];
  lonejson_int64 quantity;
} sample_xml_line;

typedef struct sample_xml_amount {
  char currency[8];
  double text;
} sample_xml_amount;

typedef struct failing_source_context {
  int read_count;
} failing_source_context;

typedef struct sample_xml_doc {
  char id[32];
  sample_xml_amount amount;
  lonejson_object_array line;
  lonejson_string_array tag;
  int active;
} sample_xml_doc;

typedef struct sample_xml_blob_doc {
  lonejson_spooled body;
} sample_xml_blob_doc;

static int sample_bytes_contains(vectis_bytes bytes, const char *needle) {
  size_t needle_size;
  size_t i;

  if (bytes.data == NULL || needle == NULL) {
    return 0;
  }
  needle_size = strlen(needle);
  if (needle_size == 0u) {
    return 1;
  }
  if (needle_size > bytes.size) {
    return 0;
  }
  for (i = 0u; i <= bytes.size - needle_size; ++i) {
    if (memcmp((const unsigned char *)bytes.data + i, needle, needle_size) ==
        0) {
      return 1;
    }
  }
  return 0;
}

static const lonejson_field sample_doc_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(sample_doc, id, "id",
                                    LONEJSON_OVERFLOW_FAIL)};

static const lonejson_field sample_error_doc_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(sample_error_doc, code, "code",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(sample_error_doc, message, "message",
                                    LONEJSON_OVERFLOW_FAIL)};

static const lonejson_field sample_dsv_doc_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(sample_dsv_doc, id, "id",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64_REQ(sample_dsv_doc, count, "count"),
    LONEJSON_FIELD_BOOL_REQ(sample_dsv_doc, active, "active")};

static const lonejson_field sample_xml_line_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(sample_xml_line, sku, "sku",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64_REQ(sample_xml_line, quantity, "quantity")};

static const lonejson_field sample_xml_amount_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(sample_xml_amount, currency, "currency",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_F64_REQ(sample_xml_amount, text, "text")};

LONEJSON_MAP_DEFINE(sample_xml_line_map, sample_xml_line,
                    sample_xml_line_fields);
LONEJSON_MAP_DEFINE(sample_xml_amount_map, sample_xml_amount,
                    sample_xml_amount_fields);

static const lonejson_field sample_xml_doc_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(sample_xml_doc, id, "id",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_OBJECT_REQ(sample_xml_doc, amount, "amount",
                              &sample_xml_amount_map),
    LONEJSON_FIELD_OBJECT_ARRAY(sample_xml_doc, line, "line", sample_xml_line,
                                &sample_xml_line_map, LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_ARRAY(sample_xml_doc, tag, "tag",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_BOOL_REQ(sample_xml_doc, active, "active")};

static const lonejson_field sample_xml_blob_doc_fields[] = {
    LONEJSON_FIELD_STRING_STREAM_REQ(sample_xml_blob_doc, body, "body")};

LONEJSON_MAP_DEFINE(sample_doc_map, sample_doc, sample_doc_fields);
LONEJSON_MAP_DEFINE(sample_error_doc_map, sample_error_doc,
                    sample_error_doc_fields);
LONEJSON_MAP_DEFINE(sample_dsv_doc_map, sample_dsv_doc, sample_dsv_doc_fields);
LONEJSON_MAP_DEFINE(sample_xml_doc_map, sample_xml_doc, sample_xml_doc_fields);
LONEJSON_MAP_DEFINE(sample_xml_blob_doc_map, sample_xml_blob_doc,
                    sample_xml_blob_doc_fields);

static vectis_status sample_json_handler(vectis_app *app,
                                         vectis_request *request, void *input,
                                         void *output, void *userdata,
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

static vectis_status
sample_json_typed_handler(vectis_app *app, vectis_request *request, void *input,
                          vectis_json_response *response, void *userdata,
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
    (void)snprintf(error_doc.message, sizeof(error_doc.message),
                   "document already exists");
    return vectis_json_reply(response, 409, &sample_error_doc_map, &error_doc,
                             error);
  }
  if (in_doc != NULL) {
    (void)snprintf(out_doc.id, sizeof(out_doc.id), "%s", in_doc->id);
  }
  return vectis_json_reply(response, 201, &sample_doc_map, &out_doc, error);
}

static size_t failing_source_read(void *context, void *buffer, size_t count,
                                  lc_error *error) {
  failing_source_context *state;
  const char partial[] = "partial certificate";
  size_t n;

  state = (failing_source_context *)context;
  if (state->read_count == 0) {
    state->read_count++;
    n = sizeof(partial) - 1u;
    if (n > count) {
      n = count;
    }
    memcpy(buffer, partial, n);
    return n;
  }
  if (error != NULL) {
    error->code = LC_ERR_TRANSPORT;
    error->message = (char *)malloc(sizeof("source failed"));
    assert(error->message != NULL);
    memcpy(error->message, "source failed", sizeof("source failed"));
  }
  return 0u;
}

static int failing_source_reset(void *context, lc_error *error) {
  failing_source_context *state;

  (void)error;
  state = (failing_source_context *)context;
  state->read_count = 0;
  return LC_OK;
}

static vectis_status sample_route_handler(vectis_app *app,
                                          vectis_request *request,
                                          vectis_response *response,
                                          void *userdata, vectis_error *error) {
  const char *id;

  (void)app;
  (void)userdata;
  id = vectis_request_path_param(request, "id");
  if (id != NULL) {
    return vectis_response_text(response, 200, "text/plain", id, error);
  }
  return vectis_response_status(response, 204, error);
}

static int sample_consumer_handler(void *context, lc_consumer_message *message,
                                   lc_error *error) {
  (void)context;
  (void)message;
  (void)error;
  return LC_OK;
}

static vectis_status sample_consumer_receiver_create(
    void *adapter_context, const void *receiver_config,
    vectis_consumer_receiver *out, vectis_error *error) {
  (void)adapter_context;
  (void)receiver_config;
  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "receiver output is required");
    return VECTIS_ERR_INVALID;
  }
  memset(out, 0, sizeof(*out));
  out->handle = sample_consumer_handler;
  return VECTIS_OK;
}

static vectis_status sample_dsv_row(void *userdata, size_t row_number,
                                    void *row, vectis_error *error) {
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

static vectis_status sample_dsv_view_storage(void *userdata, size_t row_number,
                                             void **row_storage,
                                             vectis_error *error) {
  sample_dsv_view_rows *view_rows;

  (void)row_number;
  (void)error;
  view_rows = (sample_dsv_view_rows *)userdata;
  memset(&view_rows->item, 0, sizeof(view_rows->item));
  *row_storage = &view_rows->item;
  return VECTIS_OK;
}

static vectis_status sample_dsv_view_row(void *userdata, size_t row_number,
                                         void *row, vectis_error *error) {
  sample_dsv_view_rows *view_rows;

  view_rows = (sample_dsv_view_rows *)userdata;
  return sample_dsv_row(&view_rows->rows, row_number, row, error);
}

static vectis_status sample_json_array_item(void *userdata, size_t index,
                                            void *item, vectis_error *error) {
  sample_dsv_rows *rows;
  sample_dsv_doc *doc;

  (void)error;
  rows = (sample_dsv_rows *)userdata;
  doc = (sample_dsv_doc *)item;
  assert(index == rows->count);
  rows->count++;
  rows->total += doc->count;
  if (doc->active) {
    rows->active_count++;
  }
  (void)snprintf(rows->last_id, sizeof(rows->last_id), "%s", doc->id);
  return VECTIS_OK;
}

static vectis_status sample_json_array_fail_on_second(void *userdata,
                                                      size_t index, void *item,
                                                      vectis_error *error) {
  sample_dsv_rows *rows;
  sample_dsv_doc *doc;

  rows = (sample_dsv_rows *)userdata;
  doc = (sample_dsv_doc *)item;
  rows->count++;
  rows->total += doc->count;
  if (index == 1u) {
    vectis_set_error(error, VECTIS_ERR_STATE, "array callback stopped");
    return VECTIS_ERR_STATE;
  }
  return VECTIS_OK;
}

static lonejson_status sample_json_array_rewrite_item(
    void *user, const lonejson_array_rewrite_context *context, void *item,
    lonejson_array_rewrite_result *result, lonejson_error *error) {
  sample_dsv_rows *rows;
  sample_dsv_doc *doc;

  (void)context;
  (void)error;
  rows = (sample_dsv_rows *)user;
  doc = (sample_dsv_doc *)item;
  rows->count++;
  rows->total += doc->count;
  if (doc->active) {
    rows->active_count++;
    result->action = LONEJSON_ARRAY_REWRITE_KEEP;
  } else {
    result->action = LONEJSON_ARRAY_REWRITE_DROP;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status sample_json_array_rewrite_fail(
    void *user, const lonejson_array_rewrite_context *context, void *item,
    lonejson_array_rewrite_result *result, lonejson_error *error) {
  (void)user;
  (void)context;
  (void)item;
  (void)result;
  if (error != NULL) {
    lonejson_error_init(error);
    error->code = LONEJSON_STATUS_CALLBACK_FAILED;
    (void)snprintf(error->message, sizeof(error->message), "%s",
                   "rewrite callback stopped");
  }
  return LONEJSON_STATUS_CALLBACK_FAILED;
}

static int failing_sink_write(lc_sink *self, const void *bytes, size_t count,
                              lc_error *error) {
  char *message;

  (void)self;
  (void)bytes;
  (void)count;
  if (error != NULL) {
    error->code = LC_ERR_TRANSPORT;
    message = (char *)malloc(sizeof("intentional sink failure"));
    if (message != NULL) {
      memcpy(message, "intentional sink failure",
             sizeof("intentional sink failure"));
    }
    error->message = message;
  }
  return 0;
}

static vectis_status curl_config_ok(CURL *curl, void *userdata,
                                    vectis_error *error) {
  int *count;

  (void)error;
  count = (int *)userdata;
  if (count != NULL) {
    (*count)++;
  }
  (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, "vectis-test");
  return VECTIS_OK;
}

static vectis_status curl_config_fail(CURL *curl, void *userdata,
                                      vectis_error *error) {
  (void)curl;
  (void)userdata;
  vectis_set_error(error, VECTIS_ERR_STATE, "raw curl configuration failed");
  return VECTIS_ERR_STATE;
}

static vectis_status curl_config_fail_first_transfer(CURL *curl, void *userdata,
                                                     vectis_error *error) {
  int *count;

  (void)error;
  count = (int *)userdata;
  if (count != NULL) {
    (*count)++;
    if (*count == 1) {
      (void)curl_easy_setopt(curl, CURLOPT_URL,
                             "file:///tmp/vectis_http_missing_retry.txt");
    }
  }
  return VECTIS_OK;
}

static vectis_status response_stream_ok(const void *data, size_t size,
                                        void *userdata, vectis_error *error) {
  sample_doc *doc;

  (void)error;
  doc = (sample_doc *)userdata;
  assert(size < sizeof(doc->id));
  memcpy(doc->id, data, size);
  doc->id[size] = '\0';
  return VECTIS_OK;
}

static vectis_status response_stream_fail(const void *data, size_t size,
                                          void *userdata, vectis_error *error) {
  (void)data;
  (void)size;
  (void)userdata;
  vectis_set_error(error, VECTIS_ERR_STATE, "stream callback failed");
  return VECTIS_ERR_STATE;
}

static void assert_source_equals(lc_source *source, const void *expected,
                                 size_t expected_size) {
  lc_sink *sink;
  lc_error error;
  const void *bytes;
  size_t size;
  size_t written;

  sink = NULL;
  bytes = NULL;
  size = 0u;
  written = 0u;
  lc_error_init(&error);
  assert(lc_sink_to_memory(&sink, &error) == LC_OK);
  assert(lc_copy(source, sink, &written, &error) == LC_OK);
  assert(written == expected_size);
  assert(lc_sink_memory_bytes(sink, &bytes, &size, &error) == LC_OK);
  assert(size == expected_size);
  assert(memcmp(bytes, expected, expected_size) == 0);
  lc_sink_close(sink);
  lc_error_cleanup(&error);
}

static void assert_http_surface(void) {
  vectis_http_client_config client;
  vectis_http_client *handle;
  vectis_http_client *no_retry_handle;
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
  const char json_array_path[] = "/tmp/vectis_http_array.json";
  const char bad_json_array_path[] = "/tmp/vectis_http_bad_array.json";
  const char source_body[] = "vectis file body";
  const char upload_body[] = "upload body";
  const char json_body[] = "{\"id\":\"downstream\"}";
  const char json_array_body[] =
      "{\"items\":[{\"id\":\"one\",\"count\":2,\"active\":true},{\"id\":"
      "\"two\",\"count\":5,\"active\":false}]}";
  const char bad_json_array_body[] =
      "{\"items\":[{\"id\":\"broken\",\"count\":2,\"active\":true}";
  sample_dsv_doc array_item;
  sample_dsv_rows array_rows;
  int curl_config_count;
  int retry_config_count;

  assert(strcmp(vectis_status_string(VECTIS_ERR_TIMEOUT), "timeout") == 0);
  curl_config_count = 0;
  retry_config_count = 0;
  handle = NULL;
  no_retry_handle = NULL;
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
  handle = (vectis_http_client *)(uintptr_t)1u;
  status = vectis_http_client_new(&client, &handle, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(handle == NULL);
  assert(strstr(error.message, "non-negative") != NULL);
  client.low_speed_limit_bytes_per_sec = 0L;
  client.timeout_ms = 0L;
  client.connect_timeout_ms = 0L;
  client.retry_max_attempts = 0u;
  client.retry_initial_delay_ms = 0L;
  client.retry_max_delay_ms = 0L;
  status = vectis_http_client_new(&client, &handle, &error);
  assert(status == VECTIS_OK);
  assert(handle->config.timeout_ms == 30000L);
  assert(handle->config.connect_timeout_ms == 10000L);
  assert(handle->config.retry_max_attempts == 1u);
  assert(handle->config.retry_initial_delay_ms == 250L);
  assert(handle->config.retry_max_delay_ms == 2000L);
  handle->close(handle);
  handle = NULL;
  vectis_http_client_config_init(&client);
  client.configure_curl = curl_config_ok;
  client.configure_curl_userdata = &curl_config_count;

  status = vectis_http_get(&client, NULL, &response, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "url") != NULL);

  fp = fopen(source_path, "wb");
  assert(fp != NULL);
  assert(fwrite(source_body, 1u, sizeof(source_body) - 1u, fp) ==
         sizeof(source_body) - 1u);
  assert(fclose(fp) == 0);
  fp = fopen(json_path, "wb");
  assert(fp != NULL);
  assert(fwrite(json_body, 1u, sizeof(json_body) - 1u, fp) ==
         sizeof(json_body) - 1u);
  assert(fclose(fp) == 0);
  fp = fopen(json_array_path, "wb");
  assert(fp != NULL);
  assert(fwrite(json_array_body, 1u, sizeof(json_array_body) - 1u, fp) ==
         sizeof(json_array_body) - 1u);
  assert(fclose(fp) == 0);
  fp = fopen(bad_json_array_path, "wb");
  assert(fp != NULL);
  assert(fwrite(bad_json_array_body, 1u, sizeof(bad_json_array_body) - 1u,
                fp) == sizeof(bad_json_array_body) - 1u);
  assert(fclose(fp) == 0);

  client.base_url = "file:///tmp";
  status =
      vectis_http_get(&client, "/vectis_http_source.txt", &response, &error);
  assert(status == VECTIS_OK);
  assert(curl_config_count == 1);
  assert(response.body_size == sizeof(source_body) - 1u);
  assert(memcmp(response.body, source_body, sizeof(source_body) - 1u) == 0);
  vectis_http_response_cleanup(&response);

  status = vectis_http_get(&client, "/vectis_http_doc.json", &response, &error);
  assert(status == VECTIS_OK);
  memset(&doc, 0, sizeof(doc));
  status =
      vectis_http_response_json_into(&response, &sample_doc_map, &doc, &error);
  assert(status == VECTIS_OK);
  assert(strcmp(doc.id, "downstream") == 0);
  vectis_http_response_cleanup(&response);

  status =
      vectis_http_get(&client, "/vectis_http_array.json", &response, &error);
  assert(status == VECTIS_OK);
  memset(&array_rows, 0, sizeof(array_rows));
  memset(&array_item, 0, sizeof(array_item));
  status = vectis_http_response_json_array_each(
      &response, "items", &sample_dsv_doc_map, &array_item,
      sample_json_array_item, &array_rows, &error);
  assert(status == VECTIS_OK);
  assert(array_rows.count == 2u);
  assert(array_rows.total == 7);
  assert(array_rows.active_count == 1);
  assert(strcmp(array_rows.last_id, "two") == 0);
  vectis_http_response_cleanup(&response);

  status = vectis_http_client_new(&client, &handle, &error);
  assert(status == VECTIS_OK);
  assert(handle != NULL);
  assert(handle->execute != NULL);
  assert(handle->get != NULL);
  assert(handle->get_json_array != NULL);
  assert(handle->del != NULL);
  assert(handle->head != NULL);
  assert(handle->options != NULL);
  assert(handle->download_file != NULL);
  assert(handle->upload_file != NULL);
  assert(handle->post_json != NULL);
  assert(handle->put_json != NULL);
  assert(handle->patch_json != NULL);
  assert(handle->close != NULL);
  assert(strcmp(handle->config.base_url, "file:///tmp") == 0);
  vectis_http_request_init(&request);
  assert(request.retry_conditions == VECTIS_HTTP_RETRY_INHERIT);
  request.url = "/vectis_http_source.txt";
  status = handle->execute(handle, &request, &response, &error);
  assert(status == VECTIS_OK);
  assert(curl_config_count == 4);
  assert(response.body_size == sizeof(source_body) - 1u);
  vectis_http_response_cleanup(&response);

  memset(&array_rows, 0, sizeof(array_rows));
  memset(&array_item, 0, sizeof(array_item));
  status = handle->get_json_array(
      handle, "/vectis_http_array.json", "items", &sample_dsv_doc_map,
      &array_item, sample_json_array_item, &array_rows, &response, &error);
  assert(status == VECTIS_OK);
  assert(response.body == NULL);
  assert(response.body_size == 0u);
  assert(array_rows.count == 2u);
  assert(array_rows.total == 7);
  assert(array_rows.active_count == 1);
  vectis_http_response_cleanup(&response);

  memset(&array_rows, 0, sizeof(array_rows));
  memset(&array_item, 0, sizeof(array_item));
  status = handle->get_json_array(handle, "/vectis_http_array.json", "items",
                                  &sample_dsv_doc_map, &array_item,
                                  sample_json_array_fail_on_second, &array_rows,
                                  &response, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "array callback stopped") != NULL);
  assert(response.body == NULL);
  assert(response.body_size == 0u);
  assert(array_rows.count == 2u);
  vectis_http_response_cleanup(&response);

  memset(&array_rows, 0, sizeof(array_rows));
  memset(&array_item, 0, sizeof(array_item));
  status = handle->get_json_array(
      handle, "/vectis_http_bad_array.json", "items", &sample_dsv_doc_map,
      &array_item, sample_json_array_item, &array_rows, &response, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(error.source == VECTIS_ERROR_SOURCE_LONEJSON);
  assert(response.body == NULL);
  assert(response.body_size == 0u);
  assert(array_rows.count == 0u);
  vectis_http_response_cleanup(&response);

  vectis_http_request_init(&request);
  request.url = "/vectis_http_source.txt";
  request.retry_max_attempts = 2u;
  request.retry_initial_delay_ms = 0L;
  request.retry_max_delay_ms = 0L;
  request.configure_curl = curl_config_fail_first_transfer;
  retry_config_count = 0;
  request.configure_curl_userdata = &retry_config_count;
  status = handle->execute(handle, &request, &response, &error);
  assert(status == VECTIS_OK);
  assert(retry_config_count == 2);
  assert(response.body_size == sizeof(source_body) - 1u);
  vectis_http_response_cleanup(&response);

  client.retry_max_attempts = 2u;
  client.retry_initial_delay_ms = 0L;
  client.retry_max_delay_ms = 0L;
  client.retry_conditions = VECTIS_HTTP_RETRY_DEFAULT;
  status = vectis_http_client_new(&client, &no_retry_handle, &error);
  assert(status == VECTIS_OK);
  vectis_http_request_init(&request);
  request.url = "/vectis_http_source.txt";
  request.retry_conditions = VECTIS_HTTP_RETRY_NONE;
  request.configure_curl = curl_config_fail_first_transfer;
  retry_config_count = 0;
  request.configure_curl_userdata = &retry_config_count;
  status =
      no_retry_handle->execute(no_retry_handle, &request, &response, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(error.source == VECTIS_ERROR_SOURCE_CURL);
  assert(retry_config_count == 1);
  vectis_http_response_cleanup(&response);
  no_retry_handle->close(no_retry_handle);
  no_retry_handle = NULL;
  client.retry_max_attempts = 1u;
  client.retry_initial_delay_ms = 250L;
  client.retry_max_delay_ms = 2000L;
  client.retry_conditions = VECTIS_HTTP_RETRY_DEFAULT;

  vectis_http_request_init(&request);
  request.url = "/vectis_http_source.txt";
  request.retry_max_attempts = 2u;
  request.response_body = response_stream_ok;
  status = handle->execute(handle, &request, &response, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "streaming") != NULL);

  client.retry_max_attempts = 2u;
  client.retry_conditions = VECTIS_HTTP_RETRY_NONE;
  status = vectis_http_client_new(&client, &no_retry_handle, &error);
  assert(status == VECTIS_OK);
  vectis_http_request_init(&request);
  request.url = "/vectis_http_source.txt";
  request.response_body = response_stream_ok;
  request.response_body_userdata = &doc;
  memset(&doc, 0, sizeof(doc));
  status =
      no_retry_handle->execute(no_retry_handle, &request, &response, &error);
  assert(status == VECTIS_OK);
  assert(response.body == NULL);
  assert(response.body_size == 0u);
  assert(memcmp(doc.id, source_body, sizeof(source_body) - 1u) == 0);
  vectis_http_response_cleanup(&response);
  no_retry_handle->close(no_retry_handle);
  no_retry_handle = NULL;
  client.retry_max_attempts = 1u;
  client.retry_conditions = VECTIS_HTTP_RETRY_DEFAULT;

  vectis_http_request_init(&request);
  request.url = "/vectis_http_source.txt";
  request.configure_curl = curl_config_fail;
  status = handle->execute(handle, &request, &response, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "raw curl") != NULL);

  status = handle->download_file(handle, "/vectis_http_source.txt",
                                 helper_download_path, &response, &error);
  assert(status == VECTIS_OK);
  vectis_http_response_cleanup(&response);
  fp = fopen(helper_download_path, "rb");
  assert(fp != NULL);
  memset(&doc, 0, sizeof(doc));
  assert(fread(doc.id, 1u, sizeof(source_body) - 1u, fp) ==
         sizeof(source_body) - 1u);
  assert(fclose(fp) == 0);
  assert(memcmp(doc.id, source_body, sizeof(source_body) - 1u) == 0);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_PUT;
  request.url = "file:///tmp/vectis_http_upload.txt";
  request.body = upload_body;
  request.body_size = sizeof(upload_body) - 1u;
  status = handle->execute(handle, &request, &response, &error);
  assert(status == VECTIS_OK);
  vectis_http_response_cleanup(&response);
  fp = fopen(upload_path, "rb");
  assert(fp != NULL);
  memset(&doc, 0, sizeof(doc));
  assert(fread(doc.id, 1u, sizeof(upload_body) - 1u, fp) ==
         sizeof(upload_body) - 1u);
  assert(fclose(fp) == 0);
  assert(memcmp(doc.id, upload_body, sizeof(upload_body) - 1u) == 0);

  status = vectis_http_upload_file(
      &client, VECTIS_HTTP_PUT, "file:///tmp/vectis_http_upload_helper.txt",
      source_path, "text/plain", &response, &error);
  assert(status == VECTIS_OK);
  vectis_http_response_cleanup(&response);
  fp = fopen(helper_upload_path, "rb");
  assert(fp != NULL);
  memset(&doc, 0, sizeof(doc));
  assert(fread(doc.id, 1u, sizeof(source_body) - 1u, fp) ==
         sizeof(source_body) - 1u);
  assert(fclose(fp) == 0);
  assert(memcmp(doc.id, source_body, sizeof(source_body) - 1u) == 0);

  vectis_http_request_init(&request);
  request.url = "/vectis_http_source.txt";
  request.low_speed_limit_bytes_per_sec = 1L;
  request.low_speed_time_seconds = 10L;
  request.response_body = response_stream_ok;
  request.response_body_userdata = &doc;
  memset(&doc, 0, sizeof(doc));
  status = handle->execute(handle, &request, &response, &error);
  assert(status == VECTIS_OK);
  assert(response.body == NULL);
  assert(response.body_size == 0u);
  assert(memcmp(doc.id, source_body, sizeof(source_body) - 1u) == 0);
  vectis_http_response_cleanup(&response);

  vectis_http_request_init(&request);
  request.url = "/vectis_http_source.txt";
  request.response_body = response_stream_fail;
  status = handle->execute(handle, &request, &response, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "stream callback") != NULL);

  vectis_http_request_init(&request);
  request.url = "/vectis_http_source.txt";
  request.download_path = helper_download_path;
  request.response_body = response_stream_ok;
  status = handle->execute(handle, &request, &response, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "download_path") != NULL);

  status = handle->upload_file(handle, VECTIS_HTTP_GET,
                               "file:///tmp/vectis_http_missing_upload.txt",
                               source_path, "text/plain", &response, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "upload-capable") != NULL);

  vectis_http_request_init(&request);
  request.url = "file:///tmp/vectis_http_upload.txt";
  request.body = upload_body;
  request.body_size = sizeof(upload_body) - 1u;
  status = handle->execute(handle, &request, &response, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "upload-capable") != NULL);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_HEAD;
  request.url = "file:///tmp/vectis_http_upload.txt";
  request.body = upload_body;
  request.body_size = sizeof(upload_body) - 1u;
  status = handle->execute(handle, &request, &response, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "upload-capable") != NULL);

  vectis_http_request_init(&request);
  request.method = VECTIS_HTTP_OPTIONS;
  request.url = "file:///tmp/vectis_http_upload.txt";
  request.body = upload_body;
  request.body_size = sizeof(upload_body) - 1u;
  status = handle->execute(handle, &request, &response, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "upload-capable") != NULL);

  status = handle->head(handle, "/vectis_http_source.txt", &response, &error);
  assert(status == VECTIS_OK);
  assert(response.body_size == 0u);
  vectis_http_response_cleanup(&response);
  status =
      handle->options(handle, "/vectis_http_source.txt", &response, &error);
  assert(status != VECTIS_ERR_NOT_IMPLEMENTED);
  vectis_http_response_cleanup(&response);

  status = handle->del(handle, "/vectis_http_source.txt", &response, &error);
  assert(status != VECTIS_ERR_INVALID);
  vectis_http_response_cleanup(&response);

  status = vectis_http_client_execute(NULL, &request, &response, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "HTTP client") != NULL);
  handle->close(handle);

  (void)remove(source_path);
  (void)remove(upload_path);
  (void)remove(helper_download_path);
  (void)remove(helper_upload_path);
  (void)remove(missing_upload_path);
  (void)remove(json_path);
  (void)remove(json_array_path);
  (void)remove(bad_json_array_path);
  vectis_http_response_cleanup(&response);
}

static void assert_io_surface(void) {
  vectis_sftp_config sftp;
  vectis_sftp *sftp_handle;
  lc_source *sftp_key_source;
  failing_source_context sftp_key_context;
  vectis_ssh_config ssh;
  vectis_ssh *ssh_handle;
  vectis_ssh_exec_result result;
  vectis_ssh_sftp_stat_result stat_result;
  vectis_ssh_sftp_dir_entry dir_entry;
  vectis_ssh_sftp_session *sftp_session;
  vectis_ssh_sftp_file *sftp_file;
  vectis_ssh_sftp_dir *sftp_dir;
  vectis_mqtt_config mqtt;
  vectis_mqtt *mqtt_handle;
  vectis_cert_bundle_config certs;
  vectis_error error;
  vectis_status status;
  const char payload[] = "ready";
  const char bundle_path[] = "/tmp/vectis-test-bundle.pem";
  const char cert_path[] = "/tmp/vectis-test-cert.pem";
  const char key_path[] = "/tmp/vectis-test-key.pem";
  const char sftp_upload_path[] = "/tmp/vectis-test-sftp-upload.txt";
  const char ssh_key_pem[] = "not a real ssh key";
  char line[128];
  char sftp_buffer[16];
  size_t sftp_io_size;
  FILE *fp;

  sftp_handle = NULL;
  sftp_key_source = NULL;
  ssh_handle = NULL;
  sftp_session = NULL;
  sftp_file = NULL;
  sftp_dir = NULL;
  mqtt_handle = NULL;
  vectis_ssh_sftp_dir_entry_init(&dir_entry);
  vectis_sftp_config_init(&sftp);
  assert(sftp.timeout_ms == 30000L);
  sftp.timeout_ms = 0L;
  sftp_handle = (vectis_sftp *)(uintptr_t)1u;
  status = vectis_sftp_new(&sftp, &sftp_handle, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(sftp_handle == NULL);
  status = vectis_sftp_new(&sftp, &sftp_handle, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_sftp_upload_file(&sftp, "local", "remote", &error);
  assert(status == VECTIS_ERR_INVALID);
  sftp.url = "sftp://127.0.0.1:1";
  status = vectis_sftp_new(&sftp, &sftp_handle, &error);
  assert(status == VECTIS_OK);
  assert(sftp_handle != NULL);
  assert(sftp_handle->config.timeout_ms == 30000L);
  assert(sftp_handle->upload_file != NULL);
  assert(sftp_handle->download_file != NULL);
  assert(sftp_handle->close != NULL);
  assert(strcmp(sftp_handle->config.url, "sftp://127.0.0.1:1") == 0);
  status = sftp_handle->upload_file(NULL, "local", "remote", &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "SFTP handle") != NULL);
  sftp_handle->close(sftp_handle);

  fp = fopen(sftp_upload_path, "wb");
  assert(fp != NULL);
  assert(fwrite(payload, 1u, sizeof(payload) - 1u, fp) == sizeof(payload) - 1u);
  assert(fclose(fp) == 0);
  memset(&sftp_key_context, 0, sizeof(sftp_key_context));
  assert(lc_source_from_callbacks(failing_source_read, failing_source_reset,
                                  NULL, &sftp_key_context, &sftp_key_source,
                                  NULL) == LC_OK);
  sftp.private_key = vectis_source_from_lc(sftp_key_source);
  sftp.private_key_path = NULL;
  status = vectis_sftp_upload_file(&sftp, sftp_upload_path, "/remote", &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "SFTP private key") != NULL);
  assert(strstr(error.message, "source failed") != NULL);
  assert(sftp_key_context.read_count > 0);
  lc_source_close(sftp_key_source);
  sftp_key_source = NULL;
  vectis_source_init(&sftp.private_key);
  (void)remove(sftp_upload_path);

  vectis_ssh_config_init(&ssh);
  memset(&result, 0, sizeof(result));
  assert(ssh.port == 22u);
  assert(ssh.timeout_ms == 30000L);
  ssh.port = 0u;
  ssh.timeout_ms = 0L;
  ssh_handle = (vectis_ssh *)(uintptr_t)1u;
  status = vectis_ssh_new(&ssh, &ssh_handle, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(ssh_handle == NULL);
  status = vectis_ssh_new(&ssh, &ssh_handle, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_exec(&ssh, "uptime", &result, &error);
  assert(status == VECTIS_ERR_INVALID);
  ssh.host = "127.0.0.1";
  ssh.username = "vectis";
  ssh.password = "secret";
  status = vectis_ssh_new(&ssh, &ssh_handle, &error);
  assert(status == VECTIS_OK);
  assert(ssh_handle != NULL);
  assert(ssh_handle->exec != NULL);
  assert(ssh_handle->sftp_upload_file != NULL);
  assert(ssh_handle->sftp_download_file != NULL);
  assert(ssh_handle->scp_upload_file != NULL);
  assert(ssh_handle->scp_download_file != NULL);
  assert(ssh_handle->sftp_stat != NULL);
  assert(ssh_handle->sftp_mkdir != NULL);
  assert(ssh_handle->sftp_remove != NULL);
  assert(ssh_handle->sftp_rmdir != NULL);
  assert(ssh_handle->sftp_rename != NULL);
  assert(ssh_handle->sftp_chmod != NULL);
  assert(ssh_handle->sftp_open != NULL);
  assert(ssh_handle->close != NULL);
  assert(strcmp(ssh_handle->config.host, "127.0.0.1") == 0);
  assert(ssh_handle->config.port == 22u);
  assert(ssh_handle->config.timeout_ms == 30000L);
  status = ssh_handle->exec(NULL, "uptime", &result, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "SSH handle") != NULL);
  status = ssh_handle->sftp_stat(NULL, "remote", &stat_result, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "SSH handle") != NULL);
  status = ssh_handle->sftp_open(NULL, &sftp_session, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "SSH handle") != NULL);
  ssh_handle->close(ssh_handle);
  ssh.port = 1u;
  ssh.password = NULL;
  ssh.private_key =
      vectis_source_from_memory(ssh_key_pem, sizeof(ssh_key_pem) - 1u);
  status = vectis_ssh_new(&ssh, &ssh_handle, &error);
  assert(status == VECTIS_OK);
  assert(ssh_handle->config.private_key.memory == ssh_key_pem);
  assert(ssh_handle->config.private_key.memory_size ==
         sizeof(ssh_key_pem) - 1u);
  ssh_handle->close(ssh_handle);
  ssh.password = "secret";
  vectis_source_init(&ssh.private_key);
  status = vectis_ssh_sftp_upload_file(&ssh, "local", "remote", &error);
  assert(status == VECTIS_ERR_INVALID || status == VECTIS_ERR_STATE);
  status = vectis_ssh_scp_upload_file(&ssh, NULL, "remote", &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_scp_download_file(&ssh, "remote", NULL, &error);
  assert(status == VECTIS_ERR_INVALID);
  vectis_ssh_sftp_stat_result_init(&stat_result);
  stat_result.size = 42u;
  status = vectis_ssh_sftp_stat(&ssh, NULL, &stat_result, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(stat_result.size == 0u);
  status = vectis_ssh_sftp_mkdir(&ssh, NULL, 0755UL, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_sftp_mkdir(&ssh, "remote", 010000UL, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_sftp_remove(&ssh, NULL, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_sftp_rmdir(&ssh, NULL, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_sftp_rename(&ssh, "old", NULL, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_sftp_chmod(&ssh, NULL, 0644UL, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_sftp_chmod(&ssh, "remote", 010000UL, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_sftp_session_new(&ssh, NULL, &error);
  assert(status == VECTIS_ERR_INVALID);
  sftp_session = (vectis_ssh_sftp_session *)(uintptr_t)1u;
  status = vectis_ssh_sftp_session_new(NULL, &sftp_session, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(sftp_session == NULL);
  status = vectis_ssh_sftp_session_open_file(
      NULL, "remote", VECTIS_SSH_SFTP_OPEN_READ, 0UL, &sftp_file, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_sftp_session_open_dir(NULL, "remote", &sftp_dir, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_sftp_session_stat(NULL, "remote", &stat_result, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_sftp_session_mkdir(NULL, "remote", 0755UL, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_sftp_session_remove(NULL, "remote", &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_sftp_session_rmdir(NULL, "remote", &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_sftp_session_rename(NULL, "old", "new", &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_sftp_session_chmod(NULL, "remote", 0644UL, &error);
  assert(status == VECTIS_ERR_INVALID);
  sftp_io_size = 1u;
  status = vectis_ssh_sftp_file_read(NULL, sftp_buffer, sizeof(sftp_buffer),
                                     &sftp_io_size, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(sftp_io_size == 0u);
  sftp_io_size = 1u;
  status = vectis_ssh_sftp_file_write(NULL, "data", 4u, &sftp_io_size, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(sftp_io_size == 0u);
  status = vectis_ssh_sftp_file_stat(NULL, &stat_result, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_ssh_sftp_dir_read(NULL, &dir_entry, &error);
  assert(status == VECTIS_ERR_INVALID);
  vectis_ssh_sftp_dir_entry_cleanup(&dir_entry);
  status = vectis_ssh_exec(&ssh, "true", &result, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(error.source == VECTIS_ERROR_SOURCE_LIBSSH2);
  vectis_ssh_exec_result_cleanup(&result);

  vectis_mqtt_config_init(&mqtt);
  mqtt.timeout_ms = 0L;
  mqtt_handle = (vectis_mqtt *)(uintptr_t)1u;
  status = vectis_mqtt_new(&mqtt, &mqtt_handle, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(mqtt_handle == NULL);
  status = vectis_mqtt_new(&mqtt, &mqtt_handle, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_mqtt_publish(&mqtt, "workflow/test", payload,
                               sizeof(payload) - 1u, "text/plain", &error);
  assert(status == VECTIS_ERR_INVALID);
  mqtt.broker_url = "mqtt://127.0.0.1:1";
  status = vectis_mqtt_new(&mqtt, &mqtt_handle, &error);
  assert(status == VECTIS_OK);
  assert(mqtt_handle != NULL);
  assert(mqtt_handle->publish != NULL);
  assert(mqtt_handle->publish_json != NULL);
  assert(mqtt_handle->close != NULL);
  assert(strcmp(mqtt_handle->config.broker_url, "mqtt://127.0.0.1:1") == 0);
  assert(mqtt_handle->config.timeout_ms == 30000L);
  status = mqtt_handle->publish(NULL, "workflow/test", payload,
                                sizeof(payload) - 1u, "text/plain", &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "MQTT handle") != NULL);
  mqtt_handle->close(mqtt_handle);

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
  vectis_body_spill_config spill_config;
  vectis_body_spill_result spill_result;
  char temp_response_path[4096];
  char small_body_buffer[64];
  char tiny_body_buffer[4];
  sample_doc doc;
  const char json[] = "{\"id\":\"abc\"}";
  const char text[] = "created";

  request = vectis_internal_request_new(&error);
  response = vectis_internal_response_new(&error);
  assert(request != NULL);
  assert(response != NULL);

  status = vectis_internal_request_set_body(request, json, sizeof(json) - 1u,
                                            &error);
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
  status =
      vectis_internal_request_add_query(request, "expand", "items", &error);
  assert(status == VECTIS_OK);
  status = vectis_internal_request_add_header(request, "content-type",
                                              "application/json", &error);
  assert(status == VECTIS_OK);

  assert(strcmp(vectis_request_path_param(request, "id"), "abc") == 0);
  assert(strcmp(vectis_request_query(request, "expand"), "items") == 0);
  assert(strcmp(vectis_request_header(request, "content-type"),
                "application/json") == 0);
  assert(strcmp(vectis_request_header(request, "Content-Type"),
                "application/json") == 0);
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
  status = vectis_request_body_materialize(request, &materialize_config,
                                           &materialized, &error);
  assert(status == VECTIS_OK);
  assert(materialized.kind == VECTIS_BODY_MATERIALIZED_MEMORY);
  assert(materialized.memory.data == small_body_buffer);
  assert(materialized.memory.size == sizeof(json) - 1u);
  assert(memcmp(materialized.memory.data, json, sizeof(json) - 1u) == 0);
  source = NULL;
  status = vectis_body_materialized_open_reader(&materialized, &source, &error);
  assert(status == VECTIS_OK);
  assert_source_equals(source, json, sizeof(json) - 1u);
  lc_source_close(source);
  vectis_body_materialized_cleanup(&materialized);

  memset(&body_copy, 0, sizeof(body_copy));
  status = vectis_request_body_read_all(request, &body_copy, &error);
  assert(status == VECTIS_OK);
  assert(body_copy.size == sizeof(json) - 1u);
  assert(memcmp(body_copy.data, json, sizeof(json) - 1u) == 0);
  vectis_mutable_bytes_cleanup(&body_copy);

  vectis_body_spill_config_init(&spill_config);
  spill_config.memory_limit_bytes = 64u;
  memset(&spill_result, 0, sizeof(spill_result));
  status =
      vectis_request_body_spill(request, &spill_config, &spill_result, &error);
  assert(status == VECTIS_OK);
  assert(!spill_result.spooled_to_disk);
  assert(spill_result.memory.size == sizeof(json) - 1u);
  assert(memcmp(spill_result.memory.data, json, sizeof(json) - 1u) == 0);
  vectis_body_spill_result_cleanup(&spill_result);

  vectis_body_materialize_config_init(&materialize_config);
  materialize_config.buffer = tiny_body_buffer;
  materialize_config.buffer_size = sizeof(tiny_body_buffer);
  materialize_config.prefix = "vectis-sdk-body";
  status = vectis_request_body_materialize(request, &materialize_config,
                                           &materialized, &error);
  assert(status == VECTIS_OK);
  assert(materialized.kind == VECTIS_BODY_MATERIALIZED_FILE);
  assert(materialized.path != NULL);
  assert(materialized.size == sizeof(json) - 1u);
  source = NULL;
  status = vectis_body_materialized_open_reader(&materialized, &source, &error);
  assert(status == VECTIS_OK);
  assert_source_equals(source, json, sizeof(json) - 1u);
  lc_source_close(source);
  remove(materialized.path);
  vectis_body_materialized_cleanup(&materialized);

  vectis_body_spill_config_init(&spill_config);
  spill_config.memory_limit_bytes = 4u;
  spill_config.prefix = "vectis-sdk-spill";
  memset(&spill_result, 0, sizeof(spill_result));
  status =
      vectis_request_body_spill(request, &spill_config, &spill_result, &error);
  assert(status == VECTIS_OK);
  assert(spill_result.spooled_to_disk);
  assert(spill_result.path != NULL);
  assert(spill_result.size == sizeof(json) - 1u);
  assert(lc_source_from_file(spill_result.path, &source, NULL) == LC_OK);
  assert_source_equals(source, json, sizeof(json) - 1u);
  lc_source_close(source);
  assert(remove(spill_result.path) == 0);
  vectis_body_spill_result_cleanup(&spill_result);

  status = vectis_response_text(response, 201, "text/plain", text, &error);
  assert(status == VECTIS_OK);
  body = vectis_internal_response_body(response);
  assert(vectis_internal_response_status_code(response) == 201);
  assert(strcmp(vectis_internal_response_content_type(response),
                "text/plain") == 0);
  assert(body.size == sizeof(text) - 1u);
  assert(memcmp(body.data, text, sizeof(text) - 1u) == 0);

  status = vectis_response_json(response, 200, &sample_doc_map, &doc, &error);
  assert(status == VECTIS_OK);
  body = vectis_internal_response_body(response);
  assert(vectis_internal_response_status_code(response) == 200);
  assert(strcmp(vectis_internal_response_content_type(response),
                "application/json") == 0);
  assert(body.size > 0u);
  assert(sample_bytes_contains(body, "\"abc\""));
  status = vectis_response_json_generated(response, 200, &sample_doc_map, &doc,
                                          &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_body(response).data == NULL);
  assert(vectis_internal_response_file_path(response) != NULL);
  assert(vectis_internal_response_file_temporary(response));
  source = NULL;
  assert(lc_source_from_memory(text, sizeof(text) - 1u, &source, NULL) ==
         LC_OK);
  status = vectis_response_source(response, 200, "text/plain", source, &error);
  lc_source_close(source);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_body(response).data == NULL);
  assert(vectis_internal_response_file_path(response) != NULL);
  assert(vectis_internal_response_file_temporary(response));
  assert(access(vectis_internal_response_file_path(response), F_OK) == 0);
  assert(strlen(vectis_internal_response_file_path(response)) <
         sizeof(temp_response_path));
  strcpy(temp_response_path, vectis_internal_response_file_path(response));
  status = vectis_response_status(response, 204, &error);
  assert(status == VECTIS_OK);
  assert(access(temp_response_path, F_OK) != 0);
  status = vectis_response_header(response, "x-vectis-test", "ok", &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_header_count(response) == 1u);
  assert(strcmp(vectis_internal_response_header_name(response, 0u),
                "x-vectis-test") == 0);
  assert(strcmp(vectis_internal_response_header_value(response, 0u), "ok") ==
         0);
  status = vectis_response_header(response, "bad:name", "ok", &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_response_header(response, "x-bad", "bad\nvalue", &error);
  assert(status == VECTIS_ERR_INVALID);

  status = vectis_response_error_json(response, 422, "invalid_order",
                                      "order is invalid", "missing id", &error);
  assert(status == VECTIS_OK);
  body = vectis_internal_response_body(response);
  assert(vectis_internal_response_status_code(response) == 422);
  assert(strcmp(vectis_internal_response_content_type(response),
                "application/json") == 0);
  assert(sample_bytes_contains(body, "\"code\":\"invalid_order\""));
  assert(sample_bytes_contains(body, "\"message\":\"order is invalid\""));
  assert(sample_bytes_contains(body, "\"detail\":\"missing id\""));

  vectis_internal_request_free(request);
  vectis_internal_response_free(response);
}

static void assert_json_route_surface(void) {
  vectis_app_config config;
  vectis_json_route_config route;
  vectis_json_typed_route_config typed_route;
  vectis_xml_route_config xml_route;
  vectis_dsv_route_config dsv_route;
  vectis_xml_config xml_config;
  vectis_dsv_config dsv_config;
  vectis_route_config raw_route;
  vectis_static_file_config static_file;
  vectis_static_directory_config static_dir;
  vectis_body_policy policy;
  vectis_error error;
  vectis_status status;
  vectis_app *app;
  vectis_app *root_static_app;
  vectis_request *request;
  vectis_response *response;
  vectis_bytes body;
  vectis_mutable_bytes body_copy;
  lonejson *json_runtime;
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
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  root_static_app = NULL;
  json_runtime = lonejson_new(NULL, NULL);
  assert(json_runtime != NULL);
  assert(app->start != NULL);
  assert(app->stop != NULL);
  assert(app->run != NULL);
  assert(app->wait != NULL);
  assert(app->route != NULL);
  assert(app->json_route != NULL);
  assert(app->json_typed_route != NULL);
  assert(app->xml_route != NULL);
  assert(app->dsv_route != NULL);
  assert(app->upload_stream != NULL);
  assert(app->upload_file != NULL);
  assert(app->upload_reader != NULL);
  assert(app->prefixed_route != NULL);
  assert(app->prefixed_json_route != NULL);
  assert(app->prefixed_json_typed_route != NULL);
  assert(app->prefixed_xml_route != NULL);
  assert(app->prefixed_dsv_route != NULL);
  assert(app->static_file != NULL);
  assert(app->static_directory != NULL);
  assert(app->static_embedded != NULL);
  assert(app->webdav != NULL);
  assert(app->webdav_embedded_site != NULL);
  assert(app->webdav_embedded != NULL);
  assert(app->auth_routes != NULL);
  assert(app->openapi_doc != NULL);
  assert(app->openapi != NULL);
  assert(app->route_count != NULL);
  assert(app->logger != NULL);
  assert(app->lockd_client != NULL);
  assert(app->consumer_service != NULL);
  assert(app->cai_worker_service != NULL);
  assert(app->close != NULL);
  assert(app->lockd_client(app) == NULL);
  request = vectis_internal_request_new(&error);
  response = vectis_internal_response_new(&error);
  assert(request != NULL);
  assert(response != NULL);

  raw_route =
      vectis_route(VECTIS_HTTP_GET, "/state/:id?", sample_route_handler, NULL);
  assert(raw_route.path_kind == VECTIS_ROUTE_PATH_PARAMS);
  status = app->route(app, &raw_route, &error);
  assert(status == VECTIS_OK);

  raw_route = vectis_route_regex(VECTIS_HTTP_GET, "^/internal/[0-9]+$",
                                 sample_route_handler, NULL);
  assert(raw_route.path_kind == VECTIS_ROUTE_PATH_REGEX);
  status = app->route(app, &raw_route, &error);
  assert(status == VECTIS_OK);
  raw_route = vectis_route_regex(VECTIS_HTTP_GET, "^/reports/[0-9]+$",
                                 sample_route_handler, NULL);
  status = app->prefixed_route(app, "/api", &raw_route, &error);
  assert(status == VECTIS_OK);

  raw_route = vectis_json_body_route(VECTIS_HTTP_POST, "/raw-json",
                                     sample_route_handler, NULL);
  assert(raw_route.body.mode == VECTIS_BODY_JSON);
  assert(raw_route.path_kind == VECTIS_ROUTE_PATH_LITERAL);
  status = app->prefixed_route(app, "/api/v1", &raw_route, &error);
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
  status = app->static_file(app, &static_file, &error);
  assert(status == VECTIS_OK);
  vectis_static_directory_config_init(&static_dir);
  static_dir.path_prefix = "/assets/";
  static_dir.root_dir = static_dir_path;
  static_dir.content_type = "application/javascript";
  status = app->static_directory(app, &static_dir, &error);
  assert(status == VECTIS_OK);

  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_GET, "/state/abc",
                                          request, response, &error);
  assert(status == VECTIS_OK);
  body = vectis_internal_response_body(response);
  assert(vectis_internal_response_status_code(response) == 200);
  assert(body.size == 3u);
  assert(memcmp(body.data, "abc", 3u) == 0);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_GET, "/state",
                                          request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 204);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_GET, "/internal/123",
                                          request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 204);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  status = vectis_internal_dispatch_route(
      app, VECTIS_HTTP_GET, "/api/reports/42", request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 204);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  status = vectis_internal_dispatch_route(
      app, VECTIS_HTTP_GET, "/api/reports/x", request, response, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "no route") != NULL);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  route =
      vectis_json_route(VECTIS_HTTP_POST, "^/json-regex/[0-9]+$",
                        &sample_doc_map, sizeof(sample_doc), &sample_doc_map,
                        sizeof(sample_doc), sample_json_handler, NULL);
  assert(route.path_kind == VECTIS_ROUTE_PATH_REGEX);
  status = app->prefixed_json_route(app, "/api", &route, &error);
  assert(status == VECTIS_OK);
  status = vectis_internal_request_set_body(request, json, sizeof(json) - 1u,
                                            &error);
  assert(status == VECTIS_OK);
  status = vectis_internal_dispatch_route(
      app, VECTIS_HTTP_POST, "/api/json-regex/42", request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 200);
  body = vectis_internal_response_body(response);
  memset(&output, 0, sizeof(output));
  assert(lonejson_parse_buffer(json_runtime, &sample_doc_map, &output,
                               body.data, body.size,
                               NULL) == LONEJSON_STATUS_OK);
  assert(strcmp(output.id, "abc") == 0);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  typed_route = vectis_json_typed_route(
      VECTIS_HTTP_POST, "^/typed-regex/[0-9]+$", &sample_doc_map,
      sizeof(sample_doc), sample_json_typed_handler, NULL);
  assert(typed_route.path_kind == VECTIS_ROUTE_PATH_REGEX);
  status = app->prefixed_json_typed_route(app, "/api", &typed_route, &error);
  assert(status == VECTIS_OK);
  status = vectis_internal_request_set_body(request, json, sizeof(json) - 1u,
                                            &error);
  assert(status == VECTIS_OK);
  status = vectis_internal_dispatch_route(
      app, VECTIS_HTTP_POST, "/api/typed-regex/7", request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 201);
  body = vectis_internal_response_body(response);
  memset(&output, 0, sizeof(output));
  assert(lonejson_parse_buffer(json_runtime, &sample_doc_map, &output,
                               body.data, body.size,
                               NULL) == LONEJSON_STATUS_OK);
  assert(strcmp(output.id, "abc") == 0);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  xml_config = vectis_xml_default();
  xml_config.root_element = "sample";
  xml_route = vectis_xml_route(VECTIS_HTTP_POST, "/xml", &sample_xml_doc_map,
                               sizeof(sample_xml_doc), &xml_config, NULL, NULL);
  assert(xml_route.body.mode == VECTIS_BODY_STREAMING_UPLOAD);
  assert(xml_route.buffer_bytes ==
         VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES);
  assert(xml_route.input_map == &sample_xml_doc_map);
  assert(xml_route.input_size == sizeof(sample_xml_doc));
  assert(strcmp(xml_route.config.root_element, "sample") == 0);

  dsv_config = vectis_dsv_csv();
  dsv_route = vectis_dsv_route(VECTIS_HTTP_POST, "/dsv", &sample_dsv_doc_map,
                               sizeof(sample_dsv_doc), &dsv_config, NULL, NULL);
  assert(dsv_route.body.mode == VECTIS_BODY_STREAMING_UPLOAD);
  assert(dsv_route.buffer_bytes ==
         VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES);
  assert(dsv_route.row_map == &sample_dsv_doc_map);
  assert(dsv_route.row_size == sizeof(sample_dsv_doc));
  assert(dsv_route.config.delimiter == ',');

  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_GET, "/static-file",
                                          request, response, &error);
  assert(status == VECTIS_OK);
  body = vectis_internal_response_body(response);
  assert(vectis_request_path(request) != NULL);
  assert(strcmp(vectis_request_path(request), "/static-file") == 0);
  assert(vectis_internal_response_status_code(response) == 200);
  assert(strcmp(vectis_internal_response_file_path(response),
                static_file_path) == 0);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_HEAD, "/static-file",
                                          request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 200);
  body = vectis_internal_response_body(response);
  assert(body.data == NULL);
  assert(body.size == 0u);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  assert(remove(static_file_path) == 0);
  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_HEAD, "/static-file",
                                          request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 404);
  body = vectis_internal_response_body(response);
  assert(body.data == NULL);
  assert(body.size == 0u);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  status = vectis_internal_dispatch_route(
      app, VECTIS_HTTP_GET, "/assets/app.js", request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 200);
  assert(strcmp(vectis_internal_response_file_path(response),
                static_dir_file_path) == 0);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  root_static_app = vectis_app_new(&config, &error);
  assert(root_static_app != NULL);
  vectis_static_directory_config_init(&static_dir);
  static_dir.path_prefix = "/";
  static_dir.root_dir = static_dir_path;
  static_dir.content_type = "application/javascript";
  status =
      root_static_app->static_directory(root_static_app, &static_dir, &error);
  assert(status == VECTIS_OK);
  status = vectis_internal_dispatch_route(root_static_app, VECTIS_HTTP_GET,
                                          "/app.js", request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 200);
  assert(strcmp(vectis_internal_response_file_path(response),
                static_dir_file_path) == 0);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);
  root_static_app->close(root_static_app);
  root_static_app = NULL;

  assert(remove(static_dir_file_path) == 0);
  status = vectis_internal_dispatch_route(
      app, VECTIS_HTTP_HEAD, "/assets/app.js", request, response, &error);
  assert(status == VECTIS_OK);
  assert(vectis_internal_response_status_code(response) == 404);
  body = vectis_internal_response_body(response);
  assert(body.data == NULL);
  assert(body.size == 0u);
  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);

  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_GET, "/state/..",
                                          request, response, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "dot segments") != NULL);

  route = vectis_json_route(VECTIS_HTTP_POST, "/typed/:id", &sample_doc_map,
                            sizeof(sample_doc), &sample_doc_map,
                            sizeof(sample_doc), sample_json_handler, NULL);
  assert(route.path_kind == VECTIS_ROUTE_PATH_PARAMS);
  assert(route.body.mode == VECTIS_BODY_JSON);
  status = app->prefixed_json_route(app, "/api/v1", &route, &error);
  assert(status == VECTIS_OK);
  assert(app->route_count(app) == 9u);
  status = vectis_internal_route_body_policy(
      app, VECTIS_HTTP_POST, "/api/v1/typed/abc", &policy, &error);
  assert(status == VECTIS_OK);
  assert(policy.mode == VECTIS_BODY_JSON);
  status = vectis_internal_route_body_policy(
      app, VECTIS_HTTP_DELETE, "/api/v1/typed/abc", &policy, &error);
  assert(status == VECTIS_ERR_STATE);

  status = vectis_internal_request_set_body(request, json, sizeof(json) - 1u,
                                            &error);
  assert(status == VECTIS_OK);
  status = vectis_internal_dispatch_route(
      app, VECTIS_HTTP_POST, "/api/v1/typed/abc", request, response, &error);
  assert(status == VECTIS_OK);
  body = vectis_internal_response_body(response);
  assert(vectis_internal_response_status_code(response) == 200);
  assert(strcmp(vectis_internal_response_content_type(response),
                "application/json") == 0);
  assert(body.data != NULL);
  memset(&output, 0, sizeof(output));
  assert(lonejson_parse_buffer(json_runtime, &sample_doc_map, &output,
                               body.data, body.size,
                               NULL) == LONEJSON_STATUS_OK);
  assert(strcmp(output.id, "abc") == 0);

  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);
  typed_route = vectis_json_typed_route(VECTIS_HTTP_POST, "/typed-response",
                                        &sample_doc_map, sizeof(sample_doc),
                                        sample_json_typed_handler, NULL);
  status = app->prefixed_json_typed_route(app, "/api/v1", &typed_route, &error);
  assert(status == VECTIS_OK);
  assert(app->route_count(app) == 10u);
  status = vectis_internal_request_set_body(request, json, sizeof(json) - 1u,
                                            &error);
  assert(status == VECTIS_OK);
  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_POST,
                                          "/api/v1/typed-response", request,
                                          response, &error);
  assert(status == VECTIS_OK);
  body = vectis_internal_response_body(response);
  assert(vectis_internal_response_status_code(response) == 201);
  memset(&output, 0, sizeof(output));
  assert(lonejson_parse_buffer(json_runtime, &sample_doc_map, &output,
                               body.data, body.size,
                               NULL) == LONEJSON_STATUS_OK);
  assert(strcmp(output.id, "abc") == 0);

  vectis_internal_response_cleanup(response);
  vectis_internal_request_cleanup(request);
  status = vectis_internal_request_set_body(
      request, "{\"id\":\"conflict\"}", sizeof("{\"id\":\"conflict\"}") - 1u,
      &error);
  assert(status == VECTIS_OK);
  status = vectis_internal_dispatch_route(app, VECTIS_HTTP_POST,
                                          "/api/v1/typed-response", request,
                                          response, &error);
  assert(status == VECTIS_OK);
  body = vectis_internal_response_body(response);
  assert(vectis_internal_response_status_code(response) == 409);
  memset(&error_output, 0, sizeof(error_output));
  assert(lonejson_parse_buffer(json_runtime, &sample_error_doc_map,
                               &error_output, body.data, body.size,
                               NULL) == LONEJSON_STATUS_OK);
  assert(strcmp(error_output.code, "conflict") == 0);

  status = vectis_format_key(key, sizeof(key), &error, "state/%s/%s", "orders",
                             "1001");
  assert(status == VECTIS_OK);
  assert(strcmp(key, "state/orders/1001") == 0);
  status = vectis_format_key(key, sizeof(key), &error, "state/%s", "../secret");
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_lockd_state_load(NULL, key, "owner", 30L, &sample_doc_map,
                                   &output, &error);
  assert(status == VECTIS_ERR_INVALID);
  status = vectis_consumer_service_run_until(NULL, NULL, 1L, &error);
  assert(status == VECTIS_ERR_INVALID);

  fp = fopen(spool_path, "wb");
  assert(fp != NULL);
  assert(fwrite(json, 1u, sizeof(json) - 1u, fp) == sizeof(json) - 1u);
  assert(fclose(fp) == 0);
  memset(&output, 0, sizeof(output));
  status = vectis_internal_request_set_body_path(request, spool_path,
                                                 sizeof(json) - 1u, &error);
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
  lonejson_free(json_runtime);
  app->close(app);
  (void)remove(static_file_path);
  (void)remove(static_dir_file_path);
  (void)rmdir(static_dir_path);
}

static void assert_openapi_surface(void) {
  vectis_app_config config;
  vectis_openapi_route_doc doc;
  vectis_openapi_route_doc get_doc;
  vectis_openapi_document document;
  vectis_mutable_bytes json;
  vectis_mutable_bytes yaml;
  vectis_error error;
  vectis_status status;
  vectis_app *app;
  const char *tags[] = {"orders"};
  const char *path_json;
  const char *path_yaml;

  vectis_app_config_init(&config);
  config.tls.cert_key_bundle_path = "/tmp/server.pem";
  app = vectis_app_new(&config, &error);
  assert(app != NULL);

  vectis_openapi_route_doc_init(&doc);
  vectis_openapi_route_doc_init(&get_doc);
  doc.summary = "Create order";
  doc.operation_id = "createOrder";
  doc.tags = tags;
  doc.tag_count = 1u;
  status = vectis_openapi_request_json(
      &doc, vectis_openapi_lonejson_schema("OrderRequest", &sample_xml_doc_map),
      &error);
  assert(status == VECTIS_OK);
  status = vectis_openapi_response_json(
      &doc, 201, "Created",
      vectis_openapi_lonejson_schema("OrderCreated", &sample_doc_map), &error);
  assert(status == VECTIS_OK);
  status = vectis_openapi_response_json(
      &doc, 409, "Conflict",
      vectis_openapi_lonejson_schema("ApiError", &sample_error_doc_map),
      &error);
  assert(status == VECTIS_OK);
  status = app->openapi_doc(app, VECTIS_HTTP_METHODS_POST, "/orders/:id?", &doc,
                            &error);
  assert(status == VECTIS_OK);

  get_doc.summary = "Get order";
  get_doc.operation_id = "getOrder";
  status = vectis_openapi_response_json(
      &get_doc, 200, "OK",
      vectis_openapi_lonejson_schema("OrderCreated", &sample_doc_map), &error);
  assert(status == VECTIS_OK);
  status = app->openapi_doc(app, VECTIS_HTTP_METHODS_GET, "/orders/:id?",
                            &get_doc, &error);
  assert(status == VECTIS_OK);

  vectis_openapi_document_init(&document);
  document.title = "Orders API";
  document.version = "1.2.3";
  status = app->openapi(app, &document, VECTIS_OPENAPI_JSON, &json, &error);
  assert(status == VECTIS_OK);
  assert(json.data != NULL);
  assert(strstr((const char *)json.data, "\"openapi\":\"3.1.0\"") != NULL);
  assert(strstr((const char *)json.data, "\"/orders/{id}\"") != NULL);
  path_json = strstr((const char *)json.data, "\"/orders/{id}\"");
  assert(path_json != NULL);
  assert(strstr(path_json + 1, "\"/orders/{id}\"") == NULL);
  assert(strstr((const char *)json.data,
                "\"get\":{\"operationId\":\"getOrder\"") != NULL);
  assert(strstr((const char *)json.data,
                "\"post\":{\"operationId\":\"createOrder\"") != NULL);
  assert(strstr((const char *)json.data, "\"operationId\":\"createOrder\"") !=
         NULL);
  assert(strstr((const char *)json.data, "\"201\"") != NULL);
  assert(strstr((const char *)json.data, "\"409\"") != NULL);
  assert(strstr((const char *)json.data, "\"OrderRequest\"") != NULL);
  assert(strstr((const char *)json.data, "\"sample_xml_amount\"") != NULL);
  assert(strstr((const char *)json.data, "\"sample_xml_line\"") != NULL);
  assert(strstr((const char *)json.data,
                "\"$ref\":\"#/components/schemas/sample_xml_amount\"") != NULL);
  assert(strstr((const char *)json.data,
                "\"$ref\":\"#/components/schemas/sample_xml_line\"") != NULL);
  assert(strstr((const char *)json.data, "\"ApiError\"") != NULL);
  assert(strstr((const char *)json.data, "\"required\":[\"id\"]") != NULL);
  vectis_mutable_bytes_cleanup(&json);

  status = app->openapi(app, &document, VECTIS_OPENAPI_YAML, &yaml, &error);
  assert(status == VECTIS_OK);
  assert(yaml.data != NULL);
  assert(strstr((const char *)yaml.data, "openapi: 3.1.0") != NULL);
  assert(strstr((const char *)yaml.data, "\"/orders/{id}\":") != NULL);
  path_yaml = strstr((const char *)yaml.data, "\"/orders/{id}\":");
  assert(path_yaml != NULL);
  assert(strstr(path_yaml + 1, "\"/orders/{id}\":") == NULL);
  assert(strstr((const char *)yaml.data,
                "    get:\n      operationId: \"getOrder\"") != NULL);
  assert(strstr((const char *)yaml.data,
                "    post:\n      operationId: \"createOrder\"") != NULL);
  assert(strstr((const char *)yaml.data, "operationId: \"createOrder\"") !=
         NULL);
  assert(strstr((const char *)yaml.data, "\"201\":") != NULL);
  assert(strstr((const char *)yaml.data, "OrderRequest:") != NULL);
  assert(strstr((const char *)yaml.data, "sample_xml_amount:") != NULL);
  assert(strstr((const char *)yaml.data, "sample_xml_line:") != NULL);
  assert(strstr((const char *)yaml.data,
                "$ref: \"#/components/schemas/sample_xml_amount\"") != NULL);
  assert(strstr((const char *)yaml.data,
                "$ref: \"#/components/schemas/sample_xml_line\"") != NULL);
  assert(strstr((const char *)yaml.data, "ApiError:") != NULL);
  vectis_mutable_bytes_cleanup(&yaml);

  vectis_openapi_route_doc_cleanup(&doc);
  vectis_openapi_route_doc_cleanup(&get_doc);
  app->close(app);
}

static void assert_tls_source_surface(void) {
  vectis_app_config config;
  vectis_route_config route;
  vectis_error error;
  vectis_status status;
  vectis_app *app;
  lc_source *failing_source;
  failing_source_context failing_context;
  vectis_source failing_cert_source;
  vectis_source private_key_source;
  char *certificate_only_bundle;
  const char server_bundle[] = "-----BEGIN CERTIFICATE-----\n"
                               "server\n"
                               "-----END CERTIFICATE-----\n"
                               "-----BEGIN PRIVATE KEY-----\n"
                               "key\n"
                               "-----END PRIVATE KEY-----\n";
  const char client_ca[] = "-----BEGIN CERTIFICATE-----\n"
                           "ca\n"
                           "-----END CERTIFICATE-----\n";
  const char certificate_only[] = "-----BEGIN CERTIFICATE-----\n"
                                  "server\n"
                                  "-----END CERTIFICATE-----\n";
  const char private_key[] = "-----BEGIN PRIVATE KEY-----\n"
                             "key\n"
                             "-----END PRIVATE KEY-----\n";

  failing_source = NULL;
  memset(&failing_context, 0, sizeof(failing_context));
  assert(lc_source_from_callbacks(failing_source_read, failing_source_reset,
                                  NULL, &failing_context, &failing_source,
                                  NULL) == LC_OK);
  failing_cert_source = vectis_source_from_lc(failing_source);
  private_key_source =
      vectis_source_from_memory(private_key, sizeof(private_key) - 1u);
  status = vectis_cert_validate_pair(&failing_cert_source, &private_key_source,
                                     NULL, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "source failed") != NULL);
  lc_source_close(failing_source);
  failing_source = NULL;

  vectis_app_config_init(&config);
  config.tls.cert_key_bundle =
      vectis_source_from_memory(server_bundle, sizeof(server_bundle) - 1u);
  config.lockd.unix_socket_path = "/tmp/lockd.sock";
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  status = app->start(app, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(error.source == VECTIS_ERROR_SOURCE_LOCKDC);
  assert(vectis_lockd_client(app) == NULL);
  app->close(app);

  certificate_only_bundle = malloc(sizeof(certificate_only) - 1u);
  assert(certificate_only_bundle != NULL);
  memcpy(certificate_only_bundle, certificate_only,
         sizeof(certificate_only) - 1u);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_MANUAL;
  config.tls.bind = "127.0.0.1";
  config.tls.port = 28555u;
  config.tls.cert_key_bundle = vectis_source_from_memory(
      certificate_only_bundle, sizeof(certificate_only) - 1u);
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/tls", sample_route_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "private key") != NULL);
  app->close(app);
  free(certificate_only_bundle);

  vectis_app_config_init(&config);
  config.tls.cert_key_bundle =
      vectis_source_from_memory(server_bundle, sizeof(server_bundle) - 1u);
  config.tls.require_client_certificate = 1;
  config.lockd.unix_socket_path = "/tmp/lockd.sock";
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/tls-client-ca", sample_route_handler,
                       NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "client_ca_bundle") != NULL);
  app->close(app);

  vectis_app_config_init(&config);
  config.tls.cert_key_bundle =
      vectis_source_from_memory(server_bundle, sizeof(server_bundle) - 1u);
  config.tls.client_ca_bundle =
      vectis_source_from_memory(client_ca, sizeof(client_ca) - 1u);
  config.tls.require_client_certificate = 1;
  config.lockd.unix_socket_path = "/tmp/lockd.sock";
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  status = app->start(app, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(error.source == VECTIS_ERROR_SOURCE_LOCKDC);
  app->close(app);
}

static void assert_consumer_service_surface(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  lc_consumer_config consumer;
  lc_consumer_service_config service_config;
  vectis_consumer_service *service;
  vectis_consumer_receiver_adapter adapter;
  vectis_consumer_service_receiver_config receiver_config;
  vectis_webdav_marker_receiver_config marker_config;

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
  assert(vectis_consumer_service_native(NULL) == NULL);
  vectis_consumer_service_receiver_config_init(&receiver_config);
  assert(receiver_config.visibility_timeout_seconds == 30L);
  assert(receiver_config.wait_seconds == 1L);
  assert(receiver_config.worker_count == 1u);
  vectis_webdav_marker_receiver_config_init(&marker_config);
  assert(strcmp(marker_config.site_id, "consumer") == 0);
  assert(strcmp(marker_config.processing_path, "/consumer-processing.txt") ==
         0);
  assert(strcmp(marker_config.done_path, "/consumer-done.txt") == 0);
  assert(marker_config.max_file_bytes > 0u);

  lc_consumer_config_init(&consumer);
  lc_consumer_service_config_init(&service_config);
  consumer.name = "orders-worker";
  consumer.request.queue = "orders";
  consumer.handle = sample_consumer_handler;
  service_config.consumers = &consumer;
  service_config.consumer_count = 1u;

  vectis_app_config_init(&config);
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  assert(app->register_consumer_receiver != NULL);
  assert(app->consumer_service_receiver != NULL);
  memset(&adapter, 0, sizeof(adapter));
  adapter.kind = "test_receiver";
  adapter.create = sample_consumer_receiver_create;
  status = app->register_consumer_receiver(app, &adapter, &error);
  assert(status == VECTIS_OK);
  status = app->register_consumer_receiver(app, &adapter, &error);
  assert(status == VECTIS_ERR_INVALID);

  vectis_consumer_service_receiver_config_init(&receiver_config);
  receiver_config.queue = "orders";
  receiver_config.receiver_kind = "missing_receiver";
  status =
      app->consumer_service_receiver(app, &receiver_config, &service, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "receiver_kind") != NULL);
  assert(service == NULL);

  vectis_consumer_service_receiver_config_init(&receiver_config);
  receiver_config.queue = "orders";
  receiver_config.receiver_kind = "webdav_marker";
  status =
      app->consumer_service_receiver(app, &receiver_config, &service, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "webdav_marker") != NULL);
  assert(service == NULL);

  vectis_consumer_service_receiver_config_init(&receiver_config);
  receiver_config.queue = "orders";
  receiver_config.receiver_kind = "test_receiver";
  status =
      app->consumer_service_receiver(app, &receiver_config, &service, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "lockd") != NULL);
  assert(service == NULL);

  status = app->consumer_service(app, &service_config, &service, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "lockd") != NULL);
  assert(service == NULL);
  app->close(app);

  vectis_app_config_init(&config);
  config.lockd.unix_socket_path = "/tmp/vectis-missing-lockd.sock";
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  status = app->consumer_service(app, &service_config, &service, &error);
  assert(status == VECTIS_OK);
  assert(service != NULL);
  assert(vectis_consumer_service_native(service) == NULL);
  assert(service->native(service) == NULL);
  status = service->start(service, &error);
  assert(status == VECTIS_OK);
  assert(vectis_consumer_service_native(service) == NULL);
  status = app->start(app, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(error.source == VECTIS_ERROR_SOURCE_LOCKDC);
  service->close(service);
  app->close(app);
}

static void assert_cai_worker_surface(void) {
  vectis_app_config app_config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  vectis_mailbox_config mailbox_config;
  vectis_mailbox *mailbox;
  vectis_cai_worker_service_config worker_config;
  vectis_managed_service *service;
  vectis_managed_service_state service_state;
  vectis_cai_worker_request request;
  vectis_cai_worker_response response;

  service = NULL;
  status = vectis_cai_worker_service_new(NULL, NULL, &service, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(service == NULL);

  vectis_cai_worker_service_config_init(&worker_config);
  assert(worker_config.start_with_app == 1);
  assert(worker_config.poll_timeout_ms ==
         VECTIS_CAI_WORKER_DEFAULT_POLL_TIMEOUT_MS);

  vectis_cai_worker_request_init(&request);
  assert(request.output_mode == VECTIS_CAI_WORKER_OUTPUT_TEXT);
  assert(request.max_response_bytes ==
         VECTIS_CAI_WORKER_DEFAULT_MAX_RESPONSE_BYTES);
  status = vectis_cai_worker_event_build(&request, NULL, &error);
  assert(status == VECTIS_ERR_INVALID);

  vectis_cai_worker_response_init(&response);
  assert(response.size == sizeof(response));
  assert(response.abi_version == VECTIS_SERVICE_ABI_VERSION);

  vectis_app_config_init(&app_config);
  app = vectis_app_new(&app_config, &error);
  assert(app != NULL);
  assert(app->cai_worker_service != NULL);
  status = app->cai_worker_service(app, &worker_config, &service, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(service == NULL);

  vectis_mailbox_config_init(&mailbox_config);
  mailbox = NULL;
  assert(vectis_mailbox_new(&mailbox_config, &mailbox, &error) == VECTIS_OK);
  worker_config.request_mailbox = mailbox;
  status = app->cai_worker_service(app, &worker_config, &service, &error);
  assert(status == VECTIS_OK);
  assert(service != NULL);
  assert(service->state(service, &service_state, &error) == VECTIS_OK);
  assert(service_state.declared);
  assert(service_state.start_requested);
  assert(!service_state.materialized);
  service->close(service);
  app->close(app);
  mailbox->destroy(mailbox);
}

static void assert_dsv_surface(void) {
  const char csv[] =
      "id,count,active\r\nalpha,2,true\r\n\"beta,quoted\",3,false\r\n";
  const char commented_csv[] = "  # export metadata\n"
                               "id,count,active\n"
                               "epsilon,23,true\n"
                               "# skipped row,999,false\n"
                               "\"#literal\",29,false\n";
  const char csv_with_alt_header[] =
      "external_id,total,enabled\nnamed,17,true\n";
  const char headerless_csv[] = "gamma,13,true\n";
  const char headerless_commented_csv[] = "# row-only export\n"
                                          "zeta,31,true\n"
                                          "  # skipped row,999,false\n"
                                          "eta,37,false\n";
  const char tsv[] = "id\tcount\tactive\none\t7\ttrue\n";
  const char headerless_tsv[] = "delta\t19\tfalse\n";
  const char short_row_csv[] = "id,count,active\nbad,1\n";
  const char oversized_field_csv[] = "id,count,active\nabc,1,true\n";
  const char unterminated_csv[] = "id,count,active\n\"unterminated,1,true\n";
  const char *columns[] = {"id", "count", "active"};
  const char *reordered_columns[] = {"active", "id"};
  sample_dsv_doc out_rows[2] = {{"alpha,quoted", 2, 1}, {" #comment", 3, 0}};
  vectis_dsv_config config;
  vectis_body_spill_config spill_config;
  vectis_body_spill_result spill_result;
  vectis_source dsv_source;
  vectis_mutable_bytes json;
  sample_dsv_rows rows;
  sample_dsv_view_rows view_rows;
  sample_dsv_doc item;
  lonejson_schema_view schema_view;
  lonejson *json_runtime;
  lonejson_array_rewrite_options rewrite_options;
  vectis_request *request;
  lc_sink *sink;
  lc_sink failing_sink;
  const void *sink_bytes;
  size_t sink_size;
  lc_source *source;
  FILE *fp;
  char spill_bytes[256];
  vectis_error error;
  vectis_status status;

  memset(&rows, 0, sizeof(rows));
  config = vectis_dsv_csv();
  vectis_source_init(&dsv_source);
  status = vectis_dsv_parse_lonejson_source(
      &dsv_source, &sample_dsv_doc_map, &config, sample_dsv_row, &rows, &error);
  assert(status == VECTIS_ERR_INVALID);
  vectis_error_clear(&error);

  dsv_source = vectis_source_from_memory(csv, sizeof(csv) - 1u);
  status = vectis_dsv_parse_lonejson_source(
      &dsv_source, &sample_dsv_doc_map, &config, sample_dsv_row, &rows, &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 2u);
  assert(rows.total == 5);
  assert(rows.active_count == 1);
  assert(strcmp(rows.last_id, "beta,quoted") == 0);

  memset(&view_rows, 0, sizeof(view_rows));
  memset(&schema_view, 0, sizeof(schema_view));
  json_runtime = lonejson_new(NULL, NULL);
  assert(json_runtime != NULL);
  schema_view.size = sizeof(schema_view);
  schema_view.abi_version = LONEJSON_VIEW_ABI_VERSION;
  schema_view.runtime = json_runtime;
  schema_view.map = &sample_dsv_doc_map;
  schema_view.record_size = sizeof(sample_dsv_doc);
  dsv_source = vectis_source_from_memory(csv, sizeof(csv) - 1u);
  status = vectis_dsv_parse_lonejson_view_source(
      &dsv_source, &schema_view, &config, sample_dsv_view_storage,
      sample_dsv_view_row, &view_rows, &error);
  assert(status == VECTIS_OK);
  assert(view_rows.rows.count == 2u);
  assert(view_rows.rows.total == 5);
  assert(view_rows.rows.active_count == 1);
  assert(strcmp(view_rows.rows.last_id, "beta,quoted") == 0);
  lonejson_free(json_runtime);

  dsv_source =
      vectis_source_from_memory(commented_csv, sizeof(commented_csv) - 1u);
  config = vectis_dsv_csv();
  config.comment_prefix = "#";
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson_source(
      &dsv_source, &sample_dsv_doc_map, &config, sample_dsv_row, &rows, &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 2u);
  assert(rows.total == 52);
  assert(rows.active_count == 1);
  assert(strcmp(rows.last_id, "#literal") == 0);

  dsv_source = vectis_source_from_memory(csv_with_alt_header,
                                         sizeof(csv_with_alt_header) - 1u);
  config = vectis_dsv_csv();
  config.columns = columns;
  config.column_count = 3u;
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson_source(
      &dsv_source, &sample_dsv_doc_map, &config, sample_dsv_row, &rows, &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 1u);
  assert(rows.total == 17);
  assert(rows.active_count == 1);
  assert(strcmp(rows.last_id, "named") == 0);

  memset(&json, 0, sizeof(json));
  dsv_source = vectis_source_from_memory(csv, sizeof(csv) - 1u);
  status = vectis_dsv_source_to_json_array(&dsv_source, &config, &json, &error);
  assert(status == VECTIS_OK);
  assert(json.data != NULL);
  assert(strstr((const char *)json.data, "\"id\":\"alpha\"") != NULL);
  assert(strstr((const char *)json.data, "\"id\":\"beta,quoted\"") != NULL);
  assert(strstr((const char *)json.data, "\"count\":\"3\"") != NULL);
  vectis_mutable_bytes_cleanup(&json);

  memset(&json, 0, sizeof(json));
  dsv_source = vectis_source_from_memory(csv, sizeof(csv) - 1u);
  status = vectis_dsv_source_to_lonejson_array(&dsv_source, &sample_dsv_doc_map,
                                               &config, &json, &error);
  assert(status == VECTIS_OK);
  assert(json.data != NULL);
  assert(strstr((const char *)json.data, "\"id\":\"alpha\"") != NULL);
  assert(strstr((const char *)json.data, "\"count\":3") != NULL);
  assert(strstr((const char *)json.data, "\"active\":false") != NULL);

  dsv_source = vectis_source_from_memory(json.data, json.size);
  memset(&rows, 0, sizeof(rows));
  memset(&item, 0, sizeof(item));
  status =
      vectis_json_array_each_source(&dsv_source, "", &sample_dsv_doc_map, &item,
                                    sample_json_array_item, &rows, &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 2u);
  assert(rows.total == 5);
  assert(rows.active_count == 1);
  assert(strcmp(rows.last_id, "beta,quoted") == 0);

  request = vectis_internal_request_new(&error);
  assert(request != NULL);
  status =
      vectis_internal_request_set_body(request, json.data, json.size, &error);
  assert(status == VECTIS_OK);
  memset(&rows, 0, sizeof(rows));
  memset(&item, 0, sizeof(item));
  status =
      vectis_request_json_array_each(request, "", &sample_dsv_doc_map, &item,
                                     sample_json_array_item, &rows, &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 2u);
  assert(rows.total == 5);
  vectis_internal_request_free(request);

  dsv_source = vectis_source_from_memory(json.data, json.size);
  memset(&item, 0, sizeof(item));
  status = vectis_json_array_each_source(&dsv_source, "", NULL, &item,
                                         sample_json_array_item, &rows, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "map") != NULL);

  dsv_source = vectis_source_from_memory(json.data, json.size);
  status =
      vectis_json_array_each_source(&dsv_source, "", &sample_dsv_doc_map, NULL,
                                    sample_json_array_item, &rows, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "storage") != NULL);

  dsv_source = vectis_source_from_memory(json.data, json.size);
  memset(&item, 0, sizeof(item));
  status = vectis_json_array_each_source(&dsv_source, "", &sample_dsv_doc_map,
                                         &item, NULL, &rows, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "callback") != NULL);

  dsv_source = vectis_source_from_memory(json.data, json.size);
  memset(&rows, 0, sizeof(rows));
  memset(&item, 0, sizeof(item));
  status = vectis_json_array_each_source(
      &dsv_source, "", &sample_dsv_doc_map, &item,
      sample_json_array_fail_on_second, &rows, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "array callback stopped") != NULL);
  assert(rows.count == 2u);

  dsv_source = vectis_source_from_memory(
      "{\"items\":[{\"id\":\"broken\",\"count\":2,\"active\":true}",
      sizeof("{\"items\":[{\"id\":\"broken\",\"count\":2,\"active\":true}") -
          1u);
  memset(&rows, 0, sizeof(rows));
  memset(&item, 0, sizeof(item));
  status = vectis_json_array_each_source(&dsv_source, "items",
                                         &sample_dsv_doc_map, &item,
                                         sample_json_array_item, &rows, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(error.source == VECTIS_ERROR_SOURCE_LONEJSON);
  assert(rows.count == 0u);

  dsv_source = vectis_source_from_memory(json.data, json.size);
  status = vectis_json_array_rewrite_source(&dsv_source, "", NULL,
                                            &rewrite_options, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "sink") != NULL);

  assert(lc_sink_to_memory(&sink, NULL) == LC_OK);
  dsv_source = vectis_source_from_memory(json.data, json.size);
  status =
      vectis_json_array_rewrite_source(&dsv_source, "", sink, NULL, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "options") != NULL);
  lc_sink_close(sink);

  assert(lc_sink_to_memory(&sink, NULL) == LC_OK);
  memset(&rows, 0, sizeof(rows));
  memset(&item, 0, sizeof(item));
  memset(&rewrite_options, 0, sizeof(rewrite_options));
  rewrite_options.item_map = &sample_dsv_doc_map;
  rewrite_options.item_dst = &item;
  rewrite_options.item = sample_json_array_rewrite_item;
  rewrite_options.user = &rows;
  dsv_source = vectis_source_from_memory(json.data, json.size);
  status = vectis_json_array_rewrite_source(&dsv_source, "", sink,
                                            &rewrite_options, &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 2u);
  assert(rows.total == 5);
  assert(rows.active_count == 1);
  assert(lc_sink_memory_bytes(sink, &sink_bytes, &sink_size, NULL) == LC_OK);
  assert(sink_size > 0u);
  assert(sink_size < sizeof(spill_bytes));
  memset(spill_bytes, 0, sizeof(spill_bytes));
  memcpy(spill_bytes, sink_bytes, sink_size);
  assert(strstr(spill_bytes, "\"id\":\"alpha\"") != NULL);
  assert(strstr(spill_bytes, "\"id\":\"beta,quoted\"") == NULL);
  lc_sink_close(sink);

  assert(lc_sink_to_memory(&sink, NULL) == LC_OK);
  memset(&item, 0, sizeof(item));
  memset(&rewrite_options, 0, sizeof(rewrite_options));
  rewrite_options.item_map = &sample_dsv_doc_map;
  rewrite_options.item_dst = &item;
  rewrite_options.item = sample_json_array_rewrite_fail;
  dsv_source = vectis_source_from_memory(json.data, json.size);
  status = vectis_json_array_rewrite_source(&dsv_source, "", sink,
                                            &rewrite_options, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(error.source == VECTIS_ERROR_SOURCE_LONEJSON);
  assert(strstr(error.message, "rewrite callback stopped") != NULL);
  lc_sink_close(sink);

  memset(&failing_sink, 0, sizeof(failing_sink));
  failing_sink.write = failing_sink_write;
  memset(&item, 0, sizeof(item));
  memset(&rewrite_options, 0, sizeof(rewrite_options));
  rewrite_options.item_map = &sample_dsv_doc_map;
  rewrite_options.item_dst = &item;
  rewrite_options.item = sample_json_array_rewrite_item;
  dsv_source = vectis_source_from_memory(json.data, json.size);
  status = vectis_json_array_rewrite_source(&dsv_source, "", &failing_sink,
                                            &rewrite_options, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(error.source == VECTIS_ERROR_SOURCE_LONEJSON);
  assert(strstr(error.message, "intentional sink failure") != NULL);

  vectis_mutable_bytes_cleanup(&json);

  memset(&rows, 0, sizeof(rows));
  assert(lc_source_from_memory(tsv, sizeof(tsv) - 1u, &source, NULL) == LC_OK);
  config = vectis_dsv_tsv();
  status = vectis_dsv_parse_lonejson(source, &sample_dsv_doc_map, &config,
                                     sample_dsv_row, &rows, &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 1u);
  assert(rows.total == 7);
  assert(rows.active_count == 1);
  lc_source_close(source);

  assert(lc_source_from_memory("two|11|false\n", 13u, &source, NULL) == LC_OK);
  vectis_dsv_config_init(&config);
  config.delimiter = '|';
  config.header_disabled = 1;
  config.columns = columns;
  config.column_count = 3u;
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson(source, &sample_dsv_doc_map, &config,
                                     sample_dsv_row, &rows, &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 1u);
  assert(rows.total == 11);
  assert(rows.active_count == 0);
  lc_source_close(source);

  assert(lc_source_from_memory(headerless_csv, sizeof(headerless_csv) - 1u,
                               &source, NULL) == LC_OK);
  config = vectis_dsv_csv_rows();
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson(source, &sample_dsv_doc_map, &config,
                                     sample_dsv_row, &rows, &error);
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
  status = vectis_dsv_parse_lonejson_source(
      &dsv_source, &sample_dsv_doc_map, &config, sample_dsv_row, &rows, &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 2u);
  assert(rows.total == 68);
  assert(rows.active_count == 1);
  assert(strcmp(rows.last_id, "eta") == 0);

  assert(lc_source_from_memory(headerless_tsv, sizeof(headerless_tsv) - 1u,
                               &source, NULL) == LC_OK);
  config = vectis_dsv_tsv_rows();
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson(source, &sample_dsv_doc_map, &config,
                                     sample_dsv_row, &rows, &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 1u);
  assert(rows.total == 19);
  assert(rows.active_count == 0);
  assert(strcmp(rows.last_id, "delta") == 0);
  lc_source_close(source);

  memset(&json, 0, sizeof(json));
  config = vectis_dsv_csv_rows();
  dsv_source =
      vectis_source_from_memory(headerless_csv, sizeof(headerless_csv) - 1u);
  status = vectis_dsv_source_to_lonejson_array(&dsv_source, &sample_dsv_doc_map,
                                               &config, &json, &error);
  assert(status == VECTIS_OK);
  assert(strstr((const char *)json.data, "\"id\":\"gamma\"") != NULL);
  assert(strstr((const char *)json.data, "\"count\":13") != NULL);
  assert(strstr((const char *)json.data, "\"active\":true") != NULL);
  vectis_mutable_bytes_cleanup(&json);

  vectis_body_spill_config_init(&spill_config);
  spill_config.memory_limit_bytes = 16u;
  spill_config.prefix = "vectis-dsv-json";
  memset(&spill_result, 0, sizeof(spill_result));
  dsv_source =
      vectis_source_from_memory(headerless_csv, sizeof(headerless_csv) - 1u);
  status = vectis_dsv_source_to_lonejson_array_spill(
      &dsv_source, &sample_dsv_doc_map, &config, &spill_config, &spill_result,
      &error);
  assert(status == VECTIS_OK);
  assert(spill_result.spooled_to_disk);
  assert(spill_result.path != NULL);
  assert(spill_result.size > 16u);
  fp = fopen(spill_result.path, "rb");
  assert(fp != NULL);
  memset(spill_bytes, 0, sizeof(spill_bytes));
  assert(fread(spill_bytes, 1u, sizeof(spill_bytes) - 1u, fp) > 0u);
  assert(fclose(fp) == 0);
  assert(strstr(spill_bytes, "\"id\":\"gamma\"") != NULL);
  assert(remove(spill_result.path) == 0);
  vectis_body_spill_result_cleanup(&spill_result);

  vectis_body_spill_config_init(&spill_config);
  spill_config.memory_limit_bytes = 4096u;
  memset(&spill_result, 0, sizeof(spill_result));
  dsv_source =
      vectis_source_from_memory(headerless_csv, sizeof(headerless_csv) - 1u);
  status = vectis_dsv_source_to_lonejson_array_spill(
      &dsv_source, &sample_dsv_doc_map, &config, &spill_config, &spill_result,
      &error);
  assert(status == VECTIS_OK);
  assert(!spill_result.spooled_to_disk);
  assert(spill_result.memory.data != NULL);
  assert(spill_result.memory.size < sizeof(spill_bytes));
  memset(spill_bytes, 0, sizeof(spill_bytes));
  memcpy(spill_bytes, spill_result.memory.data, spill_result.memory.size);
  assert(strstr(spill_bytes, "\"id\":\"gamma\"") != NULL);
  vectis_body_spill_result_cleanup(&spill_result);

  memset(&json, 0, sizeof(json));
  dsv_source =
      vectis_source_from_memory(headerless_csv, sizeof(headerless_csv) - 1u);
  status = vectis_dsv_source_to_json_array(&dsv_source, &config, &json, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "columns") != NULL);
  vectis_error_clear(&error);

  assert(lc_source_from_memory(short_row_csv, sizeof(short_row_csv) - 1u,
                               &source, NULL) == LC_OK);
  config = vectis_dsv_csv();
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson(source, &sample_dsv_doc_map, &config,
                                     sample_dsv_row, &rows, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "row width") != NULL);
  lc_source_close(source);
  vectis_error_clear(&error);

  assert(lc_source_from_memory(oversized_field_csv,
                               sizeof(oversized_field_csv) - 1u, &source,
                               NULL) == LC_OK);
  config = vectis_dsv_csv();
  config.max_field_bytes = 2u;
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson(source, &sample_dsv_doc_map, &config,
                                     sample_dsv_row, &rows, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "max_field_bytes") != NULL);
  lc_source_close(source);
  vectis_error_clear(&error);

  assert(lc_source_from_memory(unterminated_csv, sizeof(unterminated_csv) - 1u,
                               &source, NULL) == LC_OK);
  config = vectis_dsv_csv();
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson(source, &sample_dsv_doc_map, &config,
                                     sample_dsv_row, &rows, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "unterminated") != NULL);
  lc_source_close(source);
  vectis_error_clear(&error);

  memset(&json, 0, sizeof(json));
  config = vectis_dsv_csv();
  config.comment_prefix = "#";
  status = vectis_dsv_lonejson_rows_to_bytes(&sample_dsv_doc_map, &config,
                                             out_rows, 2u, 0u, &json, &error);
  assert(status == VECTIS_OK);
  assert(strcmp((const char *)json.data, "id,count,active\n"
                                         "\"alpha,quoted\",2,true\n"
                                         "\" #comment\",3,false\n") == 0);
  dsv_source = vectis_source_from_memory(json.data, json.size);
  memset(&rows, 0, sizeof(rows));
  status = vectis_dsv_parse_lonejson_source(
      &dsv_source, &sample_dsv_doc_map, &config, sample_dsv_row, &rows, &error);
  assert(status == VECTIS_OK);
  assert(rows.count == 2u);
  assert(rows.total == 5);
  assert(rows.active_count == 1);
  assert(strcmp(rows.last_id, " #comment") == 0);
  vectis_mutable_bytes_cleanup(&json);

  memset(&json, 0, sizeof(json));
  config = vectis_dsv_tsv_rows();
  config.columns = reordered_columns;
  config.column_count = 2u;
  status = vectis_dsv_lonejson_rows_to_bytes(&sample_dsv_doc_map, &config,
                                             out_rows, 1u, 0u, &json, &error);
  assert(status == VECTIS_OK);
  assert(strcmp((const char *)json.data, "true\talpha,quoted\n") == 0);
  vectis_mutable_bytes_cleanup(&json);

  memset(&json, 0, sizeof(json));
  config = vectis_dsv_csv();
  status = vectis_dsv_lonejson_rows_to_bytes(&sample_xml_doc_map, &config, NULL,
                                             0u, 0u, &json, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "not a scalar") != NULL);
  vectis_error_clear(&error);
}

static void assert_xml_surface(void) {
  const char xml[] = "<invoice ignored=\"yes\">"
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
  sample_xml_blob_doc blob_doc;
  sample_xml_line *lines;
  char *large_xml;
  size_t large_body_size;

  config = vectis_xml_default();
  config.root_element = "invoice";
  memset(&doc, 0, sizeof(doc));
  xml_source = vectis_source_from_memory(xml, sizeof(xml) - 1u);
  status = vectis_xml_parse_lonejson_source(&xml_source, &sample_xml_doc_map,
                                            &config, &doc, &error);
  assert(status == VECTIS_OK);
  assert(strcmp(doc.id, " inv-001 ") == 0);
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
  status = vectis_xml_parse_lonejson_source(&xml_source, &sample_xml_doc_map,
                                            &config, &doc, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "root element") != NULL);
  vectis_error_clear(&error);

  xml_source = vectis_source_from_memory(duplicate_scalar,
                                         sizeof(duplicate_scalar) - 1u);
  status = vectis_xml_parse_lonejson_source(&xml_source, &sample_xml_doc_map,
                                            &config, &doc, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "duplicate") != NULL);
  vectis_error_clear(&error);

  xml_source = vectis_source_from_memory(non_contiguous_array,
                                         sizeof(non_contiguous_array) - 1u);
  status = vectis_xml_parse_lonejson_source(&xml_source, &sample_xml_doc_map,
                                            &config, &doc, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "non-contiguous") != NULL);
  vectis_error_clear(&error);

  xml_source = vectis_source_from_memory(xml, sizeof(xml) - 1u);
  config = vectis_xml_default();
  config.root_element = "invoice";
  config.skip_unknown_disabled = 1;
  status = vectis_xml_parse_lonejson_source(&xml_source, &sample_xml_doc_map,
                                            &config, &doc, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "unknown") != NULL);
  vectis_error_clear(&error);

  memset(&blob_doc, 0, sizeof(blob_doc));
  large_body_size = 2u * 1024u * 1024u;
  large_xml = (char *)malloc(large_body_size + 24u);
  assert(large_xml != NULL);
  memcpy(large_xml, "<doc><body>", 11u);
  memset(large_xml + 11u, 'x', large_body_size);
  memcpy(large_xml + 11u + large_body_size, "</body></doc>", 13u);
  config = vectis_xml_default();
  config.root_element = "doc";
  xml_source = vectis_source_from_memory(large_xml, large_body_size + 24u);
  status = vectis_xml_parse_lonejson_source(
      &xml_source, &sample_xml_blob_doc_map, &config, &blob_doc, &error);
  assert(status == VECTIS_OK);
  assert(lonejson_spooled_size(&blob_doc.body) == large_body_size);
  assert(lonejson_spooled_spilled(&blob_doc.body));
  lonejson_cleanup(&sample_xml_blob_doc_map, &blob_doc);
  config.trim_text = 1;
  status = vectis_xml_parse_lonejson_source(
      &xml_source, &sample_xml_blob_doc_map, &config, &blob_doc, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "trim_text=0") != NULL);
  vectis_error_clear(&error);
  free(large_xml);
}

int main(void) {
  assert_http_surface();
  assert_io_surface();
  assert_request_response_surface();
  assert_json_route_surface();
  assert_openapi_surface();
  assert_tls_source_surface();
  assert_consumer_service_surface();
  assert_cai_worker_surface();
  assert_dsv_surface();
  assert_xml_surface();
  return 0;
}
