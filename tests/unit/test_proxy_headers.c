#include "vectis_proxy_framing.h"
#include "vectis_proxy_headers.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void add(vectis_proxy_headers *headers, const char *name,
                const char *value) {
  assert(vectis_proxy_headers_add(headers, name, value) ==
         VECTIS_PROXY_HEADER_OK);
}

static int contains(const vectis_proxy_headers *headers, const char *name,
                    const char *value) {
  size_t i;

  for (i = 0u; i < headers->count; ++i) {
    if (strcmp(headers->fields[i].name, name) == 0 &&
        strcmp(headers->fields[i].value, value) == 0) {
      return 1;
    }
  }
  return 0;
}

static void test_sanitized_request(void) {
  vectis_proxy_headers inbound;
  vectis_proxy_headers outbound;
  vectis_proxy_request_head head;
  const char *reason;

  vectis_proxy_headers_init(&inbound);
  vectis_proxy_headers_init(&outbound);
  add(&inbound, "Host", "public.example:8443");
  add(&inbound, "Connection", "keep-alive, X-Secret");
  add(&inbound, "Content-Length", "0005");
  add(&inbound, "content-length", "5");
  add(&inbound, "X-Secret", "strip me");
  add(&inbound, "Forwarded", "for=spoofed");
  add(&inbound, "X-Forwarded-For", "1.2.3.4");
  add(&inbound, "X-Real-IP", "1.2.3.4");
  add(&inbound, "Expect", "100-continue");
  add(&inbound, "Content-Type", "application/octet-stream");
  add(&inbound, "X-Multi", "first");
  add(&inbound, "X-Multi", "second");

  reason = "stale";
  assert(vectis_proxy_request_head_parse(&inbound, &head, &reason) ==
         VECTIS_PROXY_HEADER_OK);
  assert(reason == NULL);
  assert(strcmp(head.host, "public.example:8443") == 0);
  assert(head.has_content_length && head.content_length == 5u);
  assert(head.expect_continue && !head.chunked && !head.websocket_upgrade);
  assert(vectis_proxy_headers_sanitize_request(&inbound, &outbound) ==
         VECTIS_PROXY_HEADER_OK);
  assert(outbound.count == 3u);
  assert(contains(&outbound, "Content-Type", "application/octet-stream"));
  assert(contains(&outbound, "X-Multi", "first"));
  assert(contains(&outbound, "X-Multi", "second"));
  vectis_proxy_headers_cleanup(&outbound);
  vectis_proxy_headers_cleanup(&inbound);
}

