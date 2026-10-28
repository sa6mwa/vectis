#include "vectis_sql_lifecycle.h"
#include <cpkt/sqlite.h>
#include <vectis/sqlite.h>

#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct sqlite_state sqlite_state;
typedef struct statement_state {
  sqlite_state *owner;
  struct statement_state *next;
  cpkt_sqlite_statement *native;
  int cursor;
  int state; /* 0 unbound, 1 ready, 2 row, 3 EOF, -1 failed */
  unsigned long transaction;
  vectis_error failure;
} statement_state;
struct sqlite_state {
  vectis_sql_app_resource resource;
  vectis_sqlite_config config;
  char *path;
  char *name;
  int declaration;
  cpkt_sqlite *native;
  statement_state *statements;
  size_t references;
  pid_t process;
  pthread_t thread;
  unsigned long transaction;
  int active;
  int violated;
  int control;
  int tail_check;
  int invalid;
  vectis_sqlite_diagnostic diagnostic;
};

static vectis_status materialize(sqlite_state *, vectis_error *);

static vectis_status sql_error(vectis_error *error, vectis_status status,
                               int code, const char *message) {
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->code = status;
    error->source = VECTIS_ERROR_SOURCE_CPKT;
    error->dependency_code = code;
    if (message != NULL)
      strncpy(error->message, message, sizeof(error->message) - 1u);
  }
  return status;
}
static vectis_status native_error(sqlite_state *state, int code,
                                  vectis_error *error) {
  return sql_error(
      error, code == CPKT_SQLITE_NOMEM ? VECTIS_ERR_NOMEM : VECTIS_ERR_STATE,
      code, cpkt_sqlite_error(state->native));
}
static vectis_status check_owner(sqlite_state *state, vectis_error *error) {
  if (state->invalid)
    return sql_error(error, VECTIS_ERR_STATE, 0,
                     "SQLite connection was invalidated after cleanup failure");
  if (state->declaration) {
    if (state->resource.app == NULL ||
        !vectis_sql_app_running(state->resource.app))
      return sql_error(error, VECTIS_ERR_STATE, 0,
                       "SQLite declaration requires its started app");
    if (state->native == NULL && state->process == getpid() &&
        !pthread_equal(state->thread, pthread_self()))
      return sql_error(error, VECTIS_ERR_STATE, 0,
                       "SQLite declaration belongs to another thread");
    if (state->native == NULL)
      return materialize(state, error);
  }
  if (state->process != getpid() ||
      !pthread_equal(state->thread, pthread_self()))
    return sql_error(error, VECTIS_ERR_STATE, 0,
                     "SQLite receiver belongs to another process/thread");
  if (state->native == NULL)
    return sql_error(error, VECTIS_ERR_STATE, 0, "SQLite database is closed");
  return VECTIS_OK;
}
static int authorize(void *context, int action, const char *a, const char *b,
                     const char *c, const char *d) {
  sqlite_state *state = (sqlite_state *)context;
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  if (state->tail_check)
    return CPKT_SQLITE_DENY;
  if (state->active && !state->control &&
      (action == CPKT_SQLITE_TRANSACTION || action == CPKT_SQLITE_SAVEPOINT)) {
    state->violated = 1;
    return CPKT_SQLITE_DENY;
  }
  return CPKT_SQLITE_OK;
}
static int64_t from_i64(cpkt_sqlite_i64 value) {
  uint64_t bits = ((uint64_t)(value.high & 0xffffffffUL) << 32) |
                  (uint64_t)(value.low & 0xffffffffUL);
  if ((bits >> 63) != 0)
    return -1 - (int64_t)(~bits);
  return (int64_t)bits;
}
static cpkt_sqlite_i64 to_i64(int64_t value) {
  cpkt_sqlite_i64 result;
  uint64_t bits = (uint64_t)value;
  result.high = (unsigned long)(bits >> 32);
  result.low = (unsigned long)(bits & UINT64_C(0xffffffff));
  return result;
}
static void unref(sqlite_state *state) {
  if (--state->references == 0u) {
    free(state->path);
    free(state->name);
    free(state);
  }
}

