#include "vectis_internal.h"

vectis_status vectis_internal_kore_run(const vectis_kore_runtime_config *config,
                                       vectis_error *error) {
  (void)config;
  vectis_set_error(error, VECTIS_ERR_NOT_IMPLEMENTED,
                   "Kore runtime is not available in this build");
  return VECTIS_ERR_NOT_IMPLEMENTED;
}

vectis_status
vectis_internal_kore_validate(const vectis_kore_runtime_config *config,
                              vectis_error *error) {
  (void)config;
  vectis_set_error(error, VECTIS_ERR_NOT_IMPLEMENTED,
                   "Kore runtime is not available in this build");
  return VECTIS_ERR_NOT_IMPLEMENTED;
}

vectis_status vectis_internal_kore_stop(vectis_app *app, vectis_error *error) {
  (void)app;
  vectis_error_clear(error);
  return VECTIS_OK;
}

int vectis_internal_kore_signal_requested(void) { return 0; }

int vectis_internal_kore_signal_number(void) { return 0; }
