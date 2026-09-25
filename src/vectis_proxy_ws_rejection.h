#ifndef VECTIS_PROXY_WS_REJECTION_H
#define VECTIS_PROXY_WS_REJECTION_H

#include "vectis_proxy_framing.h"
#include "vectis_proxy_http_wire.h"

#include <vectis/proxy.h>

typedef enum vectis_proxy_ws_rejection_mode {
  VECTIS_PROXY_WS_REJECTION_HEAD = 0,
  VECTIS_PROXY_WS_REJECTION_FIXED = 1,
  VECTIS_PROXY_WS_REJECTION_CHUNKED = 2,
  VECTIS_PROXY_WS_REJECTION_CLOSE = 3,
  VECTIS_PROXY_WS_REJECTION_COMPLETE = 4
} vectis_proxy_ws_rejection_mode;

typedef struct vectis_proxy_ws_rejection {
  vectis_proxy_http_response response;
  vectis_proxy_http_wire_plan wire;
  vectis_proxy_body_framer framer;
  vectis_proxy_ws_rejection_mode mode;
  int downstream_status;
} vectis_proxy_ws_rejection;

void vectis_proxy_ws_rejection_init(vectis_proxy_ws_rejection *rejection);
void vectis_proxy_ws_rejection_cleanup(vectis_proxy_ws_rejection *rejection);

/* Parse one already bounded HTTP/1.1 response head. An informational head
 * returns an owned wire block and leaves the parser ready for the next head.
 * A final head prepares incremental body decoding and downstream framing. */
vectis_status
vectis_proxy_ws_rejection_head(vectis_proxy_ws_rejection *rejection,
                               const unsigned char *head, size_t head_length,
                               int *final, char **wire, size_t *wire_length,
                               vectis_proxy_modify_response_fn modify,
                               void *modify_userdata, vectis_error *error);

/* Consume at most one bounded body chunk per call. The caller must drain the
 * output before calling again. COMPLETE may leave a suffix unconsumed. */
vectis_status vectis_proxy_ws_rejection_feed(
    vectis_proxy_ws_rejection *rejection, const unsigned char *input,
    size_t input_length, size_t *consumed, char *output, size_t output_capacity,
    size_t *written, vectis_error *error);

/* Call after framing completes, or after EOF for a close-delimited body.
 * The owned wire block contains only bounded terminal chunk and trailers. */
vectis_status
vectis_proxy_ws_rejection_finish(vectis_proxy_ws_rejection *rejection, int eof,
                                 char **wire, size_t *wire_length,
                                 vectis_error *error);

#endif
