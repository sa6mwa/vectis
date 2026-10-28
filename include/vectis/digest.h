#ifndef VECTIS_DIGEST_H
#define VECTIS_DIGEST_H

#include <stdint.h>
#include <vectis/vectis.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VECTIS_DIGEST_MAX_BYTES 64u
#define VECTIS_DIGEST_CHUNK_BYTES 65536u

typedef struct vectis_digest vectis_digest;
typedef struct vectis_digest_result {
  unsigned char data[VECTIS_DIGEST_MAX_BYTES];
  size_t size;
  uint64_t bytes;
} vectis_digest_result;

/* A successful zero-byte read means EOF. Failure must not be reported as EOF.
 * The reader writes at most capacity bytes and retains ownership of userdata.
 */
typedef vectis_status (*vectis_digest_reader)(void *userdata,
                                              unsigned char *buffer,
                                              size_t capacity, size_t *size,
                                              vectis_error *error);

/* Caller-serialized incremental state. Final is idempotent; update after final
 * or failure is rejected. No threads, files or callbacks are owned by it. */
struct vectis_digest {
  vectis_status (*update)(vectis_digest *, const void *, size_t,
                          vectis_error *);
  vectis_status (*final)(vectis_digest *, vectis_digest_result *,
                         vectis_error *);
  void (*close)(vectis_digest *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};

vectis_status vectis_digest_new(const char *algorithm, vectis_digest **out,
                                vectis_error *error);
vectis_status vectis_digest_update(vectis_digest *, const void *, size_t,
                                   vectis_error *);
vectis_status vectis_digest_final(vectis_digest *, vectis_digest_result *,
                                  vectis_error *);
void vectis_digest_close(vectis_digest *);
/* Input is already materialized. Result storage is caller-owned. */
vectis_status vectis_digest_materialized(const char *, const void *, size_t,
                                         vectis_digest_result *,
                                         vectis_error *);
/* Blocking bounded-chunk processing, up to UINT64_MAX cumulative bytes. */
vectis_status vectis_digest_stream(const char *, vectis_digest_reader, void *,
                                   vectis_digest_result *, vectis_error *);
/* Blocking binary file reads. Hashes observed bytes, not a file snapshot. */
vectis_status vectis_digest_file(const char *, const char *,
                                 vectis_digest_result *, vectis_error *);

/* Binary key is borrowed during construction only; empty keys are valid.
 * The returned receiver has the same update/final/close contract as a digest.
 */
vectis_status vectis_hmac_new(const char *, const void *, size_t,
                              vectis_digest **, vectis_error *);
vectis_status vectis_hmac_materialized(const char *, const void *, size_t,
                                       const void *, size_t,
                                       vectis_digest_result *, vectis_error *);
vectis_status vectis_hmac_stream(const char *, const void *, size_t,
                                 vectis_digest_reader, void *,
                                 vectis_digest_result *, vectis_error *);
vectis_status vectis_hmac_file(const char *, const void *, size_t, const char *,
                               vectis_digest_result *, vectis_error *);

#ifdef __cplusplus
}
#endif
#endif
