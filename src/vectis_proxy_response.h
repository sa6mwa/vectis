#ifndef VECTIS_PROXY_RESPONSE_H
#define VECTIS_PROXY_RESPONSE_H

#include "vectis_proxy_http.h"
#include "vectis_proxy_route_internal.h"

struct vectis_proxy_response {
  const vectis_proxy_http_response *upstream;
  vectis_proxy_headers *headers;
  int status;
  vectis_status failure;
};

/* Run the route hook against sanitized output headers. The original upstream
 * response remains the framing authority. The resulting status is used only
 * for the downstream status line and metrics. */
vectis_status vectis_proxy_response_apply(
    const vectis_proxy_http_response *upstream, vectis_proxy_headers *sanitized,
    vectis_proxy_modify_response_fn modify, void *userdata,
    int *downstream_status, vectis_error *error);

#endif
