#ifndef VECTIS_PROXY_TELEMETRY_H
#define VECTIS_PROXY_TELEMETRY_H

#include <pslog.h>
#include <stdint.h>

typedef struct vectis_proxy_telemetry {
  uint64_t request_id;
  uint64_t started_ms;
  uint64_t upstream_bytes;
  uint64_t downstream_bytes;
  const char *route;
  const char *upstream;
  const char *kind;
  const char *disconnect_side;
  const char *error_category;
  int status;
} vectis_proxy_telemetry;

/* Route and upstream remain borrowed until the exchange is recorded. HTTP
 * counters include body bytes accepted by the next transport queue, excluding
 * HTTP framing. WebSocket counters include relayed frame bytes, excluding
 * handshake bytes. Counts do not claim downstream delivery after queueing. */
void vectis_proxy_telemetry_start(vectis_proxy_telemetry *record,
                                  const char *route, const char *upstream,
                                  const char *kind, uint64_t now_ms);
void vectis_proxy_telemetry_emit(const vectis_proxy_telemetry *record,
                                 pslog_logger *logger, uint64_t now_ms);

#endif
