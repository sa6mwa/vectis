#ifndef VECTIS_PROXY_WS_WIRE_H
#define VECTIS_PROXY_WS_WIRE_H

#include "vectis_proxy_headers.h"

#include <vectis/vectis.h>

typedef enum vectis_proxy_ws_head_result {
  VECTIS_PROXY_WS_HEAD_MORE = 0,
  VECTIS_PROXY_WS_HEAD_COMPLETE = 1,
  VECTIS_PROXY_WS_HEAD_INVALID = 2,
  VECTIS_PROXY_WS_HEAD_LIMIT = 3,
  VECTIS_PROXY_WS_HEAD_NOMEM = 4
} vectis_proxy_ws_head_result;

/* Build only the bounded HTTP/1.1 opening request headers. sanitized must
 * contain end-to-end fields and no Host, framing, or Upgrade fields. */
vectis_status
vectis_proxy_ws_wire_request(const char *request_target, const char *authority,
                             const vectis_proxy_headers *sanitized, char **wire,
                             size_t *wire_length, vectis_error *error);

/* Parse a complete response head from a caller-owned bounded receive span.
 * MORE retains no bytes; the caller appends another chunk and retries.
 * COMPLETE reports the header boundary, leaving any following bytes with the
 * caller for the tunnel or rejection body. The output must be empty. */
vectis_proxy_ws_head_result vectis_proxy_ws_wire_response_head(
    const unsigned char *data, size_t length, size_t *head_length,
    unsigned *status, vectis_proxy_headers *headers, const char **reason);

#endif
