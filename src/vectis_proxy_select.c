#include "vectis_proxy_select.h"

#include "vectis_internal.h"

#include <stdlib.h>

vectis_status vectis_proxy_select_route(vectis_app *app,
                                        vectis_http_method method,
                                        const char *raw_path,
                                        vectis_request *request,
                                        vectis_proxy_route_data **selected,
                                        vectis_error *error) {
  vectis_internal_websocket_match websocket;
  vectis_route_handler_fn handler;
  vectis_body_policy body;
  vectis_status status;
  char *decoded;
  void *userdata;
  int static_denied;

  if (selected != NULL) {
    *selected = NULL;
  }
  if (app == NULL || raw_path == NULL || request == NULL || selected == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy selector requires app, path, request and output");
    return VECTIS_ERR_INVALID;
  }

  decoded = NULL;
  status = vectis_internal_kore_decode_request_path(raw_path, &decoded, error);
  if (status == VECTIS_ERR_INVALID) {
    goto raw_fallback;
  }
  if (status != VECTIS_OK) {
    return status;
  }

  status = vectis_internal_validate_request_path(decoded, error);
  if (status == VECTIS_ERR_INVALID) {
    /* Body-policy matching retains the static-site trailing-slash exception.
     * It is the sole ordinary route allowed through this validation path. */
    status = vectis_internal_route_body_policy(
        app, method, decoded, &body, NULL, NULL, NULL, request, error);
    free(decoded);
    if (status == VECTIS_OK) {
      vectis_error_clear(error);
      return VECTIS_OK;
    }
    if (status != VECTIS_ERR_INVALID && status != VECTIS_ERR_STATE) {
      return status;
    }
    goto raw_fallback;
  }
  if (status != VECTIS_OK) {
    free(decoded);
    return status;
  }

  /* Application WebSocket routes have established priority over ordinary
   * routes for a valid decoded GET path, including malformed upgrades. */
  status = vectis_internal_match_websocket(app, method, decoded, request,
                                           &websocket, error);
  if (status == VECTIS_OK) {
    free(decoded);
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  if (status != VECTIS_ERR_STATE) {
    free(decoded);
    return status;
  }
  vectis_error_clear(error);

  static_denied = 0;
  status = vectis_internal_static_route_method_denied(
      app, method, decoded, &static_denied, NULL, error);
  if (status != VECTIS_OK) {
    free(decoded);
    return status;
  }
  if (static_denied) {
    free(decoded);
    return VECTIS_OK;
  }

  handler = NULL;
  userdata = NULL;
  status = vectis_internal_route_body_policy(
      app, method, decoded, &body, NULL, &handler, &userdata, request, error);
  free(decoded);
  if (status == VECTIS_OK) {
    if (handler == vectis_proxy_route_marker) {
      /* Decoded-path matching can accept a raw fragment or control byte.
       * Reject it before a proxy preflight callback observes the request. */
      status = vectis_internal_proxy_validate_raw_path(raw_path, error);
      if (status != VECTIS_OK)
        return status;
      *selected = (vectis_proxy_route_data *)userdata;
    }
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  if (status != VECTIS_ERR_INVALID) {
    if (status == VECTIS_ERR_STATE) {
      vectis_error_clear(error);
      return VECTIS_OK;
    }
    return status;
  }

raw_fallback:
  userdata = NULL;
  status = vectis_internal_proxy_raw_path_match(app, method, raw_path,
                                                vectis_proxy_route_marker,
                                                request, &userdata, error);
  if (status == VECTIS_OK) {
    *selected = (vectis_proxy_route_data *)userdata;
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  if (status == VECTIS_ERR_STATE || status == VECTIS_ERR_INVALID) {
    /* Preserve the existing ordinary-path rejection when the proxy-only raw
     * fallback is ineligible or does not match. */
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  return status;
}
