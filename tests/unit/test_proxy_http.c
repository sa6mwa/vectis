#include "vectis_proxy_http.h"

#include <assert.h>
#include <string.h>

static vectis_proxy_http_event accept(vectis_proxy_http_response *response,
                                      const char *line) {
  vectis_proxy_http_event event;
  const char *reason;

  reason = NULL;
  assert(vectis_proxy_http_response_header(response, line, strlen(line), &event,
                                           &reason) == VECTIS_PROXY_HEADER_OK);
  assert(reason == NULL);
  return event;
}

static void reject(vectis_proxy_http_response *response, const char *line,
                   vectis_proxy_header_status expected) {
  vectis_proxy_http_event event;
  const char *reason;

  reason = NULL;
  assert(vectis_proxy_http_response_header(response, line, strlen(line), &event,
                                           &reason) == expected);
  assert(reason != NULL);
}

static void test_fixed_length(void) {
  vectis_proxy_http_response response;
  vectis_proxy_headers outbound;
  const char *reason;

  vectis_proxy_http_response_init(&response, 0);
  assert(accept(&response, "HTTP/1.1 200 OK\r\n") == VECTIS_PROXY_HTTP_MORE);
  accept(&response, "Content-Length: 5\r\n");
  accept(&response, "Connection: X-Hide\r\n");
  accept(&response, "X-Hide: secret\r\n");
  accept(&response, "Set-Cookie: a=1\r\n");
  accept(&response, "Set-Cookie: b=2\r\n");
  assert(accept(&response, "\r\n") == VECTIS_PROXY_HTTP_FINAL);
  assert(response.status == 200 && response.has_content_length);
  assert(response.content_length == 5u);
  assert(response.headers.count == 5u);
  vectis_proxy_headers_init(&outbound);
  assert(vectis_proxy_headers_sanitize_response(&response.headers, &outbound) ==
         VECTIS_PROXY_HEADER_OK);
  assert(outbound.count == 2u);
  assert(strcmp(outbound.fields[0].value, "a=1") == 0);
  assert(strcmp(outbound.fields[1].value, "b=2") == 0);
  vectis_proxy_headers_cleanup(&outbound);
  reason = NULL;
  assert(vectis_proxy_http_response_body(&response, 2u, &reason) ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_http_response_finish(&response, &reason) ==
         VECTIS_PROXY_HEADER_INVALID);
  assert(vectis_proxy_http_response_body(&response, 3u, &reason) ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_http_response_finish(&response, &reason) ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_http_response_body(&response, 1u, &reason) ==
         VECTIS_PROXY_HEADER_INVALID);
  vectis_proxy_http_response_cleanup(&response);

  vectis_proxy_http_response_init(&response, 0);
  accept(&response, "HTTP/1.1 200 OK\r\n");
  accept(&response, "Connection: Content-Length\r\n");
  accept(&response, "Content-Length: 0\r\n");
  assert(accept(&response, "\r\n") == VECTIS_PROXY_HTTP_FINAL);
  vectis_proxy_headers_init(&outbound);
  assert(vectis_proxy_headers_sanitize_response(&response.headers, &outbound) ==
         VECTIS_PROXY_HEADER_INVALID);
  vectis_proxy_headers_cleanup(&outbound);
  vectis_proxy_http_response_cleanup(&response);
}

static void test_http2_sse_and_trailers(void) {
  vectis_proxy_http_response response;
  const char *reason;
  size_t i;

  vectis_proxy_http_response_init(&response, 0);
  accept(&response, "HTTP/2 103 Early Hints\r\n");
  accept(&response, "Link: </a.css>; rel=preload\r\n");
  assert(accept(&response, "\r\n") == VECTIS_PROXY_HTTP_INFORMATIONAL);
  assert(response.informational_count == 1u && response.headers.count == 0u);
  accept(&response, "HTTP/2 200\r\n");
  accept(&response, "Content-Type: text/event-stream\r\n");
  assert(accept(&response, "\r\n") == VECTIS_PROXY_HTTP_FINAL);
  assert(response.http2 && response.body_allowed);
  for (i = 0u; i < 1024u; ++i) {
    assert(vectis_proxy_http_response_body(&response, 1024u, NULL) ==
           VECTIS_PROXY_HEADER_OK);
  }
  assert(response.body_bytes == 1048576u && response.headers.count == 1u);
  accept(&response, "X-Trace: done\r\n");
  assert(accept(&response, "\r\n") == VECTIS_PROXY_HTTP_TRAILERS);
  assert(response.trailers.count == 1u);
  reason = NULL;
  assert(vectis_proxy_http_response_finish(&response, &reason) ==
         VECTIS_PROXY_HEADER_OK);
  vectis_proxy_http_response_cleanup(&response);
}

