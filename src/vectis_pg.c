#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vectis/pg.h>
#include <vectis/storage.h>

typedef struct pg_state {
  lc_xa_borrow *borrow;
  int poisoned;
  pid_t process;
  pthread_t thread;
  vectis_pg_diagnostic diagnostic;
} pg_state;
typedef struct result_state {
  lc_xa_borrow *borrow;
  cpkt_postgres_result *native;
  pid_t process;
  pthread_t thread;
} result_state;

static vectis_status failure(vectis_error *error, vectis_status code,
                             const char *message) {
  if (error) {
    memset(error, 0, sizeof(*error));
    error->code = code;
    error->source = VECTIS_ERROR_SOURCE_CPKT;
    if (message)
      strncpy(error->message, message, sizeof(error->message) - 1u);
  }
  return code;
}
static vectis_status borrow_connection(lc_xa_borrow *borrow,
                                       cpkt_postgres_connection **out,
                                       vectis_error *error) {
  lc_xa_connection_view view;
  lc_error native;
  vectis_status status;
  lc_error_init(&native);
  lc_xa_connection_view_init(&view);
  *out = NULL;
  if (lc_xa_borrow_view(borrow, &view, &native) != LC_OK) {
    status = failure(error, VECTIS_ERR_STATE, native.message);
    lc_error_cleanup(&native);
    return status;
  }
  lc_error_cleanup(&native);
  if (view.resource_kind != LC_XA_RESOURCE_POSTGRES || !view.native_connection)
    return failure(error, VECTIS_ERR_INVALID,
                   "XA borrow is not a PostgreSQL branch");
  *out = (cpkt_postgres_connection *)view.native_connection;
  return VECTIS_OK;
}
vectis_status vectis_pg_result_view(vectis_pg_result *self,
                                    const cpkt_postgres_result **out,
                                    vectis_error *error) {
  result_state *state;
  cpkt_postgres_connection *connection;
  vectis_status status;
  if (out)
    *out = NULL;
  if (!self || !out)
    return failure(error, VECTIS_ERR_INVALID, "invalid PostgreSQL result view");
  state = (result_state *)self->impl;
  status = borrow_connection(state->borrow, &connection, error);
  if (status == VECTIS_OK)
    *out = state->native;
  return status;
}
vectis_status vectis_pg_result_close(vectis_pg_result *self,
                                     vectis_error *error) {
  result_state *state;
  lc_error native;
  int status;
  if (!self)
    return VECTIS_OK;
  state = (result_state *)self->impl;
  /* Tokens remain releasable after their native scope has expired. */
  if (getpid() != state->process ||
      !pthread_equal(pthread_self(), state->thread))
    return failure(error, VECTIS_ERR_STATE,
                   "foreign-owner PostgreSQL result close");
  cpkt_postgres_result_free(state->native);
  lc_error_init(&native);
  status = lc_xa_borrow_release(state->borrow, &native);
  free(state);
  free(self);
  if (status != LC_OK) {
    failure(error, VECTIS_ERR_STATE, native.message);
    lc_error_cleanup(&native);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&native);
  return VECTIS_OK;
}
const vectis_pg_diagnostic *vectis_pg_get_diagnostic(const vectis_pg *self) {
  return self ? &((const pg_state *)self->impl)->diagnostic : NULL;
}
vectis_status vectis_pg_close(vectis_pg *self, vectis_error *error) {
  pg_state *state;
  lc_error native;
  int status;
  vectis_status previous;
  if (!self)
    return VECTIS_OK;
  state = (pg_state *)self->impl;
  if (getpid() != state->process ||
      !pthread_equal(pthread_self(), state->thread))
    return failure(error, VECTIS_ERR_STATE,
                   "foreign-owner PostgreSQL adapter close");
  previous = state->poisoned ? state->diagnostic.error.code : VECTIS_OK;
  if (state->poisoned && error)
    *error = state->diagnostic.error;
  lc_error_init(&native);
  status = lc_xa_borrow_release(state->borrow, &native);
  free(state);
  free(self);
  if (status != LC_OK) {
    failure(error, VECTIS_ERR_STATE, native.message);
    lc_error_cleanup(&native);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&native);
  return previous;
}
vectis_status vectis_pg_from_borrow(lc_xa_borrow *borrow, vectis_pg **out,
                                    vectis_error *error) {
  vectis_pg *self;
  pg_state *state;
  cpkt_postgres_connection *connection;
  lc_error native;
  vectis_status status;
  if (out)
    *out = NULL;
  if (!borrow || !out)
    return failure(error, VECTIS_ERR_INVALID,
                   "PostgreSQL adapter requires an XA borrow");
  status = borrow_connection(borrow, &connection, error);
  if (status != VECTIS_OK)
    return status;
  self = (vectis_pg *)calloc(1, sizeof(*self));
  state = (pg_state *)calloc(1, sizeof(*state));
  if (!self || !state) {
    free(self);
    free(state);
    return failure(error, VECTIS_ERR_NOMEM,
                   "cannot allocate PostgreSQL adapter");
  }
  lc_error_init(&native);
  if (lc_xa_borrow_retain(borrow, &native) != LC_OK) {
    free(self);
    free(state);
    status = failure(error, VECTIS_ERR_STATE, native.message);
    lc_error_cleanup(&native);
    return status;
  }
  lc_error_cleanup(&native);
  state->borrow = borrow;
  state->process = getpid();
  state->thread = pthread_self();
  self->impl = state;
  self->execute_materialized = vectis_pg_execute_materialized;
  self->diagnostic = vectis_pg_get_diagnostic;
  self->close = vectis_pg_close;
  *out = self;
  return VECTIS_OK;
}

