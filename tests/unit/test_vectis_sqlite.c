#include <assert.h>
#include <cpkt/sqlite.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vectis/sqlite.h>

static vectis_sqlite *db;
static vectis_error error;
static vectis_status execute(const char *sql) {
  return vectis_sqlite_exec(db, sql, strlen(sql), NULL, 0u, NULL, &error);
}
static vectis_status commit_callback(vectis_sqlite *receiver, void *context,
                                     vectis_error *failure) {
  const char *sql = "insert into data values (7, 'transaction', x'', NULL)";
  (void)context;
  return receiver->exec(receiver, sql, strlen(sql), NULL, 0u, NULL, failure);
}
static vectis_status rollback_callback(vectis_sqlite *receiver, void *context,
                                       vectis_error *failure) {
  assert(commit_callback(receiver, context, failure) == VECTIS_OK);
  memset(failure, 0, sizeof(*failure));
  failure->code = VECTIS_ERR_STATE;
  strcpy(failure->message, "caller requested rollback");
  return VECTIS_ERR_STATE;
}
static vectis_status close_callback(vectis_sqlite *receiver, void *context,
                                    vectis_error *failure) {
  (void)context;
  assert(vectis_sqlite_close(receiver, failure) == VECTIS_ERR_STATE);
  return VECTIS_OK;
}
static vectis_status manual_callback(vectis_sqlite *receiver, void *context,
                                     vectis_error *failure) {
  (void)context;
  assert(receiver->exec(receiver, "COMMIT", 6u, NULL, 0u, NULL, failure) !=
         VECTIS_OK);
  return VECTIS_OK;
}
static void *foreign_thread(void *context) {
  vectis_error failure;
  vectis_sqlite *receiver = (vectis_sqlite *)context;
  assert(receiver->exec(receiver, "select 1", 8u, NULL, 0u, NULL, &failure) ==
         VECTIS_ERR_STATE);
  assert(receiver->close(receiver, &failure) == VECTIS_ERR_STATE);
  return NULL;
}
static vectis_status app_close_callback(vectis_sqlite *receiver, void *context,
                                        vectis_error *failure) {
  vectis_app *app = (vectis_app *)context;
  (void)receiver;
  assert(app->stop(app, failure) == VECTIS_ERR_STATE);
  app->close(app); /* Must be rejected without destroying the callback's app. */
  return VECTIS_OK;
}
static void app_lifecycle(void) {
  vectis_app_config app_config;
  vectis_sqlite_config config;
  vectis_app *app;
  vectis_sqlite *owned, *duplicate;
  vectis_sqlite_statement *statement;
  vectis_error failure;
  char name[] = "copied-name", path[] = ":memory:";
  pid_t child;
  pthread_t thread;
  int child_status, row;
  vectis_app_config_init(&app_config);
  app_config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  app = vectis_app_new(&app_config, &failure);
  assert(app != NULL);
  vectis_sqlite_config_init(&config);
  assert(vectis_app_sqlite(app, name, path, &config, &owned, &failure) ==
         VECTIS_OK);
  assert(vectis_app_sqlite(app, name, path, &config, &duplicate, &failure) ==
         VECTIS_ERR_CONFLICT);
  name[0] = 'X';
  path[0] = 'X';
  assert(owned->exec(owned, "create table own (id)", 21u, NULL, 0u, NULL,
                     &failure) == VECTIS_ERR_STATE);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    assert(app->start(app, &failure) == VECTIS_OK);
    assert(owned->exec(owned, "create table own (id)", 21u, NULL, 0u, NULL,
                       &failure) == VECTIS_OK);
    assert(owned->close(owned, &failure) == VECTIS_OK);
    app->close(app);
    _exit(0);
  }
  assert(waitpid(child, &child_status, 0) == child);
  assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
  assert(app->start(app, &failure) == VECTIS_OK);
  /* Parent has its own memory database; constructor strings were copied. */
  assert(pthread_create(&thread, NULL, foreign_thread, owned) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(owned->exec(owned, "create table own (id)", 21u, NULL, 0u, NULL,
                     &failure) == VECTIS_OK);
  assert(vectis_app_sqlite(app, "late", ":memory:", &config, &duplicate,
                           &failure) == VECTIS_ERR_STATE);
  assert(vectis_sqlite_transaction(owned, "DEFERRED", app_close_callback, app,
                                   &failure) == VECTIS_ERR_STATE);
  assert(strcmp(failure.detail, "rolled_back") == 0);
  assert(owned->prepare(owned, "select 1", 8u, 1, &statement, &failure) ==
         VECTIS_OK);
  assert(statement->bind(statement, NULL, 0u, &failure) == VECTIS_OK);
  assert(app->stop(app, &failure) == VECTIS_OK);
  assert(statement->next(statement, &row, &failure) == VECTIS_ERR_STATE);
  statement->close(statement);
  assert(app->start(app, &failure) == VECTIS_OK);
  assert(owned->exec(owned, "create table own (id)", 21u, NULL, 0u, NULL,
                     &failure) == VECTIS_OK);
  assert(app->restart(app, &failure) == VECTIS_OK);
  assert(owned->exec(owned, "create table own (id)", 21u, NULL, 0u, NULL,
                     &failure) == VECTIS_OK);
  app->close(app);
  assert(owned->exec(owned, "create table own (id)", 21u, NULL, 0u, NULL,
                     &failure) == VECTIS_ERR_STATE);
  assert(owned->close(owned, &failure) == VECTIS_OK);
}
int main(void) {
  vectis_sqlite_config config;
  vectis_sqlite_statement *statement;
  vectis_sql_value values[4], value;
  vectis_sql_column column;
  vectis_sqlite_exec_result result;
  const char *insert = "insert into data values (?,?,?,?)";
  const char *select =
      "select i as duplicate,t as duplicate,b,n,typeof(b),typeof(t) from data";
  size_t columns;
  int row, child_status;
  pid_t child;
  pthread_t thread;
  vectis_sqlite_config_init(&config);
  assert(config.busy_timeout_ms == 0 && !config.uri);
  assert(vectis_sqlite_open(":memory:", &config, &db, &error) == VECTIS_OK);
  assert(execute("create table data (i INTEGER,t TEXT,b BLOB,n)") == VECTIS_OK);
  memset(values, 0, sizeof(values));
  values[0].type = VECTIS_SQL_INTEGER;
  values[0].integer = INT64_MIN;
  values[1].type = VECTIS_SQL_TEXT;
  values[1].data = "a\0b";
  values[1].size = 3u;
  values[2].type = VECTIS_SQL_BLOB;
  assert(vectis_sqlite_exec(db, insert, strlen(insert), values, 4u, &result,
                            &error) == VECTIS_OK);
  assert(result.changes == 1 && result.last_insert_rowid == 1);
  assert(vectis_sqlite_prepare(db, select, strlen(select), 0, &statement,
                               &error) == VECTIS_OK);
  assert(vectis_sqlite_next(statement, &row, &error) == VECTIS_ERR_STATE);
  assert(vectis_sqlite_bind(statement, NULL, 0u, &error) == VECTIS_OK);
  assert(vectis_sqlite_columns(statement, &columns, &error) == VECTIS_OK &&
         columns == 6u);
  assert(vectis_sqlite_column(statement, 0u, &column, &error) == VECTIS_OK);
  assert(strcmp(column.name, "duplicate") == 0 &&
         strcmp(column.declared_type, "INTEGER") == 0);
  assert(vectis_sqlite_column(statement, 1u, &column, &error) == VECTIS_OK &&
         strcmp(column.name, "duplicate") == 0);
  assert(vectis_sqlite_next(statement, &row, &error) == VECTIS_OK && row);
  assert(vectis_sqlite_value(statement, 0u, &value, &error) == VECTIS_OK);
  assert(value.type == VECTIS_SQL_INTEGER && value.integer == INT64_MIN);
  assert(vectis_sqlite_value(statement, 1u, &value, &error) == VECTIS_OK);
  assert(value.type == VECTIS_SQL_TEXT && value.size == 3u &&
         memcmp(value.data, "a\0b", 3u) == 0);
  assert(vectis_sqlite_value(statement, 2u, &value, &error) == VECTIS_OK &&
         value.type == VECTIS_SQL_BLOB && value.size == 0u);
  assert(vectis_sqlite_value(statement, 3u, &value, &error) == VECTIS_OK &&
         value.type == VECTIS_SQL_NULL);
  assert(vectis_sqlite_next(statement, &row, &error) == VECTIS_OK && !row);
  assert(vectis_sqlite_next(statement, &row, &error) == VECTIS_OK && !row);
  assert(vectis_sqlite_reset(statement, &error) == VECTIS_OK);
  assert(vectis_sqlite_bind(statement, values, 1u, &error) ==
         VECTIS_ERR_INVALID);
  assert(vectis_sqlite_bind(statement, NULL, 0u, &error) == VECTIS_OK);
  assert(vectis_sqlite_next(statement, &row, &error) == VECTIS_OK && row);
  vectis_sqlite_statement_close(statement);
  assert(execute("insert into data values (1,'',x'',NULL); delete from data") ==
         VECTIS_ERR_INVALID);
  assert(
      execute(
          "insert into data values (1,'',x'',NULL); PRAGMA user_version=8") ==
      VECTIS_ERR_INVALID);
  assert(execute("select * from data") == VECTIS_ERR_INVALID);
  assert(execute("/* no SQL */") == VECTIS_ERR_INVALID);
  assert(execute("select broken from absent") != VECTIS_OK &&
         error.dependency_code != 0);
  assert(vectis_sqlite_transaction(db, "IMMEDIATE", commit_callback, NULL,
                                   &error) == VECTIS_OK);
  assert(strcmp(error.detail, "committed") == 0);
  assert(vectis_sqlite_transaction(db, "DEFERRED", rollback_callback, NULL,
                                   &error) == VECTIS_ERR_STATE);
  assert(strcmp(error.detail, "rolled_back") == 0);
  assert(strcmp(error.message, "caller requested rollback") == 0);
  assert(vectis_sqlite_transaction(db, "DEFERRED", close_callback, NULL,
                                   &error) == VECTIS_ERR_STATE);
  assert(strcmp(error.detail, "rolled_back") == 0);
  assert(vectis_sqlite_transaction(db, "DEFERRED", manual_callback, NULL,
                                   &error) == VECTIS_ERR_STATE);
  assert(strcmp(error.detail, "rolled_back") == 0);
  assert(pthread_create(&thread, NULL, foreign_thread, db) == 0);
  assert(pthread_join(thread, NULL) == 0);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    assert(execute("delete from data") == VECTIS_ERR_STATE);
    _exit(0);
  }
  assert(waitpid(child, &child_status, 0) == child);
  assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
  assert(vectis_sqlite_prepare(db, "select 1", 8u, 1, &statement, &error) ==
         VECTIS_OK);
  assert(vectis_sqlite_bind(statement, NULL, 0u, &error) == VECTIS_OK);
  assert(vectis_sqlite_close(db, &error) == VECTIS_OK);
  assert(vectis_sqlite_next(statement, &row, &error) == VECTIS_ERR_STATE);
  vectis_sqlite_statement_close(statement);
  config.mode = "readonly";
  assert(vectis_sqlite_open("/vectis-test-no-such-directory/data.db", &config,
                            &db, &error) != VECTIS_OK);
  assert(db == NULL && error.dependency_code != 0);
  app_lifecycle();
  return 0;
}
