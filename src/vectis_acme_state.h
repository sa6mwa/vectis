#ifndef VECTIS_ACME_STATE_H
#define VECTIS_ACME_STATE_H

#include "vectis_internal.h"

typedef struct vectis_acme_state_config {
  /* Complete Lockd client settings prepared by Vectis. The state operation
   * selects its endpoint and namespace below without reconstructing transport,
   * mTLS, logging, allocator, or Pouch policy. */
  lc_client_config lockd_client_config;
  const char *endpoint;
  const char *namespace_name;
  const char *key;
  const char *owner;
  const char *runtime_dir;
  const void *client_bundle_pem;
  size_t client_bundle_pem_size;
  const char *const *domains;
  size_t domain_count;
} vectis_acme_state_config;

/*
 * Vectis-owned durable state shares one encrypted Pouch root by default.
 * The endpoint opts into Pouch multi-writer mode because independently
 * started Vectis processes (for example the server and `vectis -a`) must be
 * able to use the same root concurrently.
 */
char *vectis_persistence_default_pouch_endpoint(vectis_error *error);
char *vectis_metrics_default_pouch_endpoint(vectis_error *error);
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
