#ifndef VECTIS_SQL_H
#define VECTIS_SQL_H
#include <stdint.h>
#include <vectis/vectis.h>

#define VECTIS_SQL_MAX_BYTES 1048576u
#define VECTIS_SQL_MAX_VALUE_BYTES 16777216u
#define VECTIS_SQL_MAX_PARAMETERS 256u
#define VECTIS_SQL_MAX_COLUMNS 1024u

typedef enum vectis_sql_type {
  VECTIS_SQL_NULL,
  VECTIS_SQL_INTEGER,
  VECTIS_SQL_REAL,
  VECTIS_SQL_TEXT,
  VECTIS_SQL_BLOB
} vectis_sql_type;

/* Input bytes are borrowed during bind; returned bytes until next/reset/close.
 * NULL, empty TEXT and empty BLOB are different values. */
typedef struct vectis_sql_value {
  vectis_sql_type type;
  int64_t integer;
  double real;
  const void *data;
  size_t size;
} vectis_sql_value;

typedef struct vectis_sql_column {
  const char *name;
  const char *declared_type;
} vectis_sql_column;

#ifdef __cplusplus
extern "C" {
#endif
/* Lifecycle preflight. Active SQL callbacks reject app lifecycle changes and
 * latch the attempted change as a transaction failure, even if caught. */
vectis_status vectis_app_sql_check_idle(vectis_app *, vectis_error *);
#ifdef __cplusplus
}
#endif

#endif
