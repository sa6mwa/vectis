#include "vectis_proxy_ws_handshake.h"

#include <assert.h>
#include <string.h>

static void add(vectis_proxy_headers *headers, const char *name,
                const char *value) {
  assert(vectis_proxy_headers_add(headers, name, value) ==
         VECTIS_PROXY_HEADER_OK);
}

static void request(vectis_proxy_headers *headers) {
  vectis_proxy_headers_init(headers);
  add(headers, "Host", "example.test");
  add(headers, "Connection", "keep-alive, Upgrade");
  add(headers, "Upgrade", "websocket");
  add(headers, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
  add(headers, "Sec-WebSocket-Version", "13");
  add(headers, "Sec-WebSocket-Protocol", "chat, superchat");
  add(headers, "Sec-WebSocket-Extensions", "permessage-deflate");
}

static void response(vectis_proxy_headers *headers) {
  vectis_proxy_headers_init(headers);
  add(headers, "Upgrade", "websocket");
  add(headers, "Connection", "Upgrade");
  add(headers, "Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  add(headers, "Sec-WebSocket-Protocol", "chat");
  add(headers, "Sec-WebSocket-Extensions",
      "permessage-deflate; server_no_context_takeover");
}

static void test_valid(void) {
  vectis_proxy_headers inbound;
  vectis_proxy_headers upstream;
  const char *reason;

  request(&inbound);
  response(&upstream);
  reason = "not reset";
  assert(vectis_proxy_ws_request_valid(VECTIS_HTTP_GET, &inbound, &reason));
  assert(reason == NULL);
  assert(vectis_proxy_ws_response_valid(&inbound, 101u, &upstream, &reason));
  assert(reason == NULL);
  vectis_proxy_headers_cleanup(&upstream);
  vectis_proxy_headers_cleanup(&inbound);
}

static void test_request_rejections(void) {
  vectis_proxy_headers inbound;
  const char *reason;

  request(&inbound);
  assert(!vectis_proxy_ws_request_valid(VECTIS_HTTP_POST, &inbound, &reason));
  assert(reason != NULL);
  add(&inbound, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
  assert(!vectis_proxy_ws_request_valid(VECTIS_HTTP_GET, &inbound, &reason));
  vectis_proxy_headers_cleanup(&inbound);

  request(&inbound);
  add(&inbound, "Content-Length", "0");
  assert(!vectis_proxy_ws_request_valid(VECTIS_HTTP_GET, &inbound, &reason));
  vectis_proxy_headers_cleanup(&inbound);

  request(&inbound);
  add(&inbound, "Sec-WebSocket-Version", "12");
  assert(!vectis_proxy_ws_request_valid(VECTIS_HTTP_GET, &inbound, &reason));
  vectis_proxy_headers_cleanup(&inbound);

  request(&inbound);
  add(&inbound, "Sec-WebSocket-Protocol", "chat,,other");
  assert(!vectis_proxy_ws_request_valid(VECTIS_HTTP_GET, &inbound, &reason));
  vectis_proxy_headers_cleanup(&inbound);

  request(&inbound);
  add(&inbound, "Sec-WebSocket-Accept", "injected");
  assert(!vectis_proxy_ws_request_valid(VECTIS_HTTP_GET, &inbound, &reason));
  vectis_proxy_headers_cleanup(&inbound);
}

static void test_response_rejections(void) {
  vectis_proxy_headers inbound;
  vectis_proxy_headers upstream;
  const char *reason;

  request(&inbound);
  response(&upstream);
  assert(!vectis_proxy_ws_response_valid(&inbound, 403u, &upstream, &reason));
  vectis_proxy_headers_cleanup(&upstream);

  response(&upstream);
  add(&upstream, "Sec-WebSocket-Accept", "wrong");
  assert(!vectis_proxy_ws_response_valid(&inbound, 101u, &upstream, &reason));
  vectis_proxy_headers_cleanup(&upstream);

  response(&upstream);
  add(&upstream, "Content-Length", "0");
  assert(!vectis_proxy_ws_response_valid(&inbound, 101u, &upstream, &reason));
  vectis_proxy_headers_cleanup(&upstream);

  response(&upstream);
  add(&upstream, "Sec-WebSocket-Protocol", "unoffered");
  assert(!vectis_proxy_ws_response_valid(&inbound, 101u, &upstream, &reason));
  vectis_proxy_headers_cleanup(&upstream);

  vectis_proxy_headers_init(&upstream);
  add(&upstream, "Upgrade", "websocket");
  add(&upstream, "Connection", "Upgrade");
  add(&upstream, "Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  add(&upstream, "Sec-WebSocket-Protocol", "unoffered");
  assert(!vectis_proxy_ws_response_valid(&inbound, 101u, &upstream, &reason));
  assert(strstr(reason, "subprotocol") != NULL);
  vectis_proxy_headers_cleanup(&upstream);

  vectis_proxy_headers_init(&upstream);
  add(&upstream, "Upgrade", "websocket");
  add(&upstream, "Connection", "Upgrade");
  add(&upstream, "Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  add(&upstream, "Sec-WebSocket-Extensions", "unoffered-extension");
  assert(!vectis_proxy_ws_response_valid(&inbound, 101u, &upstream, &reason));
  assert(strstr(reason, "extension") != NULL);
  vectis_proxy_headers_cleanup(&upstream);

  vectis_proxy_headers_init(&upstream);
  add(&upstream, "Upgrade", "websocket");
  add(&upstream, "Connection", "Upgrade");
  add(&upstream, "Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  add(&upstream, "Sec-WebSocket-Extensions", "permessage-deflate; bad==value");
  assert(!vectis_proxy_ws_response_valid(&inbound, 101u, &upstream, &reason));
  vectis_proxy_headers_cleanup(&upstream);

  vectis_proxy_headers_init(&upstream);
  add(&upstream, "Upgrade", "websocket");
  add(&upstream, "Connection", "Upgrade");
  add(&upstream, "Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  add(&upstream, "Sec-WebSocket-Extensions",
      "permessage-deflate; note=\"a,b\"");
  assert(vectis_proxy_ws_response_valid(&inbound, 101u, &upstream, &reason));
  vectis_proxy_headers_cleanup(&upstream);
  vectis_proxy_headers_cleanup(&inbound);
}

int main(void) {
  test_valid();
  test_request_rejections();
  test_response_rejections();
  return 0;
}
