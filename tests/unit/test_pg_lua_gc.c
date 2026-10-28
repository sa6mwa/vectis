/* The actual Lua binding, with deterministic public-C89 receiver/interop
 * doubles and genuine cpkt result storage. Real transport is covered by the
 * separate PG adapter fault tests and normal/packed PostgreSQL service test. */
#include "lockdc_lua.h"
#include "vectis_lua_pg.h"
#include <assert.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vectis/pg.h>

static lc_xa_borrow *token;
static int adapters, results, releases;
static vectis_pg_diagnostic diagnostic;
typedef struct fixture_result {
  cpkt_postgres_result *native;
  lc_xa_borrow *borrow;
} fixture_result;
static int token_view(void *context, lc_xa_connection_view *view,
                      lc_error *error) {
  (void)context;
  (void)error;
  view->resource_kind = LC_XA_RESOURCE_POSTGRES;
  return LC_OK;
}
static int token_release(void *context, lc_error *error) {
  (void)context;
  (void)error;
  ++releases;
  return LC_OK;
}
int lc_lua_xa_borrow_retain(lua_State *L, int index, lc_xa_borrow **out,
                            lc_error *error) {
  assert(lua_touserdata(L, index) == token);
  *out = token;
  return lc_xa_borrow_retain(token, error);
}
static vectis_status view_result(vectis_pg_result *self,
                                 const cpkt_postgres_result **out,
                                 vectis_error *error) {
  (void)error;
  *out = ((fixture_result *)self->impl)->native;
  return VECTIS_OK;
}
vectis_status vectis_pg_result_close(vectis_pg_result *self,
                                     vectis_error *error) {
  fixture_result *value;
  lc_error native;
  (void)error;
  if (!self)
    return VECTIS_OK;
  value = (fixture_result *)self->impl;
  cpkt_postgres_result_free(value->native);
  lc_error_init(&native);
  assert(lc_xa_borrow_release(value->borrow, &native) == LC_OK);
  lc_error_cleanup(&native);
  free(value);
  free(self);
  --results;
  return VECTIS_OK;
}
static vectis_status
execute_fixture(vectis_pg *self, const char *sql, size_t sql_size,
                const vectis_pg_parameter *params, size_t count,
                int64_t deadline, vectis_pg_result **out, vectis_error *error) {
  cpkt_postgres_result_attribute attributes[512];
  char names[512][20], cell[32];
  fixture_result *value;
  vectis_pg_result *result;
  lc_error native;
  size_t i;
  (void)self;
  (void)sql_size;
  (void)deadline;
  (void)error;
  assert(sql && count == 512);
  value = (fixture_result *)calloc(1, sizeof(*value));
  result = (vectis_pg_result *)calloc(1, sizeof(*result));
  assert(value && result);
  value->native =
      cpkt_postgres_result_new_empty(NULL, CPKT_POSTGRES_RESULT_TUPLES_OK);
  assert(value->native);
  memset(attributes, 0, sizeof(attributes));
  for (i = 0; i < count; ++i) {
    snprintf(names[i], sizeof(names[i]), "col%u", (unsigned)i + 1u);
    attributes[i].name = names[i];
    attributes[i].type_oid = 25;
  }
  assert(cpkt_postgres_result_set_attributes(value->native, 512, attributes));
  for (i = 0; i < count; ++i) {
    assert(params[i].size < sizeof(cell));
    memcpy(cell, params[i].data, params[i].size);
    assert(cpkt_postgres_result_set_value(value->native, 0, (int)i, cell,
                                          (int)params[i].size));
  }
  lc_error_init(&native);
  assert(lc_xa_borrow_retain(token, &native) == LC_OK);
  lc_error_cleanup(&native);
  value->borrow = token;
  result->impl = value;
  result->view = view_result;
  result->close = vectis_pg_result_close;
  *out = result;
  ++results;
  return VECTIS_OK;
}
static const vectis_pg_diagnostic *get_diagnostic(const vectis_pg *self) {
  (void)self;
  return &diagnostic;
}
vectis_status vectis_pg_close(vectis_pg *self, vectis_error *error) {
  lc_error native;
  (void)error;
  if (!self)
    return VECTIS_OK;
  lc_error_init(&native);
  assert(lc_xa_borrow_release((lc_xa_borrow *)self->impl, &native) == LC_OK);
  lc_error_cleanup(&native);
  free(self);
  --adapters;
  return VECTIS_OK;
}
vectis_status vectis_pg_from_borrow(lc_xa_borrow *borrow, vectis_pg **out,
                                    vectis_error *error) {
  lc_error native;
  (void)error;
  assert(borrow == token);
  *out = (vectis_pg *)calloc(1, sizeof(**out));
  assert(*out);
  lc_error_init(&native);
  assert(lc_xa_borrow_retain(borrow, &native) == LC_OK);
  lc_error_cleanup(&native);
  (*out)->impl = borrow;
  (*out)->execute_materialized = execute_fixture;
  (*out)->diagnostic = get_diagnostic;
  (*out)->close = vectis_pg_close;
  ++adapters;
  return VECTIS_OK;
}
int main(int argc, char **argv) {
  const lc_xa_borrow_callbacks callbacks = {token_view, token_release};
  lc_error error;
  lua_State *L;
  int status;
  assert(argc == 2);
  lc_error_init(&error);
  assert(lc_xa_borrow_open(&callbacks, NULL, &token, &error) == LC_OK);
  L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  luaL_requiref(L, "pg", luaopen_vectis_pg_core, 1);
  lua_pop(L, 1);
  assert(luaL_loadfile(L, argv[1]) == LUA_OK &&
         lua_pcall(L, 0, 1, 0) == LUA_OK);
  lua_getglobal(L, "pg");
  lua_getfield(L, -1, "from_borrow");
  lua_pushlightuserdata(L, token);
  assert(lua_pcall(L, 1, 1, 0) == LUA_OK);
  lua_remove(L, -2);
  lua_pushvalue(L, -1);
  lua_setglobal(L, "db");
  status = lua_pcall(L, 1, 0, 0);
  if (status != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  assert(status == LUA_OK);
  lua_close(L);
  assert(adapters == 0 && results == 0);
  assert(lc_xa_borrow_release(token, &error) == LC_OK && releases == 1);
  lc_error_cleanup(&error);
  return 0;
}
