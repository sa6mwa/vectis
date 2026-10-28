/* Real public-provider workflows plus deterministic loss of COMMIT receipt. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vectis/pg.h>
int fixture_pg_send(cpkt_postgres_connection *, const char *, int,
                    const cpkt_postgres_oid *, const char *const *, const int *,
                    const int *, int);
int fixture_pg_consume(cpkt_postgres_connection *);
#define cpkt_postgres_send_query_params fixture_pg_send
#define cpkt_postgres_consume_input fixture_pg_consume
#include "../../src/vectis_pg_connection.c"
#undef cpkt_postgres_send_query_params
#undef cpkt_postgres_consume_input
static int lose_commit, discard_input;
int fixture_pg_send(cpkt_postgres_connection *connection, const char *command,
                    int count, const cpkt_postgres_oid *types,
                    const char *const *values, const int *lengths,
                    const int *formats, int result_format) {
  int result =
      cpkt_postgres_send_query_params(connection, command, count, types, values,
                                      lengths, formats, result_format);
  if (result && lose_commit && strcmp(command, "COMMIT") == 0)
    discard_input = 1;
  return result;
}
int fixture_pg_consume(cpkt_postgres_connection *connection) {
  if (discard_input) {
    discard_input = 0;
    return 0;
  }
  return cpkt_postgres_consume_input(connection);
}
static vectis_error failure;
static vectis_status execute(vectis_pg_connection *db, const char *sql) {
  return db->exec(db, sql, strlen(sql), NULL, 0u, -1, NULL, &failure);
}
static void expect(vectis_status status) {
  if (status != VECTIS_OK)
    fprintf(stderr, "PostgreSQL: %s (%s)\n", failure.message, failure.detail);
  assert(status == VECTIS_OK);
}
static void text_query(vectis_pg_connection *db, const char *sql, char *out,
                       size_t capacity) {
  vectis_pg_cursor *cursor;
  vectis_pg_step step;
  const vectis_sql_value *row;
  size_t count;
  expect(db->query(db, sql, strlen(sql), NULL, 0u, -1, &cursor, &failure));
  expect(cursor->next(cursor, &step, &row, &count, &failure));
  assert(step == VECTIS_PG_ROW && count == 1u &&
         row[0].type == VECTIS_SQL_TEXT && row[0].size < capacity);
  memcpy(out, row[0].data, row[0].size);
  out[row[0].size] = '\0';
  expect(cursor->next(cursor, &step, &row, &count, &failure));
  assert(step == VECTIS_PG_EOF);
  expect(cursor->close(cursor, &failure));
}
static vectis_status write_transaction(vectis_pg_connection *db, void *context,
                                       vectis_error *error) {
  const char *sql = "INSERT INTO vectis_facade_native VALUES(7)";
  (void)context;
  return db->exec(db, sql, strlen(sql), NULL, 0u, -1, NULL, error);
}
static vectis_status abort_transaction(vectis_pg_connection *db, void *context,
                                       vectis_error *error) {
  assert(write_transaction(db, context, error) == VECTIS_OK);
  memset(error, 0, sizeof(*error));
  error->code = VECTIS_ERR_CONFLICT;
  strcpy(error->message, "callback sentinel");
  return VECTIS_ERR_CONFLICT;
}
static vectis_status caught_sql_error(vectis_pg_connection *db, void *context,
                                      vectis_error *error) {
  (void)context;
  assert(db->exec(db, "SELECT 1/0", 10u, NULL, 0u, -1, NULL, error) ==
         VECTIS_ERR_STATE);
  assert(db->exec(db, "SELECT 1", 8u, NULL, 0u, -1, NULL, error) ==
         VECTIS_ERR_STATE);
  return VECTIS_OK;
}
static vectis_status deferred_constraint(vectis_pg_connection *db,
                                         void *context, vectis_error *error) {
  const char *sql = "INSERT INTO vectis_fk_child VALUES(999)";
  (void)context;
  return db->exec(db, sql, strlen(sql), NULL, 0u, -1, NULL, error);
}
static vectis_status readonly_write(vectis_pg_connection *db, void *context,
                                    vectis_error *error) {
  const char *sql =
      "CREATE TABLE vectis_native_readonly_forbidden(value integer)";
  (void)context;
  return db->exec(db, sql, strlen(sql), NULL, 0u, -1, NULL, error);
}
static vectis_status manual_transaction(vectis_pg_connection *db, void *context,
                                        vectis_error *error) {
  const char *sql = "/* outer /* nested */ comment */ COMMIT";
  (void)context;
  assert(db->exec(db, sql, strlen(sql), NULL, 0u, -1, NULL, error) ==
         VECTIS_ERR_STATE);
  return VECTIS_OK;
}
static vectis_status retained_cursor(vectis_pg_connection *db, void *context,
                                     vectis_error *error) {
  const char *sql = "SELECT generate_series(1,100)";
  return db->query(db, sql, strlen(sql), NULL, 0u, -1,
                   (vectis_pg_cursor **)context, error);
}
static vectis_status app_stop_transaction(vectis_pg_connection *db,
                                          void *context, vectis_error *error) {
  vectis_app *app = (vectis_app *)context;
  (void)db;
  assert(app->stop(app, error) == VECTIS_ERR_STATE);
  app->close(app);
  return VECTIS_OK;
}
static void *foreign(void *context) {
  vectis_pg_connection *db = (vectis_pg_connection *)context;
  vectis_error error;
  int ready;
  assert(db->poll(db, &ready, &error) == VECTIS_ERR_STATE);
  assert(db->close(db, &error) == VECTIS_ERR_STATE);
  return NULL;
}
static void app_workflow(const vectis_pg_config *config) {
  vectis_app_config app_config;
  vectis_app *app;
  vectis_pg_connection *db, *duplicate;
  vectis_pg_cursor *cursor;
  vectis_pg_step step;
  const vectis_sql_value *row;
  size_t count;
  char value[64];
  pid_t child;
  int result;
  vectis_app_config_init(&app_config);
  app_config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  app = vectis_app_new(&app_config, &failure);
  assert(app != NULL);
  expect(vectis_app_pg(app, "owned", config, &db, &failure));
  assert(vectis_app_pg(app, "owned", config, &duplicate, &failure) ==
         VECTIS_ERR_CONFLICT);
  assert(execute(db, "SELECT 1") == VECTIS_ERR_STATE);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    expect(app->start(app, &failure));
    text_query(db, "SELECT 'child'", value, sizeof(value));
    assert(strcmp(value, "child") == 0);
    expect(db->close(db, &failure));
    app->close(app);
    _exit(0);
  }
  assert(waitpid(child, &result, 0) == child && WIFEXITED(result) &&
         WEXITSTATUS(result) == 0);
  expect(app->start(app, &failure));
  text_query(db, "SELECT 'parent'", value, sizeof(value));
  assert(strcmp(value, "parent") == 0);
  assert(vectis_app_pg(app, "late", config, &duplicate, &failure) ==
         VECTIS_ERR_STATE);
  assert(db->transaction(db, NULL, app_stop_transaction, app, &failure) ==
         VECTIS_ERR_STATE);
  assert(strcmp(failure.detail, "rolled_back") == 0);
  expect(db->query(db, "SELECT 1", 8u, NULL, 0u, -1, &cursor, &failure));
  expect(app->stop(app, &failure));
  assert(cursor->next(cursor, &step, &row, &count, &failure) ==
         VECTIS_ERR_STATE);
  expect(cursor->close(cursor, &failure));
  expect(app->start(app, &failure));
  expect(execute(db, "SELECT 1"));
  expect(app->restart(app, &failure));
  expect(execute(db, "SELECT 1"));
  app->close(app);
  assert(execute(db, "SELECT 1") == VECTIS_ERR_STATE);
  expect(db->close(db, &failure));
}
int main(void) {
  const char *keys[] = {"host",   "port",     "user",
                        "dbname", "password", "sslmode"};
  const char *values[6];
  char password[256], value[128], sql[256];
  const char *port = getenv("VECTIS_PG_PORT"),
             *path = getenv("VECTIS_PG_PASSWORD_FILE");
  FILE *file;
  vectis_pg_config config;
  vectis_pg_connection *db, *other;
  vectis_pg_cursor *cursor, *escaped;
  vectis_pg_statement *statement;
  vectis_pg_parameter parameters[3];
  vectis_pg_step step;
  const vectis_sql_value *row;
  const vectis_pg_column *columns;
  const vectis_pg_diagnostic *diagnostic;
  size_t count;
  vectis_pg_command command;
  vectis_pg_transaction_config transaction_config;
  vectis_pg_readiness readiness;
  int ready = 0, child_status;
  pid_t child;
  pthread_t thread;
  const char *select =
      "SELECT $1::text AS duplicate,$2::bytea AS duplicate,$3::text";
  const char *late_error = "SELECT 1/(2-n) FROM generate_series(1,3) AS n";
  assert(port != NULL && path != NULL);
  file = fopen(path, "rb");
  assert(file != NULL && fgets(password, sizeof(password), file) != NULL);
  assert(fclose(file) == 0);
  password[strcspn(password, "\r\n")] = '\0';
  values[0] = "127.0.0.1";
  values[1] = port;
  values[2] = values[3] = "vectis";
  values[4] = password;
  values[5] = "disable";
  vectis_pg_config_init(&config);
  config.keywords = keys;
  config.values = values;
  config.parameter_count = 6u;
  expect(vectis_pg_connect_start(&config, &db, &failure));
  expect(db->readiness(db, &readiness, &failure));
  assert(readiness.fd >= 0 && readiness.write);
  /* Caller-owned external readiness waits use a separate explicit deadline. */
  while (!ready) {
    struct pollfd wait;
    expect(db->poll(db, &ready, &failure));
    if (ready)
      break;
    expect(db->readiness(db, &readiness, &failure));
    wait.fd = readiness.fd;
    wait.events = (short)((readiness.read ? POLLIN : 0) |
                          (readiness.write ? POLLOUT : 0));
    wait.revents = 0;
    assert(poll(&wait, 1, 5000) > 0);
  }
  expect(execute(db, "CREATE TEMP TABLE vectis_facade_native(value integer)"));
  memset(parameters, 0, sizeof(parameters));
  parameters[0].data = "9223372036854775807";
  parameters[0].size = 19u;
  parameters[0].oid = 25;
  parameters[1].data = "a\0b";
  parameters[1].size = 3u;
  parameters[1].oid = 17;
  parameters[1].binary = 1;
  parameters[2].is_null = 1;
  expect(db->query(db, select, strlen(select), parameters, 3u, -1, &cursor,
                   &failure));
  assert(db->query(db, "SELECT 1", 8u, NULL, 0u, -1, &escaped, &failure) ==
         VECTIS_ERR_STATE);
  expect(cursor->next(cursor, &step, &row, &count, &failure));
  assert(step == VECTIS_PG_ROW && count == 3u && row[0].size == 19u &&
         !memcmp(row[0].data, parameters[0].data, 19u));
  assert(row[1].size == 8u && !memcmp(row[1].data, "\\x610062", 8u) &&
         row[2].type == VECTIS_SQL_NULL);
  expect(cursor->columns(cursor, &columns, &count, &failure));
  assert(count == 3u && !strcmp(columns[0].name, columns[1].name) &&
         columns[1].oid == 17 && columns[1].format == 0);
  expect(cursor->next(cursor, &step, &row, &count, &failure));
  assert(step == VECTIS_PG_EOF);
  expect(cursor->next(cursor, &step, &row, &count, &failure));
  assert(step == VECTIS_PG_EOF);
  expect(cursor->close(cursor, &failure));
  expect(db->prepare(db, "reusable", select, strlen(select), NULL, 0u, -1,
                     &statement, &failure));
  expect(statement->query(statement, parameters, 3u, -1, &cursor, &failure));
  expect(cursor->next(cursor, &step, &row, &count, &failure));
  assert(step == VECTIS_PG_ROW);
  expect(cursor->next(cursor, &step, &row, &count, &failure));
  assert(step == VECTIS_PG_EOF);
  expect(cursor->close(cursor, &failure));
  parameters[0].data = "";
  parameters[0].size = 0u;
  expect(statement->query(statement, parameters, 3u, -1, &cursor, &failure));
  expect(cursor->next(cursor, &step, &row, &count, &failure));
  assert(row[0].type == VECTIS_SQL_TEXT && row[0].size == 0u);
  expect(cursor->next(cursor, &step, &row, &count, &failure));
  expect(cursor->close(cursor, &failure));
  expect(statement->close(statement, &failure));
  expect(db->query(db, late_error, strlen(late_error), NULL, 0u, -1, &cursor,
                   &failure));
  expect(cursor->next(cursor, &step, &row, &count, &failure));
  assert(step == VECTIS_PG_ROW);
  assert(cursor->next(cursor, &step, &row, &count, &failure) ==
         VECTIS_ERR_STATE);
  diagnostic = cursor->diagnostic(cursor);
  assert(!strcmp(diagnostic->sqlstate, "22012"));
  assert(cursor->next(cursor, &step, &row, &count, &failure) ==
         VECTIS_ERR_STATE);
  expect(cursor->close(cursor, &failure));
  expect(execute(db, "SELECT 1"));
  expect(db->transaction(db, NULL, write_transaction, NULL, &failure));
  assert(!strcmp(failure.detail, "committed"));
  assert(db->transaction(db, NULL, abort_transaction, NULL, &failure) ==
             VECTIS_ERR_CONFLICT &&
         !strcmp(failure.detail, "rolled_back"));
  assert(db->transaction(db, NULL, manual_transaction, NULL, &failure) ==
             VECTIS_ERR_STATE &&
         !strcmp(failure.detail, "rolled_back"));
  assert(db->transaction(db, NULL, caught_sql_error, NULL, &failure) ==
             VECTIS_ERR_STATE &&
         !strcmp(failure.detail, "rolled_back"));
  assert(
      !strcmp(vectis_pg_connection_transaction_diagnostic(db)->primary.sqlstate,
              "22012"));
  vectis_pg_transaction_config_init(&transaction_config);
  transaction_config.read_only = 1;
  /* Temporary tables permit writes in read-only transactions, so use a
   * permanent catalog-affecting command to prove the setting independently. */
  assert(db->transaction(db, &transaction_config, readonly_write, NULL,
                         &failure) == VECTIS_ERR_STATE &&
         !strcmp(failure.detail, "rolled_back"));
  assert(
      !strcmp(vectis_pg_connection_transaction_diagnostic(db)->primary.sqlstate,
              "25006"));
  expect(execute(db,
                 "CREATE TEMP TABLE vectis_fk_parent(id integer PRIMARY KEY)"));
  expect(execute(db, "CREATE TEMP TABLE vectis_fk_child(id integer REFERENCES "
                     "vectis_fk_parent(id) DEFERRABLE INITIALLY DEFERRED)"));
  assert(db->transaction(db, NULL, deferred_constraint, NULL, &failure) ==
             VECTIS_ERR_STATE &&
         !strcmp(failure.detail, "rolled_back"));
  assert(
      !strcmp(vectis_pg_connection_transaction_diagnostic(db)->primary.sqlstate,
              "23503"));
  assert(!vectis_pg_connection_transaction_diagnostic(db)->cleanup_failed);
  text_query(db, "SELECT count(*) FROM vectis_facade_native", value,
             sizeof(value));
  assert(!strcmp(value, "1"));
  expect(db->exec(db, "UPDATE vectis_facade_native SET value=8", 39u, NULL, 0u,
                  -1, &command, &failure));
  assert(!strcmp(command.command, "UPDATE 1") &&
         !strcmp(command.affected, "1"));
  assert(pthread_create(&thread, NULL, foreign, db) == 0 &&
         pthread_join(thread, NULL) == 0);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    assert(execute(db, "SELECT 1") == VECTIS_ERR_STATE);
    assert(db->close(db, &failure) == VECTIS_ERR_STATE);
    _exit(0);
  }
  assert(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) &&
         WEXITSTATUS(child_status) == 0);
  expect(execute(
      db, "SELECT 1")); /* Child cleanup did not terminate our session. */
  assert(db->transaction(db, NULL, retained_cursor, &escaped, &failure) ==
             VECTIS_ERR_STATE &&
         !strcmp(failure.detail, "unknown"));
  assert(escaped->next(escaped, &step, &row, &count, &failure) ==
         VECTIS_ERR_STATE);
  expect(escaped->close(escaped, &failure));
  expect(db->close(db, &failure));
  expect(vectis_pg_connect(&config, &db, &failure));
  expect(execute(db, "CREATE TEMP TABLE vectis_facade_native(value integer)"));
  lose_commit = 1;
  assert(db->transaction(db, NULL, write_transaction, NULL, &failure) ==
             VECTIS_ERR_STATE &&
         !strcmp(failure.detail, "unknown"));
  assert(vectis_pg_connection_transaction_diagnostic(db)->cleanup_failed);
  lose_commit = 0;
  expect(db->close(db, &failure));
  /* A server-held advisory lock gives a deterministic pending I/O probe. */
  expect(vectis_pg_connect(&config, &db, &failure));
  expect(vectis_pg_connect(&config, &other, &failure));
  expect(execute(other, "SELECT pg_advisory_lock(82736194)"));
  strcpy(sql, "SELECT pg_advisory_lock(82736194)");
  expect(db->query(db, sql, strlen(sql), NULL, 0u, 0, &cursor, &failure));
  assert(cursor->next(cursor, &step, &row, &count, &failure) ==
         VECTIS_ERR_TIMEOUT);
  expect(cursor->close(cursor, &failure));
  expect(db->close(db, &failure));
  expect(execute(other, "SELECT pg_advisory_unlock(82736194)"));
  expect(other->close(other, &failure));
  app_workflow(&config);
  memset(password, 0, sizeof(password));
  puts("standalone PostgreSQL native behavior ok");
  return 0;
}
