#ifndef VECTIS_CRYPTO_H
#define VECTIS_CRYPTO_H
#include <vectis/vectis.h>
#ifdef __cplusplus
extern "C" {
#endif
#define VECTIS_CRYPTO_MAX_BYTES 16777216u
#define VECTIS_CRYPTO_GCM_NONCE_BYTES 12u
#define VECTIS_CRYPTO_GCM_TAG_BYTES 16u
typedef struct vectis_crypto_key vectis_crypto_key;
typedef enum vectis_crypto_key_source {
  VECTIS_CRYPTO_PRIVATE_KEY = 1,
  VECTIS_CRYPTO_PUBLIC_KEY = 2,
  VECTIS_CRYPTO_CERTIFICATE = 3
} vectis_crypto_key_source;
typedef struct vectis_crypto_key_info {
  const char *type; /* Borrowed until key close. */
  int bits;
  size_t max_signature_bytes;
  int private_key;
} vectis_crypto_key_info;
/* Caller-serialized, process/thread-confined. Digest signatures use SHA-256
 * when algorithm is NULL. Unsupported key/digest combinations report errors.
 * Bytes are borrowed only during calls; result copies are caller-owned. */
struct vectis_crypto_key {
  vectis_status (*sign)(vectis_crypto_key *, const char *, const void *, size_t,
                        vectis_mutable_bytes *, vectis_error *);
  vectis_status (*verify)(vectis_crypto_key *, const char *, const void *,
                          size_t, const void *, size_t, int *, vectis_error *);
  vectis_status (*info)(vectis_crypto_key *, vectis_crypto_key_info *,
                        vectis_error *);
  vectis_status (*close)(vectis_crypto_key *, vectis_error *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
/* Explicit passphrase bytes are borrowed only during load; callback never
 * prompts. PEM/file input <=16 MiB; passphrase <=1023 bytes. Files are
 * seekable. Certificate loading extracts the public key; it does not validate a
 * chain. */
vectis_status vectis_crypto_key_load(vectis_crypto_key_source, const void *,
                                     size_t, const void *, size_t,
                                     vectis_crypto_key **, vectis_error *);
vectis_status vectis_crypto_key_load_file(vectis_crypto_key_source,
                                          const char *, const void *, size_t,
                                          vectis_crypto_key **, vectis_error *);
vectis_status vectis_crypto_key_sign(vectis_crypto_key *, const char *,
                                     const void *, size_t,
                                     vectis_mutable_bytes *, vectis_error *);
vectis_status vectis_crypto_key_verify(vectis_crypto_key *, const char *,
                                       const void *, size_t, const void *,
                                       size_t, int *, vectis_error *);
vectis_status vectis_crypto_key_get_info(vectis_crypto_key *,
                                         vectis_crypto_key_info *,
                                         vectis_error *);
vectis_status vectis_crypto_key_close(vectis_crypto_key *, vectis_error *);
typedef struct vectis_crypto_gcm_result {
  vectis_mutable_bytes ciphertext;
  unsigned char tag[VECTIS_CRYPTO_GCM_TAG_BYTES];
} vectis_crypto_gcm_result;
void vectis_crypto_gcm_result_cleanup(vectis_crypto_gcm_result *);
/* Materialized output only. Keys 16/24/32 bytes; nonce 12; tag 16. Data and AAD
 * each <=16 MiB. Auth failure releases/cleans internal plaintext and leaves the
 * output empty. Clean successful plaintext with vectis_mutable_bytes_cleanup.
 */
vectis_status vectis_crypto_gcm_encrypt_materialized(
    const void *, size_t, const void *, size_t, const void *, size_t,
    const void *, size_t, vectis_crypto_gcm_result *, vectis_error *);
vectis_status vectis_crypto_gcm_decrypt_materialized(
    const void *, size_t, const void *, size_t, const void *, size_t,
    const void *, size_t, const void *, size_t, vectis_mutable_bytes *,
    vectis_error *);
#ifdef __cplusplus
}
#endif
#endif
