#include "vectis_internal.h"
#include "vectis_proxy_local.h"

#include <assert.h>
#include <string.h>

static vectis_status custom_error(const vectis_error *cause, int default_status,
                                  vectis_proxy_local_response *response,
                                  void *userdata, vectis_error *error) {
  const char body[] = {'c', 'u', 's', 't', 'o', 'm', '\0', 'x'};

  (void)userdata;
  assert(cause != NULL && cause->code == VECTIS_ERR_STATE);
  assert(default_status == 502);
  assert(vectis_proxy_local_respond(response, 503, body, sizeof(body), error) ==
         VECTIS_OK);
  return vectis_proxy_local_add_header(response, "X-Gateway", "custom", error);
}

static vectis_status invalid_error(const vectis_error *cause,
                                   int default_status,
                                   vectis_proxy_local_response *response,
                                   void *userdata, vectis_error *error) {
  (void)cause;
  (void)default_status;
  (void)userdata;
  assert(vectis_proxy_local_respond(response, 200, "ok", 2u, error) ==
         VECTIS_OK);
  assert(vectis_proxy_local_add_header(response, "Content-Length", "2",
                                       error) == VECTIS_ERR_INVALID);
  return VECTIS_OK;
}

int main(void) {
  vectis_proxy_local_response response;
  vectis_proxy_route_data route;
  vectis_error cause;
  vectis_error error;
  char body[4];
  char oversized[VECTIS_PROXY_LOCAL_BODY_LIMIT + 1u];

  memset(&route, 0, sizeof(route));
  memset(oversized, 'x', sizeof(oversized));
  memcpy(body, "abc", sizeof(body));
  vectis_proxy_local_init(&response);
  assert(vectis_proxy_local_respond(&response, 200, body, 3u, &error) ==
         VECTIS_OK);
  body[0] = 'z';
  assert(response.body_length == 3u && memcmp(response.body, "abc", 3u) == 0);
  assert(vectis_proxy_local_add_header(&response, "Set-Cookie", "a=1",
                                       &error) == VECTIS_OK);
  assert(vectis_proxy_local_add_header(&response, "Set-Cookie", "b=2",
                                       &error) == VECTIS_OK);
  assert(response.headers.count == 2u);
  assert(vectis_proxy_local_respond(&response, 204, body, 1u, &error) ==
         VECTIS_ERR_INVALID);
  assert(response.failure == VECTIS_ERR_INVALID);
  vectis_proxy_local_cleanup(&response);

  vectis_proxy_local_init(&response);
  assert(vectis_proxy_local_respond(&response, 200, oversized,
                                    VECTIS_PROXY_LOCAL_BODY_LIMIT,
                                    &error) == VECTIS_OK);
  assert(response.body_length == VECTIS_PROXY_LOCAL_BODY_LIMIT);
  assert(vectis_proxy_local_respond(&response, 200, oversized,
                                    sizeof(oversized),
                                    &error) == VECTIS_ERR_INVALID);
  assert(vectis_proxy_local_add_header(&response, "X-Bad", "x\r\ny", &error) ==
         VECTIS_ERR_INVALID);
  vectis_proxy_local_cleanup(&response);

  vectis_set_error(&cause, VECTIS_ERR_STATE, "upstream refused connection");
  route.on_error = custom_error;
  vectis_proxy_local_error(&route, &cause, 502, &response);
  assert(response.status == 503 && response.body_length == 8u);
  assert(memcmp(response.body, "custom\0x", 8u) == 0);
  assert(response.headers.count == 1u);
  vectis_proxy_local_cleanup(&response);

  route.on_error = invalid_error;
  vectis_proxy_local_error(&route, &cause, 502, &response);
  assert(response.status == 502 && response.body_length == 11u);
  assert(memcmp(response.body, "bad gateway", 11u) == 0);
  assert(response.headers.count == 0u);
  vectis_proxy_local_cleanup(&response);

  route.on_error = NULL;
  vectis_proxy_local_error(&route, &cause, 504, &response);
  assert(response.status == 504 && response.body_length == 15u);
  assert(memcmp(response.body, "gateway timeout", 15u) == 0);
  vectis_proxy_local_cleanup(&response);
  return 0;
}
