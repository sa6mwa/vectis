#ifndef VECTIS_PROXY_SELECT_H
#define VECTIS_PROXY_SELECT_H

#include "vectis_proxy_route_internal.h"

/* A NULL selected route means that Kore's existing request path owns this
 * request. A non-NULL route borrows app-owned data until the app is closed. */
vectis_status vectis_proxy_select_route(vectis_app *app,
                                        vectis_http_method method,
                                        const char *raw_path,
                                        vectis_request *request,
                                        vectis_proxy_route_data **selected,
                                        vectis_error *error);

#endif