void vectis_sqlite_config_init(vectis_sqlite_config *config) {
  memset(config, 0, sizeof(*config));
  config->mode = "readwrite_create";
}
vectis_status vectis_sqlite_open(const char *path,
                                 const vectis_sqlite_config *config,
                                 vectis_sqlite **out, vectis_error *error) {
  vectis_sqlite_config defaults;
  vectis_sqlite *receiver;
  sqlite_state *state;
  vectis_status status;
  int flags, code;
  if (out != NULL)
    *out = NULL;
  vectis_sqlite_config_init(&defaults);
  if (config == NULL)
    config = &defaults;
  if (out == NULL || path == NULL || config->mode == NULL ||
      config->busy_timeout_ms < 0 || (config->uri != 0 && config->uri != 1))
    return sql_error(error, VECTIS_ERR_INVALID, 0,
                     "invalid SQLite configuration");
  if (strcmp(config->mode, "readwrite_create") == 0)
    flags = CPKT_SQLITE_OPEN_READWRITE | CPKT_SQLITE_OPEN_CREATE;
  else if (strcmp(config->mode, "readwrite") == 0)
    flags = CPKT_SQLITE_OPEN_READWRITE;
  else if (strcmp(config->mode, "readonly") == 0)
    flags = CPKT_SQLITE_OPEN_READONLY;
  else
    return sql_error(error, VECTIS_ERR_INVALID, 0, "invalid SQLite open mode");
  if (config->uri)
    flags |= CPKT_SQLITE_OPEN_URI;
  receiver = (vectis_sqlite *)calloc(1u, sizeof(*receiver));
  state = (sqlite_state *)calloc(1u, sizeof(*state));
  if (receiver == NULL || state == NULL) {
    free(receiver);
    free(state);
    return sql_error(error, VECTIS_ERR_NOMEM, 0,
                     "SQLite owner allocation failed");
  }
  state->process = getpid();
  state->thread = pthread_self();
  state->references = 1u;
  state->native = cpkt_sqlite_open(path, flags, NULL);
  code = state->native == NULL ? CPKT_SQLITE_NOMEM
                               : cpkt_sqlite_error_code(state->native);
  if (code == CPKT_SQLITE_OK)
    code = cpkt_sqlite_extended_result_codes(state->native, 1);
  if (code == CPKT_SQLITE_OK)
    code = cpkt_sqlite_busy_timeout(state->native, config->busy_timeout_ms);
  if (code == CPKT_SQLITE_OK)
    code = cpkt_sqlite_set_authorizer(state->native, authorize, state);
  if (code != CPKT_SQLITE_OK) {
    status = state->native == NULL ? sql_error(error, VECTIS_ERR_NOMEM, code,
                                               "SQLite open failed")
                                   : native_error(state, code, error);
    if (state->native != NULL)
      cpkt_sqlite_close(state->native);
    free(state);
    free(receiver);
    return status;
  }
  receiver->impl = state;
  receiver->prepare = vectis_sqlite_prepare;
  receiver->exec = vectis_sqlite_exec;
  receiver->close = vectis_sqlite_close;
  *out = receiver;
  return VECTIS_OK;
}

