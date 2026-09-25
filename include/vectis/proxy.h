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

typedef struct vectis_proxy_inbound vectis_proxy_inbound;
typedef struct vectis_proxy_outbound vectis_proxy_outbound;

/**
 * Synchronous request rewrite, called after Vectis validates and sanitizes
 * the inbound headers and before any upstream connection. Both views are
 * borrowed for this call. Returning an error rejects the request locally.
 * The callback must not block, retain either view, or consume a request body.
 */
typedef vectis_status (*vectis_proxy_rewrite_fn)(
    const vectis_proxy_inbound *inbound, vectis_proxy_outbound *outbound,
    void *userdata, vectis_error *error);

/** Borrowed raw request metadata. NULL means no query or path parameter. */
vectis_http_method vectis_proxy_inbound_method(const vectis_proxy_inbound *in);
const char *vectis_proxy_inbound_path(const vectis_proxy_inbound *in);
const char *vectis_proxy_inbound_query(const vectis_proxy_inbound *in);
const char *vectis_proxy_inbound_host(const vectis_proxy_inbound *in);
int vectis_proxy_inbound_websocket(const vectis_proxy_inbound *in);
const char *vectis_proxy_inbound_path_param(const vectis_proxy_inbound *in,
                                            const char *name);
size_t vectis_proxy_inbound_header_count(const vectis_proxy_inbound *in);
/** Header names and values remain valid only during the rewrite call. */
vectis_status vectis_proxy_inbound_header_at(const vectis_proxy_inbound *in,
                                             size_t index, const char **name,
                                             const char **value);

/**
 * Mutate sanitized outbound metadata. Setters copy strings before returning.
 * Target indices refer to target (zero) followed by alternate_targets in
 * registration order. Only end-to-end fields may be edited through these
 * helpers; Host has its own setter. Vectis validates the final raw target and
 * handshake again before connecting.
 */
vectis_status vectis_proxy_outbound_select_target(vectis_proxy_outbound *out,
                                                  size_t index,
                                                  vectis_error *error);
vectis_status vectis_proxy_outbound_set_method(vectis_proxy_outbound *out,
                                               vectis_http_method method,
                                               vectis_error *error);
vectis_status vectis_proxy_outbound_set_path(vectis_proxy_outbound *out,
                                             const char *raw_path,
                                             vectis_error *error);
vectis_status vectis_proxy_outbound_set_query(vectis_proxy_outbound *out,
                                              const char *raw_query,
                                              vectis_error *error);
vectis_status vectis_proxy_outbound_set_host(vectis_proxy_outbound *out,
                                             const char *host,
                                             vectis_error *error);
vectis_status vectis_proxy_outbound_add_header(vectis_proxy_outbound *out,
                                               const char *name,
                                               const char *value,
                                               vectis_error *error);
vectis_status vectis_proxy_outbound_set_header(vectis_proxy_outbound *out,
                                               const char *name,
                                               const char *value,
                                               vectis_error *error);
vectis_status vectis_proxy_outbound_remove_header(vectis_proxy_outbound *out,
                                                  const char *name,
                                                  vectis_error *error);

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
  /* Optional synchronous rewrite hook and borrowed application context. */
  vectis_proxy_rewrite_fn rewrite;
  void *rewrite_userdata;
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