static vectis_status check_deadline(int64_t deadline, vectis_error *error) {
  struct timespec now;
  uint64_t milliseconds;
  if (deadline >= 0) {
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > (uint64_t)INT64_MAX / 1000u)
      return failure(error, VECTIS_ERR_STATE, "cannot read monotonic clock");
    milliseconds =
        (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
    if (milliseconds >= (uint64_t)deadline)
      return failure(error, VECTIS_ERR_TIMEOUT,
                     "PostgreSQL execution deadline expired");
  }
  return VECTIS_OK;
}
/* Nonblocking descriptor readiness, not periodic discovery. */
static vectis_status ready(pg_state *state, unsigned events, int64_t deadline,
                           vectis_error *error) {
  lc_xa_wait wait;
  cpkt_postgres_connection *connection;
  vectis_status status = check_deadline(deadline, error);
  if (status != VECTIS_OK)
    return status;
  status = borrow_connection(state->borrow, &connection, error);
  if (status != VECTIS_OK)
    return status;
  memset(&wait, 0, sizeof(wait));
  wait.fd = cpkt_postgres_socket(connection);
  wait.events = events;
  wait.deadline_ms = deadline;
  if (wait.fd < 0)
    return failure(error, VECTIS_ERR_STATE, "PostgreSQL branch has no socket");
  return vectis_storage_wait_blocking(&wait, error);
}