vectis_status vectis_sqlite_prepare(vectis_sqlite *receiver, const char *sql,
                                    size_t size, int cursor,
                                    vectis_sqlite_statement **out,
                                    vectis_error *error) {
  sqlite_state *state;
  vectis_sqlite_statement *result;
  statement_state *statement;
  cpkt_sqlite_statement *native = NULL, *extra = NULL;
  const char *tail = NULL;
  int code;
  vectis_status status;
  if (out != NULL)
    *out = NULL;
  if (receiver == NULL || out == NULL || sql == NULL || size == 0u ||
      size > VECTIS_SQL_MAX_BYTES || memchr(sql, 0, size) != NULL)
    return sql_error(error, VECTIS_ERR_INVALID, 0, "invalid SQLite SQL input");
  state = (sqlite_state *)receiver->impl;
  status = check_owner(state, error);
  if (status != VECTIS_OK)
    return status;
  result = (vectis_sqlite_statement *)calloc(1u, sizeof(*result));
  statement = (statement_state *)calloc(1u, sizeof(*statement));
  if (result == NULL || statement == NULL) {
    free(result);
    free(statement);
    return sql_error(error, VECTIS_ERR_NOMEM, 0,
                     "SQLite statement allocation failed");
  }
  code = cpkt_sqlite_prepare(state->native, sql, (int)size, 0, &native, &tail);
  if (code == CPKT_SQLITE_OK && native == NULL) {
    status =
        sql_error(error, VECTIS_ERR_INVALID, 0, "SQL contains no statement");
  } else if (code != CPKT_SQLITE_OK) {
    status = native_error(state, code, error);
  } else {
    /* Parse remaining SQL under a deny-all authorizer, including PRAGMAs whose
     * effects can otherwise happen during preparation. */
    /* Do not replace the authorizer per operation: that would expire every
     * reusable statement on this connection. The owner is caller-serialized. */
    state->tail_check = 1;
    if (tail != NULL && tail < sql + size)
      code = cpkt_sqlite_prepare(state->native, tail, (int)(sql + size - tail),
                                 0, &extra, NULL);
    state->tail_check = 0;
    if (code != CPKT_SQLITE_OK || extra != NULL)
      status = sql_error(error, VECTIS_ERR_INVALID, code,
                         "exactly one SQL statement is required");
    else if (native->column_count(native) > (int)VECTIS_SQL_MAX_COLUMNS ||
             native->parameter_count(native) > (int)VECTIS_SQL_MAX_PARAMETERS)
      status = sql_error(error, VECTIS_ERR_INVALID, 0,
                         "SQLite column/parameter limit exceeded");
  }
  if (extra != NULL)
    cpkt_sqlite_statement_finalize(extra);
  if (status != VECTIS_OK) {
    if (native != NULL)
      cpkt_sqlite_statement_finalize(native);
    free(statement);
    free(result);
    return status;
  }
  statement->native = native;
  statement->owner = state;
  statement->next = state->statements;
  statement->cursor = cursor != 0;
  statement->transaction = state->active ? state->transaction : 0;
  state->statements = statement;
  state->references++;
  result->impl = statement;
  result->bind = vectis_sqlite_bind;
  result->next = vectis_sqlite_next;
  result->reset = vectis_sqlite_reset;
  result->close = vectis_sqlite_statement_close;
  *out = result;
  return VECTIS_OK;
}

