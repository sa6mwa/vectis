#include "vectis_proxy_http.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  vectis_proxy_http_response response;
  vectis_proxy_headers outbound;
  vectis_proxy_http_event event;
  vectis_proxy_header_status status;
  size_t start;
  size_t end;

  if (size == 0u || size > 131072u) {
    return 0;
  }
  vectis_proxy_http_response_init(&response, (data[0] & 1u) != 0u);
  vectis_proxy_headers_init(&outbound);
  start = 1u;
  while (start < size) {
    end = start;
    while (end < size && data[end] != '\n') {
      end++;
    }
    if (end == size) {
      break;
    }
    status = vectis_proxy_http_response_header(
        &response, (const char *)data + start, end - start + 1u, &event, NULL);
    if (status != VECTIS_PROXY_HEADER_OK) {
      break;
    }
    if (event == VECTIS_PROXY_HTTP_FINAL) {
      status =
          vectis_proxy_headers_sanitize_response(&response.headers, &outbound);
      if (status == VECTIS_PROXY_HEADER_OK) {
        assert(outbound.count <= response.headers.count);
      }
      vectis_proxy_headers_cleanup(&outbound);
      (void)vectis_proxy_http_response_body(&response, size - end - 1u, NULL);
    }
    start = end + 1u;
  }
  if (response.phase == 2 || response.phase == 4) {
    assert(response.headers.count <= VECTIS_PROXY_HEADER_COUNT_LIMIT);
    (void)vectis_proxy_http_response_finish(&response, NULL);
  }
  vectis_proxy_http_response_cleanup(&response);
  vectis_proxy_headers_cleanup(&outbound);
  return 0;
}

#if defined(VECTIS_AFL_FUZZER)
int main(void) {
  uint8_t data[131072];
  size_t size;

  size = fread(data, 1u, sizeof(data), stdin);
  (void)LLVMFuzzerTestOneInput(data, size);
  return 0;
}
#endif
