#ifndef VECTIS_COMPRESSION_H
#define VECTIS_COMPRESSION_H
#include <vectis/vectis.h>
#ifdef __cplusplus
extern "C" {
#endif
#define VECTIS_COMPRESSION_CHUNK_BYTES 65536u
typedef struct vectis_compression vectis_compression;
typedef struct vectis_compression_config {
  int level;
  int level_set; /* 0 selects native default; 1 uses level (including XZ 0). */
  unsigned int max_window_log; /* Zstd decoder only; default 26 (64 MiB). */
  size_t memory_limit;         /* XZ decoder only; default 64 MiB. */
} vectis_compression_config;
/* Called synchronously, outside provider stacks, with <=65536 borrowed bytes.
 * Failure latches the receiver; no more output follows. Never called by close.
 * Callbacks must not longjmp or reenter/close their active receiver. */
typedef vectis_status (*vectis_compression_sink)(void *, const void *, size_t,
                                                 vectis_error *);
/* Caller-serialized and process/thread-confined; no native workers. One frame
 * per receiver, no concatenation/trailing bytes. Finish is required (including
 * decompression) and idempotent after success. Close frees the C shell. */
struct vectis_compression {
  vectis_status (*process)(vectis_compression *, const void *, size_t,
                           vectis_compression_sink, void *, vectis_error *);
  vectis_status (*finish)(vectis_compression *, vectis_compression_sink, void *,
                          vectis_error *);
  vectis_status (*close)(vectis_compression *, vectis_error *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
void vectis_compression_config_init(vectis_compression_config *);
vectis_status vectis_compression_new(const char *, const char *,
                                     const vectis_compression_config *,
                                     vectis_compression **, vectis_error *);
vectis_status vectis_compression_process(vectis_compression *, const void *,
                                         size_t, vectis_compression_sink,
                                         void *, vectis_error *);
vectis_status vectis_compression_finish(vectis_compression *,
                                        vectis_compression_sink, void *,
                                        vectis_error *);
vectis_status vectis_compression_close(vectis_compression *, vectis_error *);
#ifdef __cplusplus
}
#endif
#endif