static void invalidate(void *context) {
  sqlite_state *state = (sqlite_state *)context;
  statement_state *statement;
  int local = state->process == getpid() &&
              pthread_equal(state->thread, pthread_self());
  for (statement = state->statements; statement != NULL;
       statement = statement->next) {
    if (local && statement->native != NULL)
      cpkt_sqlite_statement_finalize(statement->native);
    statement->native = NULL;
    statement->state = 0;
  }
  if (local && state->native != NULL)
    cpkt_sqlite_close(state->native);
  state->native = NULL;
  state->invalid = 0;
}
static vectis_status idle(void *context, vectis_error *error) {
  sqlite_state *state = (sqlite_state *)context;
  if (state->process == getpid() &&
      !pthread_equal(state->thread, pthread_self()))
    return sql_error(error, VECTIS_ERR_STATE, 0,
                     "SQL app lifecycle requires the declaring thread");
  if (state->active) {
    state->violated = 1;
    return sql_error(
        error, VECTIS_ERR_STATE, 0,
        "app lifecycle is forbidden inside a SQL transaction callback");
  }
  return VECTIS_OK;
}
static vectis_status materialize(sqlite_state *state, vectis_error *error) {
  vectis_sqlite *temporary = NULL;
  sqlite_state *opened;
  vectis_status status;
  int code;
  status = vectis_sqlite_open(state->path, &state->config, &temporary, error);
  if (status != VECTIS_OK)
    return status;
  opened = (sqlite_state *)temporary->impl;
  code = cpkt_sqlite_set_authorizer(opened->native, authorize, state);
  if (code != CPKT_SQLITE_OK) {
    status = native_error(opened, code, error);
    vectis_sqlite_close(temporary, NULL);
    return status;
  }
  state->native = opened->native;
  state->process = getpid();
  state->thread = pthread_self();
  opened->native = NULL;
  vectis_sqlite_close(temporary, NULL);
  return VECTIS_OK;
}
vectis_status vectis_app_sqlite(vectis_app *app, const char *name,
                                const char *path,
                                const vectis_sqlite_config *config,
                                vectis_sqlite **out, vectis_error *error) {
  vectis_sqlite_config defaults;
  vectis_sqlite *receiver;
  sqlite_state *state;
  vectis_status status;
  if (out != NULL)
    *out = NULL;
  vectis_sqlite_config_init(&defaults);
  if (config == NULL)
    config = &defaults;
  if (out == NULL || name == NULL || name[0] == '\0' || path == NULL ||
      config->mode == NULL || config->busy_timeout_ms < 0 ||
      (config->uri != 0 && config->uri != 1) ||
      (strcmp(config->mode, "readwrite_create") != 0 &&
       strcmp(config->mode, "readwrite") != 0 &&
       strcmp(config->mode, "readonly") != 0))
    return sql_error(error, VECTIS_ERR_INVALID, 0,
                     "invalid SQLite declaration");
  receiver = (vectis_sqlite *)calloc(1u, sizeof(*receiver));
  state = (sqlite_state *)calloc(1u, sizeof(*state));
  if (receiver == NULL || state == NULL) {
    free(receiver);
    free(state);
    return sql_error(error, VECTIS_ERR_NOMEM, 0,
                     "SQLite declaration allocation failed");
  }
  state->references = 1u;
  state->declaration = 1;
  state->process = getpid();
  state->thread = pthread_self();
  state->path = (char *)malloc(strlen(path) + 1u);
  state->name = (char *)malloc(strlen(name) + 1u);
  if (state->path == NULL || state->name == NULL) {
    unref(state);
    free(receiver);
    return sql_error(error, VECTIS_ERR_NOMEM, 0,
                     "SQLite declaration copy failed");
  }
  strcpy(state->path, path);
  strcpy(state->name, name);
  state->config = *config;
  state->config.mode = strcmp(config->mode, "readonly") == 0 ? "readonly"
                       : strcmp(config->mode, "readwrite") == 0
                           ? "readwrite"
                           : "readwrite_create";
  state->resource.name = state->name;
  state->resource.context = state;
  state->resource.check_idle = idle;
  state->resource.invalidate = invalidate;
  status = vectis_sql_app_attach(app, &state->resource, error);
  if (status != VECTIS_OK) {
    unref(state);
    free(receiver);
    return status;
  }
  receiver->impl = state;
  receiver->prepare = vectis_sqlite_prepare;
  receiver->exec = vectis_sqlite_exec;
  receiver->close = vectis_sqlite_close;
  *out = receiver;
  return VECTIS_OK;
}

static vectis_status check_statement(vectis_sqlite_statement *receiver,
                                     statement_state **out,
                                     vectis_error *error) {
  vectis_status status;
  if (receiver == NULL)
    return sql_error(error, VECTIS_ERR_INVALID, 0,
                     "SQLite statement is required");
  *out = (statement_state *)receiver->impl;
  if ((*out)->native == NULL && (*out)->state != 3)
    return sql_error(error, VECTIS_ERR_STATE, 0, "SQLite statement is invalid");
  status = check_owner((*out)->owner, error);
  if (status != VECTIS_OK)
    return status;
  if ((*out)->state == -1) {
    if (error != NULL)
      *error = (*out)->failure;
    return (*out)->failure.code;
  }
  if ((*out)->native == NULL && (*out)->state != 3)
    return sql_error(error, VECTIS_ERR_STATE, 0, "SQLite statement is invalid");
  return VECTIS_OK;
}