static void test_chunked_and_head(void) {
  vectis_proxy_http_response response;
  vectis_proxy_http_event event;

  vectis_proxy_http_response_init(&response, 0);
  accept(&response, "HTTP/1.1 200 OK\r\n");
  accept(&response, "Transfer-Encoding: chunked\r\n");
  accept(&response, "Trailer: X-Trace\r\n");
  assert(accept(&response, "\r\n") == VECTIS_PROXY_HTTP_FINAL);
  assert(response.chunked);
  assert(vectis_proxy_http_response_body(&response, 8192u, NULL) ==
         VECTIS_PROXY_HEADER_OK);
  accept(&response, "X-Trace: done\r\n");
  assert(accept(&response, "\r\n") == VECTIS_PROXY_HTTP_TRAILERS);
  assert(vectis_proxy_http_response_finish(&response, NULL) ==
         VECTIS_PROXY_HEADER_OK);
  vectis_proxy_http_response_cleanup(&response);

  vectis_proxy_http_response_init(&response, 0);
  accept(&response, "HTTP/1.1 200 OK\r\n");
  accept(&response, "Transfer-Encoding: chunked\r\n");
  accept(&response, "\r\n");
  accept(&response, "X-Trace: done\r\n");
  assert(vectis_proxy_http_response_finish(&response, NULL) ==
         VECTIS_PROXY_HEADER_INVALID);
  assert(vectis_proxy_http_response_curl_complete(&response, &event, NULL) ==
         VECTIS_PROXY_HEADER_OK);
  assert(event == VECTIS_PROXY_HTTP_TRAILERS);
  vectis_proxy_http_response_cleanup(&response);

  vectis_proxy_http_response_init(&response, 1);
  accept(&response, "HTTP/1.1 200 OK\r\n");
  accept(&response, "Content-Length: 12345\r\n");
  assert(accept(&response, "\r\n") == VECTIS_PROXY_HTTP_FINAL);
  assert(!response.body_allowed && response.has_content_length);
  assert(vectis_proxy_http_response_finish(&response, NULL) ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_http_response_body(&response, 1u, NULL) ==
         VECTIS_PROXY_HEADER_INVALID);
  vectis_proxy_http_response_cleanup(&response);
}

static void test_rejections(void) {
  vectis_proxy_http_response response;
  vectis_proxy_http_event event;
  const char *reason;
  static const char embedded_nul[] = "X-Test: a\0b\r\n";

  vectis_proxy_http_response_init(&response, 0);
  reject(&response, "HTTP/1.1 \r\n", VECTIS_PROXY_HEADER_INVALID);
  vectis_proxy_http_response_cleanup(&response);

  vectis_proxy_http_response_init(&response, 0);
  reject(&response, "HTTP/1.1 101 Switching Protocols\r\n",
         VECTIS_PROXY_HEADER_UNSUPPORTED_UPGRADE);
  vectis_proxy_http_response_cleanup(&response);

  vectis_proxy_http_response_init(&response, 0);
  accept(&response, "HTTP/1.1 204 No Content\r\n");
  accept(&response, "Content-Length: 1\r\n");
  reject(&response, "\r\n", VECTIS_PROXY_HEADER_INVALID);
  vectis_proxy_http_response_cleanup(&response);

  vectis_proxy_http_response_init(&response, 0);
  accept(&response, "HTTP/1.1 200 OK\r\n");
  accept(&response, "Content-Length: 3\r\n");
  accept(&response, "Content-Length: 4\r\n");
  reject(&response, "\r\n", VECTIS_PROXY_HEADER_INVALID);
  vectis_proxy_http_response_cleanup(&response);

  vectis_proxy_http_response_init(&response, 0);
  accept(&response, "HTTP/1.1 200 OK\r\n");
  reason = NULL;
  assert(vectis_proxy_http_response_header(
             &response, embedded_nul, sizeof(embedded_nul) - 1u, &event,
             &reason) == VECTIS_PROXY_HEADER_INVALID);
  assert(reason != NULL);
  vectis_proxy_http_response_cleanup(&response);

  vectis_proxy_http_response_init(&response, 0);
  accept(&response, "HTTP/1.1 200 OK\r\n");
  assert(accept(&response, "\r\n") == VECTIS_PROXY_HTTP_FINAL);
  reject(&response, "X-Trace: done\r\n", VECTIS_PROXY_HEADER_INVALID);
  vectis_proxy_http_response_cleanup(&response);
}

int main(void) {
  test_fixed_length();
  test_http2_sse_and_trailers();
  test_chunked_and_head();
  test_rejections();
  return 0;
}
