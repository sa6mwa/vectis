#ifndef VECTIS_PROXY_URL_H
#define VECTIS_PROXY_URL_H

#include <vectis/vectis.h>

/* Build an origin-form target from a configured base URL and raw inbound
 * components. Both outputs are owned by the caller; neither holds body bytes.
 * raw_query excludes the leading '?'. */
vectis_status vectis_proxy_target_build(const char *base_url,
                                        const char *raw_path,
                                        const char *raw_query,
                                        char **request_target, char **authority,
                                        vectis_error *error);

#endif
