#include "vectis_sql_lifecycle.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vectis/pg.h>

typedef struct pg_connection_state pg_connection_state;
typedef struct pg_cursor_state pg_cursor_state;
typedef struct pg_statement_state pg_statement_state;
typedef struct pg_parameter_scratch {
  char *copies[VECTIS_SQL_MAX_PARAMETERS];
  const char *values[VECTIS_SQL_MAX_PARAMETERS];
  cpkt_postgres_oid types[VECTIS_SQL_MAX_PARAMETERS];
  int lengths[VECTIS_SQL_MAX_PARAMETERS], formats[VECTIS_SQL_MAX_PARAMETERS];
} pg_parameter_scratch;
struct pg_connection_state {
  vectis_pg_connection receiver;
  pg_parameter_scratch parameters;
  cpkt_postgres_connection *native;
  pg_cursor_state *cursor;
  const char *keywords[130], *values[130];
  char *configuration, *name;
  size_t references;
  unsigned long generation;
  int64_t connect_timeout, io_timeout;
  cpkt_postgres_poll_status action;
  int ready, closed, invalid, declaration, active, control, violated;
  pid_t process;
  pthread_t thread;
  vectis_sql_app_resource resource;
  vectis_pg_transaction_diagnostic diagnostic;
  vectis_pg_diagnostic last;
  vectis_pg_diagnostic transaction_failure;
};
struct pg_cursor_state {
  vectis_pg_cursor receiver;
  pg_connection_state *owner;
  unsigned long generation;
  cpkt_postgres_result *row;
  vectis_sql_value values[VECTIS_SQL_MAX_COLUMNS];
  vectis_pg_column columns[VECTIS_SQL_MAX_COLUMNS];
  char *names[VECTIS_SQL_MAX_COLUMNS];
  size_t column_count;
  int metadata, terminal, drained, flushing, preparing;
  int64_t budget;
  vectis_pg_command command;
  vectis_pg_diagnostic diagnostic;
};
struct pg_statement_state {
  vectis_pg_statement receiver;
  pg_connection_state *owner;
  unsigned long generation;
  char *name;
  int control;
};
static vectis_status pg_failure(vectis_error *error, vectis_status status,
                                const char *message) {
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->code = status;
    error->source = VECTIS_ERROR_SOURCE_CPKT;
    strncpy(error->message,
            message == NULL || message[0] == '\0'
                ? "PostgreSQL operation failed"
                : message,
            sizeof(error->message) - 1u);
  }
  return status;
}
static void pg_disconnect(pg_connection_state *state) {
  /* PQfinish may send Terminate. Never run it on an inherited live socket,
   * which still belongs to the parent process's session. */
  if (state->process != getpid() ||
      !pthread_equal(state->thread, pthread_self())) {
    state->invalid = 1;
    ++state->generation;
    return;
  }
  if (state->cursor != NULL && state->cursor->row != NULL) {
    cpkt_postgres_result_free(state->cursor->row);
    state->cursor->row = NULL;
  }
  if (state->native != NULL)
    cpkt_postgres_connection_free(state->native);
  state->native = NULL;
  state->ready = 0;
  state->cursor = NULL;
  state->invalid = 1;
  ++state->generation;
}
static void pg_unref(pg_connection_state *state) {
  if (--state->references == 0u) {
    if (state->native != NULL)
      cpkt_postgres_connection_free(state->native);
    free(state->configuration);
    free(state->name);
    free(state);
  }
}
static vectis_status pg_owner(pg_connection_state *state, vectis_error *error) {
  if (state == NULL || state->closed)
    return pg_failure(error, VECTIS_ERR_STATE,
                      "PostgreSQL connection is closed");
  if (state->declaration && (state->resource.app == NULL ||
                             !vectis_sql_app_running(state->resource.app)))
    return pg_failure(error, VECTIS_ERR_STATE,
                      "PostgreSQL declaration requires a running app");
  if (state->process != getpid()) {
    if (state->declaration && state->native == NULL && !state->invalid) {
      state->process = getpid();
      state->thread = pthread_self();
    } else
      return pg_failure(error, VECTIS_ERR_STATE,
                        "PostgreSQL connection belongs to another process");
  }
  if (!pthread_equal(state->thread, pthread_self()))
    return pg_failure(error, VECTIS_ERR_STATE,
                      "PostgreSQL connection belongs to another thread");
  if (state->invalid)
    return pg_failure(error, VECTIS_ERR_STATE,
                      "PostgreSQL connection is invalid; reopen or restart its "
                      "app declaration");
  return VECTIS_OK;
}
static vectis_status pg_native_failure(pg_connection_state *state,
                                       vectis_error *error) {
  vectis_status status = pg_failure(&state->last.error, VECTIS_ERR_STATE,
                                    cpkt_postgres_error_message(state->native));
  if (error != NULL)
    *error = state->last.error;
  pg_disconnect(state);
  return status;
}
static vectis_status pg_native_start(pg_connection_state *state,
                                     vectis_error *error) {
  if (state->native != NULL)
    return VECTIS_OK;
  state->native =
      cpkt_postgres_connect_start_params(state->keywords, state->values, 0);
  if (state->native == NULL) {
    state->invalid = 1;
    return pg_failure(error, VECTIS_ERR_NOMEM,
                      "allocating PostgreSQL connection");
  }
  if (cpkt_postgres_connection_status_get(state->native) ==
      CPKT_POSTGRES_CONNECTION_BAD)
    return pg_native_failure(state, error);
  if (cpkt_postgres_set_nonblocking(state->native, 1) != 0)
    return pg_native_failure(state, error);
  state->action = CPKT_POSTGRES_POLL_WRITING;
  return VECTIS_OK;
}
void vectis_pg_config_init(vectis_pg_config *config) {
  if (config != NULL) {
    memset(config, 0, sizeof(*config));
    config->connect_timeout_ms = 5000;
    config->io_timeout_ms = 30000;
  }
}
void vectis_pg_transaction_config_init(vectis_pg_transaction_config *config) {
  if (config != NULL) {
    config->isolation = "read_committed";
    config->read_only = 0;
  }
}
static vectis_status pg_new_state(const vectis_pg_config *config,
                                  pg_connection_state **out,
                                  vectis_error *error) {
  vectis_pg_config defaults;
  pg_connection_state *state;
  size_t i, j, total = 0u, length, count;
  int ssl = 0;
  char *next;
  *out = NULL;
  vectis_pg_config_init(&defaults);
  if (config == NULL)
    config = &defaults;
  count = config->parameter_count;
  if (count > 128u ||
      (count != 0u && (config->keywords == NULL || config->values == NULL)) ||
      config->connect_timeout_ms < 0 || config->io_timeout_ms < 0)
    return pg_failure(error, VECTIS_ERR_INVALID,
                      "invalid PostgreSQL configuration");
  for (i = 0u; i < count; ++i) {
    if (config->keywords[i] == NULL || config->keywords[i][0] == '\0' ||
        config->values[i] == NULL)
      return pg_failure(error, VECTIS_ERR_INVALID,
                        "missing PostgreSQL keyword/value");
    length = strlen(config->keywords[i]);
    if (length > 1024u || length + 2u > VECTIS_SQL_MAX_BYTES - total)
      return pg_failure(error, VECTIS_ERR_INVALID,
                        "PostgreSQL configuration exceeds byte limit");
    total += length + 2u;
    length = strlen(config->values[i]);
    if (length > VECTIS_SQL_MAX_BYTES - total)
      return pg_failure(error, VECTIS_ERR_INVALID,
                        "PostgreSQL configuration exceeds byte limit");
    total += length;
    if (strcmp(config->keywords[i], "sslmode") == 0)
      ssl = 1;
    for (j = 0u; j < i; ++j)
      if (strcmp(config->keywords[i], config->keywords[j]) == 0)
        return pg_failure(error, VECTIS_ERR_INVALID,
                          "duplicate PostgreSQL connection keyword");
  }
  if (!ssl)
    total += sizeof("sslmode") + sizeof("verify-full");
  state = (pg_connection_state *)calloc(1u, sizeof(*state));
  if (state == NULL)
    return pg_failure(error, VECTIS_ERR_NOMEM,
                      "allocating PostgreSQL receiver");
  state->configuration = (char *)malloc(total == 0u ? 1u : total);
  if (state->configuration == NULL) {
    free(state);
    return pg_failure(error, VECTIS_ERR_NOMEM,
                      "copying PostgreSQL configuration");
  }
  next = state->configuration;
  for (i = 0u; i < count; ++i) {
    state->keywords[i] = next;
    length = strlen(config->keywords[i]) + 1u;
    memcpy(next, config->keywords[i], length);
    next += length;
    state->values[i] = next;
    length = strlen(config->values[i]) + 1u;
    memcpy(next, config->values[i], length);
    next += length;
  }
  if (!ssl) {
    state->keywords[count] = next;
    memcpy(next, "sslmode", sizeof("sslmode"));
    next += sizeof("sslmode");
    state->values[count] = next;
    memcpy(next, "verify-full", sizeof("verify-full"));
  }
  state->references = 1u;
  state->generation = 1u;
  state->process = getpid();
  state->thread = pthread_self();
  state->connect_timeout = config->connect_timeout_ms;
  state->io_timeout = config->io_timeout_ms;
  state->receiver.poll = vectis_pg_connection_poll;
  state->receiver.readiness = vectis_pg_connection_readiness;
  state->receiver.query = vectis_pg_connection_query;
  state->receiver.prepare = vectis_pg_connection_prepare;
  state->receiver.exec = vectis_pg_connection_exec;
  state->receiver.transaction = vectis_pg_connection_transaction;
  state->receiver.close = vectis_pg_connection_close;
  state->receiver.impl = state;
  *out = state;
  return VECTIS_OK;
}
static vectis_status pg_clock(int64_t *milliseconds, vectis_error *error) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
      (uint64_t)now.tv_sec > (uint64_t)INT64_MAX / 1000u)
    return pg_failure(error, VECTIS_ERR_STATE,
                      "cannot read PostgreSQL monotonic clock");
  *milliseconds = (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
  return VECTIS_OK;
}
static vectis_status pg_wait(pg_connection_state *state, int read, int write,
                             int64_t deadline, vectis_error *error) {
  struct pollfd wait;
  int64_t now, remaining;
  int result, timeout;
  vectis_status status;
  memset(&wait, 0, sizeof(wait));
  wait.fd = cpkt_postgres_socket(state->native);
  wait.events = (short)((read ? POLLIN : 0) | (write ? POLLOUT : 0));
  if (wait.fd < 0 || wait.events == 0)
    return pg_failure(error, VECTIS_ERR_STATE,
                      "PostgreSQL has no waitable socket");
  for (;;) {
    status = pg_clock(&now, error);
    if (status != VECTIS_OK)
      return status;
    remaining = deadline > now ? deadline - now : 0;
    timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
    result = poll(&wait, 1, timeout);
    if (result < 0) {
      if (errno == EINTR)
        continue;
      return pg_failure(error, VECTIS_ERR_STATE,
                        "PostgreSQL readiness wait failed");
    }
    if (result > 0) {
      if (wait.revents & POLLNVAL)
        return pg_failure(error, VECTIS_ERR_STATE,
                          "PostgreSQL wait descriptor is closed");
      return VECTIS_OK;
    }
    if (remaining <= INT_MAX)
      return pg_failure(error, VECTIS_ERR_TIMEOUT,
                        "PostgreSQL I/O budget exhausted");
  }
}
vectis_status vectis_pg_connection_poll(vectis_pg_connection *receiver,
                                        int *ready, vectis_error *error) {
  pg_connection_state *state =
      receiver == NULL ? NULL : (pg_connection_state *)receiver->impl;
  vectis_status status = pg_owner(state, error);
  if (ready != NULL)
    *ready = 0;
  if (status != VECTIS_OK)
    return status;
  if (ready == NULL)
    return pg_failure(error, VECTIS_ERR_INVALID,
                      "missing PostgreSQL connect progress output");
  status = pg_native_start(state, error);
  if (status != VECTIS_OK)
    return status;
  if (!state->ready) {
    state->action = cpkt_postgres_connect_poll(state->native);
    if (state->action == CPKT_POSTGRES_POLL_FAILED)
      return pg_native_failure(state, error);
    if (state->action == CPKT_POSTGRES_POLL_OK)
      state->ready = 1;
  }
  *ready = state->ready;
  return VECTIS_OK;
}
vectis_status vectis_pg_connection_readiness(vectis_pg_connection *receiver,
                                             vectis_pg_readiness *out,
                                             vectis_error *error) {
  pg_connection_state *state =
      receiver == NULL ? NULL : (pg_connection_state *)receiver->impl;
  vectis_status status;
  if (out == NULL)
    return pg_failure(error, VECTIS_ERR_INVALID,
                      "missing PostgreSQL readiness output");
  memset(out, 0, sizeof(*out));
  out->fd = -1;
  status = pg_owner(state, error);
  if (status != VECTIS_OK)
    return status;
  status = pg_native_start(state, error);
  if (status != VECTIS_OK)
    return status;
  out->fd = cpkt_postgres_socket(state->native);
  if (!state->ready) {
    out->read = state->action == CPKT_POSTGRES_POLL_READING;
    out->write = !out->read;
  } else if (state->cursor != NULL) {
    out->read = 1;
    out->write = state->cursor->flushing;
  }
  return VECTIS_OK;
}
static vectis_status pg_connect_wait(pg_connection_state *state,
                                     vectis_error *error) {
  int ready = 0;
  int64_t now, deadline;
  vectis_pg_readiness wait;
  vectis_status status = pg_clock(&now, error);
  if (status != VECTIS_OK)
    return status;
  deadline = state->connect_timeout > INT64_MAX - now
                 ? INT64_MAX
                 : now + state->connect_timeout;
  while (!ready) {
    status = vectis_pg_connection_poll(&state->receiver, &ready, error);
    if (status != VECTIS_OK || ready)
      return status;
    status = vectis_pg_connection_readiness(&state->receiver, &wait, error);
    if (status == VECTIS_OK)
      status = pg_wait(state, wait.read, wait.write, deadline, error);
    if (status != VECTIS_OK) {
      pg_disconnect(state);
      return status;
    }
  }
  return VECTIS_OK;
}
vectis_status vectis_pg_connect_start(const vectis_pg_config *config,
                                      vectis_pg_connection **out,
                                      vectis_error *error) {
  pg_connection_state *state;
  vectis_status status;
  if (out == NULL)
    return pg_failure(error, VECTIS_ERR_INVALID,
                      "missing PostgreSQL connection output");
  *out = NULL;
  status = pg_new_state(config, &state, error);
  if (status != VECTIS_OK)
    return status;
  status = pg_native_start(state, error);
  if (status != VECTIS_OK) {
    pg_unref(state);
    return status;
  }
  *out = &state->receiver;
  return VECTIS_OK;
}
vectis_status vectis_pg_connect(const vectis_pg_config *config,
                                vectis_pg_connection **out,
                                vectis_error *error) {
  vectis_pg_connection *receiver;
  vectis_status status;
  if (out == NULL)
    return pg_failure(error, VECTIS_ERR_INVALID,
                      "missing PostgreSQL connection output");
  *out = NULL;
  status = vectis_pg_connect_start(config, &receiver, error);
  if (status != VECTIS_OK)
    return status;
  status = pg_connect_wait((pg_connection_state *)receiver->impl, error);
  if (status != VECTIS_OK) {
    vectis_pg_connection_close(receiver, NULL);
    return status;
  }
  *out = receiver;
  return VECTIS_OK;
}
static void pg_app_invalidate(void *context) {
  pg_connection_state *state = (pg_connection_state *)context;
  pg_disconnect(state);
  state->invalid = 0;
}
static vectis_status pg_app_idle(void *context, vectis_error *error) {
  pg_connection_state *state = (pg_connection_state *)context;
  if (state->process == getpid() &&
      !pthread_equal(state->thread, pthread_self()))
    return pg_failure(error, VECTIS_ERR_STATE,
                      "PostgreSQL app lifecycle requires its declaring thread");
  if (state->active) {
    state->violated = 1;
    return pg_failure(
        error, VECTIS_ERR_STATE,
        "app lifecycle is forbidden inside PostgreSQL transaction callbacks");
  }
  return VECTIS_OK;
}
vectis_status vectis_app_pg(vectis_app *app, const char *name,
                            const vectis_pg_config *config,
                            vectis_pg_connection **out, vectis_error *error) {
  pg_connection_state *state;
  vectis_status status;
  if (out != NULL)
    *out = NULL;
  if (out == NULL || name == NULL || name[0] == '\0' || strlen(name) > 255u)
    return pg_failure(error, VECTIS_ERR_INVALID,
                      "invalid PostgreSQL app declaration");
  status = pg_new_state(config, &state, error);
  if (status != VECTIS_OK)
    return status;
  state->name = (char *)malloc(strlen(name) + 1u);
  if (state->name == NULL) {
    pg_unref(state);
    return pg_failure(error, VECTIS_ERR_NOMEM,
                      "copying PostgreSQL declaration name");
  }
  strcpy(state->name, name);
  state->declaration = 1;
  state->resource.name = state->name;
  state->resource.context = state;
  state->resource.check_idle = pg_app_idle;
  state->resource.invalidate = pg_app_invalidate;
  status = vectis_sql_app_attach(app, &state->resource, error);
  if (status != VECTIS_OK) {
    pg_unref(state);
    return status;
  }
  *out = &state->receiver;
  return VECTIS_OK;
}
vectis_status vectis_pg_connection_close(vectis_pg_connection *receiver,
                                         vectis_error *error) {
  pg_connection_state *state;
  if (receiver == NULL)
    return VECTIS_OK;
  state = (pg_connection_state *)receiver->impl;
  if (state->declaration && state->native == NULL &&
      state->process != getpid()) {
    state->process = getpid();
    state->thread = pthread_self();
  }
  if (state->process != getpid() ||
      !pthread_equal(state->thread, pthread_self()))
    return pg_failure(
        error, VECTIS_ERR_STATE,
        "cannot close PostgreSQL connection from another process/thread");
  if (state->active) {
    state->violated = 1;
    return pg_failure(
        error, VECTIS_ERR_STATE,
        "cannot close transaction callback's PostgreSQL connection");
  }
  vectis_sql_app_detach(&state->resource);
  state->closed = 1;
  pg_disconnect(state);
  pg_unref(state);
  return VECTIS_OK;
}

