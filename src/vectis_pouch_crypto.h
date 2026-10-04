#ifndef VECTIS_POUCH_CRYPTO_H
#define VECTIS_POUCH_CRYPTO_H
#include <lc/lc.h>
#define VECTIS_POUCH_CRYPTO_KEY_ENV "VECTIS_POUCH_CRYPTO_KEY"
/* Inspect decoded names through the same authoritative parser as root open.
 * Malformed endpoints propagate their native error; never infer no encryption.
 */
static int vectis_pouch_endpoint_has_crypto_option(const char *endpoint,
                                                   int *present,
                                                   lc_error *error) {
  int key = 0, file = 0, rc;
  *present = 0;
  if (endpoint == NULL)
    return LC_OK;
  rc = lc_pouch_endpoint_has_option(endpoint, "crypto_key", &key, error);
  if (rc != LC_OK)
    return rc;
  rc = lc_pouch_endpoint_has_option(endpoint, "crypto_key_file", &file, error);
  if (rc == LC_OK)
    *present = key || file;
  return rc;
}
#endif
