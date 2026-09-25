#include "vectis_proxy_telemetry.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

typedef struct log_capture {
  char data[4096];
  size_t length;
} log_capture;

static int capture_write(void *userdata, const char *data, size_t length,
                         size_t *written) {
  log_capture *capture;

  capture = (log_capture *)userdata;
  assert(length <= sizeof(capture->data) - 1u - capture->length);
  memcpy(capture->data + capture->length, data, length);
  capture->length += length;
  capture->data[capture->length] = '\0';
  if (written != NULL)
    *written = length;
  return 0;
}

int main(void) {
  vectis_proxy_telemetry record;
  vectis_proxy_telemetry second;
  pslog_config config;
  pslog_logger *logger;
  log_capture capture;

  memset(&capture, 0, sizeof(capture));
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.output.write = capture_write;
  config.output.close = NULL;
  config.output.isatty = NULL;
  config.output.userdata = &capture;
  config.output.owned = 0;
  logger = pslog_new(&config);
  assert(logger != NULL);

  vectis_proxy_telemetry_start(&record, "/items", "https://origin.test", "http",
                               100u);
  vectis_proxy_telemetry_start(&second, "/items", "https://origin.test", "http",
                               101u);
  assert(record.request_id != 0u);
  assert(second.request_id != record.request_id);
  record.status = 502;
  record.upstream_bytes = 17u;
  record.downstream_bytes = 5u;
  record.disconnect_side = "upstream";
  record.error_category = "connect";
  vectis_proxy_telemetry_emit(&record, logger, 125u);
  assert(strstr(capture.data, "\"msg\":\"vectis.proxy.exchange\"") != NULL);
  assert(strstr(capture.data, "\"request_id\":") != NULL);
  assert(strstr(capture.data, "\"route\":\"/items\"") != NULL);
  assert(strstr(capture.data, "\"upstream\":\"https://origin.test\"") != NULL);
  assert(strstr(capture.data, "\"kind\":\"http\"") != NULL);
  assert(strstr(capture.data, "\"status\":502") != NULL);
  assert(strstr(capture.data, "\"upstream_bytes\":17") != NULL);
  assert(strstr(capture.data, "\"downstream_bytes\":5") != NULL);
  assert(strstr(capture.data, "\"duration_ms\":25") != NULL);
  assert(strstr(capture.data, "\"disconnect_side\":\"upstream\"") != NULL);
  assert(strstr(capture.data, "\"error_category\":\"connect\"") != NULL);
  logger->destroy(logger);
  return 0;
}
