#ifndef VECTIS_PROXY_H
#define VECTIS_PROXY_H

#include <vectis/vectis.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Protocol selection for ordinary HTTP and SSE upstream exchanges. */
typedef enum vectis_proxy_http_version {
  /* Prefer HTTP/2 over HTTPS with HTTP/1.1 fallback; cleartext uses HTTP/1.1.
   */
  VECTIS_PROXY_HTTP_AUTO = 0,
  /* Force HTTP/1.1 for every exchange. WebSocket always uses HTTP/1.1. */
  VECTIS_PROXY_HTTP_1_1 = 1
} vectis_proxy_http_version;

/**
 * Registration input for one in-process reverse proxy route.
 *
 * Zero-initialize or call vectis_proxy_route_config_init(). Vectis copies the
 * path and configured target URLs on successful registration. The route uses
 * existing Vectis path matching and registration order. Request and response
 * bodies flow through bounded chunk buffers; they are never materialized.
 */
struct vectis_proxy_route_config {
  /* Required Vectis route pattern; copied before registration returns. */
  const char *path;
  /* Zero selects all supported Vectis HTTP methods. WebSocket requires GET. */
  vectis_http_methods methods;
  /* Zero selects a literal path. Parameter and regex routes are supported. */
  vectis_route_path_kind path_kind;
  /* Required HTTP or HTTPS base URL. Its authority is fixed for this route. */
  const char *target;
  /* Optional additional allowed base URLs. The director may select only a
   * configured target. Each string and the array are borrowed until this
   * registration call returns, then copied by Vectis. */
  const char *const *alternate_targets;
  size_t alternate_target_count;
  /* Zero selects VECTIS_PROXY_HTTP_AUTO. */
  vectis_proxy_http_version upstream_http_version;
  /* Zero uses a 10-second connection deadline; negative values are invalid. */
  long connect_timeout_ms;
  /* Zero uses a 60-second no-progress deadline; negative values are invalid. */
  long idle_timeout_ms;
  /* Zero disables the total deadline, including for SSE and WebSocket. */
  long total_timeout_ms;
  /* Zero uses a 16 KiB per-direction transport chunk limit. Allowed range is
   * 8 KiB through 1 MiB. Header limits are separate. */
  size_t buffer_limit_bytes;
  /* Optional PEM CA bundle for verifying HTTPS and WSS upstream peers.
   * NULL uses libcurl's configured default trust store. The PEM text is
   * borrowed until registration returns, then copied. It replaces libcurl's
   * default CA bundle for this route; peer and hostname checks remain on.
   * A nonempty bundle may contain at most 256 KiB of PEM text. */
  const char *tls_ca_pem;
};

/** Set the documented zero-value defaults. NULL is ignored. */
void vectis_proxy_route_config_init(vectis_proxy_route_config *config);

/**
 * Register a proxy route before app start. The app owns its copied route
 * configuration until app->close(). Returns VECTIS_ERR_NOT_IMPLEMENTED when
 * the Kore runtime is disabled; errors describe invalid targets and route
 * conflicts. Prefer app->proxy_route(app, config, error).
 */
vectis_status
vectis_register_proxy_route(vectis_app *app,
                            const vectis_proxy_route_config *config,
                            vectis_error *error);

#ifdef __cplusplus
}
#endif

#endif
