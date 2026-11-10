#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <cpkt/sqlite.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vectis/sqlite.h>

static void scalar(vectis_sqlite *db, const char *sql, int expected) {
  vectis_sqlite_statement *statement = NULL;
  vectis_sql_value value;
  vectis_error error;
  int row;
  assert(db->prepare(db, sql, strlen(sql), 1, &statement, &error) == VECTIS_OK);
  assert(statement->bind(statement, NULL, 0u, &error) == VECTIS_OK);
  assert(statement->next(statement, &row, &error) == VECTIS_OK && row);
  assert(vectis_sqlite_value(statement, 0u, &value, &error) == VECTIS_OK);
  assert(value.type == VECTIS_SQL_INTEGER && value.integer == expected);
  assert(statement->next(statement, &row, &error) == VECTIS_OK && !row);
  statement->close(statement);
}

static void execute(vectis_sqlite *db, const char *sql) {
  vectis_error error;
  assert(db->exec(db, sql, strlen(sql), NULL, 0u, NULL, &error) == VECTIS_OK);
}

static void *worker(void *unused) {
  vectis_sqlite *db = NULL;
  vectis_sqlite_statement *statement = NULL;
  vectis_error error;
  vectis_status status;
  char *sql, *at;
  char *directory, *path;
  char target[24];
  size_t i;
#if defined(__linux__)
  pthread_attr_t attributes;
  size_t size, guard;
  assert(pthread_getattr_np(pthread_self(), &attributes) == 0);
  assert(pthread_attr_getstacksize(&attributes, &size) == 0);
  assert(pthread_attr_getguardsize(&attributes, &guard) == 0 && guard > 0u);
  assert(pthread_attr_destroy(&attributes) == 0);
  printf("SQLite worker stack: %lu bytes; default attributes, guard %lu\n",
         (unsigned long)size, (unsigned long)guard);
#endif
  (void)unused;
  assert(strcmp(cpkt_sqlite_library_version(), "3.54.1") == 0);
  assert(vectis_sqlite_open(":memory:", NULL, &db, &error) == VECTIS_OK);
  sql = (char *)malloc(16384u);
  assert(sql != NULL);
  strcpy(sql, "SELECT 1");
  at = sql + strlen(sql);
  for (i = 1u; i < 1000u; ++i) {
    memcpy(at, "+1", 2u);
    at += 2;
  }
  *at = '\0';
  scalar(db, sql, 1000);
  memcpy(at, "+1", 3u);
  assert(db->prepare(db, sql, strlen(sql), 1, &statement, &error) ==
         VECTIS_ERR_STATE);
  assert(statement == NULL && error.dependency_code == CPKT_SQLITE_ERROR);
  assert(strstr(error.message, "Expression tree") != NULL);
  scalar(db, "SELECT 42", 42);
  execute(db, "CREATE TABLE operand(x)");
  execute(db, "INSERT INTO operand VALUES(1),(-3),(0),(5),(NULL)");
  execute(db, "CREATE INDEX operand_expr ON operand(x+2)");
  scalar(db, "SELECT sum((x+2)*(x+3)) FROM operand INDEXED BY operand_expr",
         74);
  scalar(db, "SELECT sum((x+2)*(x+3)) FROM operand NOT INDEXED", 74);
  strcpy(sql, "SELECT ");
  at = sql + strlen(sql);
  for (i = 0u; i < 400u; ++i) {
    memcpy(at, "abs(", 4u);
    at += 4;
  }
  *at++ = '1';
  for (i = 0u; i < 400u; ++i)
    *at++ = ')';
  *at = '\0';
  status = db->prepare(db, sql, strlen(sql), 1, &statement, &error);
  if (status == VECTIS_OK) {
    vectis_sql_value value;
    int row;
    assert(statement->bind(statement, NULL, 0u, &error) == VECTIS_OK);
    assert(statement->next(statement, &row, &error) == VECTIS_OK && row);
    assert(vectis_sqlite_value(statement, 0u, &value, &error) == VECTIS_OK &&
           value.type == VECTIS_SQL_INTEGER && value.integer == 1);
    assert(statement->next(statement, &row, &error) == VECTIS_OK && !row);
    statement->close(statement);
  } else {
    assert(status == VECTIS_ERR_STATE && statement == NULL &&
           error.dependency_code == CPKT_SQLITE_ERROR);
    assert(strstr(error.message, "Recursion limit") != NULL ||
           strstr(error.message, "Expression tree") != NULL ||
           strstr(error.message, "native stack budget exhausted") != NULL);
  }
  scalar(db, "SELECT 42", 42);
  assert(db->close(db, &error) == VECTIS_OK);
  free(sql);

  /* The former recursive 4 KiB pathname buffer overflowed small workers. */
  directory = (char *)malloc(64u);
  path = (char *)malloc(128u);
  assert(directory != NULL && path != NULL);
  strcpy(directory, "sqlite-worker-XXXXXX");
  assert(mkdtemp(directory) != NULL);
  for (i = 0u; i < 40u; ++i) {
    sprintf(path, "%s/link%02lu", directory, (unsigned long)i);
    if (i + 1u == 40u)
      strcpy(target, "final.db");
    else
      sprintf(target, "link%02lu", (unsigned long)(i + 1u));
    assert(symlink(target, path) == 0);
  }
  sprintf(path, "%s/link00", directory);
  assert(vectis_sqlite_open(path, NULL, &db, &error) == VECTIS_OK);
  execute(db, "CREATE TABLE saved(value)");
  execute(db, "INSERT INTO saved VALUES(42)");
  assert(db->close(db, &error) == VECTIS_OK);
  sprintf(path, "%s/final.db", directory);
  assert(vectis_sqlite_open(path, NULL, &db, &error) == VECTIS_OK);
  scalar(db, "SELECT value FROM saved", 42);
  assert(db->close(db, &error) == VECTIS_OK);
  assert(unlink(path) == 0);
  for (i = 0u; i < 40u; ++i) {
    sprintf(path, "%s/link%02lu", directory, (unsigned long)i);
    assert(unlink(path) == 0);
  }
  assert(rmdir(directory) == 0);
  free(path);
  free(directory);
  return NULL;
}
int main(void) {
  pthread_t thread;
  assert(pthread_create(&thread, NULL, worker, NULL) == 0);
  assert(pthread_join(thread, NULL) == 0);
  return 0;
}
