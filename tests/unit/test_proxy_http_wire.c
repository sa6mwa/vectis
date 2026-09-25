#include "vectis_proxy_http_wire.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void line(vectis_proxy_http_response *response, const char *value) {
  vectis_proxy_http_event event;

  assert(vectis_proxy_http_response_header(response, value, strlen(value),
                                           &event,
                                           NULL) == VECTIS_PROXY_HEADER_OK);
}

static void test_http2_chunked_with_trailers(void) {
  vectis_proxy_http_response response;
  vectis_proxy_headers headers;
  vectis_proxy_http_wire_plan plan;
  vectis_error error;
  char chunk[32];
  char *final;
  size_t written;
  size_t final_length;

  vectis_proxy_http_response_init(&response, 0);
  line(&response, "HTTP/2 200\r\n");
  line(&response, "Content-Length: 3\r\n");
  line(&response, "Connection: X-Private\r\n");
  line(&response, "X-Private: hidden\r\n");
  line(&response, "Set-Cookie: a=1\r\n");
  line(&response, "Set-Cookie: b=2\r\n");
  line(&response, "Trailer: X-Final\r\n");
  line(&response, "\r\n");
  vectis_proxy_headers_init(&headers);
  assert(vectis_proxy_headers_sanitize_response(&response.headers, &headers) ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_http_wire_plan_build(&response, &headers, 1, &plan,
                                           &error) == VECTIS_OK);
  assert(plan.chunked && plan.body_allowed);
  assert(strstr(plan.head, "Content-Length:") == NULL);
  assert(strstr(plan.head, "X-Private") == NULL);
  assert(strstr(plan.head, "Set-Cookie: a=1\r\n") != NULL);
  assert(strstr(plan.head, "Set-Cookie: b=2\r\n") != NULL);
  assert(strstr(plan.head, "Transfer-Encoding: chunked\r\n") != NULL);
  assert(strstr(plan.head, "Trailer: X-Final\r\n") != NULL);
  assert(vectis_proxy_http_wire_chunk(&plan, (const unsigned char *)"abc", 3u,
                                      chunk, sizeof(chunk), &written,
                                      &error) == VECTIS_OK);
  assert(written == 8u && memcmp(chunk, "3\r\nabc\r\n", written) == 0);
  assert(vectis_proxy_http_response_body(&response, 3u, NULL) ==
         VECTIS_PROXY_HEADER_OK);
  line(&response, "X-Final: yes\r\n");
  line(&response, "\r\n");
  assert(vectis_proxy_http_response_finish(&response, NULL) ==
         VECTIS_PROXY_HEADER_OK);
  final = NULL;
  final_length = 0u;
  assert(vectis_proxy_http_wire_finish(&plan, &response.trailers, &final,
                                       &final_length, &error) == VECTIS_OK);
  assert(final_length == strlen("0\r\nX-Final: yes\r\n\r\n"));
  assert(memcmp(final, "0\r\nX-Final: yes\r\n\r\n", final_length) == 0);
  free(final);
  vectis_proxy_http_wire_plan_cleanup(&plan);
  vectis_proxy_headers_cleanup(&headers);
  vectis_proxy_http_response_cleanup(&response);
}

static void test_fixed_and_bodyless(void) {
  vectis_proxy_http_response response;
  vectis_proxy_headers headers;
  vectis_proxy_http_wire_plan plan;
  vectis_error error;
  char chunk[8];
  char *final;
  size_t written;
  size_t final_length;

  vectis_proxy_http_response_init(&response, 0);
  line(&response, "HTTP/1.1 200 OK\r\n");
  line(&response, "Content-Length: 3\r\n");
  line(&response, "\r\n");
  vectis_proxy_headers_init(&headers);
  assert(vectis_proxy_headers_sanitize_response(&response.headers, &headers) ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_http_wire_plan_build(&response, &headers, 0, &plan,
                                           &error) == VECTIS_OK);
  assert(!plan.chunked && plan.body_allowed);
  assert(strstr(plan.head, "Content-Length: 3\r\n") != NULL);
  assert(vectis_proxy_http_wire_chunk(&plan, (const unsigned char *)"abc", 3u,
                                      chunk, sizeof(chunk), &written,
                                      &error) == VECTIS_OK);
  assert(written == 3u && memcmp(chunk, "abc", written) == 0);
  assert(vectis_proxy_http_wire_finish(&plan, &response.trailers, &final,
                                       &final_length, &error) == VECTIS_OK);
  assert(final == NULL && final_length == 0u);
  vectis_proxy_http_wire_plan_cleanup(&plan);
  vectis_proxy_headers_cleanup(&headers);
  vectis_proxy_http_response_cleanup(&response);

  vectis_proxy_http_response_init(&response, 1);
  line(&response, "HTTP/1.1 304 Not Modified\r\n");
  line(&response, "Content-Length: 123\r\n");
  line(&response, "\r\n");
  vectis_proxy_headers_init(&headers);
  assert(vectis_proxy_headers_sanitize_response(&response.headers, &headers) ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_http_wire_plan_build(&response, &headers, 1, &plan,
                                           &error) == VECTIS_OK);
  assert(!plan.chunked && !plan.body_allowed);
  assert(strstr(plan.head, "Content-Length: 123\r\n") != NULL);
  assert(strstr(plan.head, "Transfer-Encoding:") == NULL);
  vectis_proxy_http_wire_plan_cleanup(&plan);
  vectis_proxy_headers_cleanup(&headers);
  vectis_proxy_http_response_cleanup(&response);

  vectis_proxy_http_response_init(&response, 0);
  line(&response, "HTTP/1.1 205 Reset Content\r\n");
  line(&response, "Content-Length: 0\r\n");
  line(&response, "\r\n");
  vectis_proxy_headers_init(&headers);
  assert(vectis_proxy_headers_sanitize_response(&response.headers, &headers) ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_http_wire_plan_build(&response, &headers, 1, &plan,
                                           &error) == VECTIS_OK);
  assert(!plan.chunked && !plan.body_allowed);
  assert(strstr(plan.head, "Content-Length:") == NULL);
  assert(strstr(plan.head, "Transfer-Encoding:") == NULL);
  vectis_proxy_http_wire_plan_cleanup(&plan);
  vectis_proxy_headers_cleanup(&headers);
  vectis_proxy_http_response_cleanup(&response);
}

int main(void) {
  test_http2_chunked_with_trailers();
  test_fixed_and_bodyless();
  return 0;
}
