#ifndef VECTIS_PROXY_LOCAL_H
#define VECTIS_PROXY_LOCAL_H

#include "vectis_proxy_headers.h"
#include "vectis_proxy_route_internal.h"

struct vectis_proxy_local_response {
  vectis_proxy_headers headers;
  unsigned char *body;
  size_t body_length;
  int status;
  vectis_status failure;
};

void vectis_proxy_local_init(vectis_proxy_local_response *response);
void vectis_proxy_local_cleanup(vectis_proxy_local_response *response);

/* Run the optional hook and fill a complete 502/504 response on fallback. */
void vectis_proxy_local_error(const vectis_proxy_route_data *route,
                              const vectis_error *cause, int default_status,
                              vectis_proxy_local_response *response);

#endif
