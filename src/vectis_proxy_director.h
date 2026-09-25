#ifndef VECTIS_PROXY_DIRECTOR_H
#define VECTIS_PROXY_DIRECTOR_H

#include "vectis_proxy_headers.h"
#include "vectis_proxy_route_internal.h"

struct vectis_proxy_inbound {
  vectis_http_method method;
  const char *path;
  const char *query;
  const char *host;
  const vectis_proxy_headers *headers;
  vectis_request *matched_request;
  int websocket;
};

struct vectis_proxy_outbound {
  const vectis_proxy_route_data *route;
  vectis_proxy_headers headers;
  char *path;
  char *query;
  char *host;
  vectis_http_method method;
  size_t target_index;
  int websocket;
  vectis_status failure;
};

/* Copy sanitized into out, invoke the optional route callback, then validate
 * the final target. On success target and authority are caller-owned. out
 * remains valid until cleanup; on failure cleanup is still required. */
vectis_status vectis_proxy_director_prepare(
    const vectis_proxy_route_data *route, vectis_http_method method,
    const char *path, const char *query, const char *host, int websocket,
    vectis_request *matched_request, const vectis_proxy_headers *inbound,
    const vectis_proxy_headers *sanitized, vectis_proxy_outbound *out,
    char **target, char **authority, vectis_error *error);

void vectis_proxy_director_cleanup(vectis_proxy_outbound *out);

#endif
