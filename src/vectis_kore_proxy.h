#ifndef VECTIS_KORE_PROXY_H
#define VECTIS_KORE_PROXY_H

#include <vectis/vectis.h>

struct http_request;

/* Called after Kore has parsed request headers, before its body path. */
int vectis_kore_proxy_prebody(struct http_request *request, const void *surplus,
                              size_t surplus_length, vectis_app *app,
                              vectis_http_method method);
/* Cancel active worker-owned transfers before the curl pools are torn down. */
void vectis_kore_proxy_worker_cleanup(void);

#endif