vectis_status vectis_sqlite_bind(vectis_sqlite_statement *receiver,
                                 const vectis_sql_value *values, size_t count,
                                 vectis_error *error) {
  statement_state *state;
  vectis_status status;
  size_t i, bytes = 0u;
  int code;
  status = check_statement(receiver, &state, error);
  if (status != VECTIS_OK)
    return status;
  if (state->state != 0 || (values == NULL && count != 0u) ||
      count > VECTIS_SQL_MAX_PARAMETERS ||
      count != (size_t)state->native->parameter_count(state->native))
    return sql_error(error, VECTIS_ERR_INVALID, 0,
                     "bind requires reset and an exact parameter count");
  for (i = 0; i < count; ++i) {
    if (values[i].type < VECTIS_SQL_NULL || values[i].type > VECTIS_SQL_BLOB ||
        (values[i].type == VECTIS_SQL_REAL && !isfinite(values[i].real)))
      return sql_error(error, VECTIS_ERR_INVALID, 0,
                       "invalid SQL parameter value");
    if (values[i].type == VECTIS_SQL_TEXT ||
        values[i].type == VECTIS_SQL_BLOB) {
      if (values[i].size > VECTIS_SQL_MAX_VALUE_BYTES - bytes ||
          (values[i].data == NULL && values[i].size != 0u))
        return sql_error(error, VECTIS_ERR_INVALID, 0,
                         "SQL parameter byte limit exceeded");
      bytes += values[i].size;
    }
  }
  for (i = 0; i < count; ++i) {
    const vectis_sql_value *value = &values[i];
    cpkt_sqlite_statement *native = state->native;
    int index = (int)i + 1;
    if (value->type == VECTIS_SQL_NULL)
      code = native->bind_null(native, index);
    else if (value->type == VECTIS_SQL_INTEGER)
      code = native->bind_i64(native, index, to_i64(value->integer));
    else if (value->type == VECTIS_SQL_REAL)
      code = native->bind_double(native, index, value->real);
    else if (value->type == VECTIS_SQL_TEXT)
      code = native->bind_text(
          native, index, value->data == NULL ? "" : (const char *)value->data,
          (int)value->size);
    else
      code = native->bind_blob(native, index,
                               value->data == NULL ? "" : value->data,
                               (int)value->size);
    if (code != CPKT_SQLITE_OK) {
      status = native_error(state->owner, code, error);
      native->clear_bindings(native);
      return status;
    }
  }
  state->state = 1;
  return VECTIS_OK;
}

static vectis_status fail_statement(statement_state *state, vectis_status code,
                                    const vectis_error *error) {
  state->state = -1;
  if (error != NULL)
    state->failure = *error;
  else
    sql_error(&state->failure, code, 0, "SQLite operation failed");
  return code;
}
vectis_status vectis_sqlite_next(vectis_sqlite_statement *receiver, int *row,
                                 vectis_error *error) {
  statement_state *state;
  vectis_error local;
  vectis_status status;
  int code, i, columns;
  size_t bytes = 0u;
  if (error == NULL)
    error = &local;
  if (row == NULL)
    return sql_error(error, VECTIS_ERR_INVALID, 0, "row output is required");
  *row = 0;
  status = check_statement(receiver, &state, error);
  if (status != VECTIS_OK)
    return status;
  if (state->state == 0)
    return sql_error(error, VECTIS_ERR_STATE, 0,
                     "bind parameters before stepping");
  if (state->state == 3)
    return VECTIS_OK;
  code = state->native->step(state->native);
  if (code == CPKT_SQLITE_DONE) {
    state->state = 3;
    if (state->cursor) {
      code = cpkt_sqlite_statement_finalize(state->native);
      state->native = NULL;
      if (code != CPKT_SQLITE_OK)
        return fail_statement(state, native_error(state->owner, code, error),
                              error);
    }
    return VECTIS_OK;
  }
  if (code != CPKT_SQLITE_ROW)
    return fail_statement(state, native_error(state->owner, code, error),
                          error);
  columns = state->native->column_count(state->native);
  if (columns > (int)VECTIS_SQL_MAX_COLUMNS) {
    status = sql_error(error, VECTIS_ERR_STATE, 0,
                       "SQLite column limit exceeded after reprepare");
    return fail_statement(state, status, error);
  }
  for (i = 0; i < columns; ++i) {
    int type = state->native->column_type(state->native, i);
    if (type == CPKT_SQLITE_TEXT || type == CPKT_SQLITE_BLOB) {
      const void *value =
          type == CPKT_SQLITE_TEXT
              ? (const void *)state->native->column_text(state->native, i)
              : state->native->column_blob(state->native, i);
      int size = state->native->column_bytes(state->native, i);
      if (value == NULL && (size != 0 || type == CPKT_SQLITE_TEXT)) {
        status = sql_error(error, VECTIS_ERR_NOMEM, CPKT_SQLITE_NOMEM,
                           "SQLite column allocation failed");
        return fail_statement(state, status, error);
      }
      if (size < 0 || (size_t)size > VECTIS_SQL_MAX_VALUE_BYTES - bytes) {
        status = sql_error(error, VECTIS_ERR_INVALID, 0,
                           "SQLite row byte limit exceeded");
        return fail_statement(state, status, error);
      }
      bytes += (size_t)size;
    }
  }
  state->state = 2;
  *row = 1;
  return VECTIS_OK;
}

