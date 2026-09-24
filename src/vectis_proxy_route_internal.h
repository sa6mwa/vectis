#ifndef VECTIS_PROXY_ROUTE_INTERNAL_H
#define VECTIS_PROXY_ROUTE_INTERNAL_H

#include <vectis/proxy.h>

typedef struct vectis_proxy_route_data {
  char **targets;
  size_t target_count;
  vectis_proxy_http_version upstream_http_version;
  long connect_timeout_ms;
  long idle_timeout_ms;
  long total_timeout_ms;
  size_t buffer_limit_bytes;
} vectis_proxy_route_data;

/* The selector compares this function pointer with the winning route handler
 * and borrows its route-owned userdata as vectis_proxy_route_data. */
vectis_status vectis_proxy_route_marker(vectis_app *app,
                                        vectis_request *request,
                                        vectis_response *response,
                                        void *userdata, vectis_error *error);

#endif
