#ifndef VECTIS_PROXY_CURL_H
#define VECTIS_PROXY_CURL_H

#include <curl/curl.h>
#include <vectis/vectis.h>

typedef struct vectis_proxy_curl_transfer vectis_proxy_curl_transfer;
typedef void (*vectis_proxy_curl_done_fn)(CURL *easy, CURLcode result,
                                          void *userdata);

/* Copy a route-owned PEM bundle into an easy before submission. NULL leaves
 * libcurl's default trust store and peer/hostname verification untouched. */
vectis_status vectis_proxy_curl_set_ca(CURL *easy, char *pem, size_t pem_length,
                                       vectis_error *error);

/* Submission takes ownership of easy only on success. Completion invokes
 * done while easy remains valid, then closes it. Cancellation never invokes
 * done. All calls run on the owning Kore worker. */
vectis_status vectis_proxy_curl_submit(CURL *easy, int force_http1,
                                       vectis_proxy_curl_done_fn done,
                                       void *userdata,
                                       vectis_proxy_curl_transfer **out,
                                       vectis_error *error);

/* Establish a raw TCP/TLS connection on the HTTP/1.1 pool. A successful
 * completion retains the easy in its multi and consumes one worker exchange
 * slot until cancel. The owner must call handoff_socket before registering a
 * raw Kore socket watcher; the fd remains owned by curl. An unsuccessful
 * completion closes the easy after invoking done, as submit does. */
vectis_status vectis_proxy_curl_submit_connect(CURL *easy,
                                               vectis_proxy_curl_done_fn done,
                                               void *userdata,
                                               vectis_proxy_curl_transfer **out,
                                               vectis_error *error);
vectis_status
vectis_proxy_curl_handoff_socket(vectis_proxy_curl_transfer *transfer,
                                 curl_socket_t *out, vectis_error *error);
void vectis_proxy_curl_cancel(vectis_proxy_curl_transfer *transfer);
void vectis_proxy_curl_worker_cleanup(void);
size_t vectis_proxy_curl_active_count(void);

#endif