vectis_status vectis_sqlite_columns(vectis_sqlite_statement *receiver,
                                    size_t *count, vectis_error *error) {
  statement_state *state;
  vectis_status status;
  if (count == NULL)
    return sql_error(error, VECTIS_ERR_INVALID, 0,
                     "column count output is required");
  *count = 0u;
  status = check_statement(receiver, &state, error);
  if (status != VECTIS_OK)
    return status;
  if (state->native == NULL)
    return sql_error(error, VECTIS_ERR_STATE, 0,
                     "cursor metadata is retired at EOF");
  *count = (size_t)state->native->column_count(state->native);
  return VECTIS_OK;
}
vectis_status vectis_sqlite_column(vectis_sqlite_statement *receiver,
                                   size_t index, vectis_sql_column *column,
                                   vectis_error *error) {
  statement_state *state;
  vectis_status status;
  size_t count;
  if (column == NULL)
    return sql_error(error, VECTIS_ERR_INVALID, 0, "column output is required");
  memset(column, 0, sizeof(*column));
  status = vectis_sqlite_columns(receiver, &count, error);
  if (status != VECTIS_OK)
    return status;
  if (index >= count)
    return sql_error(error, VECTIS_ERR_INVALID, 0, "column index out of range");
  state = (statement_state *)receiver->impl;
  column->name = state->native->column_name(state->native, (int)index);
  column->declared_type =
      state->native->column_declared_type(state->native, (int)index);
  if (column->name == NULL)
    return sql_error(error, VECTIS_ERR_NOMEM, 0,
                     "column metadata allocation failed");
  return VECTIS_OK;
}
vectis_status vectis_sqlite_value(vectis_sqlite_statement *receiver,
                                  size_t index, vectis_sql_value *value,
                                  vectis_error *error) {
  statement_state *state;
  vectis_status status;
  int type;
  size_t count;
  if (value == NULL)
    return sql_error(error, VECTIS_ERR_INVALID, 0, "value output is required");
  memset(value, 0, sizeof(*value));
  status = vectis_sqlite_columns(receiver, &count, error);
  if (status != VECTIS_OK)
    return status;
  state = (statement_state *)receiver->impl;
  if (state->state != 2 || index >= count)
    return sql_error(error, VECTIS_ERR_STATE, 0,
                     "no current SQLite row/column");
  type = state->native->column_type(state->native, (int)index);
  if (type == CPKT_SQLITE_INTEGER) {
    value->type = VECTIS_SQL_INTEGER;
    value->integer =
        from_i64(state->native->column_i64(state->native, (int)index));
  } else if (type == CPKT_SQLITE_FLOAT) {
    value->type = VECTIS_SQL_REAL;
    value->real = state->native->column_double(state->native, (int)index);
  } else if (type == CPKT_SQLITE_TEXT || type == CPKT_SQLITE_BLOB) {
    value->type = type == CPKT_SQLITE_TEXT ? VECTIS_SQL_TEXT : VECTIS_SQL_BLOB;
    value->data = type == CPKT_SQLITE_TEXT
                      ? (const void *)state->native->column_text(state->native,
                                                                 (int)index)
                      : state->native->column_blob(state->native, (int)index);
    value->size =
        (size_t)state->native->column_bytes(state->native, (int)index);
  }
  return VECTIS_OK;
}
vectis_status vectis_sqlite_reset(vectis_sqlite_statement *receiver,
                                  vectis_error *error) {
  statement_state *state;
  vectis_status status;
  int code, clear;
  if (receiver == NULL)
    return sql_error(error, VECTIS_ERR_INVALID, 0,
                     "SQLite statement is required");
  state = (statement_state *)receiver->impl;
  status = check_owner(state->owner, error);
  if (status != VECTIS_OK)
    return status;
  if (state->cursor || state->native == NULL)
    return sql_error(error, VECTIS_ERR_STATE, 0,
                     "only reusable statements reset");
  code = state->native->reset(state->native);
  clear = state->native->clear_bindings(state->native);
  state->state = 0;
  memset(&state->failure, 0, sizeof(state->failure));
  if (code == CPKT_SQLITE_OK)
    code = clear;
  return code == CPKT_SQLITE_OK ? VECTIS_OK
                                : native_error(state->owner, code, error);
}
void vectis_sqlite_statement_close(vectis_sqlite_statement *receiver) {
  statement_state *state, **link;
  sqlite_state *owner;
  if (receiver == NULL)
    return;
  state = (statement_state *)receiver->impl;
  owner = state->owner;
  if (owner->process != getpid() ||
      !pthread_equal(owner->thread, pthread_self()))
    return;
  if (state->native != NULL)
    cpkt_sqlite_statement_finalize(state->native);
  for (link = &owner->statements; *link != state; link = &(*link)->next) {
  }
  *link = state->next;
  free(state);
  free(receiver);
  unref(owner);
}
vectis_status
vectis_sqlite_exec(vectis_sqlite *receiver, const char *sql, size_t size,
                   const vectis_sql_value *parameters, size_t count,
                   vectis_sqlite_exec_result *result, vectis_error *error) {
  vectis_sqlite_statement *statement = NULL;
  sqlite_state *state;
  vectis_status status;
  size_t columns;
  int row;
  if (result != NULL)
    memset(result, 0, sizeof(*result));
  status = vectis_sqlite_prepare(receiver, sql, size, 1, &statement, error);
  if (status != VECTIS_OK)
    return status;
  state = (sqlite_state *)receiver->impl;
  status = vectis_sqlite_columns(statement, &columns, error);
  if (status == VECTIS_OK && columns != 0u)
    status = sql_error(error, VECTIS_ERR_INVALID, 0,
                       "exec rejects row results; use query");
  if (status == VECTIS_OK)
    status = vectis_sqlite_bind(statement, parameters, count, error);
  if (status == VECTIS_OK)
    status = vectis_sqlite_next(statement, &row, error);
  if (status == VECTIS_OK && result != NULL) {
    result->changes = from_i64(cpkt_sqlite_changes64(state->native));
    result->last_insert_rowid =
        from_i64(cpkt_sqlite_last_insert_rowid(state->native));
  }
  vectis_sqlite_statement_close(statement);
  return status;
}
static vectis_status transaction_not_started(sqlite_state *state,
                                             vectis_error *error,
                                             vectis_status status) {
  strcpy(error->detail, "not_started");
  if (state != NULL && state->process == getpid() &&
      pthread_equal(state->thread, pthread_self())) {
    memset(&state->diagnostic, 0, sizeof(state->diagnostic));
    state->diagnostic.primary = *error;
  }
  return status;
}
vectis_status vectis_sqlite_transaction(vectis_sqlite *receiver,
                                        const char *mode,
                                        vectis_sqlite_transaction_fn callback,
                                        void *context, vectis_error *error) {
  sqlite_state *state;
  statement_state *statement;
  vectis_error primary, cleanup, local;
  vectis_status status, rollback;
  char begin[32];
  if (error == NULL)
    error = &local;
  if (receiver == NULL || callback == NULL || mode == NULL ||
      (strcmp(mode, "DEFERRED") != 0 && strcmp(mode, "IMMEDIATE") != 0 &&
       strcmp(mode, "EXCLUSIVE") != 0))
    return transaction_not_started(
        NULL, error,
        sql_error(error, VECTIS_ERR_INVALID, 0,
                  "invalid SQLite transaction mode/callback"));
  state = (sqlite_state *)receiver->impl;
  status = check_owner(state, error);
  if (status != VECTIS_OK)
    return transaction_not_started(state, error, status);
  if (state->active || !cpkt_sqlite_autocommit(state->native)) {
    state->violated = state->active;
    return transaction_not_started(
        state, error,
        sql_error(error, VECTIS_ERR_STATE, 0,
                  "SQLite transaction nesting is forbidden"));
  }
  strcpy(begin, "BEGIN ");
  memset(&state->diagnostic, 0, sizeof(state->diagnostic));
  strcat(begin, mode);
  status = vectis_sqlite_exec(receiver, begin, strlen(begin), NULL, 0u, NULL,
                              &primary);
  if (status != VECTIS_OK) {
    strcpy(primary.detail, "not_started");
    state->diagnostic.primary = primary;
    if (error != NULL)
      *error = primary;
    return status;
  }
  state->transaction++;
  state->active = 1;
  state->violated = 0;
  memset(&primary, 0, sizeof(primary));
  status = callback(receiver, context, &primary);
  if (status != VECTIS_OK && primary.code == VECTIS_OK)
    sql_error(&primary, status, 0, "SQLite transaction callback failed");
  for (statement = state->statements; statement != NULL;
       statement = statement->next) {
    if (statement->native != NULL &&
        (statement->transaction == state->transaction ||
         statement->state == 2)) {
      cpkt_sqlite_statement_finalize(statement->native);
      statement->native = NULL;
      statement->state = 0;
      state->violated = 1;
    }
  }
  if (status == VECTIS_OK &&
      (state->violated || cpkt_sqlite_autocommit(state->native)))
    status = sql_error(&primary, VECTIS_ERR_STATE, 0,
                       "transaction callback changed ownership or retained an "
                       "active statement");
  state->control = 1;
  if (status == VECTIS_OK) {
    status =
        vectis_sqlite_exec(receiver, "COMMIT", 6u, NULL, 0u, NULL, &primary);
    if (status == VECTIS_OK)
      strcpy(primary.detail, "committed");
  }
  if (status != VECTIS_OK) {
    /* A provider statement failure can already have rolled back. */
    rollback = cpkt_sqlite_autocommit(state->native)
                   ? VECTIS_OK
                   : vectis_sqlite_exec(receiver, "ROLLBACK", 8u, NULL, 0u,
                                        NULL, &cleanup);
    strcpy(primary.detail, rollback == VECTIS_OK ? "rolled_back" : "unknown");
    if (rollback != VECTIS_OK) {
      state->diagnostic.cleanup_failed = 1;
      state->diagnostic.cleanup = cleanup;
    }
    if (rollback != VECTIS_OK || !cpkt_sqlite_autocommit(state->native)) {
      for (statement = state->statements; statement != NULL;
           statement = statement->next) {
        if (statement->native != NULL)
          cpkt_sqlite_statement_finalize(statement->native);
        statement->native = NULL;
        statement->state = 0;
      }
      cpkt_sqlite_close(state->native);
      state->native = NULL;
      state->invalid = 1;
    }
  }
  state->active = 0;
  state->control = 0;
  state->diagnostic.primary = primary;
  if (error != NULL)
    *error = primary;
  return status;
}
const vectis_sqlite_diagnostic *
vectis_sqlite_get_transaction_diagnostic(const vectis_sqlite *receiver) {
  sqlite_state *state;
  if (receiver == NULL)
    return NULL;
  state = (sqlite_state *)receiver->impl;
  if (state->process != getpid() ||
      !pthread_equal(state->thread, pthread_self()))
    return NULL;
  return &state->diagnostic;
}
vectis_status vectis_sqlite_close(vectis_sqlite *receiver,
                                  vectis_error *error) {
  sqlite_state *state;
  statement_state *statement;
  if (receiver == NULL)
    return VECTIS_OK;
  state = (sqlite_state *)receiver->impl;
  if (state->declaration && state->native == NULL &&
      state->process != getpid()) {
    state->process = getpid();
    state->thread = pthread_self();
  }
  if (state->process != getpid() ||
      !pthread_equal(state->thread, pthread_self()))
    return sql_error(error, VECTIS_ERR_STATE, 0,
                     "foreign-process/thread SQLite close");
  if (state->active) {
    state->violated = 1;
    return sql_error(error, VECTIS_ERR_STATE, 0,
                     "cannot close a transaction callback's database");
  }
  vectis_sql_app_detach(&state->resource);
  for (statement = state->statements; statement != NULL;
       statement = statement->next) {
    if (statement->native != NULL)
      cpkt_sqlite_statement_finalize(statement->native);
    statement->native = NULL;
    statement->state = 0;
  }
  if (state->native != NULL)
    cpkt_sqlite_close(state->native);
  state->native = NULL;
  free(receiver);
  unref(state);
  return VECTIS_OK;
}
