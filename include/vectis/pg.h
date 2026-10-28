#ifndef VECTIS_PG_H
#define VECTIS_PG_H
#include <cpkt/postgres.h>
#include <lc/lc.h>
#include <stdint.h>
#include <vectis/sql.h>
#include <vectis/vectis.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct vectis_pg vectis_pg;
typedef struct vectis_pg_result vectis_pg_result;
typedef struct vectis_pg_parameter {
  const void *data;
  size_t size;
  cpkt_postgres_oid oid;
  int binary;
  int is_null;
} vectis_pg_parameter;
typedef struct vectis_pg_diagnostic {
  vectis_error error;
  char sqlstate[6];
  cpkt_postgres_retrieval_error retrieval;
} vectis_pg_diagnostic;
/* Caller-serialized exclusive XA branch adapter; no connection ownership
 * transfer. Application SQL must not control transactions, COPY or pipelines.
 * Errors poison the adapter; close reports failure after releasing its token.
 * Close every result and adapter before the XA prepare/resolve boundary. */
struct vectis_pg {
  vectis_status (*execute_materialized)(vectis_pg *, const char *, size_t,
                                        const vectis_pg_parameter *, size_t,
                                        int64_t, vectis_pg_result **,
                                        vectis_error *);
  const vectis_pg_diagnostic *(*diagnostic)(const vectis_pg *);
  vectis_status (*close)(vectis_pg *, vectis_error *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
/* view validates the retained token and returns a borrowed C89 materialized
 * result, valid until result close or invalidation of its native XA scope.
 * Never free/mutate this view. Use public cpkt result metadata/value readers.
 */
struct vectis_pg_result {
  vectis_status (*view)(vectis_pg_result *, const cpkt_postgres_result **,
                        vectis_error *);
  vectis_status (*close)(vectis_pg_result *, vectis_error *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
vectis_status vectis_pg_from_borrow(lc_xa_borrow *, vectis_pg **,
                                    vectis_error *);
/* Explicit parameter array, including count zero. Deadline is absolute
 * CLOCK_MONOTONIC milliseconds, -1 for infinite. Text/SQL cannot contain NUL.
 * Native parameterized protocol accepts exactly one statement. */
vectis_status vectis_pg_execute_materialized(vectis_pg *, const char *, size_t,
                                             const vectis_pg_parameter *,
                                             size_t, int64_t,
                                             vectis_pg_result **,
                                             vectis_error *);
const vectis_pg_diagnostic *vectis_pg_get_diagnostic(const vectis_pg *);
vectis_status vectis_pg_close(vectis_pg *, vectis_error *);
vectis_status vectis_pg_result_view(vectis_pg_result *,
                                    const cpkt_postgres_result **,
                                    vectis_error *);
vectis_status vectis_pg_result_close(vectis_pg_result *, vectis_error *);

/* Standalone ownership is independent of the XA adapter above. No existing
 * receiver layout changes. Configuration strings are copied on construction. */
typedef struct vectis_pg_connection vectis_pg_connection;
typedef struct vectis_pg_cursor vectis_pg_cursor;
typedef struct vectis_pg_statement vectis_pg_statement;
typedef struct vectis_pg_config {
  const char *const *keywords;
  const char *const *values;
  size_t parameter_count;
  int64_t connect_timeout_ms;
  int64_t io_timeout_ms;
} vectis_pg_config;
typedef struct vectis_pg_readiness {
  int fd;
  int read;
  int write;
} vectis_pg_readiness;
typedef enum vectis_pg_step {
  VECTIS_PG_PENDING = 0,
  VECTIS_PG_ROW = 1,
  VECTIS_PG_EOF = 2
} vectis_pg_step;
typedef struct vectis_pg_column {
  const char *name;
  cpkt_postgres_oid oid;
  int format;
} vectis_pg_column;
typedef struct vectis_pg_command {
  char command[64];
  char affected[32]; /* Exact decimal text; empty if the provider has none. */
} vectis_pg_command;
typedef struct vectis_pg_transaction_config {
  const char *isolation;
  int read_only;
} vectis_pg_transaction_config;
typedef struct vectis_pg_transaction_diagnostic {
  vectis_pg_diagnostic primary;
  vectis_pg_diagnostic cleanup;
  int cleanup_failed;
} vectis_pg_transaction_diagnostic;
const vectis_pg_diagnostic *
vectis_pg_connection_diagnostic(const vectis_pg_connection *);
typedef vectis_status (*vectis_pg_transaction_fn)(vectis_pg_connection *,
                                                  void *, vectis_error *);

/* Caller-serialized, process/thread-confined. One active cursor per connection.
 * Native input/output buffering plus one row result; no full-result collection.
 * Borrowed row/column views expire on advancement/close. Expected EOF requires
 * terminal success AND checked drainage. Cursor abandonment closes the owned
 * connection without network drainage. No network callbacks occur during close.
 * timeout=-1 selects the captured I/O budget; zero is an immediate probe. */
struct vectis_pg_connection {
  vectis_status (*poll)(vectis_pg_connection *, int *, vectis_error *);
  vectis_status (*readiness)(vectis_pg_connection *, vectis_pg_readiness *,
                             vectis_error *);
  vectis_status (*query)(vectis_pg_connection *, const char *, size_t,
                         const vectis_pg_parameter *, size_t, int64_t,
                         vectis_pg_cursor **, vectis_error *);
  vectis_status (*prepare)(vectis_pg_connection *, const char *, const char *,
                           size_t, const cpkt_postgres_oid *, size_t, int64_t,
                           vectis_pg_statement **, vectis_error *);
  vectis_status (*exec)(vectis_pg_connection *, const char *, size_t,
                        const vectis_pg_parameter *, size_t, int64_t,
                        vectis_pg_command *, vectis_error *);
  vectis_status (*transaction)(vectis_pg_connection *,
                               const vectis_pg_transaction_config *,
                               vectis_pg_transaction_fn, void *,
                               vectis_error *);
  vectis_status (*close)(vectis_pg_connection *, vectis_error *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
struct vectis_pg_cursor {
  vectis_status (*next_ready)(vectis_pg_cursor *, vectis_pg_step *,
                              const vectis_sql_value **, size_t *,
                              vectis_error *);
  vectis_status (*next)(vectis_pg_cursor *, vectis_pg_step *,
                        const vectis_sql_value **, size_t *, vectis_error *);
  vectis_status (*columns)(vectis_pg_cursor *, const vectis_pg_column **,
                           size_t *, vectis_error *);
  vectis_status (*readiness)(vectis_pg_cursor *, vectis_pg_readiness *,
                             vectis_error *);
  const vectis_pg_diagnostic *(*diagnostic)(const vectis_pg_cursor *);
  vectis_status (*close)(vectis_pg_cursor *, vectis_error *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
/* A view of a caller-named server statement, owned by its connection. Closing
 * a client view does not send SQL or remove that server statement. */
struct vectis_pg_statement {
  vectis_status (*query)(vectis_pg_statement *, const vectis_pg_parameter *,
                         size_t, int64_t, vectis_pg_cursor **, vectis_error *);
  vectis_status (*close)(vectis_pg_statement *, vectis_error *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
void vectis_pg_config_init(vectis_pg_config *);
void vectis_pg_transaction_config_init(vectis_pg_transaction_config *);
vectis_status vectis_pg_connect_start(const vectis_pg_config *,
                                      vectis_pg_connection **, vectis_error *);
vectis_status vectis_pg_connect(const vectis_pg_config *,
                                vectis_pg_connection **, vectis_error *);
vectis_status vectis_app_pg(vectis_app *, const char *,
                            const vectis_pg_config *, vectis_pg_connection **,
                            vectis_error *);
vectis_status vectis_pg_connection_poll(vectis_pg_connection *, int *,
                                        vectis_error *);
vectis_status vectis_pg_connection_readiness(vectis_pg_connection *,
                                             vectis_pg_readiness *,
                                             vectis_error *);
vectis_status vectis_pg_connection_query(vectis_pg_connection *, const char *,
                                         size_t, const vectis_pg_parameter *,
                                         size_t, int64_t, vectis_pg_cursor **,
                                         vectis_error *);
vectis_status vectis_pg_connection_prepare(vectis_pg_connection *, const char *,
                                           const char *, size_t,
                                           const cpkt_postgres_oid *, size_t,
                                           int64_t, vectis_pg_statement **,
                                           vectis_error *);
vectis_status vectis_pg_connection_exec(vectis_pg_connection *, const char *,
                                        size_t, const vectis_pg_parameter *,
                                        size_t, int64_t, vectis_pg_command *,
                                        vectis_error *);
vectis_status vectis_pg_connection_transaction(
    vectis_pg_connection *, const vectis_pg_transaction_config *,
    vectis_pg_transaction_fn, void *, vectis_error *);
const vectis_pg_transaction_diagnostic *
vectis_pg_connection_transaction_diagnostic(const vectis_pg_connection *);
vectis_status vectis_pg_connection_close(vectis_pg_connection *,
                                         vectis_error *);
vectis_status vectis_pg_cursor_next_ready(vectis_pg_cursor *, vectis_pg_step *,
                                          const vectis_sql_value **, size_t *,
                                          vectis_error *);
vectis_status vectis_pg_cursor_next(vectis_pg_cursor *, vectis_pg_step *,
                                    const vectis_sql_value **, size_t *,
                                    vectis_error *);
vectis_status vectis_pg_cursor_columns(vectis_pg_cursor *,
                                       const vectis_pg_column **, size_t *,
                                       vectis_error *);
vectis_status vectis_pg_cursor_readiness(vectis_pg_cursor *,
                                         vectis_pg_readiness *, vectis_error *);
const vectis_pg_diagnostic *
vectis_pg_cursor_diagnostic(const vectis_pg_cursor *);
vectis_status vectis_pg_cursor_close(vectis_pg_cursor *, vectis_error *);
vectis_status vectis_pg_statement_query(vectis_pg_statement *,
                                        const vectis_pg_parameter *, size_t,
                                        int64_t, vectis_pg_cursor **,
                                        vectis_error *);
vectis_status vectis_pg_statement_close(vectis_pg_statement *, vectis_error *);
#ifdef __cplusplus
}
#endif
#endif
