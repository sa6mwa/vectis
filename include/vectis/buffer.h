#ifndef VECTIS_BUFFER_H
#define VECTIS_BUFFER_H
#include <vectis/vectis.h>
#ifdef __cplusplus
extern "C" {
#endif
#define VECTIS_BUFFER_CHUNK_BYTES 65536u
typedef struct vectis_buffer vectis_buffer;
typedef struct vectis_buffer_span vectis_buffer_span;
typedef struct vectis_buffer_lock vectis_buffer_lock;
struct vectis_buffer_lock {
  void (*close)(vectis_buffer_lock *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
/* Independent write-lock capability; retains storage, denies mutation through
 * every checked alias, and releases only its own lock. C pointer borrowers
 * remain caller-serialized. Read and parent/span close remain valid. */
vectis_status vectis_buffer_span_lock_writes(vectis_buffer_span *,
                                             vectis_buffer_lock **,
                                             vectis_error *);
void vectis_buffer_lock_close(vectis_buffer_lock *);
struct vectis_buffer {
  vectis_status (*size)(vectis_buffer *, size_t *, vectis_error *);
  vectis_status (*read)(vectis_buffer *, size_t, void *, size_t,
                        vectis_error *);
  vectis_status (*write)(vectis_buffer *, size_t, const void *, size_t,
                         vectis_error *);
  vectis_status (*resize)(vectis_buffer *, size_t, vectis_error *);
  vectis_status (*data)(vectis_buffer *, const void **, size_t *,
                        vectis_error *);
  vectis_status (*data_mutable)(vectis_buffer *, void **, size_t *,
                                vectis_error *);
  vectis_status (*borrow)(vectis_buffer *, size_t, size_t, int,
                          vectis_buffer_span **, vectis_error *);
  void (*close)(vectis_buffer *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
/* Spans retain storage after parent close. Pointer borrows end at span close.
 * Caller-serialized; resize/direct buffer mutation require no active spans.
 * readonly is 0 or 1. Lua never exposes these pointer values as addresses. */
struct vectis_buffer_span {
  vectis_status (*borrow)(vectis_buffer_span *, size_t, size_t, int,
                          vectis_buffer_span **, vectis_error *);
  vectis_status (*size)(vectis_buffer_span *, size_t *, vectis_error *);
  vectis_status (*read)(vectis_buffer_span *, size_t, void *, size_t,
                        vectis_error *);
  vectis_status (*write)(vectis_buffer_span *, size_t, const void *, size_t,
                         vectis_error *);
  vectis_status (*data)(vectis_buffer_span *, const void **, size_t *,
                        vectis_error *);
  vectis_status (*data_mutable)(vectis_buffer_span *, void **, size_t *,
                                vectis_error *);
  void (*close)(vectis_buffer_span *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
vectis_status vectis_buffer_new(size_t, vectis_buffer **, vectis_error *);
vectis_status vectis_buffer_size(vectis_buffer *, size_t *, vectis_error *);
vectis_status vectis_buffer_read(vectis_buffer *, size_t, void *, size_t,
                                 vectis_error *);
vectis_status vectis_buffer_write(vectis_buffer *, size_t, const void *, size_t,
                                  vectis_error *);
vectis_status vectis_buffer_resize(vectis_buffer *, size_t, vectis_error *);
vectis_status vectis_buffer_data(vectis_buffer *, const void **, size_t *,
                                 vectis_error *);
vectis_status vectis_buffer_data_mutable(vectis_buffer *, void **, size_t *,
                                         vectis_error *);
vectis_status vectis_buffer_borrow(vectis_buffer *, size_t, size_t, int,
                                   vectis_buffer_span **, vectis_error *);
void vectis_buffer_close(vectis_buffer *);
vectis_status vectis_buffer_span_size(vectis_buffer_span *, size_t *,
                                      vectis_error *);
vectis_status vectis_buffer_span_borrow(vectis_buffer_span *, size_t, size_t,
                                        int, vectis_buffer_span **,
                                        vectis_error *);
vectis_status vectis_buffer_span_read(vectis_buffer_span *, size_t, void *,
                                      size_t, vectis_error *);
vectis_status vectis_buffer_span_write(vectis_buffer_span *, size_t,
                                       const void *, size_t, vectis_error *);
vectis_status vectis_buffer_span_data(vectis_buffer_span *, const void **,
                                      size_t *, vectis_error *);
vectis_status vectis_buffer_span_data_mutable(vectis_buffer_span *, void **,
                                              size_t *, vectis_error *);
void vectis_buffer_span_close(vectis_buffer_span *);
#ifdef __cplusplus
}
#endif
#endif
