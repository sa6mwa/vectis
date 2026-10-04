#ifndef VECTIS_OPCUA_LOG_H
#define VECTIS_OPCUA_LOG_H
#include <cpkt/opcua.h>
#include <pslog.h>
/* Qualified native sink only. log_view preserves byte lengths and does not
 * implement fatal()/panic() termination semantics. */
static void vectis_opcua_log_record(const cpkt_opcua_log_record *record,
                                    void *user) {
  pslog_logger *logger = (pslog_logger *)user;
  pslog_field fields[3];
  pslog_string_view message;
  pslog_level level;
  if (record == NULL || logger == NULL)
    return;
  level = record->level >= CPKT_OPCUA_LOG_FATAL     ? PSLOG_LEVEL_FATAL
          : record->level >= CPKT_OPCUA_LOG_ERROR   ? PSLOG_LEVEL_ERROR
          : record->level >= CPKT_OPCUA_LOG_WARNING ? PSLOG_LEVEL_WARN
          : record->level >= CPKT_OPCUA_LOG_INFO    ? PSLOG_LEVEL_INFO
          : record->level >= CPKT_OPCUA_LOG_DEBUG   ? PSLOG_LEVEL_DEBUG
                                                    : PSLOG_LEVEL_TRACE;
  fields[0] = pslog_i64("category", record->category);
  fields[1] = pslog_u64("format_status", record->format_status);
  fields[2] = pslog_i64("opcua_level", record->level);
  message.data = record->message;
  message.len = record->message_length;
  logger->log_view(logger, level, message, fields, 3u);
}
#endif
