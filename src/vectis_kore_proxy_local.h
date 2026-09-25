#ifndef VECTIS_KORE_PROXY_LOCAL_H
#define VECTIS_KORE_PROXY_LOCAL_H

#include "vectis_proxy_local.h"

struct http_request;

/* Queue a bounded local reply, close after send, and count its HTTP status. */
int vectis_kore_proxy_local_send(struct http_request *request, vectis_app *app,
                                 const vectis_proxy_local_response *response);

#endif