static void test_chunked_trailer_and_websocket(void) {
  vectis_proxy_headers inbound;
  vectis_proxy_request_head head;

  vectis_proxy_headers_init(&inbound);
  add(&inbound, "Host", "[::1]:8080");
  add(&inbound, "Transfer-Encoding", "chunked");
  add(&inbound, "Trailer", "X-Trace, X-Signature");
  assert(vectis_proxy_request_head_parse(&inbound, &head, NULL) ==
         VECTIS_PROXY_HEADER_OK);
  assert(head.chunked && head.has_trailers);
  assert(vectis_proxy_request_trailer_declared(&inbound, "x-trace"));
  assert(vectis_proxy_request_trailer_declared(&inbound, "X-Signature"));
  assert(!vectis_proxy_request_trailer_declared(&inbound, "X-Other"));
  assert(!vectis_proxy_request_trailer_declared(&inbound, "Content-Length"));
  vectis_proxy_headers_cleanup(&inbound);

  vectis_proxy_headers_init(&inbound);
  add(&inbound, "Host", "svc_name.local:8080");
  assert(vectis_proxy_request_head_parse(&inbound, &head, NULL) ==
         VECTIS_PROXY_HEADER_OK);
  vectis_proxy_headers_cleanup(&inbound);

  vectis_proxy_headers_init(&inbound);
  add(&inbound, "Host", "backend.local");
  add(&inbound, "Connection", "keep-alive, Upgrade");
  add(&inbound, "Upgrade", "websocket");
  add(&inbound, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
  assert(vectis_proxy_request_head_parse(&inbound, &head, NULL) ==
         VECTIS_PROXY_HEADER_OK);
  assert(head.websocket_upgrade);
  vectis_proxy_headers_cleanup(&inbound);
}

static size_t consume_body(void *userdata, const unsigned char *data,
                           size_t length) {
  (void)userdata;
  (void)data;
  return length;
}

static int declared_trailer(void *userdata, const char *name,
                            const char *value) {
  (void)value;
  return vectis_proxy_request_trailer_declared(
      (const vectis_proxy_headers *)userdata, name);
}

static void test_trailer_handoff(void) {
  static const unsigned char allowed[] = "0\r\nX-Trace: done\r\n\r\nNEXT";
  static const unsigned char undeclared[] = "0\r\nX-Other: bad\r\n\r\n";
  vectis_proxy_headers inbound;
  vectis_proxy_body_framer framer;
  vectis_proxy_request_head head;
  size_t consumed;

  vectis_proxy_headers_init(&inbound);
  add(&inbound, "Host", "example.test");
  add(&inbound, "Transfer-Encoding", "chunked");
  add(&inbound, "Trailer", "X-Trace");
  assert(vectis_proxy_request_head_parse(&inbound, &head, NULL) ==
         VECTIS_PROXY_HEADER_OK);
  vectis_proxy_body_framer_chunked(&framer);
  assert(vectis_proxy_body_framer_feed(
             &framer, allowed, sizeof(allowed) - 1u, &consumed, consume_body,
             declared_trailer, &inbound) == VECTIS_PROXY_FRAME_COMPLETE);
  assert(consumed == sizeof(allowed) - sizeof("NEXT"));

  vectis_proxy_body_framer_chunked(&framer);
  assert(vectis_proxy_body_framer_feed(&framer, undeclared,
                                       sizeof(undeclared) - 1u, &consumed,
                                       consume_body, declared_trailer,
                                       &inbound) == VECTIS_PROXY_FRAME_INVALID);
  vectis_proxy_headers_cleanup(&inbound);
}

static void test_rejected_framing(void) {
  struct case_entry {
    const char *name;
    const char *value;
    vectis_proxy_header_status expected;
  } cases[] = {
      {"Content-Length", "-1", VECTIS_PROXY_HEADER_INVALID},
      {"Content-Length", "18446744073709551616", VECTIS_PROXY_HEADER_INVALID},
      {"Content-Length", "3, 3", VECTIS_PROXY_HEADER_INVALID},
      {"Transfer-Encoding", "gzip, chunked", VECTIS_PROXY_HEADER_INVALID},
      {"Transfer-Encoding", "chunked", VECTIS_PROXY_HEADER_INVALID},
      {"Expect", "fancy", VECTIS_PROXY_HEADER_INVALID},
      {"Trailer", "Content-Length", VECTIS_PROXY_HEADER_INVALID},
      {"Connection", "Content-Length", VECTIS_PROXY_HEADER_INVALID},
      {"Connection", "X-Good,", VECTIS_PROXY_HEADER_INVALID},
      {"Connection", "Upgrade", VECTIS_PROXY_HEADER_UNSUPPORTED_UPGRADE},
      {"Upgrade", "h2c", VECTIS_PROXY_HEADER_UNSUPPORTED_UPGRADE},
      {"Upgrade", "websocket, h2c", VECTIS_PROXY_HEADER_UNSUPPORTED_UPGRADE}};
  vectis_proxy_headers inbound;
  vectis_proxy_request_head head;
  const char *reason;
  size_t i;

  for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    vectis_proxy_headers_init(&inbound);
    add(&inbound, "Host", "example.test");
    if (strcmp(cases[i].name, "Transfer-Encoding") == 0 &&
        strcmp(cases[i].value, "chunked") == 0) {
      add(&inbound, "Content-Length", "5");
    }
    add(&inbound, cases[i].name, cases[i].value);
    reason = NULL;
    assert(vectis_proxy_request_head_parse(&inbound, &head, &reason) ==
           cases[i].expected);
    assert(reason != NULL && reason[0] != '\0');
    vectis_proxy_headers_cleanup(&inbound);
  }
  vectis_proxy_headers_init(&inbound);
  add(&inbound, "Host", "example.test");
  add(&inbound, "Content-Length", "5");
  add(&inbound, "Content-Length", "6");
  assert(vectis_proxy_request_head_parse(&inbound, &head, NULL) ==
         VECTIS_PROXY_HEADER_INVALID);
  vectis_proxy_headers_cleanup(&inbound);

  {
    static const char *const invalid_host[] = {
        "bad:port",  "[]",           "[::1",         "foo/bar", "foo@evil",
        "bad..name", "-bad.example", "bad-.example", "name:0",  "name:65536"};
    for (i = 0u; i < sizeof(invalid_host) / sizeof(invalid_host[0]); ++i) {
      vectis_proxy_headers_init(&inbound);
      add(&inbound, "Host", invalid_host[i]);
      assert(vectis_proxy_request_head_parse(&inbound, &head, NULL) ==
             VECTIS_PROXY_HEADER_INVALID);
      vectis_proxy_headers_cleanup(&inbound);
    }
  }
}

static void test_header_limits(void) {
  vectis_proxy_headers inbound;
  char *large;
  size_t i;

  vectis_proxy_headers_init(&inbound);
  assert(vectis_proxy_headers_add(&inbound, "Bad Name", "x") ==
         VECTIS_PROXY_HEADER_INVALID);
  assert(vectis_proxy_headers_add(&inbound, "X-Test", "bad\r\nvalue") ==
         VECTIS_PROXY_HEADER_INVALID);
  for (i = 0u; i < VECTIS_PROXY_HEADER_COUNT_LIMIT; ++i) {
    add(&inbound, "X-Test", "x");
  }
  assert(vectis_proxy_headers_add(&inbound, "X-Test", "x") ==
         VECTIS_PROXY_HEADER_LIMIT);
  vectis_proxy_headers_cleanup(&inbound);

  large = (char *)malloc(VECTIS_PROXY_HEADER_BLOCK_LIMIT + 1u);
  assert(large != NULL);
  memset(large, 'a', VECTIS_PROXY_HEADER_BLOCK_LIMIT);
  large[VECTIS_PROXY_HEADER_BLOCK_LIMIT] = '\0';
  assert(vectis_proxy_headers_add(&inbound, "X-Test", large) ==
         VECTIS_PROXY_HEADER_LIMIT);
  free(large);
  vectis_proxy_headers_cleanup(&inbound);
}

int main(void) {
  test_sanitized_request();
  test_chunked_trailer_and_websocket();
  test_trailer_handoff();
  test_rejected_framing();
  test_header_limits();
  puts("proxy headers ok");
  return 0;
}