/* Inspect only transaction-control prefixes, not arbitrary SQL semantics.
 * Extended query protocol supplies the single-statement boundary. */
static const char *pg_word(const char *sql, const char *end, char *word,
                           size_t capacity) {
  size_t size = 0u;
  unsigned int depth;
  for (;;) {
    while (sql < end && isspace((unsigned char)*sql))
      ++sql;
    if (end - sql >= 2 && sql[0] == '-' && sql[1] == '-') {
      while (sql < end && *sql != '\n')
        ++sql;
      continue;
    }
    if (end - sql >= 2 && sql[0] == '/' && sql[1] == '*') {
      sql += 2;
      depth = 1u;
      while (sql < end && depth != 0u) {
        if (end - sql >= 2 && sql[0] == '/' && sql[1] == '*') {
          ++depth;
          sql += 2;
        } else if (end - sql >= 2 && sql[0] == '*' && sql[1] == '/') {
          --depth;
          sql += 2;
        } else
          ++sql;
      }
      continue;
    }
    break;
  }
  while (sql < end && (isalpha((unsigned char)*sql) || *sql == '_')) {
    if (size + 1u < capacity)
      word[size++] = (char)toupper((unsigned char)*sql);
    ++sql;
  }
  word[size] = '\0';
  return sql;
}
static int pg_transaction_control(const char *sql, size_t size) {
  char first[24], second[24], third[24], fourth[24], fifth[24];
  const char *next = pg_word(sql, sql + size, first, sizeof(first));
  if (strcmp(first, "BEGIN") == 0 || strcmp(first, "START") == 0 ||
      strcmp(first, "COMMIT") == 0 || strcmp(first, "END") == 0 ||
      strcmp(first, "ROLLBACK") == 0 || strcmp(first, "ABORT") == 0 ||
      strcmp(first, "SAVEPOINT") == 0 || strcmp(first, "RELEASE") == 0 ||
      strcmp(first, "PREPARE") == 0)
    return 1;
  if (strcmp(first, "SET") == 0) {
    next = pg_word(next, sql + size, second, sizeof(second));
    if (strcmp(second, "TRANSACTION") == 0)
      return 1;
    if (strcmp(second, "SESSION") == 0) {
      next = pg_word(next, sql + size, third, sizeof(third));
      next = pg_word(next, sql + size, fourth, sizeof(fourth));
      (void)pg_word(next, sql + size, fifth, sizeof(fifth));
      if (strcmp(third, "CHARACTERISTICS") == 0 && strcmp(fourth, "AS") == 0 &&
          strcmp(fifth, "TRANSACTION") == 0)
        return 1;
    }
  }
  return 0;
}
static vectis_status pg_ready(pg_connection_state *state, vectis_error *error) {
  vectis_status status = pg_owner(state, error);
  if (status != VECTIS_OK)
    return status;
  if (!state->ready) {
    if (state->declaration)
      return pg_connect_wait(state, error);
    return pg_failure(error, VECTIS_ERR_STATE,
                      "PostgreSQL connection is pending; advance poll first");
  }
  return VECTIS_OK;
}
static vectis_status pg_cursor_owner(pg_cursor_state *cursor,
                                     vectis_error *error) {
  if (cursor == NULL)
    return pg_failure(error, VECTIS_ERR_INVALID, "missing PostgreSQL cursor");
  if (cursor->owner->process != getpid() ||
      !pthread_equal(cursor->owner->thread, pthread_self()))
    return pg_failure(error, VECTIS_ERR_STATE,
                      "PostgreSQL cursor belongs to another process/thread");
  if (cursor->diagnostic.error.code != VECTIS_OK) {
    if (error != NULL)
      *error = cursor->diagnostic.error;
    return cursor->diagnostic.error.code;
  }
  if (cursor->owner->closed || cursor->generation != cursor->owner->generation)
    return pg_failure(error, VECTIS_ERR_STATE,
                      "PostgreSQL cursor was invalidated");
  return VECTIS_OK;
}
static vectis_status pg_cursor_failed(pg_cursor_state *cursor,
                                      vectis_status status, const char *message,
                                      int disconnect, vectis_error *error) {
  if (cursor->diagnostic.error.code == VECTIS_OK)
    pg_failure(&cursor->diagnostic.error, status, message);
  cursor->owner->last = cursor->diagnostic;
  if (cursor->owner->active &&
      cursor->owner->transaction_failure.error.code == VECTIS_OK)
    cursor->owner->transaction_failure = cursor->diagnostic;
  if (disconnect)
    pg_disconnect(cursor->owner);
  if (error != NULL)
    *error = cursor->diagnostic.error;
  return cursor->diagnostic.error.code;
}
static vectis_status pg_cursor_metadata(pg_cursor_state *cursor,
                                        const cpkt_postgres_result *result,
                                        vectis_error *error) {
  int count = cpkt_postgres_result_field_count(result), i;
  const char *name;
  size_t length, total = 0u;
  if (count < 0 || count > (int)VECTIS_SQL_MAX_COLUMNS)
    return pg_cursor_failed(cursor, VECTIS_ERR_INVALID,
                            "PostgreSQL column limit exceeded", 1, error);
  if (cursor->metadata) {
    if ((size_t)count != cursor->column_count)
      return pg_cursor_failed(cursor, VECTIS_ERR_STATE,
                              "PostgreSQL result metadata changed", 1, error);
    return VECTIS_OK;
  }
  cursor->column_count = (size_t)count;
  for (i = 0; i < count; ++i) {
    name = cpkt_postgres_result_field_name(result, i);
    if (name == NULL)
      return pg_cursor_failed(cursor, VECTIS_ERR_STATE,
                              "missing PostgreSQL column name", 1, error);
    length = strlen(name);
    if (length >= VECTIS_SQL_MAX_VALUE_BYTES - total)
      return pg_cursor_failed(cursor, VECTIS_ERR_INVALID,
                              "PostgreSQL metadata byte limit exceeded", 1,
                              error);
    total += length + 1u;
    cursor->names[i] = (char *)malloc(length + 1u);
    if (cursor->names[i] == NULL)
      return pg_cursor_failed(cursor, VECTIS_ERR_NOMEM,
                              "copying PostgreSQL columns", 1, error);
    memcpy(cursor->names[i], name, length + 1u);
    cursor->columns[i].name = cursor->names[i];
    cursor->columns[i].oid = cpkt_postgres_result_field_type(result, i);
    cursor->columns[i].format = cpkt_postgres_result_field_format(result, i);
  }
  cursor->metadata = 1;
  return VECTIS_OK;
}
static vectis_status pg_cursor_row(pg_cursor_state *cursor,
                                   vectis_error *error) {
  size_t i, total = 0u;
  int size;
  if (cpkt_postgres_result_row_count(cursor->row) != 1)
    return pg_cursor_failed(cursor, VECTIS_ERR_STATE,
                            "expected one PostgreSQL row per result", 1, error);
  for (i = 0u; i < cursor->column_count; ++i) {
    memset(&cursor->values[i], 0, sizeof(cursor->values[i]));
    if (cpkt_postgres_result_value_is_null(cursor->row, 0, (int)i))
      cursor->values[i].type = VECTIS_SQL_NULL;
    else {
      size = cpkt_postgres_result_value_length(cursor->row, 0, (int)i);
      if (size < 0 || (size_t)size > VECTIS_SQL_MAX_VALUE_BYTES - total)
        return pg_cursor_failed(cursor, VECTIS_ERR_INVALID,
                                "PostgreSQL cell/row byte limit exceeded", 1,
                                error);
      total += (size_t)size;
      cursor->values[i].type = VECTIS_SQL_TEXT;
      cursor->values[i].size = (size_t)size;
      cursor->values[i].data =
          cpkt_postgres_result_value(cursor->row, 0, (int)i);
    }
  }
  return VECTIS_OK;
}
static void pg_server_error(pg_cursor_state *cursor,
                            cpkt_postgres_result *result,
                            cpkt_postgres_result_status status) {
  const char *sqlstate;
  if (cursor->diagnostic.error.code != VECTIS_OK)
    return;
  pg_failure(&cursor->diagnostic.error, VECTIS_ERR_STATE,
             cpkt_postgres_result_error_message(result));
  cursor->diagnostic.error.dependency_code = (long)status;
  sqlstate =
      cpkt_postgres_result_error_field(result, CPKT_POSTGRES_DIAG_SQLSTATE);
  if (sqlstate != NULL) {
    strncpy(cursor->diagnostic.sqlstate, sqlstate, 5u);
    cursor->diagnostic.sqlstate[5] = '\0';
  }
  if (cursor->owner->active &&
      cursor->owner->transaction_failure.error.code == VECTIS_OK)
    cursor->owner->transaction_failure = cursor->diagnostic;
}
vectis_status vectis_pg_cursor_next_ready(vectis_pg_cursor *receiver,
                                          vectis_pg_step *step,
                                          const vectis_sql_value **row,
                                          size_t *count, vectis_error *error) {
  pg_cursor_state *cursor =
      receiver == NULL ? NULL : (pg_cursor_state *)receiver->impl;
  pg_connection_state *state;
  cpkt_postgres_result *result = NULL;
  cpkt_postgres_retrieval_status retrieved;
  cpkt_postgres_result_status outcome;
  cpkt_postgres_retrieval_error retrieval;
  vectis_status status;
  int flush;
  const char *command, *affected;
  if (step != NULL)
    *step = VECTIS_PG_PENDING;
  if (row != NULL)
    *row = NULL;
  if (count != NULL)
    *count = 0u;
  if (step == NULL || row == NULL || count == NULL)
    return pg_failure(error, VECTIS_ERR_INVALID,
                      "missing PostgreSQL cursor outputs");
  /* SQL errors must still drain. Other latched failures never progress. */
  if (cursor != NULL && cursor->terminal && !cursor->drained &&
      cursor->diagnostic.error.code != VECTIS_OK &&
      cursor->owner->native != NULL &&
      cursor->generation == cursor->owner->generation) {
    if (cursor->owner->process != getpid() ||
        !pthread_equal(cursor->owner->thread, pthread_self()))
      return pg_failure(error, VECTIS_ERR_STATE, "foreign PostgreSQL cursor");
  } else {
    status = pg_cursor_owner(cursor, error);
    if (status != VECTIS_OK)
      return status;
  }
  state = cursor->owner;
  if (cursor->drained) {
    *step = VECTIS_PG_EOF;
    return VECTIS_OK;
  }
  if (cursor->row != NULL) {
    cpkt_postgres_result_free(cursor->row);
    cursor->row = NULL;
  }
  flush = cpkt_postgres_flush(state->native);
  if (flush < 0)
    return pg_cursor_failed(cursor, VECTIS_ERR_STATE,
                            cpkt_postgres_error_message(state->native), 1,
                            error);
  cursor->flushing = flush != 0;
  if (!cpkt_postgres_consume_input(state->native))
    return pg_cursor_failed(cursor, VECTIS_ERR_STATE,
                            cpkt_postgres_error_message(state->native), 1,
                            error);
  for (;;) {
    if (cpkt_postgres_is_busy(state->native))
      return VECTIS_OK;
    retrieved =
        cpkt_postgres_get_result_checked(state->native, &result, &retrieval);
    if (retrieved == CPKT_POSTGRES_RETRIEVAL_FAILED) {
      cursor->diagnostic.retrieval = retrieval;
      if (cursor->diagnostic.error.code == VECTIS_OK) {
        memcpy(cursor->diagnostic.sqlstate, retrieval.sqlstate,
               sizeof(cursor->diagnostic.sqlstate));
        cursor->diagnostic.sqlstate[5] = '\0';
      }
      return pg_cursor_failed(cursor, VECTIS_ERR_STATE,
                              retrieval.server_message[0]
                                  ? retrieval.server_message
                                  : cpkt_postgres_error_message(state->native),
                              1, error);
    }
    if (retrieved == CPKT_POSTGRES_RETRIEVAL_DRAINED) {
      cursor->drained = 1;
      state->cursor = NULL;
      if (cursor->diagnostic.error.code != VECTIS_OK) {
        state->last = cursor->diagnostic;
        if (error != NULL)
          *error = cursor->diagnostic.error;
        return cursor->diagnostic.error.code;
      }
      if (!cursor->terminal)
        return pg_cursor_failed(cursor, VECTIS_ERR_STATE,
                                "PostgreSQL drained without terminal success",
                                1, error);
      *step = VECTIS_PG_EOF;
      return VECTIS_OK;
    }
    outcome = cpkt_postgres_result_status_get(result);
    if (outcome == CPKT_POSTGRES_RESULT_FATAL_ERROR ||
        outcome == CPKT_POSTGRES_RESULT_NONFATAL_ERROR) {
      pg_server_error(cursor, result, outcome);
      cursor->terminal = 1;
      cpkt_postgres_result_free(result);
      result = NULL;
      continue;
    }
    if (outcome != CPKT_POSTGRES_RESULT_SINGLE_TUPLE &&
        outcome != CPKT_POSTGRES_RESULT_TUPLES_OK &&
        outcome != CPKT_POSTGRES_RESULT_COMMAND_OK) {
      pg_failure(&cursor->diagnostic.error, VECTIS_ERR_STATE,
                 "unsupported PostgreSQL COPY/pipeline/empty result");
      cursor->diagnostic.error.dependency_code = (long)outcome;
      cpkt_postgres_result_free(result);
      return pg_cursor_failed(
          cursor, VECTIS_ERR_STATE,
          "unsupported PostgreSQL COPY/pipeline/empty result", 1, error);
    }
    if (cursor->terminal) {
      cpkt_postgres_result_free(result);
      return pg_cursor_failed(
          cursor, VECTIS_ERR_STATE,
          "unexpected PostgreSQL result after terminal status", 1, error);
    }
    status = pg_cursor_metadata(cursor, result, error);
    if (status != VECTIS_OK) {
      cpkt_postgres_result_free(result);
      return status;
    }
    if (outcome == CPKT_POSTGRES_RESULT_SINGLE_TUPLE) {
      cursor->row = result;
      status = pg_cursor_row(cursor, error);
      if (status != VECTIS_OK)
        return status;
      *step = VECTIS_PG_ROW;
      *row = cursor->values;
      *count = cursor->column_count;
      return VECTIS_OK;
    }
    command = cpkt_postgres_result_command_status(result);
    affected = cpkt_postgres_result_command_tuples(result);
    if (command == NULL || affected == NULL ||
        strlen(command) >= sizeof(cursor->command.command) ||
        strlen(affected) >= sizeof(cursor->command.affected)) {
      cpkt_postgres_result_free(result);
      return pg_cursor_failed(cursor, VECTIS_ERR_STATE,
                              "invalid PostgreSQL command metadata", 1, error);
    }
    strcpy(cursor->command.command, command);
    strcpy(cursor->command.affected, affected);
    cursor->terminal = 1;
    cpkt_postgres_result_free(result);
    result = NULL;
  }
}
vectis_status vectis_pg_cursor_readiness(vectis_pg_cursor *receiver,
                                         vectis_pg_readiness *out,
                                         vectis_error *error) {
  pg_cursor_state *cursor =
      receiver == NULL ? NULL : (pg_cursor_state *)receiver->impl;
  vectis_status status = pg_cursor_owner(cursor, error);
  if (status != VECTIS_OK &&
      !(cursor != NULL && cursor->terminal && !cursor->drained &&
        cursor->owner->native != NULL && cursor->owner->process == getpid() &&
        pthread_equal(cursor->owner->thread, pthread_self())))
    return status;
  return vectis_pg_connection_readiness(&cursor->owner->receiver, out, error);
}
vectis_status vectis_pg_cursor_next(vectis_pg_cursor *receiver,
                                    vectis_pg_step *step,
                                    const vectis_sql_value **row, size_t *count,
                                    vectis_error *error) {
  pg_cursor_state *cursor =
      receiver == NULL ? NULL : (pg_cursor_state *)receiver->impl;
  vectis_pg_readiness wait;
  int64_t started, now, deadline, elapsed;
  vectis_status status;
  if (cursor == NULL)
    return pg_failure(error, VECTIS_ERR_INVALID, "missing PostgreSQL cursor");
  status = pg_clock(&started, error);
  if (status != VECTIS_OK)
    return status;
  deadline = cursor->budget > INT64_MAX - started ? INT64_MAX
                                                  : started + cursor->budget;
  for (;;) {
    status = vectis_pg_cursor_next_ready(receiver, step, row, count, error);
    if (status != VECTIS_OK || *step != VECTIS_PG_PENDING)
      break;
    status = vectis_pg_cursor_readiness(receiver, &wait, error);
    if (status == VECTIS_OK)
      status = pg_wait(cursor->owner, wait.read, wait.write, deadline, error);
    if (status != VECTIS_OK) {
      status = pg_cursor_failed(
          cursor, status,
          error == NULL ? "PostgreSQL I/O failed" : error->message, 1, error);
      break;
    }
  }
  if (pg_clock(&now, NULL) == VECTIS_OK) {
    elapsed = now > started ? now - started : 0;
    cursor->budget = elapsed > cursor->budget ? 0 : cursor->budget - elapsed;
  }
  return status;
}
vectis_status vectis_pg_cursor_columns(vectis_pg_cursor *receiver,
                                       const vectis_pg_column **columns,
                                       size_t *count, vectis_error *error) {
  pg_cursor_state *cursor =
      receiver == NULL ? NULL : (pg_cursor_state *)receiver->impl;
  vectis_status status = pg_cursor_owner(cursor, error);
  if (columns != NULL)
    *columns = NULL;
  if (count != NULL)
    *count = 0u;
  if (status != VECTIS_OK)
    return status;
  if (columns == NULL || count == NULL || !cursor->metadata)
    return pg_failure(error, VECTIS_ERR_STATE,
                      "PostgreSQL columns require the first received result");
  *columns = cursor->columns;
  *count = cursor->column_count;
  return VECTIS_OK;
}
const vectis_pg_diagnostic *
vectis_pg_cursor_diagnostic(const vectis_pg_cursor *receiver) {
  const pg_cursor_state *cursor =
      receiver == NULL ? NULL : (const pg_cursor_state *)receiver->impl;
  return cursor == NULL ? NULL : &cursor->diagnostic;
}
vectis_status vectis_pg_cursor_close(vectis_pg_cursor *receiver,
                                     vectis_error *error) {
  pg_cursor_state *cursor;
  size_t i;
  if (receiver == NULL)
    return VECTIS_OK;
  cursor = (pg_cursor_state *)receiver->impl;
  if (cursor->owner->process != getpid() ||
      !pthread_equal(cursor->owner->thread, pthread_self()))
    return pg_failure(
        error, VECTIS_ERR_STATE,
        "cannot close PostgreSQL cursor from another process/thread");
  if (cursor->owner->cursor == cursor && !cursor->drained)
    pg_disconnect(cursor->owner);
  if (cursor->row != NULL)
    cpkt_postgres_result_free(cursor->row);
  for (i = 0u; i < cursor->column_count; ++i)
    free(cursor->names[i]);
  pg_unref(cursor->owner);
  free(cursor);
  return VECTIS_OK;
}
static vectis_status
pg_send(pg_connection_state *state, const char *sql, size_t sql_size,
        const char *name, const vectis_pg_parameter *parameters, size_t count,
        const cpkt_postgres_oid *prepare_types, size_t prepare_count,
        int preparing, int64_t timeout, pg_cursor_state **out,
        vectis_error *error) {
  pg_cursor_state *cursor;
  char *command = NULL;
  pg_parameter_scratch *scratch;
  size_t i, total = 0u;
  int sent;
  vectis_status status;
  *out = NULL;
  if (timeout < -1 || count > VECTIS_SQL_MAX_PARAMETERS ||
      (count != 0u && parameters == NULL) ||
      prepare_count > VECTIS_SQL_MAX_PARAMETERS ||
      (prepare_count != 0u && prepare_types == NULL) ||
      ((name == NULL || preparing) &&
       (sql == NULL || sql_size == 0u || sql_size > VECTIS_SQL_MAX_BYTES ||
        memchr(sql, 0, sql_size) != NULL)))
    return pg_failure(error, VECTIS_ERR_INVALID,
                      "invalid PostgreSQL SQL/parameters/timeout");
  status = pg_ready(state, error);
  if (status != VECTIS_OK)
    return status;
  if (state->cursor != NULL)
    return pg_failure(error, VECTIS_ERR_STATE,
                      "PostgreSQL connection already owns an active operation");
  memset(&state->last, 0, sizeof(state->last));
  if (state->active && !state->control && sql != NULL &&
      pg_transaction_control(sql, sql_size)) {
    state->violated = 1;
    return pg_failure(error, VECTIS_ERR_STATE,
                      "manual transaction control is forbidden in owned "
                      "PostgreSQL callbacks");
  }
  scratch = &state->parameters;
  memset(scratch->copies, 0, sizeof(scratch->copies));
  for (i = 0u; i < count; ++i) {
    if ((parameters[i].binary != 0 && parameters[i].binary != 1) ||
        (parameters[i].is_null != 0 && parameters[i].is_null != 1) ||
        parameters[i].size > VECTIS_SQL_MAX_VALUE_BYTES - total ||
        parameters[i].size > INT_MAX ||
        (!parameters[i].is_null && parameters[i].size != 0u &&
         parameters[i].data == NULL) ||
        (!parameters[i].is_null && !parameters[i].binary &&
         parameters[i].size != 0u &&
         memchr(parameters[i].data, 0, parameters[i].size) != NULL)) {
      status = pg_failure(error, VECTIS_ERR_INVALID,
                          "invalid PostgreSQL parameter/aggregate byte limit");
      goto cleanup;
    }
    total += parameters[i].size;
    scratch->types[i] = parameters[i].oid;
    scratch->lengths[i] = (int)parameters[i].size;
    scratch->formats[i] = parameters[i].binary;
    scratch->values[i] =
        parameters[i].is_null ? NULL : (const char *)parameters[i].data;
    if (!parameters[i].is_null && !parameters[i].binary) {
      scratch->copies[i] = (char *)malloc(parameters[i].size + 1u);
      if (scratch->copies[i] == NULL) {
        status = pg_failure(error, VECTIS_ERR_NOMEM,
                            "copying PostgreSQL text parameter");
        goto cleanup;
      }
      if (parameters[i].size != 0u)
        memcpy(scratch->copies[i], parameters[i].data, parameters[i].size);
      scratch->copies[i][parameters[i].size] = '\0';
      scratch->values[i] = scratch->copies[i];
    } else if (!parameters[i].is_null && scratch->values[i] == NULL)
      scratch->values[i] = "";
  }
  if (sql != NULL) {
    command = (char *)malloc(sql_size + 1u);
    if (command == NULL) {
      status = pg_failure(error, VECTIS_ERR_NOMEM, "copying PostgreSQL SQL");
      goto cleanup;
    }
    memcpy(command, sql, sql_size);
    command[sql_size] = '\0';
  }
  cursor = (pg_cursor_state *)calloc(1u, sizeof(*cursor));
  if (cursor == NULL) {
    status =
        pg_failure(error, VECTIS_ERR_NOMEM, "allocating PostgreSQL cursor");
    goto cleanup;
  }
  cursor->owner = state;
  cursor->generation = state->generation;
  cursor->budget = timeout < 0 ? state->io_timeout : timeout;
  cursor->preparing = preparing;
  cursor->receiver.next_ready = vectis_pg_cursor_next_ready;
  cursor->receiver.next = vectis_pg_cursor_next;
  cursor->receiver.columns = vectis_pg_cursor_columns;
  cursor->receiver.readiness = vectis_pg_cursor_readiness;
  cursor->receiver.diagnostic = vectis_pg_cursor_diagnostic;
  cursor->receiver.close = vectis_pg_cursor_close;
  cursor->receiver.impl = cursor;
  if (preparing)
    sent = cpkt_postgres_send_prepare(state->native, name, command,
                                      (int)prepare_count, prepare_types);
  else if (name != NULL)
    sent = cpkt_postgres_send_query_prepared(state->native, name, (int)count,
                                             scratch->values, scratch->lengths,
                                             scratch->formats, 0);
  else
    sent = cpkt_postgres_send_query_params(
        state->native, command, (int)count, scratch->types, scratch->values,
        scratch->lengths, scratch->formats, 0);
  if (!sent ||
      (!preparing && !cpkt_postgres_set_single_row_mode(state->native))) {
    free(cursor);
    status = pg_native_failure(state, error);
    goto cleanup;
  }
  ++state->references;
  state->cursor = cursor;
  cursor->flushing = 1;
  *out = cursor;
  status = VECTIS_OK;
cleanup:
  for (i = 0u; i < count; ++i)
    free(scratch->copies[i]);
  free(command);
  return status;
}
vectis_status vectis_pg_connection_query(vectis_pg_connection *receiver,
                                         const char *sql, size_t sql_size,
                                         const vectis_pg_parameter *parameters,
                                         size_t count, int64_t timeout,
                                         vectis_pg_cursor **out,
                                         vectis_error *error) {
  pg_cursor_state *cursor;
  vectis_status status;
  if (out != NULL)
    *out = NULL;
  if (out == NULL || receiver == NULL)
    return pg_failure(error, VECTIS_ERR_INVALID,
                      "missing PostgreSQL query output/receiver");
  status = pg_send((pg_connection_state *)receiver->impl, sql, sql_size, NULL,
                   parameters, count, NULL, 0u, 0, timeout, &cursor, error);
  if (status == VECTIS_OK)
    *out = &cursor->receiver;
  return status;
}
static vectis_status pg_drain(pg_cursor_state *cursor, vectis_error *error) {
  vectis_pg_step step = VECTIS_PG_PENDING;
  const vectis_sql_value *row;
  size_t count;
  vectis_status status = VECTIS_OK;
  while (status == VECTIS_OK && step != VECTIS_PG_EOF)
    status =
        vectis_pg_cursor_next(&cursor->receiver, &step, &row, &count, error);
  return status;
}
vectis_status vectis_pg_connection_exec(vectis_pg_connection *receiver,
                                        const char *sql, size_t sql_size,
                                        const vectis_pg_parameter *parameters,
                                        size_t count, int64_t timeout,
                                        vectis_pg_command *command,
                                        vectis_error *error) {
  vectis_pg_cursor *cursor;
  vectis_status status;
  if (command != NULL)
    memset(command, 0, sizeof(*command));
  status = vectis_pg_connection_query(receiver, sql, sql_size, parameters,
                                      count, timeout, &cursor, error);
  if (status != VECTIS_OK)
    return status;
  status = pg_drain((pg_cursor_state *)cursor->impl, error);
  if (status != VECTIS_OK)
    ((pg_connection_state *)receiver->impl)->last =
        ((pg_cursor_state *)cursor->impl)->diagnostic;
  if (status == VECTIS_OK && command != NULL)
    *command = ((pg_cursor_state *)cursor->impl)->command;
  vectis_pg_cursor_close(cursor, NULL);
  return status;
}
vectis_status vectis_pg_connection_prepare(
    vectis_pg_connection *receiver, const char *name, const char *sql,
    size_t sql_size, const cpkt_postgres_oid *types, size_t count,
    int64_t timeout, vectis_pg_statement **out, vectis_error *error) {
  pg_statement_state *statement;
  pg_cursor_state *cursor;
  pg_connection_state *state =
      receiver == NULL ? NULL : (pg_connection_state *)receiver->impl;
  vectis_status status;
  if (out != NULL)
    *out = NULL;
  if (state == NULL || out == NULL || name == NULL || name[0] == '\0' ||
      strlen(name) > 1024u)
    return pg_failure(error, VECTIS_ERR_INVALID,
                      "invalid PostgreSQL prepared name/output");
  statement = (pg_statement_state *)calloc(1u, sizeof(*statement));
  if (statement == NULL)
    return pg_failure(error, VECTIS_ERR_NOMEM,
                      "allocating PostgreSQL prepared view");
  statement->name = (char *)malloc(strlen(name) + 1u);
  if (statement->name == NULL) {
    free(statement);
    return pg_failure(error, VECTIS_ERR_NOMEM,
                      "copying PostgreSQL prepared name");
  }
  strcpy(statement->name, name);
  status = pg_send(state, sql, sql_size, name, NULL, 0u, types, count, 1,
                   timeout, &cursor, error);
  if (status == VECTIS_OK) {
    status = pg_drain(cursor, error);
    vectis_pg_cursor_close(&cursor->receiver, NULL);
  }
  if (status != VECTIS_OK) {
    free(statement->name);
    free(statement);
    return status;
  }
  statement->owner = state;
  ++state->references;
  statement->generation = state->generation;
  statement->control = pg_transaction_control(sql, sql_size);
  statement->receiver.query = vectis_pg_statement_query;
  statement->receiver.close = vectis_pg_statement_close;
  statement->receiver.impl = statement;
  *out = &statement->receiver;
  return VECTIS_OK;
}
vectis_status vectis_pg_statement_query(vectis_pg_statement *receiver,
                                        const vectis_pg_parameter *parameters,
                                        size_t count, int64_t timeout,
                                        vectis_pg_cursor **out,
                                        vectis_error *error) {
  pg_statement_state *statement =
      receiver == NULL ? NULL : (pg_statement_state *)receiver->impl;
  pg_cursor_state *cursor;
  vectis_status status;
  if (out != NULL)
    *out = NULL;
  if (statement == NULL || out == NULL)
    return pg_failure(error, VECTIS_ERR_INVALID,
                      "missing PostgreSQL prepared query output");
  status = pg_owner(statement->owner, error);
  if (status != VECTIS_OK)
    return status;
  if (statement->generation != statement->owner->generation)
    return pg_failure(error, VECTIS_ERR_STATE,
                      "PostgreSQL prepared view was invalidated");
  if (statement->control && statement->owner->active &&
      !statement->owner->control) {
    statement->owner->violated = 1;
    return pg_failure(
        error, VECTIS_ERR_STATE,
        "prepared transaction control is forbidden in owned callbacks");
  }
  status = pg_send(statement->owner, NULL, 0u, statement->name, parameters,
                   count, NULL, 0u, 0, timeout, &cursor, error);
  if (status == VECTIS_OK)
    *out = &cursor->receiver;
  return status;
}
vectis_status vectis_pg_statement_close(vectis_pg_statement *receiver,
                                        vectis_error *error) {
  pg_statement_state *statement;
  if (receiver == NULL)
    return VECTIS_OK;
  statement = (pg_statement_state *)receiver->impl;
  if (statement->owner->process != getpid() ||
      !pthread_equal(statement->owner->thread, pthread_self()))
    return pg_failure(
        error, VECTIS_ERR_STATE,
        "cannot close PostgreSQL prepared view from another process/thread");
  pg_unref(statement->owner);
  free(statement->name);
  free(statement);
  return VECTIS_OK;
}