vectis_status vectis_pg_execute_materialized(
    vectis_pg *self, const char *sql, size_t sql_size,
    const vectis_pg_parameter *parameters, size_t count, int64_t deadline,
    vectis_pg_result **out, vectis_error *error) {
  pg_state *state;
  result_state *result;
  vectis_pg_result *receiver;
  cpkt_postgres_connection *connection;
  cpkt_postgres_result *value = NULL;
  cpkt_postgres_result_status outcome;
  cpkt_postgres_retrieval_status retrieved;
  cpkt_postgres_retrieval_error retrieval;
  cpkt_postgres_oid *types = NULL;
  char **values = NULL;
  int *lengths = NULL, *formats = NULL;
  char *command = NULL;
  const char *message;
  size_t i;
  int flush, seen = 0;
  vectis_status status = VECTIS_OK;
  lc_error native;
  vectis_error local_error;
  if (!error)
    error = &local_error;
  if (out)
    *out = NULL;
  if (!self || !out)
    return failure(error, VECTIS_ERR_INVALID,
                   "invalid PostgreSQL execution output");
  state = (pg_state *)self->impl;
  if (getpid() != state->process ||
      !pthread_equal(pthread_self(), state->thread))
    return failure(error, VECTIS_ERR_STATE,
                   "foreign-owner PostgreSQL execution");
  if (state->poisoned) {
    if (error)
      *error = state->diagnostic.error;
    return state->diagnostic.error.code;
  }
  lc_error_init(&native);
  memset(&state->diagnostic, 0, sizeof(state->diagnostic));
  /* Allocate result ownership before any query can execute. */
  receiver = (vectis_pg_result *)calloc(1, sizeof(*receiver));
  result = (result_state *)calloc(1, sizeof(*result));
  if (!receiver || !result) {
    status = failure(error, VECTIS_ERR_NOMEM,
                     "cannot allocate PostgreSQL result owner");
    goto cleanup;
  }
  if (!sql || !sql_size || sql_size == SIZE_MAX || memchr(sql, 0, sql_size) ||
      (count && !parameters) || count > 65535u || deadline < -1) {
    status = failure(error, VECTIS_ERR_INVALID,
                     "invalid PostgreSQL template, parameters or deadline");
    goto cleanup;
  }
  command = (char *)malloc(sql_size + 1u);
  if (count) {
    values = (char **)calloc(count, sizeof(*values));
    types = (cpkt_postgres_oid *)calloc(count, sizeof(*types));
    lengths = (int *)calloc(count, sizeof(*lengths));
    formats = (int *)calloc(count, sizeof(*formats));
  }
  if (!command || (count && (!values || !types || !lengths || !formats))) {
    status = failure(error, VECTIS_ERR_NOMEM,
                     "cannot allocate PostgreSQL parameter arrays");
    goto cleanup;
  }
  memcpy(command, sql, sql_size);
  command[sql_size] = 0;
  for (i = 0; i < count; ++i) {
    const vectis_pg_parameter *p = &parameters[i];
    if (p->size > (size_t)INT_MAX || p->oid > 0xffffffffUL ||
        (p->binary != 0 && p->binary != 1) ||
        (p->is_null != 0 && p->is_null != 1) ||
        (!p->data && p->size && !p->is_null) ||
        (!p->binary && !p->is_null && p->size && memchr(p->data, 0, p->size))) {
      status = failure(error, VECTIS_ERR_INVALID,
                       "invalid PostgreSQL parameter value");
      goto cleanup;
    }
    types[i] = p->oid;
    formats[i] = p->binary;
    lengths[i] = (int)p->size;
    if (!p->is_null) {
      values[i] = (char *)malloc(p->size + 1u);
      if (!values[i]) {
        status = failure(error, VECTIS_ERR_NOMEM,
                         "cannot copy materialized PostgreSQL parameter");
        goto cleanup;
      }
      if (p->size)
        memcpy(values[i], p->data, p->size);
      values[i][p->size] = 0;
    }
  }
  status = check_deadline(deadline, error);
  if (status != VECTIS_OK)
    goto cleanup;
  status = borrow_connection(state->borrow, &connection, error);
  if (status != VECTIS_OK)
    goto cleanup;
  if (!cpkt_postgres_is_nonblocking(connection) ||
      cpkt_postgres_is_busy(connection) ||
      cpkt_postgres_pipeline_status_get(connection) !=
          CPKT_POSTGRES_PIPELINE_OFF ||
      cpkt_postgres_transaction_status_get(connection) !=
          CPKT_POSTGRES_TRANSACTION_IN_TRANSACTION) {
    status = failure(
        error, VECTIS_ERR_STATE,
        "PostgreSQL XA branch is not idle in a nonblocking transaction");
    goto cleanup;
  }
  if (lc_xa_borrow_retain(state->borrow, &native) != LC_OK) {
    status = failure(error, VECTIS_ERR_STATE, native.message);
    goto cleanup;
  }
  result->borrow = state->borrow;
  result->process = state->process;
  result->thread = state->thread;
  if (!cpkt_postgres_send_query_params(connection, command, (int)count, types,
                                       (const char *const *)values, lengths,
                                       formats, 0)) {
    status = failure(error, VECTIS_ERR_STATE,
                     cpkt_postgres_error_message(connection));
    goto cleanup;
  }
  for (;;) {
    status = check_deadline(deadline, error);
    if (status != VECTIS_OK)
      goto cleanup;
    status = borrow_connection(state->borrow, &connection, error);
    if (status != VECTIS_OK)
      goto cleanup;
    flush = cpkt_postgres_flush(connection);
    if (flush < 0) {
      status = failure(error, VECTIS_ERR_STATE,
                       cpkt_postgres_error_message(connection));
      goto cleanup;
    }
    if (!flush)
      break;
    /* Read as well as write to prevent a full-duplex protocol deadlock. */
    status = ready(state, 3u, deadline, error);
    if (status != VECTIS_OK)
      goto cleanup;
    if (!cpkt_postgres_consume_input(connection)) {
      status = failure(error, VECTIS_ERR_STATE,
                       cpkt_postgres_error_message(connection));
      goto cleanup;
    }
  }
  for (;;) {
    status = check_deadline(deadline, error);
    if (status != VECTIS_OK)
      goto cleanup;
    status = borrow_connection(state->borrow, &connection, error);
    if (status != VECTIS_OK)
      goto cleanup;
    if (!cpkt_postgres_consume_input(connection)) {
      status = failure(error, VECTIS_ERR_STATE,
                       cpkt_postgres_error_message(connection));
      goto cleanup;
    }
    if (cpkt_postgres_is_busy(connection)) {
      status = ready(state, 1u, deadline, error);
      if (status != VECTIS_OK)
        goto cleanup;
      continue;
    }
    retrieved =
        cpkt_postgres_get_result_checked(connection, &value, &retrieval);
    if (retrieved == CPKT_POSTGRES_RETRIEVAL_FAILED) {
      state->diagnostic.retrieval = retrieval;
      memcpy(state->diagnostic.sqlstate, retrieval.sqlstate, 6u);
      status = failure(error, VECTIS_ERR_STATE,
                       retrieval.server_message[0]
                           ? retrieval.server_message
                           : "PostgreSQL checked result retrieval failed");
      goto cleanup;
    }
    if (retrieved == CPKT_POSTGRES_RETRIEVAL_DRAINED)
      break;
    if (++seen != 1) {
      status = failure(error, VECTIS_ERR_STATE,
                       "unexpected additional PostgreSQL result");
      goto cleanup;
    }
    outcome = cpkt_postgres_result_status_get(value);
    if (outcome != CPKT_POSTGRES_RESULT_COMMAND_OK &&
        outcome != CPKT_POSTGRES_RESULT_TUPLES_OK) {
      message =
          cpkt_postgres_result_error_field(value, CPKT_POSTGRES_DIAG_SQLSTATE);
      if (message)
        strncpy(state->diagnostic.sqlstate, message, 5u);
      status = failure(error, VECTIS_ERR_STATE,
                       cpkt_postgres_result_error_message(value));
      goto cleanup;
    }
    result->native = value;
    value = NULL;
  }
  if (seen != 1 || cpkt_postgres_transaction_status_get(connection) !=
                       CPKT_POSTGRES_TRANSACTION_IN_TRANSACTION) {
    status =
        failure(error, VECTIS_ERR_STATE,
                "PostgreSQL query did not complete inside its XA transaction");
    goto cleanup;
  }
  receiver->impl = result;
  receiver->view = vectis_pg_result_view;
  receiver->close = vectis_pg_result_close;
  *out = receiver;
  receiver = NULL;
  result = NULL;
cleanup:
  if (value)
    cpkt_postgres_result_free(value);
  if (result) {
    if (result->native)
      cpkt_postgres_result_free(result->native);
    if (result->borrow)
      (void)lc_xa_borrow_release(result->borrow, &native);
  }
  free(result);
  free(receiver);
  free(command);
  lc_error_cleanup(&native);
  if (values) {
    for (i = 0; i < count; ++i)
      free(values[i]);
  }
  free(values);
  free(types);
  free(lengths);
  free(formats);
  if (status != VECTIS_OK) {
    state->poisoned = 1;
    if (error)
      state->diagnostic.error = *error;
    else
      failure(&state->diagnostic.error, status,
              "PostgreSQL application operation failed");
    if (error) {
      error->dependency_code = (long)state->diagnostic.retrieval.code;
      strncpy(error->detail, state->diagnostic.sqlstate,
              sizeof(error->detail) - 1u);
      state->diagnostic.error = *error;
    }
  }
  return status;
}
