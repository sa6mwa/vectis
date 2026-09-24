#ifndef VECTIS_PROXY_CURL_H
#define VECTIS_PROXY_CURL_H

#include <curl/curl.h>
#include <vectis/vectis.h>

typedef struct vectis_proxy_curl_transfer vectis_proxy_curl_transfer;
typedef void (*vectis_proxy_curl_done_fn)(CURL *easy, CURLcode result,
                                          void *userdata);

/* Submission takes ownership of easy only on success. Completion invokes
 * done while easy remains valid, then closes it. Cancellation never invokes
 * done. All calls run on the owning Kore worker. */
vectis_status vectis_proxy_curl_submit(CURL *easy, int force_http1,
                                       vectis_proxy_curl_done_fn done,
                                       void *userdata,
                                       vectis_proxy_curl_transfer **out,
                                       vectis_error *error);
void vectis_proxy_curl_cancel(vectis_proxy_curl_transfer *transfer);
void vectis_proxy_curl_worker_cleanup(void);
size_t vectis_proxy_curl_active_count(void);

#endif
