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

vectis_status vectis_websocket_send(vectis_websocket *websocket,
                                    vectis_websocket_opcode opcode,
                                    const void *data, size_t size,
                                    vectis_error *error) {
  (void)websocket;
  (void)opcode;
  (void)data;
  (void)size;
  vectis_set_error(error, VECTIS_ERR_NOT_IMPLEMENTED,
                   "Kore websocket runtime is not available in this build");
  return VECTIS_ERR_NOT_IMPLEMENTED;
}

vectis_status vectis_websocket_send_text(vectis_websocket *websocket,
                                         const char *text,
                                         vectis_error *error) {
  (void)text;
  return vectis_websocket_send(websocket, VECTIS_WEBSOCKET_TEXT, NULL, 0u,
                               error);
}

vectis_status vectis_websocket_send_binary(vectis_websocket *websocket,
                                           const void *data, size_t size,
                                           vectis_error *error) {
  return vectis_websocket_send(websocket, VECTIS_WEBSOCKET_BINARY, data, size,
                               error);
}

vectis_status vectis_websocket_close(vectis_websocket *websocket,
                                     vectis_error *error) {
  return vectis_websocket_send(websocket, VECTIS_WEBSOCKET_CLOSE, NULL, 0u,
                               error);
}
