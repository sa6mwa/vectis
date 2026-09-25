#ifndef VECTIS_PROXY_HTTP_WIRE_H
#define VECTIS_PROXY_HTTP_WIRE_H

#include "vectis_proxy_http.h"

#include <vectis/vectis.h>

typedef struct vectis_proxy_http_wire_plan {
  char *head;
  size_t head_length;
  int chunked;
  int body_allowed;
} vectis_proxy_http_wire_plan;

/* Format bounded HTTP/1.1 response metadata after response sanitization.
 * auto_version uses chunked output for every body-bearing response, so an
 * HTTP/2 upstream can still add trailers after declaring Content-Length. */
vectis_status vectis_proxy_http_wire_plan_build(
    const vectis_proxy_http_response *response,
    const vectis_proxy_headers *outbound_headers, int auto_version,
    vectis_proxy_http_wire_plan *plan, vectis_error *error);
void vectis_proxy_http_wire_plan_cleanup(vectis_proxy_http_wire_plan *plan);

/* Encode exactly one bounded producer chunk into caller-owned storage. */
vectis_status
vectis_proxy_http_wire_chunk(const vectis_proxy_http_wire_plan *plan,
                             const unsigned char *body, size_t body_length,
                             char *buffer, size_t capacity, size_t *written,
                             vectis_error *error);

/* Frame a held body chunk using caller-owned space before and after it.
 * The body bytes stay in place; the returned span remains valid until the
 * caller reuses or frees that storage. */
vectis_status vectis_proxy_http_wire_chunk_in_place(
    const vectis_proxy_http_wire_plan *plan, unsigned char *body,
    size_t body_length, size_t headroom, size_t tailroom,
    const unsigned char **wire, size_t *written, vectis_error *error);

/* Emit the final chunk only after the upstream transfer and trailers finish.
 * out is owned by the caller. A fixed-length response has no final bytes. */
vectis_status
vectis_proxy_http_wire_finish(const vectis_proxy_http_wire_plan *plan,
                              const vectis_proxy_headers *trailers, char **out,
                              size_t *out_length, vectis_error *error);

#endif
