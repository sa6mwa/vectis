#include "vectis_proxy_headers.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  vectis_proxy_headers inbound;
  vectis_proxy_headers outbound;
  vectis_proxy_request_head head;
  vectis_proxy_header_status status;
  char line[4096];
  size_t start;
  size_t end;
  size_t length;
  size_t i;

  vectis_proxy_headers_init(&inbound);
  vectis_proxy_headers_init(&outbound);
  if (vectis_proxy_headers_add(&inbound, "Host", "example.test") !=
      VECTIS_PROXY_HEADER_OK) {
    return 0;
  }
  start = 0u;
  while (start < size && inbound.count < VECTIS_PROXY_HEADER_COUNT_LIMIT) {
    end = start;
    while (end < size && data[end] != '\n') {
      end++;
    }
    length = end - start;
    if (length != 0u && length < sizeof(line)) {
      memcpy(line, data + start, length);
      line[length] = '\0';
      for (i = 0u; i < length && line[i] != ':'; ++i) {
      }
      if (i != 0u && i < length) {
        line[i++] = '\0';
        while (i < length && (line[i] == ' ' || line[i] == '\t')) {
          i++;
        }
        (void)vectis_proxy_headers_add(&inbound, line, line + i);
      }
    }
    start = end < size ? end + 1u : size;
  }
  status = vectis_proxy_request_head_parse(&inbound, &head, NULL);
  if (status == VECTIS_PROXY_HEADER_OK) {
    assert(head.host != NULL);
    assert(vectis_proxy_headers_sanitize_request(&inbound, &outbound) ==
           VECTIS_PROXY_HEADER_OK);
    assert(outbound.count <= inbound.count);
    assert(outbound.bytes <= inbound.bytes);
    (void)vectis_proxy_request_trailer_declared(&inbound, "X-Trace");
  }
  vectis_proxy_headers_cleanup(&outbound);
  vectis_proxy_headers_cleanup(&inbound);
  return 0;
}

#if defined(VECTIS_AFL_FUZZER)
int main(void) {
  uint8_t data[65536];
  size_t size;

  size = fread(data, 1u, sizeof(data), stdin);
  (void)LLVMFuzzerTestOneInput(data, size);
  return 0;
}
#endif