const vectis_pg_diagnostic *
vectis_pg_connection_diagnostic(const vectis_pg_connection *receiver) {
  const pg_connection_state *state =
      receiver == NULL ? NULL : (const pg_connection_state *)receiver->impl;
  return state == NULL ? NULL : &state->last;
}
static vectis_status pg_not_started(pg_connection_state *state,
                                    vectis_error *error, vectis_status status) {
  if (error != NULL) {
    strcpy(error->detail, "not_started");
    if (state != NULL) {
      memset(&state->diagnostic, 0, sizeof(state->diagnostic));
      state->diagnostic.primary.error = *error;
    }
  }
  return status;
}
vectis_status vectis_pg_connection_transaction(
    vectis_pg_connection *receiver, const vectis_pg_transaction_config *config,
    vectis_pg_transaction_fn callback, void *context, vectis_error *error) {
  pg_connection_state *state =
      receiver == NULL ? NULL : (pg_connection_state *)receiver->impl;
  vectis_pg_transaction_config defaults;
  vectis_pg_diagnostic primary, cleanup;
  vectis_error local;
  vectis_status status, rollback;
  vectis_pg_command command;
  const char *isolation;
  char begin[96];
  int commit_attempted = 0, unexpected_idle = 0, had_native;
  if (error == NULL)
    error = &local;
  vectis_pg_transaction_config_init(&defaults);
  if (config == NULL)
    config = &defaults;
  isolation = config->isolation;
  if (state == NULL || callback == NULL || isolation == NULL ||
      (config->read_only != 0 && config->read_only != 1) ||
      (strcmp(isolation, "read_committed") != 0 &&
       strcmp(isolation, "repeatable_read") != 0 &&
       strcmp(isolation, "serializable") != 0))
    return pg_not_started(
        state, error,
        pg_failure(error, VECTIS_ERR_INVALID,
                   "invalid PostgreSQL transaction configuration"));
  status = pg_ready(state, error);
  if (status != VECTIS_OK)
    return pg_not_started(state, error, status);
  if (state->active || state->cursor != NULL ||
      cpkt_postgres_transaction_status_get(state->native) !=
          CPKT_POSTGRES_TRANSACTION_IDLE) {
    if (state->active)
      state->violated = 1;
    return pg_not_started(
        state, error,
        pg_failure(
            error, VECTIS_ERR_STATE,
            "PostgreSQL transaction nesting/active cursor is forbidden"));
  }
  memset(&state->diagnostic, 0, sizeof(state->diagnostic));
  memset(&primary, 0, sizeof(primary));
  memset(&cleanup, 0, sizeof(cleanup));
  strcpy(begin, "BEGIN ISOLATION LEVEL ");
  strcat(begin, strcmp(isolation, "serializable") == 0      ? "SERIALIZABLE"
                : strcmp(isolation, "repeatable_read") == 0 ? "REPEATABLE READ"
                                                            : "READ COMMITTED");
  strcat(begin, config->read_only ? " READ ONLY" : " READ WRITE");
  status = vectis_pg_connection_exec(receiver, begin, strlen(begin), NULL, 0u,
                                     -1, &command, &primary.error);
  if (status != VECTIS_OK) {
    if (state->last.error.code != VECTIS_OK)
      primary = state->last;
    strcpy(primary.error.detail, "not_started");
    state->diagnostic.primary = primary;
    *error = primary.error;
    return status;
  }
  state->active = 1;
  state->violated = 0;
  memset(&state->transaction_failure, 0, sizeof(state->transaction_failure));
  status = callback(receiver, context, &primary.error);
  if (status != VECTIS_OK && primary.error.code == VECTIS_OK)
    pg_failure(&primary.error, status,
               "PostgreSQL transaction callback failed");
  if (status != VECTIS_OK && state->last.error.code == primary.error.code &&
      strcmp(state->last.error.message, primary.error.message) == 0)
    primary = state->last;
  if (state->cursor != NULL) {
    pg_disconnect(state);
    state->violated = 1;
  }
  if (state->native != NULL)
    unexpected_idle = cpkt_postgres_transaction_status_get(state->native) ==
                      CPKT_POSTGRES_TRANSACTION_IDLE;
  if (status == VECTIS_OK &&
      (state->violated || state->native == NULL ||
       cpkt_postgres_transaction_status_get(state->native) !=
           CPKT_POSTGRES_TRANSACTION_IN_TRANSACTION)) {
    if (state->transaction_failure.error.code != VECTIS_OK) {
      primary = state->transaction_failure;
      status = primary.error.code;
    } else
      status = pg_failure(&primary.error, VECTIS_ERR_STATE,
                          "transaction callback violated ownership or left an "
                          "invalid/aborted transaction");
  }
  state->control = 1;
  if (status == VECTIS_OK) {
    commit_attempted = 1;
    status = vectis_pg_connection_exec(receiver, "COMMIT", 6u, NULL, 0u, -1,
                                       &command, &primary.error);
    if (status != VECTIS_OK && state->last.error.code != VECTIS_OK)
      primary = state->last;
    if (status == VECTIS_OK &&
        (strcmp(command.command, "COMMIT") != 0 || state->native == NULL ||
         cpkt_postgres_transaction_status_get(state->native) !=
             CPKT_POSTGRES_TRANSACTION_IDLE))
      status = pg_failure(
          &primary.error, VECTIS_ERR_STATE,
          "PostgreSQL commit did not acknowledge a committed transaction");
    if (status == VECTIS_OK)
      strcpy(primary.error.detail, "committed");
  }
  if (status != VECTIS_OK) {
    had_native = state->native != NULL;
    if (had_native && commit_attempted && primary.sqlstate[0] != '\0' &&
        cpkt_postgres_transaction_status_get(state->native) ==
            CPKT_POSTGRES_TRANSACTION_IDLE)
      rollback = VECTIS_OK; /* Server already acknowledged failed COMMIT. */
    else if (had_native)
      rollback = vectis_pg_connection_exec(receiver, "ROLLBACK", 8u, NULL, 0u,
                                           -1, &command, &cleanup.error);
    else
      rollback = pg_failure(
          &cleanup.error, VECTIS_ERR_STATE,
          "cannot acknowledge rollback on a closed PostgreSQL connection");
    if (rollback != VECTIS_OK && had_native &&
        state->last.error.code != VECTIS_OK)
      cleanup = state->last;
    if (rollback == VECTIS_OK && state->native != NULL &&
        cpkt_postgres_transaction_status_get(state->native) ==
            CPKT_POSTGRES_TRANSACTION_IDLE &&
        !unexpected_idle && (!commit_attempted || primary.sqlstate[0] != '\0'))
      strcpy(primary.error.detail, "rolled_back");
    else {
      strcpy(primary.error.detail, "unknown");
      if (rollback == VECTIS_OK)
        pg_failure(&cleanup.error, VECTIS_ERR_STATE,
                   "transaction outcome was not acknowledged");
      state->diagnostic.cleanup_failed = 1;
      state->diagnostic.cleanup = cleanup;
      pg_disconnect(state);
    }
  }
  state->active = 0;
  state->control = 0;
  state->diagnostic.primary = primary;
  *error = primary.error;
  return status;
}
const vectis_pg_transaction_diagnostic *
vectis_pg_connection_transaction_diagnostic(
    const vectis_pg_connection *receiver) {
  const pg_connection_state *state =
      receiver == NULL ? NULL : (const pg_connection_state *)receiver->impl;
  return state == NULL ? NULL : &state->diagnostic;
}
