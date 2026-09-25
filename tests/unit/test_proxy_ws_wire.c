#include "vectis_proxy_ws_wire.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void test_request(void) {
  vectis_proxy_headers fields;
  vectis_error error;
  char *oversized_target;
  char *wire;
  size_t length;

  vectis_proxy_headers_init(&fields);
  assert(vectis_proxy_headers_add(&fields, "Sec-WebSocket-Key",
                                  "dGhlIHNhbXBsZSBub25jZQ==") ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_headers_add(&fields, "Sec-WebSocket-Version", "13") ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_ws_wire_request("/chat?q=1&q=2", "example.test:8080",
                                      &fields, &wire, &length,
                                      &error) == VECTIS_OK);
  assert(length == strlen(wire));
  assert(strstr(wire, "GET /chat?q=1&q=2 HTTP/1.1\r\n") == wire);
  assert(strstr(wire, "Host: example.test:8080\r\n") != NULL);
  assert(strstr(wire, "Connection: Upgrade\r\nUpgrade: websocket\r\n") != NULL);
  assert(strstr(wire, "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") !=
         NULL);
  assert(length >= 4u && memcmp(wire + length - 4u, "\r\n\r\n", 4u) == 0);
  free(wire);
  assert(vectis_proxy_headers_add(&fields, "Connection", "close") ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_ws_wire_request("/chat", "example.test", &fields, &wire,
                                      &length, &error) == VECTIS_ERR_INVALID);
  assert(wire == NULL && length == 0u);
  assert(vectis_proxy_ws_wire_request("/chat\r\nInjected: yes", "example.test",
                                      &fields, &wire, &length,
                                      &error) == VECTIS_ERR_INVALID);
  vectis_proxy_headers_cleanup(&fields);

  oversized_target = (char *)malloc(131074u);
  assert(oversized_target != NULL);
  memset(oversized_target, 'a', 131073u);
  oversized_target[0] = '/';
  oversized_target[131073u] = '\0';
  vectis_proxy_headers_init(&fields);
  assert(vectis_proxy_ws_wire_request(oversized_target, "example.test", &fields,
                                      &wire, &length,
                                      &error) == VECTIS_ERR_INVALID);
  assert(wire == NULL && length == 0u);
  vectis_proxy_headers_cleanup(&fields);
  free(oversized_target);
}

static void test_response(void) {
  static const unsigned char accepted[] =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"
      "\201\004pong";
  static const unsigned char bad_status[] =
      "HTTP/2 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n";
  static const unsigned char bad_control[] =
      "HTTP/1.1 101 Switching Protocols\r\nX-A: yes\000no\r\n\r\n";
  vectis_proxy_headers fields;
  vectis_proxy_ws_head_result result;
  const char *reason;
  size_t head_length;
  size_t expected_head;
  size_t i;
  unsigned status;

  expected_head = (size_t)(strstr((const char *)accepted, "\r\n\r\n") -
                           (const char *)accepted) +
                  4u;
  for (i = 0u; i < expected_head; ++i) {
    vectis_proxy_headers_init(&fields);
    result = vectis_proxy_ws_wire_response_head(accepted, i, &head_length,
                                                &status, &fields, &reason);
    assert(result == VECTIS_PROXY_WS_HEAD_MORE);
    assert(fields.count == 0u);
  }
  vectis_proxy_headers_init(&fields);
  result = vectis_proxy_ws_wire_response_head(
      accepted, sizeof(accepted) - 1u, &head_length, &status, &fields, &reason);
  assert(result == VECTIS_PROXY_WS_HEAD_COMPLETE);
  assert(status == 101u && fields.count == 3u);
  assert(head_length < sizeof(accepted) - 1u);
  assert(memcmp(accepted + head_length, "\201\004pong", 6u) == 0);
  vectis_proxy_headers_cleanup(&fields);

  vectis_proxy_headers_init(&fields);
  result = vectis_proxy_ws_wire_response_head(
      bad_status, sizeof(bad_status) - 1u, &head_length, &status, &fields,
      &reason);
  assert(result == VECTIS_PROXY_WS_HEAD_INVALID && reason != NULL);
  assert(fields.count == 0u);

  vectis_proxy_headers_init(&fields);
  result = vectis_proxy_ws_wire_response_head(
      bad_control, sizeof(bad_control) - 1u, &head_length, &status, &fields,
      &reason);
  assert(result == VECTIS_PROXY_WS_HEAD_INVALID && reason != NULL);
  assert(fields.count == 0u);
}

static void test_head_limits(void) {
  unsigned char *oversized;
  vectis_proxy_headers fields;
  vectis_proxy_ws_head_result result;
  const char *reason;
  size_t head_length;
  unsigned status;

  oversized = (unsigned char *)malloc(VECTIS_PROXY_HEADER_BLOCK_LIMIT);
  assert(oversized != NULL);
  memset(oversized, 'X', VECTIS_PROXY_HEADER_BLOCK_LIMIT);
  vectis_proxy_headers_init(&fields);
  result = vectis_proxy_ws_wire_response_head(
      oversized, VECTIS_PROXY_HEADER_BLOCK_LIMIT, &head_length, &status,
      &fields, &reason);
  assert(result == VECTIS_PROXY_WS_HEAD_LIMIT && reason != NULL);
  assert(fields.count == 0u && head_length == 0u);
  free(oversized);
}

int main(void) {
  test_request();
  test_response();
  test_head_limits();
  return 0;
}
