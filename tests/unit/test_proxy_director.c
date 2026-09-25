#include "vectis_proxy_director.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static vectis_status change_request(const vectis_proxy_inbound *in,
                                    vectis_proxy_outbound *out, void *userdata,
                                    vectis_error *error) {
  const char *name;
  const char *value;

  (void)userdata;
  assert(vectis_proxy_inbound_method(in) == VECTIS_HTTP_POST);
  assert(strcmp(vectis_proxy_inbound_path(in), "/original") == 0);
  assert(strcmp(vectis_proxy_inbound_query(in), "a=1&a=2") == 0);
  assert(strcmp(vectis_proxy_inbound_host(in), "client.test") == 0);
  assert(!vectis_proxy_inbound_websocket(in));
  assert(vectis_proxy_inbound_header_count(in) == 2u);
  assert(vectis_proxy_inbound_header_at(in, 1u, &name, &value) == VECTIS_OK);
  assert(strcmp(name, "X-Trace") == 0);
  assert(strcmp(value, "inbound") == 0);
  assert(vectis_proxy_outbound_select_target(out, 1u, error) == VECTIS_OK);
  assert(vectis_proxy_outbound_set_method(out, VECTIS_HTTP_PUT, error) ==
         VECTIS_OK);
  assert(vectis_proxy_outbound_set_path(out, "/other", error) == VECTIS_OK);
  assert(vectis_proxy_outbound_set_query(out, "new=3&new=4", error) ==
         VECTIS_OK);
  assert(vectis_proxy_outbound_set_host(out, "public.test:8080", error) ==
         VECTIS_OK);
  assert(vectis_proxy_outbound_set_header(out, "X-Trace", "changed", error) ==
         VECTIS_OK);
  return vectis_proxy_outbound_add_header(out, "X-Extra", "ok", error);
}

static vectis_status invalid_edit(const vectis_proxy_inbound *in,
                                  vectis_proxy_outbound *out, void *userdata,
                                  vectis_error *error) {
  int kind;

  (void)in;
  kind = *(int *)userdata;
  if (kind == 1)
    assert(vectis_proxy_outbound_select_target(out, 2u, error) ==
           VECTIS_ERR_INVALID);
  if (kind == 2)
    assert(vectis_proxy_outbound_set_host(out, "bad\r\nHost: injected",
                                          error) == VECTIS_ERR_INVALID);
  if (kind == 3)
    assert(vectis_proxy_outbound_add_header(out, "Connection", "keep-alive",
                                            error) == VECTIS_ERR_INVALID);
  if (kind == 4)
    assert(vectis_proxy_outbound_set_path(out, "/../secret", error) ==
           VECTIS_ERR_INVALID);
  if (kind == 5)
    assert(vectis_proxy_outbound_set_method(out, VECTIS_HTTP_POST, error) ==
           VECTIS_ERR_INVALID);
  if (kind == 6)
    assert(vectis_proxy_outbound_add_header(out, "Sec-WebSocket-Key", "bad",
                                            error) == VECTIS_ERR_INVALID);
  if (kind == 7)
    assert(vectis_proxy_outbound_add_header(out, "Forwarded", "for=bad",
                                            error) == VECTIS_ERR_INVALID);
  if (kind == 8)
    assert(vectis_proxy_outbound_set_query(out, "q=%0a", error) == VECTIS_OK);
  if (kind == 9)
    assert(vectis_proxy_outbound_add_header(out, "X-Invalid", "bad\r\nnext",
                                            error) == VECTIS_ERR_INVALID);
  /* Even if the callback ignores the failed setter, admission must fail. */
  return VECTIS_OK;
}

static void check_change(void) {
  char primary[] = "https://primary.test/base";
  char alternate[] = "http://other.test/alt";
  char *targets[2];
  vectis_proxy_route_data route;
  vectis_proxy_headers inbound;
  vectis_proxy_headers sanitized;
  vectis_proxy_outbound out;
  vectis_error error;
  char *target;
  char *authority;

  memset(&route, 0, sizeof(route));
  targets[0] = primary;
  targets[1] = alternate;
  route.targets = targets;
  route.target_count = 2u;
  route.rewrite = change_request;
  vectis_proxy_headers_init(&inbound);
  vectis_proxy_headers_init(&sanitized);
  assert(vectis_proxy_headers_add(&inbound, "Host", "client.test") ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_headers_add(&inbound, "X-Trace", "inbound") ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_headers_add(&sanitized, "X-Trace", "inbound") ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_director_prepare(&route, VECTIS_HTTP_POST, "/original",
                                       "a=1&a=2", "client.test", 0, NULL,
                                       &inbound, &sanitized, &out, &target,
                                       &authority, &error) == VECTIS_OK);
  assert(strcmp(target, "/alt/other?new=3&new=4") == 0);
  assert(strcmp(authority, "public.test:8080") == 0);
  assert(strcmp(route.targets[out.target_index], targets[1]) == 0);
  assert(out.method == VECTIS_HTTP_PUT);
  assert(out.headers.count == 2u);
  assert(strcmp(out.headers.fields[0].value, "changed") == 0);
  assert(strcmp(out.headers.fields[1].name, "X-Extra") == 0);
  assert(strcmp(sanitized.fields[0].value, "inbound") == 0);
  free(target);
  free(authority);
  vectis_proxy_director_cleanup(&out);
  vectis_proxy_headers_cleanup(&sanitized);
  vectis_proxy_headers_cleanup(&inbound);
}

static void check_invalid(void) {
  char primary[] = "https://primary.test/base";
  char alternate[] = "http://other.test/alt";
  char *targets[2];
  vectis_proxy_route_data route;
  vectis_proxy_headers inbound;
  vectis_proxy_headers sanitized;
  vectis_proxy_outbound out;
  vectis_error error;
  char *target;
  char *authority;
  int kind;

  memset(&route, 0, sizeof(route));
  targets[0] = primary;
  targets[1] = alternate;
  route.targets = targets;
  route.target_count = 2u;
  route.rewrite = invalid_edit;
  route.rewrite_userdata = &kind;
  vectis_proxy_headers_init(&inbound);
  vectis_proxy_headers_init(&sanitized);
  assert(vectis_proxy_headers_add(&inbound, "Host", "client.test") ==
         VECTIS_PROXY_HEADER_OK);
  for (kind = 1; kind <= 9; ++kind) {
    target = NULL;
    authority = NULL;
    assert(vectis_proxy_director_prepare(
               &route, VECTIS_HTTP_GET, "/original", "a=1", "client.test",
               kind == 5 || kind == 6, NULL, &inbound, &sanitized, &out,
               &target, &authority, &error) == VECTIS_ERR_INVALID);
    assert(target == NULL);
    assert(authority == NULL);
    vectis_proxy_director_cleanup(&out);
  }
  assert(vectis_proxy_director_prepare(
             &route, VECTIS_HTTP_GET, "/original", "a=%0a", "client.test", 0,
             NULL, &inbound, &sanitized, &out, &target, &authority,
             &error) == VECTIS_ERR_INVALID);
  vectis_proxy_director_cleanup(&out);
  vectis_proxy_headers_cleanup(&sanitized);
  vectis_proxy_headers_cleanup(&inbound);
}

int main(void) {
  check_change();
  check_invalid();
  return 0;
}
