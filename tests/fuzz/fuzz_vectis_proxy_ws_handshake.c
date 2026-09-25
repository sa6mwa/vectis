#include "vectis_proxy_ws_handshake.h"
#include "vectis_proxy_ws_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  vectis_proxy_headers request;
  vectis_proxy_headers response;
  vectis_proxy_headers parsed;
  vectis_proxy_ws_head_result head_result;
  const char *request_key;
  const char *request_version;
  const char *request_protocol;
  const char *request_extensions;
  const char *response_accept;
  const char *response_protocol;
  const char *response_extensions;
  char value[1025];
  unsigned selection;
  unsigned parsed_status;
  size_t parsed_length;

  if (size == 0u || size > sizeof(value))
    return 0;
  if (size > 1u && data[size - 1u] == '\n')
    --size;
  selection = (unsigned)(data[0] % 7u);
  memcpy(value, data + 1u, size - 1u);
  value[size - 1u] = '\0';
  request_key = selection == 0u ? value : "dGhlIHNhbXBsZSBub25jZQ==";
  request_version = selection == 1u ? value : "13";
  request_protocol = selection == 2u ? value : "chat, superchat";
  request_extensions = selection == 3u ? value : "permessage-deflate";
  response_accept = selection == 4u ? value : "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
  response_protocol = selection == 5u ? value : "chat";
  response_extensions = selection == 6u ? value : "permessage-deflate";

  vectis_proxy_headers_init(&request);
  vectis_proxy_headers_init(&response);
  (void)vectis_proxy_headers_add(&request, "Host", "example.test");
  (void)vectis_proxy_headers_add(&request, "Connection", "Upgrade");
  (void)vectis_proxy_headers_add(&request, "Upgrade", "websocket");
  (void)vectis_proxy_headers_add(&request, "Sec-WebSocket-Key", request_key);
  (void)vectis_proxy_headers_add(&request, "Sec-WebSocket-Version",
                                 request_version);
  (void)vectis_proxy_headers_add(&request, "Sec-WebSocket-Protocol",
                                 request_protocol);
  (void)vectis_proxy_headers_add(&request, "Sec-WebSocket-Extensions",
                                 request_extensions);
  (void)vectis_proxy_headers_add(&response, "Upgrade", "websocket");
  (void)vectis_proxy_headers_add(&response, "Connection", "Upgrade");
  (void)vectis_proxy_headers_add(&response, "Sec-WebSocket-Accept",
                                 response_accept);
  (void)vectis_proxy_headers_add(&response, "Sec-WebSocket-Protocol",
                                 response_protocol);
  (void)vectis_proxy_headers_add(&response, "Sec-WebSocket-Extensions",
                                 response_extensions);
  (void)vectis_proxy_ws_request_valid(VECTIS_HTTP_GET, &request, NULL);
  (void)vectis_proxy_ws_response_valid(&request, 101u, &response, NULL);
  vectis_proxy_headers_init(&parsed);
  head_result = vectis_proxy_ws_wire_response_head(
      data + 1u, size - 1u, &parsed_length, &parsed_status, &parsed, NULL);
  if (head_result == VECTIS_PROXY_WS_HEAD_COMPLETE && parsed_status == 101u)
    (void)vectis_proxy_ws_response_valid(&request, parsed_status, &parsed,
                                         NULL);
  vectis_proxy_headers_cleanup(&parsed);
  vectis_proxy_headers_cleanup(&response);
  vectis_proxy_headers_cleanup(&request);
  return 0;
}

#if defined(VECTIS_AFL_FUZZER)
int main(void) {
  uint8_t input[1025];
  size_t size;

  size = fread(input, 1u, sizeof(input), stdin);
  (void)LLVMFuzzerTestOneInput(input, size);
  return 0;
}
#endif
