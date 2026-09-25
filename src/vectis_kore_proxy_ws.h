#ifndef VECTIS_KORE_PROXY_WS_H
#define VECTIS_KORE_PROXY_WS_H

#include "vectis_proxy_headers.h"
#include "vectis_proxy_route_internal.h"

#include <vectis/vectis.h>

struct http_request;

/* Takes ownership of inbound and route_request only on KORE_RESULT_RETRY. */
int vectis_kore_proxy_ws_start(struct http_request *request,
                               const void *surplus, size_t surplus_length,
                               vectis_app *app, vectis_request *route_request,
                               vectis_proxy_route_data *route,
                               vectis_proxy_headers *inbound,
                               const vectis_proxy_headers *outbound,
                               const char *url, const char *request_target,
                               const char *authority);

void vectis_kore_proxy_ws_worker_cleanup(void);

#endif
