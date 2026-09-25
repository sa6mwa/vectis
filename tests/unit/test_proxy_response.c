#include "vectis_proxy_response.h"

#include <assert.h>
#include <string.h>

static vectis_status modify(vectis_proxy_response *response, void *userdata,
                            vectis_error *error) {
  const char *name;
  const char *value;

  (void)userdata;
  assert(vectis_proxy_response_status(response) == 200);
  assert(vectis_proxy_response_header_count(response) == 2u);
  assert(vectis_proxy_response_header_at(response, 1u, &name, &value) ==
         VECTIS_OK);
  assert(strcmp(name, "X-Source") == 0);
  assert(strcmp(value, "original") == 0);
  assert(vectis_proxy_response_set_status(response, 202, error) == VECTIS_OK);
  assert(vectis_proxy_response_set_header(response, "X-Source", "changed",
                                          error) == VECTIS_OK);
  assert(vectis_proxy_response_remove_header(response, "X-Remove", error) ==
         VECTIS_OK);
  return vectis_proxy_response_add_header(response, "X-New", "yes", error);
}

static vectis_status invalid(vectis_proxy_response *response, void *userdata,
                             vectis_error *error) {
  int kind;

  kind = *(int *)userdata;
  if (kind == 1)
    assert(vectis_proxy_response_set_status(response, 204, error) ==
           VECTIS_ERR_INVALID);
  if (kind == 2)
    assert(vectis_proxy_response_set_status(response, 101, error) ==
           VECTIS_ERR_INVALID);
  if (kind == 3)
    assert(vectis_proxy_response_add_header(response, "Content-Length", "2",
                                            error) == VECTIS_ERR_INVALID);
  if (kind == 4)
    assert(vectis_proxy_response_set_header(response, "Transfer-Encoding",
                                            "chunked",
                                            error) == VECTIS_ERR_INVALID);
  if (kind == 5)
    assert(vectis_proxy_response_add_header(response, "Sec-WebSocket-Accept",
                                            "bad",
                                            error) == VECTIS_ERR_INVALID);
  if (kind == 6)
    assert(vectis_proxy_response_add_header(response, "X-Invalid", "a\r\nb",
                                            error) == VECTIS_ERR_INVALID);
  if (kind == 7)
    assert(vectis_proxy_response_add_header(response, "Forwarded", "for=bad",
                                            error) == VECTIS_ERR_INVALID);
  if (kind == 8)
    assert(vectis_proxy_response_set_status(response, 600, error) ==
           VECTIS_ERR_INVALID);
  return VECTIS_OK;
}

static vectis_status change_bodyless(vectis_proxy_response *response,
                                     void *userdata, vectis_error *error) {
  (void)userdata;
  assert(vectis_proxy_response_status(response) == 204);
  return vectis_proxy_response_set_status(response, 304, error);
}

static void check_modify(void) {
  vectis_proxy_http_response upstream;
  vectis_proxy_headers sanitized;
  vectis_error error;
  int status;

  memset(&upstream, 0, sizeof(upstream));
  upstream.status = 200;
  upstream.body_allowed = 1;
  vectis_proxy_headers_init(&sanitized);
  assert(vectis_proxy_headers_add(&sanitized, "X-Remove", "gone") ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_headers_add(&sanitized, "X-Source", "original") ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_response_apply(&upstream, &sanitized, modify, NULL,
                                     &status, &error) == VECTIS_OK);
  assert(status == 202);
  assert(upstream.status == 200);
  assert(sanitized.count == 2u);
  assert(strcmp(sanitized.fields[0].name, "X-Source") == 0);
  assert(strcmp(sanitized.fields[0].value, "changed") == 0);
  assert(strcmp(sanitized.fields[1].name, "X-New") == 0);
  vectis_proxy_headers_cleanup(&sanitized);
}

static void check_invalid(void) {
  vectis_proxy_http_response upstream;
  vectis_proxy_headers sanitized;
  vectis_error error;
  int status;
  int kind;

  memset(&upstream, 0, sizeof(upstream));
  upstream.status = 200;
  upstream.body_allowed = 1;
  vectis_proxy_headers_init(&sanitized);
  for (kind = 1; kind <= 8; ++kind) {
    status = 0;
    assert(vectis_proxy_response_apply(&upstream, &sanitized, invalid, &kind,
                                       &status, &error) == VECTIS_ERR_INVALID);
    assert(status == 0);
    vectis_proxy_headers_cleanup(&sanitized);
  }
  upstream.status = 204;
  upstream.body_allowed = 0;
  status = 0;
  assert(vectis_proxy_response_apply(&upstream, &sanitized, NULL, NULL, &status,
                                     &error) == VECTIS_OK);
  assert(status == 204);
  assert(vectis_proxy_response_apply(&upstream, &sanitized, change_bodyless,
                                     NULL, &status, &error) == VECTIS_OK);
  assert(status == 304);
  assert(upstream.status == 204);
  upstream.status = 101;
  assert(vectis_proxy_response_apply(&upstream, &sanitized, NULL, NULL, &status,
                                     &error) == VECTIS_ERR_INVALID);
  vectis_proxy_headers_cleanup(&sanitized);
}

int main(void) {
  check_modify();
  check_invalid();
  return 0;
}
