#ifndef VECTIS_SQLITE_H
#define VECTIS_SQLITE_H
#include <vectis/sql.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct vectis_sqlite vectis_sqlite;
typedef struct vectis_sqlite_statement vectis_sqlite_statement;
typedef struct vectis_sqlite_config {
  /* readwrite_create (default), readwrite, readonly. */
  const char *mode;
  int uri;
  int busy_timeout_ms;
} vectis_sqlite_config;
typedef struct vectis_sqlite_exec_result {
  int64_t changes;
  int64_t last_insert_rowid;
} vectis_sqlite_exec_result;
typedef struct vectis_sqlite_diagnostic {
  vectis_error primary;
  vectis_error cleanup;
  int cleanup_failed;
} vectis_sqlite_diagnostic;
typedef vectis_status (*vectis_sqlite_transaction_fn)(vectis_sqlite *, void *,
                                                      vectis_error *);

/* Connections and statements are confined to their creating process/thread.
 * Parent close invalidates children but leaves child shells caller-owned.
 * All close functions free the C shell, except rejected active-transaction
 * connection close. Never use a C shell again after successful close. */
struct vectis_sqlite {
  vectis_status (*prepare)(vectis_sqlite *, const char *, size_t, int,
                           vectis_sqlite_statement **, vectis_error *);
  vectis_status (*exec)(vectis_sqlite *, const char *, size_t,
                        const vectis_sql_value *, size_t,
                        vectis_sqlite_exec_result *, vectis_error *);
  vectis_status (*close)(vectis_sqlite *, vectis_error *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
struct vectis_sqlite_statement {
  vectis_status (*bind)(vectis_sqlite_statement *, const vectis_sql_value *,
                        size_t, vectis_error *);
  vectis_status (*next)(vectis_sqlite_statement *, int *, vectis_error *);
  vectis_status (*reset)(vectis_sqlite_statement *, vectis_error *);
  void (*close)(vectis_sqlite_statement *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};

void vectis_sqlite_config_init(vectis_sqlite_config *);
vectis_status vectis_sqlite_open(const char *, const vectis_sqlite_config *,
                                 vectis_sqlite **, vectis_error *);
/* Copied named declaration, before app startup. Opens lazily per using
 * process after startup; stop invalidates children, restart permits reopening.
 * App close detaches it; the caller still owns this receiver shell. */
vectis_status vectis_app_sqlite(vectis_app *, const char *, const char *,
                                const vectis_sqlite_config *, vectis_sqlite **,
                                vectis_error *);
/* cursor=1 releases native statement at EOF; cursor=0 permits explicit reset.
 */
vectis_status vectis_sqlite_prepare(vectis_sqlite *, const char *, size_t, int,
                                    vectis_sqlite_statement **, vectis_error *);
vectis_status vectis_sqlite_bind(vectis_sqlite_statement *,
                                 const vectis_sql_value *, size_t,
                                 vectis_error *);
vectis_status vectis_sqlite_next(vectis_sqlite_statement *, int *,
                                 vectis_error *);
vectis_status vectis_sqlite_columns(vectis_sqlite_statement *, size_t *,
                                    vectis_error *);
vectis_status vectis_sqlite_column(vectis_sqlite_statement *, size_t,
                                   vectis_sql_column *, vectis_error *);
vectis_status vectis_sqlite_value(vectis_sqlite_statement *, size_t,
                                  vectis_sql_value *, vectis_error *);
vectis_status vectis_sqlite_reset(vectis_sqlite_statement *, vectis_error *);
void vectis_sqlite_statement_close(vectis_sqlite_statement *);
vectis_status vectis_sqlite_exec(vectis_sqlite *, const char *, size_t,
                                 const vectis_sql_value *, size_t,
                                 vectis_sqlite_exec_result *, vectis_error *);
/* Mode is DEFERRED, IMMEDIATE or EXCLUSIVE. Non-OK callback requests rollback.
 * error.detail records committed/rolled_back/unknown/not_started. */
vectis_status vectis_sqlite_transaction(vectis_sqlite *, const char *,
                                        vectis_sqlite_transaction_fn, void *,
                                        vectis_error *);
vectis_status vectis_sqlite_close(vectis_sqlite *, vectis_error *);
/* Borrowed last transaction diagnostic, until another transaction or close.
 * Available even after an app declaration's native connection was invalidated.
 */
const vectis_sqlite_diagnostic *
vectis_sqlite_get_transaction_diagnostic(const vectis_sqlite *);

#ifdef __cplusplus
}
#endif
#endif
