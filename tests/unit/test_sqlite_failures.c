/* Real public C89 provider with deterministic owner-allocation and rollback
 * faults. Assertions concern returned diagnostics, reuse and cleanup. */
#include <assert.h>
#include <cpkt/sqlite.h>
#include <stdlib.h>
#include <string.h>

static int fail_rollback, allocation_armed;
static size_t allocations_left;
static void *owner_calloc(size_t count, size_t size) {
  if (allocation_armed) {
    if (allocations_left == 0u)
      return NULL;
    --allocations_left;
  }
  return calloc(count, size);
}
static int prepare_fault(cpkt_sqlite *db, const char *sql, int size,
                         unsigned long flags, cpkt_sqlite_statement **out,
                         const char **tail) {
  if (fail_rollback && size == 8 && memcmp(sql, "ROLLBACK", 8u) == 0) {
    *out = NULL;
    if (tail != NULL)
      *tail = NULL;
    (void)cpkt_sqlite_set_error(db, CPKT_SQLITE_NOMEM,
                                "injected rollback failure");
    return CPKT_SQLITE_NOMEM;
  }
  return cpkt_sqlite_prepare(db, sql, size, flags, out, tail);
}
#define calloc owner_calloc
#define cpkt_sqlite_prepare prepare_fault
#include "../../src/vectis_sqlite.c"
#undef cpkt_sqlite_prepare
#undef calloc

static vectis_status reject_transaction(vectis_sqlite *db, void *context,
                                        vectis_error *error) {
  (void)db;
  (void)context;
  memset(error, 0, sizeof(*error));
  error->code = VECTIS_ERR_CONFLICT;
  strcpy(error->message, "primary callback failure");
  return VECTIS_ERR_CONFLICT;
}
static const unsigned char *
column_allocation_fault(const cpkt_sqlite_statement *statement, int column) {
  (void)statement;
  (void)column;
  return NULL;
}
int main(void) {
  vectis_sqlite *db = NULL;
  vectis_sqlite_statement *statement = NULL;
  vectis_sqlite_statement *other = NULL;
  vectis_sqlite_config config;
  vectis_app_config app_config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  const vectis_sqlite_diagnostic *diagnostic;
  size_t index;
  int completed = 0;
  int row;
  for (index = 0; index < 16u; ++index) {
    allocation_armed = 1;
    allocations_left = index;
    status = vectis_sqlite_open(":memory:", NULL, &db, &error);
    allocation_armed = 0;
    if (status == VECTIS_OK) {
      completed = 1;
      break;
    }
    assert(status == VECTIS_ERR_NOMEM && db == NULL);
  }
  assert(completed);
  completed = 0;
  for (index = 0; index < 16u; ++index) {
    allocation_armed = 1;
    allocations_left = index;
    status = vectis_sqlite_prepare(db, "select 1", 8u, 0, &statement, &error);
    allocation_armed = 0;
    if (status == VECTIS_OK) {
      vectis_sqlite_statement_close(statement);
      completed = 1;
      break;
    }
    assert(status == VECTIS_ERR_NOMEM && statement == NULL);
  }
  assert(completed);
  assert(vectis_sqlite_prepare(db, "select 'text'", 13u, 0, &statement,
                               &error) == VECTIS_OK);
  ((statement_state *)statement->impl)->native->column_text =
      column_allocation_fault;
  assert(vectis_sqlite_bind(statement, NULL, 0u, &error) == VECTIS_OK);
  assert(vectis_sqlite_next(statement, &row, &error) == VECTIS_ERR_NOMEM &&
         !row);
  assert(error.dependency_code == CPKT_SQLITE_NOMEM);
  assert(vectis_sqlite_next(statement, &row, &error) == VECTIS_ERR_NOMEM &&
         !row);
  vectis_sqlite_statement_close(statement);
  assert(vectis_sqlite_prepare(db, "select 42", 9u, 0, &statement, &error) ==
         VECTIS_OK);
  assert(vectis_sqlite_bind(statement, NULL, 0u, &error) == VECTIS_OK);
  assert(vectis_sqlite_next(statement, &row, &error) == VECTIS_OK && row);
  assert(vectis_sqlite_next(statement, &row, &error) == VECTIS_OK && !row);
  assert(vectis_sqlite_prepare(db, "select 1", 8u, 0, &other, &error) ==
         VECTIS_OK);
  vectis_sqlite_statement_close(other);
  assert(vectis_sqlite_reset(statement, &error) == VECTIS_OK);
  assert(vectis_sqlite_bind(statement, NULL, 0u, &error) == VECTIS_OK);
  assert(vectis_sqlite_next(statement, &row, &error) == VECTIS_OK && row);
  {
    cpkt_sqlite_statement *native =
        ((statement_state *)statement->impl)->native;
    assert(native->status(native, CPKT_SQLITE_STMTSTATUS_REPREPARE, 0) == 0);
  }
  vectis_sqlite_statement_close(statement);
  fail_rollback = 1;
  assert(vectis_sqlite_transaction(db, "DEFERRED", reject_transaction, NULL,
                                   &error) == VECTIS_ERR_CONFLICT);
  assert(strcmp(error.message, "primary callback failure") == 0);
  assert(strcmp(error.detail, "unknown") == 0);
  diagnostic = vectis_sqlite_get_transaction_diagnostic(db);
  assert(diagnostic != NULL && diagnostic->cleanup_failed);
  assert(diagnostic->primary.code == VECTIS_ERR_CONFLICT);
  assert(diagnostic->cleanup.dependency_code == CPKT_SQLITE_NOMEM);
  assert(strstr(diagnostic->cleanup.message, "injected rollback failure") !=
         NULL);
  fail_rollback = 0;
  assert(vectis_sqlite_prepare(db, "select 1", 8u, 0, &statement, &error) ==
         VECTIS_ERR_STATE);
  assert(vectis_sqlite_close(db, &error) == VECTIS_OK);
  vectis_app_config_init(&app_config);
  app_config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  app = vectis_app_new(&app_config, &error);
  assert(app != NULL);
  vectis_sqlite_config_init(&config);
  assert(vectis_app_sqlite(app, "fault", ":memory:", &config, &db, &error) ==
         VECTIS_OK);
  assert(app->start(app, &error) == VECTIS_OK);
  fail_rollback = 1;
  assert(vectis_sqlite_transaction(db, "DEFERRED", reject_transaction, NULL,
                                   &error) == VECTIS_ERR_CONFLICT);
  fail_rollback = 0;
  /* A declared connection must not silently reopen after unknown outcome. */
  assert(vectis_sqlite_prepare(db, "select 1", 8u, 0, &statement, &error) ==
         VECTIS_ERR_STATE);
  assert(app->restart(app, &error) == VECTIS_OK);
  assert(vectis_sqlite_prepare(db, "select 1", 8u, 0, &statement, &error) ==
         VECTIS_OK);
  vectis_sqlite_statement_close(statement);
  assert(vectis_sqlite_close(db, &error) == VECTIS_OK);
  app->close(app);
  return 0;
}
