/* Deterministic public-C89 transport faults; real generic borrow ownership. */
#define cpkt_postgres_is_nonblocking fixture_nonblocking
#define cpkt_postgres_is_busy fixture_busy
#define cpkt_postgres_pipeline_status_get fixture_pipeline
#define cpkt_postgres_transaction_status_get fixture_transaction
#define cpkt_postgres_send_query_params fixture_send
#define cpkt_postgres_flush fixture_flush
#define cpkt_postgres_consume_input fixture_consume
#define cpkt_postgres_get_result_checked fixture_retrieve
#define cpkt_postgres_error_message fixture_message
#define cpkt_postgres_result_status_get fixture_status
#define cpkt_postgres_result_error_field fixture_field
#define cpkt_postgres_result_error_message fixture_result_message
#define cpkt_postgres_result_free fixture_free
#define cpkt_postgres_socket fixture_socket
#include "../../src/vectis_pg.c"
#include <assert.h>
struct cpkt_postgres_connection {
  int unused;
};
struct cpkt_postgres_result {
  int unused;
};
static struct cpkt_postgres_connection connection_fixture;
static struct cpkt_postgres_result result_fixture;
static int retrievals, sends, freed, released, invalid, mode, consume_calls,
    ready_fd;
static int token_view(void *context, lc_xa_connection_view *view,
                      lc_error *error) {
  (void)context;
  if (invalid) {
    lc_error_set(error, LC_ERR_INVALID, 0, "expired branch", NULL, NULL, NULL);
    return LC_ERR_INVALID;
  }
  view->resource_kind = LC_XA_RESOURCE_POSTGRES;
  view->native_connection = &connection_fixture;
  return LC_OK;
}
static int token_release(void *context, lc_error *error) {
  (void)context;
  (void)error;
  ++released;
  return LC_OK;
}
int fixture_nonblocking(const cpkt_postgres_connection *c) {
  (void)c;
  return 1;
}
int fixture_busy(cpkt_postgres_connection *c) {
  (void)c;
  return mode == 3 && consume_calls == 1;
}
cpkt_postgres_pipeline_status
fixture_pipeline(const cpkt_postgres_connection *c) {
  (void)c;
  return CPKT_POSTGRES_PIPELINE_OFF;
}
cpkt_postgres_transaction_status
fixture_transaction(const cpkt_postgres_connection *c) {
  (void)c;
  return CPKT_POSTGRES_TRANSACTION_IN_TRANSACTION;
}
int fixture_send(cpkt_postgres_connection *c, const char *sql, int count,
                 const cpkt_postgres_oid *types, const char *const *values,
                 const int *lengths, const int *formats, int result_format) {
  (void)c;
  (void)sql;
  (void)result_format;
  ++sends;
  if (count) {
    assert(count == 3 && types[0] == 25 && !formats[0] && lengths[0] == 3 &&
           !strcmp(values[0], "abc"));
    assert(types[1] == 17 && formats[1] && lengths[1] == 3 &&
           !memcmp(values[1], "a\0b", 3u));
    assert(!values[2]);
  }
  return 1;
}
int fixture_flush(cpkt_postgres_connection *c) {
  (void)c;
  return 0;
}
int fixture_consume(cpkt_postgres_connection *c) {
  (void)c;
  ++consume_calls;
  return 1;
}
int fixture_socket(const cpkt_postgres_connection *c) {
  (void)c;
  return ready_fd;
}
char *fixture_message(const cpkt_postgres_connection *c) {
  (void)c;
  return NULL;
}
cpkt_postgres_retrieval_status
fixture_retrieve(cpkt_postgres_connection *c, cpkt_postgres_result **out,
                 cpkt_postgres_retrieval_error *error) {
  (void)c;
  memset(error, 0, sizeof(*error));
  *out = NULL;
  ++retrievals;
  if (mode == 1) {
    error->code = CPKT_POSTGRES_RETRIEVAL_ERROR_EVENT_PREPARATION;
    strcpy(error->sqlstate, "23505");
    strcpy(error->server_message, "discarded duplicate key");
    error->result_discarded = 1;
    return CPKT_POSTGRES_RETRIEVAL_FAILED;
  }
  if (retrievals == 1) {
    *out = &result_fixture;
    return CPKT_POSTGRES_RETRIEVAL_RESULT;
  }
  return CPKT_POSTGRES_RETRIEVAL_DRAINED;
}
cpkt_postgres_result_status fixture_status(const cpkt_postgres_result *r) {
  (void)r;
  return mode == 2 ? CPKT_POSTGRES_RESULT_FATAL_ERROR
                   : CPKT_POSTGRES_RESULT_TUPLES_OK;
}
char *fixture_field(const cpkt_postgres_result *r, int field) {
  static char value[] = "22012";
  (void)r;
  assert(field == CPKT_POSTGRES_DIAG_SQLSTATE);
  return value;
}
char *fixture_result_message(const cpkt_postgres_result *r) {
  static char value[] = "division by zero";
  (void)r;
  return value;
}
void fixture_free(cpkt_postgres_result *r) {
  assert(r == &result_fixture);
  ++freed;
}
static vectis_pg *open_adapter(lc_xa_borrow **borrow) {
  static const lc_xa_borrow_callbacks callbacks = {token_view, token_release};
  vectis_pg *pg = NULL;
  vectis_error error;
  lc_error native;
  lc_error_init(&native);
  retrievals = sends = freed = released = invalid = consume_calls = 0;
  assert(lc_xa_borrow_open(&callbacks, NULL, borrow, &native) == LC_OK);
  assert(vectis_pg_from_borrow(*borrow, &pg, &error) == VECTIS_OK);
  assert(lc_xa_borrow_release(*borrow, &native) == LC_OK);
  lc_error_cleanup(&native);
  return pg;
}
int main(void) {
  lc_xa_borrow *borrow;
  vectis_pg *pg;
  vectis_pg_result *result = NULL;
  const cpkt_postgres_result *view;
  vectis_error error;
  vectis_pg_parameter parameters[3];
  int descriptors[2];
  lc_xa_wait wait;
  mode = 0;
  pg = open_adapter(&borrow);
  memset(parameters, 0, sizeof(parameters));
  parameters[0].data = "abc";
  parameters[0].size = 3;
  parameters[0].oid = 25;
  parameters[1].data = "a\0b";
  parameters[1].size = 3;
  parameters[1].oid = 17;
  parameters[1].binary = 1;
  parameters[2].is_null = 1;
  assert(pg->execute_materialized(pg, "SELECT $1,$2,$3", 15u, parameters, 3u,
                                  -1, &result, &error) == VECTIS_OK);
  assert(retrievals == 2 && sends == 1 && freed == 0 && released == 0);
  assert(pg->close(pg, &error) == VECTIS_OK && released == 0);
  assert(result->view(result, &view, &error) == VECTIS_OK &&
         view == &result_fixture);
  invalid = 1;
  assert(result->view(result, &view, &error) == VECTIS_ERR_STATE && !view);
  assert(result->close(result, &error) == VECTIS_OK && released == 1 &&
         freed == 1);
  mode = 1;
  pg = open_adapter(&borrow);
  assert(pg->execute_materialized(pg, "SELECT 1", 8u, NULL, 0, -1, &result,
                                  &error) == VECTIS_ERR_STATE &&
         !result);
  assert(retrievals == 1 && !strcmp(error.detail, "23505"));
  assert(pg->diagnostic(pg)->retrieval.result_discarded &&
         error.dependency_code ==
             CPKT_POSTGRES_RETRIEVAL_ERROR_EVENT_PREPARATION);
  assert(pg->execute_materialized(pg, "SELECT 1", 8u, NULL, 0, -1, &result,
                                  &error) == VECTIS_ERR_STATE &&
         sends == 1);
  assert(pg->close(pg, &error) == VECTIS_ERR_STATE && released == 1);
  mode = 2;
  pg = open_adapter(&borrow);
  assert(pg->execute_materialized(pg, "SELECT 1/0", 10u, NULL, 0, -1, &result,
                                  &error) == VECTIS_ERR_STATE);
  assert(!strcmp(error.detail, "22012") && freed == 1);
  assert(pg->close(pg, &error) == VECTIS_ERR_STATE && released == 1);
  mode = 0;
  pg = open_adapter(&borrow);
  assert(pg->execute_materialized(pg, "SELECT 1", 8u, NULL, 0, 0, &result,
                                  &error) == VECTIS_ERR_TIMEOUT);
  assert(!sends && !result);
  assert(pg->close(pg, NULL) == VECTIS_ERR_TIMEOUT && released == 1);
  mode = 0;
  pg = open_adapter(&borrow);
  assert(pg->execute_materialized(pg, "SELECT\0bad", 10u, NULL, 0, -1, &result,
                                  &error) == VECTIS_ERR_INVALID &&
         sends == 0);
  assert(pg->close(pg, &error) == VECTIS_ERR_INVALID && released == 1);
  pg = open_adapter(&borrow);
  parameters[0].data = "a\0b";
  assert(pg->execute_materialized(pg, "SELECT $1", 9u, parameters, 1u, -1,
                                  &result, &error) == VECTIS_ERR_INVALID &&
         sends == 0);
  assert(pg->close(pg, &error) == VECTIS_ERR_INVALID);
  assert(pipe(descriptors) == 0 && write(descriptors[1], "x", 1u) == 1);
  mode = 3;
  ready_fd = descriptors[0];
  pg = open_adapter(&borrow);
  assert(pg->execute_materialized(pg, "SELECT 1", 8u, NULL, 0, -1, &result,
                                  &error) == VECTIS_OK);
  assert(consume_calls >= 2 && result->close(result, &error) == VECTIS_OK &&
         pg->close(pg, &error) == VECTIS_OK);
  memset(&wait, 0, sizeof(wait));
  wait.fd = descriptors[0];
  wait.events = 1u;
  wait.deadline_ms = -1;
  assert(vectis_storage_wait_blocking(&wait, &error) == VECTIS_OK);
  close(descriptors[0]);
  close(descriptors[1]);
  wait.fd = -1;
  wait.events = 0;
  wait.deadline_ms = 0;
  assert(vectis_storage_wait_blocking(&wait, &error) == VECTIS_OK);
  wait.deadline_ms = -1;
  assert(vectis_storage_wait_blocking(&wait, &error) == VECTIS_ERR_STATE);
  wait.fd = 0;
  wait.events = 4;
  assert(vectis_storage_wait_blocking(&wait, &error) == VECTIS_ERR_STATE);
  return 0;
}
