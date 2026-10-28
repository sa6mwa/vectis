#ifndef VECTIS_ZLIB_H
#define VECTIS_ZLIB_H

#include <stdint.h>
#include <vectis/vectis.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VECTIS_ZLIB_CHUNK_BYTES 65536u

typedef enum vectis_zlib_format {
  VECTIS_ZLIB_ZLIB = 0,
  VECTIS_ZLIB_GZIP = 1,
  VECTIS_ZLIB_AUTO = 2
} vectis_zlib_format;
typedef enum vectis_zlib_flush {
  VECTIS_ZLIB_NO_FLUSH = 0,
  VECTIS_ZLIB_SYNC_FLUSH = 1,
  VECTIS_ZLIB_FINISH = 2
} vectis_zlib_flush;

typedef struct vectis_zlib_options {
  int compress;
  vectis_zlib_format format;
  int level;
  uint64_t max_output_bytes;
} vectis_zlib_options;

typedef struct vectis_zlib_result {
  uint64_t input_bytes;
  uint64_t output_bytes;
  uint64_t members;
  int finished;
} vectis_zlib_result;

typedef struct vectis_zlib vectis_zlib;
/* Caller-serialized receiver; no callbacks, threads or files. Each process
 * borrows non-overlapping buffers only for that call, both bounded by
 * CHUNK_BYTES and UINT_MAX. Repeat with
 * unconsumed input; drain with empty input when the output buffer fills. Repeat
 * FINISH until result.finished. FINISH declares EOF; no new input afterwards.
 * SYNC_FLUSH needs output capacity greater than six bytes (zlib's progress
 * requirement). Discard output on failure; previously emitted chunks persist.
 * Gzip accepts concatenated members; zlib rejects any trailing input.
 * Failure is latched; close releases the shell, so aliases become invalid.
 */
struct vectis_zlib {
  vectis_status (*process)(vectis_zlib *, const void *, size_t, size_t *,
                           void *, size_t, size_t *, vectis_zlib_flush,
                           vectis_error *);
  vectis_status (*result)(vectis_zlib *, vectis_zlib_result *, vectis_error *);
  void (*close)(vectis_zlib *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};

/* Defaults: compression, zlib, level -1, UINT64_MAX output (no arbitrary cap).
 */
void vectis_zlib_options_init(vectis_zlib_options *);
vectis_status vectis_zlib_new(const vectis_zlib_options *, vectis_zlib **,
                              vectis_error *);
vectis_status vectis_zlib_process(vectis_zlib *, const void *, size_t, size_t *,
                                  void *, size_t, size_t *, vectis_zlib_flush,
                                  vectis_error *);
vectis_status vectis_zlib_get_result(vectis_zlib *, vectis_zlib_result *,
                                     vectis_error *);
void vectis_zlib_close(vectis_zlib *);

typedef enum vectis_zlib_checksum_kind {
  VECTIS_ZLIB_ADLER32 = 0,
  VECTIS_ZLIB_CRC32 = 1
} vectis_zlib_checksum_kind;
/* NULL with size zero requests native initialization; empty non-NULL bytes
 * preserve seed. No payload is retained. Combine lengths are nonnegative. */
vectis_status vectis_zlib_checksum(vectis_zlib_checksum_kind, uint32_t,
                                   const void *, size_t, uint32_t *,
                                   vectis_error *);
vectis_status vectis_zlib_checksum_combine(vectis_zlib_checksum_kind, uint32_t,
                                           uint32_t, uint64_t, uint32_t *,
                                           vectis_error *);
vectis_status vectis_zlib_crc32_combine_gen(uint64_t, uint32_t *,
                                            vectis_error *);
uint32_t vectis_zlib_crc32_combine_op(uint32_t, uint32_t, uint32_t);

#ifdef __cplusplus
}
#endif
#endif
