#ifndef VECTIS_PG_H
#define VECTIS_PG_H
#include <cpkt/postgres.h>
#include <lc/lc.h>
#include <stdint.h>
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
#ifdef __cplusplus
}
#endif
#endif
