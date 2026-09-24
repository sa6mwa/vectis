#ifndef VECTIS_PROXY_HTTP_H
#define VECTIS_PROXY_HTTP_H

#include "vectis_proxy_headers.h"

#include <stdint.h>

typedef enum vectis_proxy_http_event {
  VECTIS_PROXY_HTTP_MORE = 0,
  VECTIS_PROXY_HTTP_INFORMATIONAL = 1,
  VECTIS_PROXY_HTTP_FINAL = 2,
  VECTIS_PROXY_HTTP_TRAILERS = 3
} vectis_proxy_http_event;

typedef struct vectis_proxy_http_response {
  vectis_proxy_headers headers;
  vectis_proxy_headers trailers;
  uint64_t content_length;
  uint64_t body_bytes;
  int status;
  int http2;
  int has_content_length;
  int chunked;
  int body_allowed;
  int head_request;
  int phase;
  unsigned informational_count;
} vectis_proxy_http_response;

void vectis_proxy_http_response_init(vectis_proxy_http_response *response,
                                     int head_request);
void vectis_proxy_http_response_cleanup(vectis_proxy_http_response *response);

/* Feed one complete libcurl header callback line, including its CRLF. The
 * event reports an informational block, final headers, or final trailers.
 * Metadata is bounded separately from streamed body bytes. */
vectis_proxy_header_status vectis_proxy_http_response_header(
    vectis_proxy_http_response *response, const char *line, size_t length,
    vectis_proxy_http_event *event, const char **reason);

/* Account for each received body chunk before passing it to a bounded writer.
 * finish checks declared length and a completed trailer block. */
vectis_proxy_header_status
vectis_proxy_http_response_body(vectis_proxy_http_response *response,
                                size_t length, const char **reason);
/* libcurl delivers trailer fields but can omit their terminal empty line.
 * Call only after a successful curl transfer, which has verified wire framing.
 * A reported TRAILERS event can then be emitted before the downstream final
 * chunk. Direct parser users still need an explicit terminal empty line. */
vectis_proxy_header_status
vectis_proxy_http_response_curl_complete(vectis_proxy_http_response *response,
                                         vectis_proxy_http_event *event,
                                         const char **reason);
vectis_proxy_header_status
vectis_proxy_http_response_finish(const vectis_proxy_http_response *response,
                                  const char **reason);

#endif
