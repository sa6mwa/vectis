#include "vectis_lua_pg.h"
#include "lockdc_lua.h"
#include <lauxlib.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vectis/pg.h>
#define PG "vectis.pg.session"
#define RESULT "vectis.pg.result"
#define NULL_KEY "vectis.pg.NULL"
typedef struct pg_owner {
  vectis_pg *value;
  pid_t process;
  int busy;
} pg_owner;
typedef struct result_owner {
  vectis_pg_result *value;
  pid_t process;
  int busy;
} result_owner;
static int push_error(lua_State *L, const vectis_error *error) {
  lua_pushnil(L);
  lua_createtable(L, 0, 9);
  lua_pushinteger(L, error->code);
  lua_setfield(L, -2, "status");
  lua_pushstring(L, vectis_status_string(error->code));
  lua_setfield(L, -2, "status_string");
  lua_pushinteger(L, error->source);
  lua_setfield(L, -2, "source_code");
  lua_pushstring(L, vectis_error_source_string(error->source));
  lua_setfield(L, -2, "source");
  lua_pushstring(L, error->message);
  lua_setfield(L, -2, "message");
  lua_pushstring(L, error->detail);
  lua_setfield(L, -2, "sqlstate");
  if (error->detail[0]) {
    lua_pushstring(L, error->detail);
    lua_setfield(L, -2, "detail");
  }
  lua_pushinteger(L, error->dependency_code);
  lua_setfield(L, -2, "retrieval_code");
  if (error->dependency_code) {
    lua_pushinteger(L, error->dependency_code);
    lua_setfield(L, -2, "dependency_code");
  }
  return 2;
}
static pg_owner *session(lua_State *L) {
  pg_owner *owner = (pg_owner *)luaL_checkudata(L, 1, PG);
  luaL_argcheck(L, owner->value != NULL, 1, "PostgreSQL adapter is closed");
  luaL_argcheck(L, owner->process == getpid(), 1,
                "foreign-process PostgreSQL adapter");
  return owner;
}
static const cpkt_postgres_result *result_view(lua_State *L) {
  result_owner *owner = (result_owner *)luaL_checkudata(L, 1, RESULT);
  const cpkt_postgres_result *value = NULL;
  vectis_error error;
  luaL_argcheck(L, owner->value != NULL, 1, "PostgreSQL result is closed");
  if (owner->value->view(owner->value, &value, &error) != VECTIS_OK)
    luaL_error(L, "%s", error.message);
  return value;
}
static int session_close(lua_State *L) {
  pg_owner *owner = (pg_owner *)luaL_checkudata(L, 1, PG);
  vectis_error error;
  vectis_status status;
  if (owner->busy) {
    if (lua_toboolean(L, lua_upvalueindex(1)))
      return 0;
    return luaL_error(L, "cannot close an active PostgreSQL adapter");
  }
  if (owner->process != getpid()) {
    if (lua_toboolean(L, lua_upvalueindex(1)))
      return 0;
    return luaL_error(L, "foreign-process PostgreSQL adapter close");
  }
  status = vectis_pg_close(owner->value, &error);
  owner->value = NULL;
  if (status != VECTIS_OK && !lua_toboolean(L, lua_upvalueindex(1)))
    return push_error(L, &error);
  lua_pushboolean(L, 1);
  return 1;
}
static int result_close(lua_State *L) {
  result_owner *owner = (result_owner *)luaL_checkudata(L, 1, RESULT);
  vectis_error error;
  vectis_status status;
  if (owner->busy) {
    if (lua_toboolean(L, lua_upvalueindex(1)))
      return 0;
    return luaL_error(L, "cannot close an active PostgreSQL result");
  }
  if (owner->process != getpid()) {
    if (lua_toboolean(L, lua_upvalueindex(1)))
      return 0;
    return luaL_error(L, "foreign-process PostgreSQL result close");
  }
  status = vectis_pg_result_close(owner->value, &error);
  owner->value = NULL;
  if (status != VECTIS_OK && !lua_toboolean(L, lua_upvalueindex(1)))
    return push_error(L, &error);
  lua_pushboolean(L, 1);
  return 1;
}
static int from_borrow(lua_State *L) {
  pg_owner *owner;
  lc_xa_borrow *borrow = NULL;
  lc_error native, cleanup;
  vectis_error error;
  vectis_status status;
  char message[256];
  luaL_checkany(L, 1);
  owner = (pg_owner *)lua_newuserdatauv(L, sizeof(*owner), 0);
  memset(owner, 0, sizeof(*owner));
  owner->process = getpid();
  luaL_setmetatable(L, PG);
  lc_error_init(&native);
  lc_error_init(&cleanup);
  if (lc_lua_xa_borrow_retain(L, 1, &borrow, &native) != LC_OK) {
    memset(message, 0, sizeof(message));
    if (native.message)
      strncpy(message, native.message, sizeof(message) - 1u);
    lc_error_cleanup(&native);
    return luaL_error(L, "%s", message);
  }
  lc_error_cleanup(&native);
  status = vectis_pg_from_borrow(borrow, &owner->value, &error);
  /* No Lua allocation between temporary retain and release. */
  if (lc_xa_borrow_release(borrow, &cleanup) != LC_OK && status == VECTIS_OK) {
    (void)vectis_pg_close(owner->value, &error);
    owner->value = NULL;
    memset(message, 0, sizeof(message));
    if (cleanup.message)
      strncpy(message, cleanup.message, sizeof(message) - 1u);
    lc_error_cleanup(&cleanup);
    return luaL_error(L, "%s", message);
  }
  lc_error_cleanup(&cleanup);
  if (status != VECTIS_OK)
    return push_error(L, &error);
  return 1;
}
static int is_null(lua_State *L, int index) {
  int equal;
  index = lua_absindex(L, index);
  lua_getfield(L, LUA_REGISTRYINDEX, NULL_KEY);
  equal = lua_rawequal(L, index, -1);
  lua_pop(L, 1);
  return equal;
}
static int execute_inner(lua_State *L) {
  pg_owner *owner = session(L);
  result_owner *result;
  vectis_pg_parameter *parameters;
  const char *sql;
  size_t sql_size, count, i;
  int64_t deadline;
  int anchors;
  lua_Integer integer;
  struct timespec now;
  vectis_error error;
  vectis_status status;
  luaL_checktype(L, 2, LUA_TSTRING);
  sql = lua_tolstring(L, 2, &sql_size);
  luaL_checktype(L, 3, LUA_TTABLE);
  count = lua_rawlen(L, 3);
  luaL_argcheck(L, count <= 65535u, 3, "too many PostgreSQL parameters");
  lua_pushnil(L);
  while (lua_next(L, 3)) {
    luaL_argcheck(L, lua_isinteger(L, -2), 3,
                  "parameters must be a dense array");
    integer = lua_tointeger(L, -2);
    luaL_argcheck(L, integer >= 1 && (lua_Unsigned)integer <= count, 3,
                  "parameters must be a dense array");
    lua_pop(L, 1);
  }
  if (lua_isnoneornil(L, 4)) {
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > ((uint64_t)INT64_MAX - 30999u) / 1000u)
      return luaL_error(L, "cannot read monotonic clock");
    deadline =
        (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000 + 30000;
  } else {
    luaL_argcheck(L, lua_isinteger(L, 4), 4,
                  "deadline must be monotonic integer milliseconds");
    deadline = (int64_t)lua_tointeger(L, 4);
  }
  /* Retain exact parameter strings independently of mutable caller tables
   * throughout all subsequent Lua allocations and the native copy. */
  lua_createtable(L, (int)count, 0);
  anchors = lua_gettop(L);
  parameters = (vectis_pg_parameter *)lua_newuserdatauv(
      L, count * sizeof(*parameters), 0);
  memset(parameters, 0, count * sizeof(*parameters));
  for (i = 0; i < count; ++i) {
    vectis_pg_parameter *p = &parameters[i];
    lua_rawgeti(L, 3, (lua_Integer)i + 1);
    if (is_null(L, -1))
      p->is_null = 1;
    else if (lua_type(L, -1) == LUA_TSTRING) {
      p->data = lua_tolstring(L, -1, &p->size);
      lua_pushvalue(L, -1);
      lua_rawseti(L, anchors, (lua_Integer)i + 1);
    } else {
      luaL_checktype(L, -1, LUA_TTABLE);
      lua_pushliteral(L, "oid");
      lua_rawget(L, -2);
      if (!lua_isnil(L, -1)) {
        luaL_argcheck(L, lua_isinteger(L, -1), 3,
                      "parameter OID must be uint32");
        integer = lua_tointeger(L, -1);
        luaL_argcheck(L, integer >= 0 && (lua_Unsigned)integer <= 0xffffffffu,
                      3, "parameter OID must be uint32");
        p->oid = (cpkt_postgres_oid)integer;
      }
      lua_pop(L, 1);
      lua_pushliteral(L, "binary");
      lua_rawget(L, -2);
      luaL_argcheck(L, lua_isnil(L, -1) || lua_isboolean(L, -1), 3,
                    "binary must be boolean");
      p->binary = lua_toboolean(L, -1);
      lua_pop(L, 1);
      lua_pushliteral(L, "value");
      lua_rawget(L, -2);
      if (is_null(L, -1))
        p->is_null = 1;
      else {
        luaL_checktype(L, -1, LUA_TSTRING);
        p->data = lua_tolstring(L, -1, &p->size);
        lua_pushvalue(L, -1);
        lua_rawseti(L, anchors, (lua_Integer)i + 1);
      }
      lua_pop(L, 1);
    }
    luaL_argcheck(L, p->size <= (size_t)INT_MAX, 3,
                  "parameter exceeds native int length");
    lua_pop(L, 1);
  }
  result = (result_owner *)lua_newuserdatauv(L, sizeof(*result), 0);
  memset(result, 0, sizeof(*result));
  result->process = getpid();
  luaL_setmetatable(L, RESULT);
  status = owner->value->execute_materialized(owner->value, sql, sql_size,
                                              parameters, count, deadline,
                                              &result->value, &error);
  if (status != VECTIS_OK)
    return push_error(L, &error);
  return 1;
}
static int protected_call(lua_State *L, int *busy, lua_CFunction operation) {
  int args = lua_gettop(L), status, i;
  luaL_checkstack(L, args + 1, "PostgreSQL execution arguments");
  luaL_argcheck(L, !*busy, 1, "PostgreSQL callback reentry is not allowed");
  lua_pushcfunction(L, operation);
  for (i = 1; i <= args; ++i)
    lua_pushvalue(L, i);
  *busy = 1;
  status = lua_pcall(L, args, LUA_MULTRET, 0);
  *busy = 0;
  return status;
}
static int guarded_session(lua_State *L) {
  pg_owner *owner = session(L);
  int args = lua_gettop(L);
  int status =
      protected_call(L, &owner->busy, lua_tocfunction(L, lua_upvalueindex(1)));
  if (status != LUA_OK) {
    vectis_pg_result *unused = NULL;
    vectis_error error;
    /* Caught shape/OOM exceptions still make managed work rollback-only. */
    if (owner->value)
      (void)owner->value->execute_materialized(owner->value, NULL, 0, NULL, 0,
                                               -1, &unused, &error);
    return lua_error(L);
  }
  return lua_gettop(L) - args;
}
static int guarded_result(lua_State *L) {
  result_owner *owner = (result_owner *)luaL_checkudata(L, 1, RESULT);
  int args = lua_gettop(L);
  if (protected_call(L, &owner->busy,
                     lua_tocfunction(L, lua_upvalueindex(1))) != LUA_OK)
    return lua_error(L);
  return lua_gettop(L) - args;
}
static int rows(lua_State *L) {
  lua_pushinteger(L, cpkt_postgres_result_row_count(result_view(L)));
  return 1;
}
static int columns(lua_State *L) {
  const cpkt_postgres_result *value = result_view(L);
  int count = cpkt_postgres_result_field_count(value), i;
  lua_createtable(L, count, 0);
  for (i = 0; i < count; ++i) {
    lua_createtable(L, 0, 3);
    lua_pushstring(L, cpkt_postgres_result_field_name(value, i));
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, (lua_Integer)cpkt_postgres_result_field_type(value, i));
    lua_setfield(L, -2, "oid");
    lua_pushinteger(L, cpkt_postgres_result_field_format(value, i));
    lua_setfield(L, -2, "format");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}
