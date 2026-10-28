#ifndef VECTIS_STORAGE_H
#define VECTIS_STORAGE_H
#include <lc/lc.h>
#include <vectis/vectis.h>
#ifdef __cplusplus
extern "C" {
#endif
/* App-configured factories. Require a started app in the calling process.
 * Returned upstream C89 receiver shells are independently owned; close all
 * child operations/borrows and receivers before the app. No extra client or
 * collection worker is created. read() keeps the native byte/metadata streams.
 * Collections are independent appends and never enlist in XA. */
vectis_status vectis_app_create_collection(vectis_app *,
                                           const lc_collection_config *,
                                           lc_collection **, vectis_error *);
vectis_status vectis_app_open_collection(vectis_app *,
                                         const lc_collection_config *,
                                         lc_collection **, vectis_error *);
vectis_status vectis_app_list_collections(vectis_app *, const char *,
                                          lc_collection_visit_fn, void *,
                                          vectis_error *);
vectis_status vectis_app_collection_capabilities(vectis_app *,
                                                 lc_collection_capabilities *,
                                                 vectis_error *);
/* Explicitly register lc_postgres_driver() with the returned coordinator.
 * It is linked by Vectis's public static/shared dependency closure. Native
 * wait descriptors and durable decisions remain authoritative; no SQL replay.
 */
vectis_status vectis_app_xa_coordinator_open(vectis_app *,
                                             const lc_xa_coordinator_config *,
                                             lc_xa_coordinator **,
                                             vectis_error *);
/* Explicit blocking readiness wait on a borrowed XA descriptor. Never reads or
 * closes fd, advances the operation, or adds a periodic polling delay. Honors
 * absolute CLOCK_MONOTONIC deadlines, including already-due negative-fd waits.
 * EINTR returns an error so callers can cancel/close during app shutdown. */
vectis_status vectis_storage_wait_blocking(const lc_xa_wait *, vectis_error *);
#ifdef __cplusplus
}
#endif
#endif
