#ifndef VECTIS_PROXY_FRAMING_H
#define VECTIS_PROXY_FRAMING_H

#include <stddef.h>
#include <stdint.h>

/* The caller owns each input span until feed reports it consumed. The body
 * callback may accept a prefix to apply backpressure; zero means pause. */
typedef size_t (*vectis_proxy_body_accept_fn)(void *userdata,
                                              const unsigned char *data,
                                              size_t length);

/* Trailer names and values are borrowed for the duration of the callback.
 * Return zero to reject an undeclared or otherwise disallowed field. */
typedef int (*vectis_proxy_trailer_accept_fn)(void *userdata, const char *name,
                                              const char *value);

typedef enum vectis_proxy_frame_result {
  VECTIS_PROXY_FRAME_MORE = 0,
  VECTIS_PROXY_FRAME_COMPLETE = 1,
  VECTIS_PROXY_FRAME_PAUSED = 2,
  VECTIS_PROXY_FRAME_INVALID = 3
} vectis_proxy_frame_result;

typedef enum vectis_proxy_frame_phase {
  VECTIS_PROXY_PHASE_FIXED = 0,
  VECTIS_PROXY_PHASE_CHUNK_SIZE = 1,
  VECTIS_PROXY_PHASE_CHUNK_DATA = 2,
  VECTIS_PROXY_PHASE_CHUNK_DATA_CR = 3,
  VECTIS_PROXY_PHASE_CHUNK_DATA_LF = 4,
  VECTIS_PROXY_PHASE_TRAILERS = 5,
  VECTIS_PROXY_PHASE_COMPLETE = 6,
  VECTIS_PROXY_PHASE_INVALID = 7
} vectis_proxy_frame_phase;

#define VECTIS_PROXY_CHUNK_LINE_LIMIT 128u
#define VECTIS_PROXY_TRAILER_LINE_LIMIT 8192u
#define VECTIS_PROXY_TRAILER_BLOCK_LIMIT 16384u
#define VECTIS_PROXY_TRAILER_COUNT_LIMIT 100u

typedef struct vectis_proxy_body_framer {
  vectis_proxy_frame_phase phase;
  uint64_t remaining;
  size_t line_length;
  size_t trailer_bytes;
  size_t trailer_count;
  int line_cr;
  char line[VECTIS_PROXY_TRAILER_LINE_LIMIT + 1u];
} vectis_proxy_body_framer;

void vectis_proxy_body_framer_fixed(vectis_proxy_body_framer *framer,
                                    uint64_t length);
void vectis_proxy_body_framer_chunked(vectis_proxy_body_framer *framer);

/* Validate a field name for use in a trailer. The caller separately checks
 * that a request trailer was declared before forwarding it. */
int vectis_proxy_trailer_field_allowed(const char *name, size_t length);

/* Consumes only this request's body; a suffix belongs to the next request or
 * an upgraded tunnel. COMPLETE may therefore leave input unconsumed. */
vectis_proxy_frame_result vectis_proxy_body_framer_feed(
    vectis_proxy_body_framer *framer, const unsigned char *data, size_t length,
    size_t *consumed, vectis_proxy_body_accept_fn body,
    vectis_proxy_trailer_accept_fn trailer, void *userdata);

#endif