static int value_materialized(lua_State *L) {
  const cpkt_postgres_result *value = result_view(L);
  lua_Integer row = luaL_checkinteger(L, 2), column = luaL_checkinteger(L, 3);
  luaL_argcheck(L, row >= 1 && row <= cpkt_postgres_result_row_count(value), 2,
                "row outside result");
  luaL_argcheck(
      L, column >= 1 && column <= cpkt_postgres_result_field_count(value), 3,
      "column outside result");
  if (cpkt_postgres_result_value_is_null(value, (int)row - 1, (int)column - 1))
    lua_getfield(L, LUA_REGISTRYINDEX, NULL_KEY);
  else
    lua_pushlstring(
        L, cpkt_postgres_result_value(value, (int)row - 1, (int)column - 1),
        (size_t)cpkt_postgres_result_value_length(value, (int)row - 1,
                                                  (int)column - 1));
  return 1;
}
static int diagnostic(lua_State *L) {
  pg_owner *owner = session(L);
  push_error(L, &owner->value->diagnostic(owner->value)->error);
  return 1;
}
static void metatable(lua_State *L, const char *name, lua_CFunction close,
                      const luaL_Reg *methods, lua_CFunction guard) {
  luaL_newmetatable(L, name);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  for (; methods->name; ++methods) {
    lua_pushcfunction(L, methods->func);
    lua_pushcclosure(L, guard, 1);
    lua_setfield(L, -2, methods->name);
  }
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, close, 1);
  lua_setfield(L, -2, "close");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, close, 1);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);
}
int luaopen_vectis_pg_core(lua_State *L) {
  static const luaL_Reg pg_methods[] = {{"execute_materialized", execute_inner},
                                        {"diagnostic", diagnostic},
                                        {NULL, NULL}};
  static const luaL_Reg result_methods[] = {
      {"rows", rows},
      {"columns", columns},
      {"value_materialized", value_materialized},
      {NULL, NULL}};
  metatable(L, PG, session_close, pg_methods, guarded_session);
  metatable(L, RESULT, result_close, result_methods, guarded_result);
  lua_getfield(L, LUA_REGISTRYINDEX, NULL_KEY);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, NULL_KEY);
  }
  lua_newtable(L);
  lua_insert(L, -2);
  lua_setfield(L, -2, "NULL");
  lua_pushcfunction(L, from_borrow);
  lua_setfield(L, -2, "from_borrow");
  return 1;
}
