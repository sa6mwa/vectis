#include "vectis_internal.h"
#include <assert.h>
#include <cai/cai.h>
#include <stdio.h>
#include <string.h>
#include <vectis/vectis.h>

static void expect_cai_source_bytes(cai_source *source, const char *expected) {
  cai_error error;
  char buffer[64];
  size_t nread;

  cai_error_init(&error);
  nread = cai_source_read(source, buffer, sizeof(buffer), &error);
  assert(nread == strlen(expected));
  assert(memcmp(buffer, expected, nread) == 0);
  assert(cai_source_read(source, buffer, sizeof(buffer), &error) == 0u);
  assert(cai_source_reset(source, &error) == CAI_OK);
  nread = cai_source_read(source, buffer, sizeof(buffer), &error);
  assert(nread == strlen(expected));
  assert(memcmp(buffer, expected, nread) == 0);
  cai_error_cleanup(&error);
}

static void test_source_from_memory(void) {
  vectis_source source;
  vectis_error error;
  cai_source *cai_source;
  const char payload[] = "vectis cai source";

  vectis_error_clear(&error);
  source = vectis_source_from_memory(payload, sizeof(payload) - 1u);
  cai_source = NULL;
  assert(vectis_cai_source_from_source(&source, &cai_source, &error) ==
         VECTIS_OK);
  assert(cai_source != NULL);
  expect_cai_source_bytes(cai_source, payload);
  cai_source_close(cai_source);
}

static void test_source_from_request(void) {
  vectis_request *request;
  vectis_error error;
  cai_source *source;
  const char payload[] = "request body";

  vectis_error_clear(&error);
  request = vectis_internal_request_new(&error);
  assert(request != NULL);
  assert(vectis_internal_request_set_body(request, payload,
                                          sizeof(payload) - 1u,
                                          &error) == VECTIS_OK);
  source = NULL;
  assert(vectis_cai_source_from_request(request, &source, &error) ==
         VECTIS_OK);
  assert(source != NULL);
  expect_cai_source_bytes(source, payload);
  cai_source_close(source);
  vectis_internal_request_free(request);
}

static void test_borrowed_client_contract(void) {
  vectis_app_config config;
  vectis_error error;
  vectis_app *app;
  cai_client borrowed;
  cai_client *client;

  memset(&borrowed, 0, sizeof(borrowed));
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.cai.client = &borrowed;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  client = NULL;
  assert(app->cai_client(app, &client, &error) == VECTIS_OK);
  assert(client == &borrowed);
  client = NULL;
  assert(vectis_app_cai_client(app, &client, &error) == VECTIS_OK);
  assert(client == &borrowed);
  vectis_destroy(app);
}

static void test_error_mapping(void) {
  vectis_error error;
  cai_error caierr;
  char message[] = "server rejected request";
  char detail[] = "quota exhausted";

  cai_error_init(&caierr);
  caierr.code = CAI_ERR_SERVER;
  caierr.http_status = 429;
  caierr.message = message;
  caierr.detail = detail;
  assert(vectis_cai_error(&error, &caierr, "fallback") == VECTIS_ERR_STATE);
  assert(error.source == VECTIS_ERROR_SOURCE_CAI);
  assert(error.dependency_code == CAI_ERR_SERVER);
  assert(error.http_status == 429);
  assert(strcmp(error.message, message) == 0);
  assert(strcmp(error.detail, detail) == 0);
  caierr.message = NULL;
  caierr.detail = NULL;
  caierr.code = CAI_ERR_LIMIT;
  assert(vectis_cai_error(&error, &caierr, "limit") == VECTIS_ERR_CONFLICT);
  assert(error.source == VECTIS_ERROR_SOURCE_CAI);
  assert(strcmp(error.message, "limit") == 0);
}

static void test_invalid_output_adapters(void) {
  vectis_error error;
  size_t written;

  assert(vectis_cai_output_file(NULL, "/tmp/vectis-cai-unit.out", &written,
                                &error) == VECTIS_ERR_INVALID);
  assert(error.source == VECTIS_ERROR_SOURCE_VECTIS);
  assert(vectis_cai_output_response(NULL, NULL, 200, NULL, &error) ==
         VECTIS_ERR_INVALID);
  assert(vectis_cai_output_enqueue(NULL, NULL, NULL, NULL, &error) ==
         VECTIS_ERR_INVALID);
}

int main(void) {
  vectis_cai_config config;

  vectis_cai_config_init(&config);
  assert(config.client == NULL);
  assert(config.logger == NULL);
  assert(config.logger_disabled == 0);
  assert(strcmp(vectis_error_source_string(VECTIS_ERROR_SOURCE_CAI), "cai") ==
         0);

  test_source_from_memory();
  test_source_from_request();
  test_borrowed_client_contract();
  test_error_mapping();
  test_invalid_output_adapters();

  return 0;
}
