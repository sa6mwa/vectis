#ifndef VECTIS_PROXY_HTTP_UPSTREAM_H
#define VECTIS_PROXY_HTTP_UPSTREAM_H

#include "vectis_proxy_http.h"

#include <curl/curl.h>
#include <vectis/vectis.h>

typedef struct vectis_proxy_http_upstream vectis_proxy_http_upstream;
typedef void (*vectis_proxy_http_upstream_ready_fn)(
    vectis_proxy_http_upstream *upstream, vectis_proxy_http_event event,
    void *userdata);

/* A producer may return CURL_READFUNC_PAUSE while its bounded queue is empty.
 * Once input arrives, the owner resumes the easy handle. The producer and
 * userdata remain valid until the transfer is cancelled or completed. */
typedef struct vectis_proxy_http_upload {
  size_t (*read)(char *buffer, size_t size, size_t count, void *userdata);
  int (*trailers)(struct curl_slist **list, void *userdata);
  void *userdata;
  uint64_t content_length;
  int known_length;
} vectis_proxy_http_upload;

/* The owner retains easy, url, target, method and request_headers through the
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
    const vectis_proxy_http_upload *upload,
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
