#include "vectis_proxy_telemetry.h"

#include <string.h>
#include <unistd.h>

void vectis_proxy_telemetry_start(vectis_proxy_telemetry *record,
                                  const char *route, const char *upstream,
                                  const char *kind, uint64_t now_ms) {
  static uint32_t next_request_id;

  if (record == NULL)
    return;
  memset(record, 0, sizeof(*record));
  ++next_request_id;
  if (next_request_id == 0u)
    ++next_request_id;
  record->request_id =
      ((uint64_t)(unsigned long)getpid() << 32u) | (uint64_t)next_request_id;
  record->started_ms = now_ms;
  record->route = route != NULL ? route : "";
  record->upstream = upstream != NULL ? upstream : "";
  record->kind = kind != NULL ? kind : "http";
  record->disconnect_side = "none";
  record->error_category = "none";
}

void vectis_proxy_telemetry_emit(const vectis_proxy_telemetry *record,
                                 pslog_logger *logger, uint64_t now_ms) {
  pslog_field fields[10];
  uint64_t duration_ms;

  if (record == NULL || logger == NULL)
    return;
  duration_ms = now_ms >= record->started_ms ? now_ms - record->started_ms : 0u;
  fields[0] = pslog_u64("request_id", record->request_id);
  fields[1] = pslog_str("route", record->route);
  fields[2] = pslog_str("upstream", record->upstream);
  fields[3] = pslog_str("kind", record->kind);
  fields[4] = pslog_i64("status", record->status);
  fields[5] = pslog_u64("upstream_bytes", record->upstream_bytes);
  fields[6] = pslog_u64("downstream_bytes", record->downstream_bytes);
  fields[7] = pslog_u64("duration_ms", duration_ms);
  fields[8] = pslog_str("disconnect_side", record->disconnect_side);
  fields[9] = pslog_str("error_category", record->error_category);
  logger->info(logger, "vectis.proxy.exchange", fields,
               sizeof(fields) / sizeof(fields[0]));
}
