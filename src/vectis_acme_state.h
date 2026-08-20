#ifndef VECTIS_ACME_STATE_H
#define VECTIS_ACME_STATE_H

#include "vectis_internal.h"

typedef struct vectis_acme_state_config {
  const char *endpoint;
  const char *namespace_name;
  const char *key;
  const char *owner;
  const char *runtime_dir;
  const char *client_bundle_path;
  const void *client_bundle_pem;
  size_t client_bundle_pem_size;
  const char *pouch_crypto_key;
  const char *pouch_crypto_key_file;
  int pouch_crypto_generate_key_file;
  int pouch_crypto_generate_key_file_set;
  const char *pouch_compression;
  const char *const *domains;
  size_t domain_count;
  long timeout_ms;
} vectis_acme_state_config;

char *vectis_acme_state_default_endpoint(vectis_error *error);
char *vectis_persistence_default_pouch_key_file(void);
char *vectis_acme_state_default_key(const char *const *domains,
                                    size_t domain_count, vectis_error *error);
char *vectis_acme_state_runtime_dir_new(vectis_error *error);
int vectis_acme_state_runtime_dir_remove(const char *path);
vectis_status vectis_acme_state_hydrate(const vectis_acme_state_config *config,
                                        int *hydrated, vectis_error *error);
vectis_status vectis_acme_state_persist(const vectis_acme_state_config *config,
                                        vectis_error *error);

#endif
