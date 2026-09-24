#ifndef VECTIS_PROXY_HTTP_UPSTREAM_H
#define VECTIS_PROXY_HTTP_UPSTREAM_H

#include "vectis_proxy_http.h"

#include <curl/curl.h>
#include <vectis/vectis.h>

typedef struct vectis_proxy_http_upstream vectis_proxy_http_upstream;
typedef void (*vectis_proxy_http_upstream_ready_fn)(
    vectis_proxy_http_upstream *upstream, vectis_proxy_http_event event,
    void *userdata);

/* The owner retains easy, url, target and request_headers through the
 * transfer. One curl callback chunk is held until the downstream consumes it;
 * curl is paused before another chunk can be copied. Header metadata has its
 * separate 64 KiB cap. The transfer owner cancels before cleanup. */
struct vectis_proxy_http_upstream {
  vectis_proxy_http_response response;
  vectis_proxy_headers outbound_headers;
  CURL *easy;
  unsigned char *body;
  size_t body_capacity;
  size_t body_length;
  size_t body_offset;
  vectis_proxy_http_upstream_ready_fn ready;
  void *userdata;
  int paused;
  int failed;
};

vectis_status vectis_proxy_http_upstream_init(
    vectis_proxy_http_upstream *upstream, CURL *easy, const char *url,
    const char *request_target, const char *method,
    struct curl_slist *request_headers, size_t buffer_limit,
    long connect_timeout_ms, long total_timeout_ms,
    vectis_proxy_http_upstream_ready_fn ready, void *userdata,
    vectis_error *error);

/* The pointer remains valid until the next consume or cleanup. */
const unsigned char *
vectis_proxy_http_upstream_body(const vectis_proxy_http_upstream *upstream,
                                size_t *length);
void vectis_proxy_http_upstream_consume(vectis_proxy_http_upstream *upstream,
                                        size_t length);
/* Resume only after the entire held curl chunk has been consumed. */
vectis_status
vectis_proxy_http_upstream_resume(vectis_proxy_http_upstream *upstream,
                                  vectis_error *error);
vectis_proxy_header_status
vectis_proxy_http_upstream_finish(vectis_proxy_http_upstream *upstream,
                                  CURLcode result, const char **reason);
void vectis_proxy_http_upstream_cleanup(vectis_proxy_http_upstream *upstream);

#endif
