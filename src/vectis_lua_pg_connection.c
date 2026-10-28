#include "vectis_lua_pg_connection.h"
#include <lauxlib.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <vectis/pg.h>

#define PG_DB "vectis.pg.connection"
#define PG_CURSOR "vectis.pg.cursor"
#define PG_STATEMENT "vectis.pg.statement"
#define PG_NULL "vectis.pg.NULL"
enum { PG_LUA_DB = 1, PG_LUA_CURSOR = 2, PG_LUA_STATEMENT = 3 };
typedef struct pg_lua_owner {
  void *native;
  struct pg_lua_owner *database;
  int kind, busy, declaration, transaction;
  pid_t process;
  void *app_context;
  void (*enter)(void *);
  void (*leave)(void *);
} pg_lua_owner;
static const char *pg_lua_type(int kind) {
  return kind == PG_LUA_DB       ? PG_DB
         : kind == PG_LUA_CURSOR ? PG_CURSOR
                                 : PG_STATEMENT;
}
static pg_lua_owner *pg_lua_check(lua_State *L, int kind) {
  pg_lua_owner *owner =
      (pg_lua_owner *)luaL_checkudata(L, 1, pg_lua_type(kind));
  pg_lua_owner *db = kind == PG_LUA_DB ? owner : owner->database;
  luaL_argcheck(L, owner->native != NULL, 1, "PostgreSQL receiver is closed");
  luaL_argcheck(L, db->declaration || db->process == getpid(), 1,
                "foreign-process PostgreSQL receiver");
  return owner;
}
static const char *pg_lua_string(lua_State *L, int index, size_t *size) {
  const char *value;
  luaL_checktype(L, index, LUA_TSTRING);
  value = lua_tolstring(L, index, size);
  luaL_argcheck(L, memchr(value, 0, *size) == NULL, index,
                "PostgreSQL string contains NUL");
  return value;
}
static void pg_lua_field(lua_State *L, int index, const char *name) {
  if (lua_isnoneornil(L, index)) {
    lua_pushnil(L);
    return;
  }
  index = lua_absindex(L, index);
  lua_pushstring(L, name);
  lua_rawget(L, index);
}
static void pg_lua_fields(lua_State *L, int index, const char *const *fields) {
  const char *key;
  size_t size;
  int i;
  index = lua_absindex(L, index);
  if (lua_isnoneornil(L, index))
    return;
  luaL_checktype(L, index, LUA_TTABLE);
  lua_pushnil(L);
  while (lua_next(L, index)) {
    key = pg_lua_string(L, -2, &size);
    for (i = 0; fields[i] != NULL && strcmp(key, fields[i]) != 0; ++i) {
    }
    if (fields[i] == NULL)
      luaL_error(L, "unknown PostgreSQL option '%s'", key);
    lua_pop(L, 1);
  }
}
static int64_t pg_lua_timeout(lua_State *L, int index, int64_t fallback) {
  lua_Integer value;
  if (lua_isnil(L, index))
    return fallback;
  value = luaL_checkinteger(L, index);
  luaL_argcheck(L, value >= 0, index,
                "PostgreSQL timeout must be nonnegative integer milliseconds");
  return (int64_t)value;
}
static int pg_lua_error(lua_State *L, const vectis_error *error,
                        const vectis_pg_diagnostic *diagnostic,
                        const char *operation) {
  lua_pushnil(L);
  lua_createtable(L, 0, 10);
  lua_pushinteger(L, error->code);
  lua_setfield(L, -2, "status");
  lua_pushstring(L, vectis_status_string(error->code));
  lua_setfield(L, -2, "status_string");
  lua_pushinteger(L, error->source);
  lua_setfield(L, -2, "source_code");
  lua_pushstring(L, vectis_error_source_string(error->source));
  lua_setfield(L, -2, "source");
  lua_pushinteger(L, error->dependency_code);
  lua_setfield(L, -2, "dependency_code");
  lua_pushstring(L, error->message);
  lua_setfield(L, -2, "message");
  lua_pushstring(L, operation);
  lua_setfield(L, -2, "operation");
  if (diagnostic != NULL) {
    lua_pushstring(L, diagnostic->sqlstate);
    lua_setfield(L, -2, "sqlstate");
    lua_pushinteger(L, diagnostic->retrieval.code);
    lua_setfield(L, -2, "retrieval_code");
  }
  if (error->detail[0]) {
    lua_pushstring(L, error->detail);
    lua_setfield(L, -2, "transaction_outcome");
  }
  return 2;
}
static const vectis_pg_diagnostic *pg_lua_diagnostic(pg_lua_owner *owner) {
  pg_lua_owner *db = owner->kind == PG_LUA_DB ? owner : owner->database;
  if (owner->kind == PG_LUA_CURSOR)
    return vectis_pg_cursor_diagnostic((vectis_pg_cursor *)owner->native);
  return vectis_pg_connection_diagnostic((vectis_pg_connection *)db->native);
}
static int pg_lua_close(lua_State *L) {
  int kind = (int)lua_tointeger(L, lua_upvalueindex(1));
  int finalizer = lua_toboolean(L, lua_upvalueindex(2));
  pg_lua_owner *owner =
      (pg_lua_owner *)luaL_checkudata(L, 1, pg_lua_type(kind));
  pg_lua_owner *db = kind == PG_LUA_DB ? owner : owner->database;
  vectis_error error;
  vectis_status status;
  if (owner->native == NULL) {
    if (!finalizer)
      lua_pushboolean(L, 1);
    return !finalizer;
  }
  if (owner->busy || db->busy ||
      (!db->declaration && db->process != getpid())) {
    if (finalizer)
      return 0;
    return luaL_error(
        L, "cannot close active or foreign-process PostgreSQL receiver");
  }
  if (kind == PG_LUA_DB)
    status = vectis_pg_connection_close((vectis_pg_connection *)owner->native,
                                        &error);
  else if (kind == PG_LUA_CURSOR)
    status = vectis_pg_cursor_close((vectis_pg_cursor *)owner->native, &error);
  else
    status =
        vectis_pg_statement_close((vectis_pg_statement *)owner->native, &error);
  if (status != VECTIS_OK) {
    if (finalizer)
      return 0;
    return pg_lua_error(L, &error, NULL, "close");
  }
  owner->native = NULL;
  if (!finalizer)
    lua_pushboolean(L, 1);
  return !finalizer;
}
static pg_lua_owner *pg_lua_new_owner(lua_State *L, int kind, int parent) {
  pg_lua_owner *owner;
  pg_lua_owner *db = NULL;
  if (parent != 0) {
    parent = lua_absindex(L, parent);
    db = (pg_lua_owner *)luaL_checkudata(L, parent, PG_DB);
  }
  owner = (pg_lua_owner *)lua_newuserdatauv(L, sizeof(*owner), 1);
  memset(owner, 0, sizeof(*owner));
  owner->kind = kind;
  owner->database = db;
  owner->process = getpid();
  luaL_setmetatable(L, pg_lua_type(kind));
  if (parent != 0) {
    lua_pushvalue(L, parent);
    lua_setiuservalue(L, -2, 1);
  }
  return owner;
}
static void pg_lua_config(lua_State *L, int index, int declaration,
                          vectis_pg_config *config, const char **keys,
                          const char **values, const char **name) {
  static const char *const fields[] = {"parameters", "connect_timeout_ms",
                                       "io_timeout_ms", NULL};
  static const char *const declaration_fields[] = {
      "name", "parameters", "connect_timeout_ms", "io_timeout_ms", NULL};
  int anchors, parameters;
  size_t size, count = 0u;
  index = lua_absindex(L, index);
  pg_lua_fields(L, index, declaration ? declaration_fields : fields);
  vectis_pg_config_init(config);
  pg_lua_field(L, index, "connect_timeout_ms");
  config->connect_timeout_ms =
      pg_lua_timeout(L, -1, config->connect_timeout_ms);
  lua_pop(L, 1);
  pg_lua_field(L, index, "io_timeout_ms");
  config->io_timeout_ms = pg_lua_timeout(L, -1, config->io_timeout_ms);
  lua_pop(L, 1);
  if (declaration) {
    pg_lua_field(L, index, "name");
    *name = pg_lua_string(L, -1, &size);
    if (size == 0u)
      luaL_error(L, "PostgreSQL declaration requires a name");
  }
  lua_createtable(L, 256, 0);
  anchors = lua_gettop(L);
  pg_lua_field(L, index, "parameters");
  parameters = lua_gettop(L);
  if (!lua_isnil(L, parameters)) {
    luaL_checktype(L, parameters, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, parameters)) {
      if (count == 128u)
        luaL_error(L, "too many PostgreSQL connection parameters");
      keys[count] = pg_lua_string(L, -2, &size);
      values[count] = pg_lua_string(L, -1, &size);
      lua_pushvalue(L, -2);
      lua_rawseti(L, anchors, (lua_Integer)count * 2 + 1);
      lua_pushvalue(L, -1);
      lua_rawseti(L, anchors, (lua_Integer)count * 2 + 2);
      ++count;
      lua_pop(L, 1);
    }
  }
  config->keywords = keys;
  config->values = values;
  config->parameter_count = count;
}
static int pg_lua_connect(lua_State *L) {
  pg_lua_owner *owner;
  vectis_pg_connection *connection = NULL;
  vectis_pg_config config;
  const char *keys[128], *values[128];
  vectis_error error;
  vectis_status status;
  int result, started = lua_toboolean(L, lua_upvalueindex(1));
  lua_settop(L, 1);
  luaL_checkstack(L, 24, "PostgreSQL constructor stack");
  owner = pg_lua_new_owner(L, PG_LUA_DB, 0);
  result = lua_gettop(L);
  pg_lua_config(L, 1, 0, &config, keys, values, NULL);
  if (started)
    status = vectis_pg_connect_start(&config, &connection, &error);
  else
    status = vectis_pg_connect(&config, &connection, &error);
  owner->native = connection;
  if (status != VECTIS_OK)
    return pg_lua_error(L, &error, NULL, started ? "connect_start" : "connect");
  lua_pushvalue(L, result);
  return 1;
}
int vectis_lua_pg_declare(lua_State *L, vectis_app *app, void *context,
                          void (*enter)(void *), void (*leave)(void *)) {
  pg_lua_owner *owner;
  vectis_pg_connection *connection = NULL;
  vectis_pg_config config;
  const char *keys[128], *values[128], *name;
  vectis_error error;
  int result;
  lua_settop(L, 2);
  luaL_checkstack(L, 24, "PostgreSQL declaration stack");
  luaL_requiref(L, "vectis.pg.connection.core",
                luaopen_vectis_pg_connection_core, 0);
  lua_pop(L, 1);
  owner = pg_lua_new_owner(L, PG_LUA_DB, 0);
  result = lua_gettop(L);
  owner->declaration = 1;
  owner->app_context = context;
  owner->enter = enter;
  owner->leave = leave;
  lua_pushvalue(L, 1);
  lua_setiuservalue(L, result, 1);
  pg_lua_config(L, 2, 1, &config, keys, values, &name);
  if (vectis_app_pg(app, name, &config, &connection, &error) != VECTIS_OK)
    return pg_lua_error(L, &error, NULL, "declare");
  owner->native = connection;
  lua_pushvalue(L, result);
  return 1;
}
static size_t pg_lua_dense(lua_State *L, int index, size_t limit) {
  size_t count;
  lua_Integer key;
  index = lua_absindex(L, index);
  luaL_checktype(L, index, LUA_TTABLE);
  count = lua_rawlen(L, index);
  if (count > limit)
    luaL_argerror(L, index, "PostgreSQL array exceeds limit");
  lua_pushnil(L);
  while (lua_next(L, index)) {
    luaL_argcheck(L, lua_isinteger(L, -2), index,
                  "PostgreSQL parameters must be a dense array");
    key = lua_tointeger(L, -2);
    luaL_argcheck(L, key >= 1 && (lua_Unsigned)key <= count, index,
                  "PostgreSQL parameters must be a dense array");
    lua_pop(L, 1);
  }
  return count;
}
static int pg_lua_null(lua_State *L, int index) {
  int equal;
  index = lua_absindex(L, index);
  lua_getfield(L, LUA_REGISTRYINDEX, PG_NULL);
  equal = lua_rawequal(L, index, -1);
  lua_pop(L, 1);
  return equal;
}
static cpkt_postgres_oid pg_lua_oid(lua_State *L, int index) {
  lua_Integer value;
  if (lua_isnil(L, index))
    return 0u;
  value = luaL_checkinteger(L, index);
  luaL_argcheck(L, value >= 0 && (lua_Unsigned)value <= UINT_MAX, index,
                "PostgreSQL OID must be uint32");
  return (cpkt_postgres_oid)value;
}
static size_t pg_lua_parameters(lua_State *L, int index,
                                vectis_pg_parameter *parameters) {
  static const char *const fields[] = {"value", "oid", "binary", NULL};
  size_t count = pg_lua_dense(L, index, VECTIS_SQL_MAX_PARAMETERS), i;
  int anchors, parameter, value;
  index = lua_absindex(L, index);
  memset(parameters, 0, count * sizeof(*parameters));
  lua_createtable(L, (int)count, 0);
  anchors = lua_gettop(L);
  for (i = 0u; i < count; ++i) {
    vectis_pg_parameter *p = &parameters[i];
    lua_rawgeti(L, index, (lua_Integer)i + 1);
    parameter = lua_gettop(L);
    value = parameter;
    if (!pg_lua_null(L, value) && lua_istable(L, value)) {
      pg_lua_fields(L, value, fields);
      pg_lua_field(L, parameter, "oid");
      p->oid = pg_lua_oid(L, -1);
      lua_pop(L, 1);
      pg_lua_field(L, parameter, "binary");
      if (!lua_isnil(L, -1))
        luaL_checktype(L, -1, LUA_TBOOLEAN);
      p->binary = lua_toboolean(L, -1);
      lua_pop(L, 1);
      pg_lua_field(L, parameter, "value");
      value = lua_gettop(L);
    }
    if (pg_lua_null(L, value))
      p->is_null = 1;
    else {
      if (p->binary)
        luaL_checktype(L, value, LUA_TSTRING);
      if (lua_isinteger(L, value))
        (void)lua_tolstring(L, value, NULL);
      else if (lua_type(L, value) == LUA_TBOOLEAN) {
        lua_pushstring(L, lua_toboolean(L, value) ? "true" : "false");
        value = lua_gettop(L);
      }
      luaL_argcheck(
          L, lua_type(L, value) == LUA_TSTRING, index,
          "PostgreSQL parameters require strings, integers, booleans, NULL or "
          "typed values; non-integer numbers require decimal text");
      p->data = lua_tolstring(L, value, &p->size);
      lua_pushvalue(L, value);
      lua_rawseti(L, anchors, (lua_Integer)i + 1);
    }
    lua_settop(L, parameter - 1);
  }
  return count;
}
static int64_t pg_lua_query_timeout(lua_State *L, int index) {
  static const char *const fields[] = {"timeout_ms", NULL};
  int64_t timeout;
  pg_lua_fields(L, index, fields);
  pg_lua_field(L, index, "timeout_ms");
  timeout = pg_lua_timeout(L, -1, -1);
  lua_pop(L, 1);
  return timeout;
}
static int pg_lua_poll(lua_State *L) {
  pg_lua_owner *owner = pg_lua_check(L, PG_LUA_DB);
  vectis_error error;
  int ready;
  if (vectis_pg_connection_poll((vectis_pg_connection *)owner->native, &ready,
                                &error) != VECTIS_OK)
    return pg_lua_error(L, &error, pg_lua_diagnostic(owner), "poll");
  lua_pushboolean(L, ready);
  return 1;
}
static int pg_lua_readiness(lua_State *L) {
  pg_lua_owner *owner = (pg_lua_owner *)lua_touserdata(L, 1);
  vectis_pg_readiness readiness;
  vectis_error error;
  vectis_status status =
      owner->kind == PG_LUA_DB
          ? vectis_pg_connection_readiness(
                (vectis_pg_connection *)owner->native, &readiness, &error)
          : vectis_pg_cursor_readiness((vectis_pg_cursor *)owner->native,
                                       &readiness, &error);
  if (status != VECTIS_OK)
    return pg_lua_error(L, &error, pg_lua_diagnostic(owner), "readiness");
  lua_createtable(L, 0, 3);
  lua_pushinteger(L, readiness.fd);
  lua_setfield(L, -2, "fd");
  lua_pushboolean(L, readiness.read);
  lua_setfield(L, -2, "read");
  lua_pushboolean(L, readiness.write);
  lua_setfield(L, -2, "write");
  return 1;
}
static vectis_pg_parameter *pg_lua_parameter_scratch(lua_State *L, int index) {
  size_t count;
  luaL_checktype(L, index, LUA_TTABLE);
  count = lua_rawlen(L, index);
  luaL_argcheck(L, count <= VECTIS_SQL_MAX_PARAMETERS, index,
                "too many PostgreSQL parameters");
  /* Allocation may run a finalizer that grows the table before parsing. */
  return (vectis_pg_parameter *)lua_newuserdatauv(
      L, VECTIS_SQL_MAX_PARAMETERS * sizeof(vectis_pg_parameter), 0);
}
static int pg_lua_query(lua_State *L) {
  pg_lua_owner *parent = (pg_lua_owner *)lua_touserdata(L, 1), *owner;
  vectis_pg_parameter *parameters;
  vectis_pg_cursor *cursor = NULL;
  vectis_error error;
  vectis_status status;
  const char *sql = NULL;
  size_t size = 0u, count;
  int result, parent_index = 1;
  int64_t timeout;
  if (parent->kind == PG_LUA_DB) {
    sql = pg_lua_string(L, 2, &size);
    timeout = pg_lua_query_timeout(L, 4);
    parameters = pg_lua_parameter_scratch(L, 3);
    count = pg_lua_parameters(L, 3, parameters);
  } else {
    timeout = pg_lua_query_timeout(L, 3);
    parameters = pg_lua_parameter_scratch(L, 2);
    count = pg_lua_parameters(L, 2, parameters);
    lua_getiuservalue(L, 1, 1);
    parent_index = lua_gettop(L);
  }
  owner = pg_lua_new_owner(L, PG_LUA_CURSOR, parent_index);
  result = lua_gettop(L);
  if (parent->kind == PG_LUA_DB)
    status = vectis_pg_connection_query((vectis_pg_connection *)parent->native,
                                        sql, size, parameters, count, timeout,
                                        &cursor, &error);
  else
    status =
        vectis_pg_statement_query((vectis_pg_statement *)parent->native,
                                  parameters, count, timeout, &cursor, &error);
  owner->native = cursor;
  if (status != VECTIS_OK)
    return pg_lua_error(L, &error, pg_lua_diagnostic(parent), "query");
  lua_pushvalue(L, result);
  return 1;
}
static int pg_lua_exec(lua_State *L) {
  pg_lua_owner *owner = pg_lua_check(L, PG_LUA_DB);
  vectis_pg_parameter *parameters;
  vectis_pg_command command;
  vectis_error error;
  size_t size, count;
  const char *sql = pg_lua_string(L, 2, &size);
  int64_t timeout = pg_lua_query_timeout(L, 4);
  parameters = pg_lua_parameter_scratch(L, 3);
  count = pg_lua_parameters(L, 3, parameters);
  if (vectis_pg_connection_exec((vectis_pg_connection *)owner->native, sql,
                                size, parameters, count, timeout, &command,
                                &error) != VECTIS_OK)
    return pg_lua_error(L, &error, pg_lua_diagnostic(owner), "exec");
  lua_createtable(L, 0, 2);
  lua_pushstring(L, command.command);
  lua_setfield(L, -2, "command");
  lua_pushstring(L, command.affected);
  lua_setfield(L, -2, "affected");
  return 1;
}
static int pg_lua_prepare(lua_State *L) {
  static const char *const fields[] = {"parameter_types", "timeout_ms", NULL};
  pg_lua_owner *db = pg_lua_check(L, PG_LUA_DB), *owner;
  cpkt_postgres_oid types[VECTIS_SQL_MAX_PARAMETERS];
  const char *name, *sql;
  size_t size, sql_size, count = 0u, i;
  int64_t timeout;
  int index, result;
  vectis_error error;
  vectis_pg_statement *statement = NULL;
  name = pg_lua_string(L, 2, &size);
  sql = pg_lua_string(L, 3, &sql_size);
  pg_lua_fields(L, 4, fields);
  pg_lua_field(L, 4, "timeout_ms");
  timeout = pg_lua_timeout(L, -1, -1);
  lua_pop(L, 1);
  pg_lua_field(L, 4, "parameter_types");
  index = lua_gettop(L);
  if (!lua_isnil(L, index)) {
    count = pg_lua_dense(L, index, VECTIS_SQL_MAX_PARAMETERS);
    for (i = 0u; i < count; ++i) {
      lua_rawgeti(L, index, (lua_Integer)i + 1);
      types[i] = pg_lua_oid(L, -1);
      lua_pop(L, 1);
    }
  }
  owner = pg_lua_new_owner(L, PG_LUA_STATEMENT, 1);
  result = lua_gettop(L);
  if (vectis_pg_connection_prepare((vectis_pg_connection *)db->native, name,
                                   sql, sql_size, types, count, timeout,
                                   &statement, &error) != VECTIS_OK)
    return pg_lua_error(L, &error, pg_lua_diagnostic(db), "prepare");
  owner->native = statement;
  lua_pushvalue(L, result);
  return 1;
}
static int pg_lua_next(lua_State *L, int blocking) {
  pg_lua_owner *owner = pg_lua_check(L, PG_LUA_CURSOR);
  vectis_pg_step step;
  const vectis_sql_value *row;
  size_t count, i;
  vectis_error error;
  vectis_status status =
      blocking ? vectis_pg_cursor_next((vectis_pg_cursor *)owner->native, &step,
                                       &row, &count, &error)
               : vectis_pg_cursor_next_ready((vectis_pg_cursor *)owner->native,
                                             &step, &row, &count, &error);
  if (status != VECTIS_OK)
    return pg_lua_error(L, &error, pg_lua_diagnostic(owner),
                        blocking ? "next" : "next_ready");
  if (step != VECTIS_PG_ROW) {
    lua_pushnil(L);
    if (step == VECTIS_PG_PENDING)
      lua_pushliteral(L, "pending");
    else
      lua_pushnil(L);
    return 2;
  }
  lua_createtable(L, (int)count, 0);
  for (i = 0u; i < count; ++i) {
    if (row[i].type == VECTIS_SQL_NULL)
      lua_getfield(L, LUA_REGISTRYINDEX, PG_NULL);
    else
      lua_pushlstring(L, (const char *)row[i].data, row[i].size);
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  if (!blocking) {
    lua_pushliteral(L, "row");
    return 2;
  }
  return 1;
}
static int pg_lua_next_blocking(lua_State *L) { return pg_lua_next(L, 1); }
static int pg_lua_next_ready(lua_State *L) { return pg_lua_next(L, 0); }
static int pg_lua_columns(lua_State *L) {
  pg_lua_owner *owner = pg_lua_check(L, PG_LUA_CURSOR);
  const vectis_pg_column *columns;
  size_t count, i;
  vectis_error error;
  if (vectis_pg_cursor_columns((vectis_pg_cursor *)owner->native, &columns,
                               &count, &error) != VECTIS_OK)
    return pg_lua_error(L, &error, pg_lua_diagnostic(owner), "columns");
  lua_createtable(L, (int)count, 0);
  for (i = 0u; i < count; ++i) {
    lua_createtable(L, 0, 3);
    lua_pushstring(L, columns[i].name);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, columns[i].oid);
    lua_setfield(L, -2, "oid");
    lua_pushinteger(L, columns[i].format);
    lua_setfield(L, -2, "format");
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}

typedef struct pg_lua_transaction_call {
  lua_State *lua;
  int base, results, raised, rejected;
  vectis_error error;
} pg_lua_transaction_call;
static int pg_lua_transaction_invoke(lua_State *L) {
  pg_lua_transaction_call *call =
      (pg_lua_transaction_call *)lua_touserdata(L, 1);
  int results;
  const char *message;
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_call(L, 1, LUA_MULTRET);
  results = lua_gettop(L) - 3;
  if (results >= 2 && lua_isnil(L, 4)) {
    call->rejected = 1;
    memset(&call->error, 0, sizeof(call->error));
    call->error.code = VECTIS_ERR_STATE;
    call->error.source = VECTIS_ERROR_SOURCE_CPKT;
    strcpy(call->error.message,
           "PostgreSQL transaction callback requested rollback");
    if (lua_istable(L, 5)) {
      pg_lua_field(L, 5, "status");
      if (lua_isinteger(L, -1) && lua_tointeger(L, -1) > 0 &&
          lua_tointeger(L, -1) <= VECTIS_ERR_TIMEOUT)
        call->error.code = (vectis_status)lua_tointeger(L, -1);
      lua_pop(L, 1);
      pg_lua_field(L, 5, "source_code");
      if (lua_isinteger(L, -1) && lua_tointeger(L, -1) >= 0 &&
          lua_tointeger(L, -1) <= VECTIS_ERROR_SOURCE_CAI)
        call->error.source = (vectis_error_source)lua_tointeger(L, -1);
      lua_pop(L, 1);
      pg_lua_field(L, 5, "message");
      if (lua_type(L, -1) == LUA_TSTRING) {
        message = lua_tostring(L, -1);
        strncpy(call->error.message, message, sizeof(call->error.message) - 1u);
        call->error.message[sizeof(call->error.message) - 1u] = '\0';
      }
      lua_pop(L, 1);
    }
  }
  return results;
}
static vectis_status pg_lua_transaction_callback(vectis_pg_connection *db,
                                                 void *context,
                                                 vectis_error *error) {
  pg_lua_transaction_call *call = (pg_lua_transaction_call *)context;
  lua_State *L = call->lua;
  (void)db;
  lua_pushcfunction(L, pg_lua_transaction_invoke);
  lua_pushlightuserdata(L, call);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 1);
  call->raised = lua_pcall(L, 3, LUA_MULTRET, 0) != LUA_OK;
  call->results = lua_gettop(L) - call->base;
  if (call->raised) {
    memset(error, 0, sizeof(*error));
    error->code = VECTIS_ERR_STATE;
    error->source = VECTIS_ERROR_SOURCE_CPKT;
    strcpy(error->message, "PostgreSQL transaction callback raised");
    return VECTIS_ERR_STATE;
  }
  if (call->rejected) {
    *error = call->error;
    return error->code;
  }
  return VECTIS_OK;
}
static void pg_lua_transaction_config(lua_State *L, int index,
                                      vectis_pg_transaction_config *config) {
  static const char *const fields[] = {"isolation", "read_only", NULL};
  const char *isolation;
  size_t size;
  vectis_pg_transaction_config_init(config);
  pg_lua_fields(L, index, fields);
  pg_lua_field(L, index, "isolation");
  if (!lua_isnil(L, -1)) {
    isolation = pg_lua_string(L, -1, &size);
    if (strcmp(isolation, "read_committed") == 0)
      config->isolation = "read_committed";
    else if (strcmp(isolation, "repeatable_read") == 0)
      config->isolation = "repeatable_read";
    else if (strcmp(isolation, "serializable") == 0)
      config->isolation = "serializable";
    else
      luaL_error(L, "invalid PostgreSQL transaction isolation");
  }
  lua_pop(L, 1);
  pg_lua_field(L, index, "read_only");
  if (!lua_isnil(L, -1))
    luaL_checktype(L, -1, LUA_TBOOLEAN);
  config->read_only = lua_toboolean(L, -1);
  lua_pop(L, 1);
}
static void
pg_lua_transaction_error(lua_State *L,
                         const vectis_pg_transaction_diagnostic *diagnostic,
                         const vectis_error *error) {
  (void)pg_lua_error(L, error, diagnostic == NULL ? NULL : &diagnostic->primary,
                     "transaction");
  if (diagnostic != NULL && diagnostic->cleanup_failed) {
    (void)pg_lua_error(L, &diagnostic->cleanup.error, &diagnostic->cleanup,
                       "rollback");
    lua_remove(L, -2);
    lua_setfield(L, -2, "cleanup_error");
  }
}
static int pg_lua_transaction(lua_State *L) {
  pg_lua_owner *owner = pg_lua_check(L, PG_LUA_DB);
  vectis_pg_transaction_config config;
  vectis_error error;
  vectis_status status;
  pg_lua_transaction_call call;
  const vectis_pg_transaction_diagnostic *diagnostic;
  int previous = owner->transaction;
  luaL_checktype(L, 2, LUA_TFUNCTION);
  pg_lua_transaction_config(L, 3, &config);
  luaL_checkstack(L, 8, "PostgreSQL transaction callback stack");
  memset(&call, 0, sizeof(call));
  call.lua = L;
  call.base = lua_gettop(L);
  owner->transaction = 1;
  owner->busy = 0;
  status = vectis_pg_connection_transaction(
      (vectis_pg_connection *)owner->native, &config,
      pg_lua_transaction_callback, &call, &error);
  owner->busy = 1;
  owner->transaction = previous;
  if (call.raised)
    return lua_error(L);
  diagnostic = vectis_pg_connection_transaction_diagnostic(
      (vectis_pg_connection *)owner->native);
  if (status != VECTIS_OK) {
    pg_lua_transaction_error(L, diagnostic, &error);
    if (call.rejected) {
      lua_pushvalue(L, call.base + 2);
      lua_setfield(L, -2, "cause");
    }
    return 2;
  }
  lua_pushboolean(L, 1);
  lua_insert(L, call.base + 1);
  return call.results + 1;
}
static int pg_lua_transaction_diagnostic(lua_State *L) {
  pg_lua_owner *owner = pg_lua_check(L, PG_LUA_DB);
  const vectis_pg_transaction_diagnostic *diagnostic =
      vectis_pg_connection_transaction_diagnostic(
          (vectis_pg_connection *)owner->native);
  if (diagnostic == NULL || diagnostic->primary.error.detail[0] == '\0') {
    lua_pushnil(L);
    return 1;
  }
  pg_lua_transaction_error(L, diagnostic, &diagnostic->primary.error);
  return 1;
}
static int pg_lua_guard(lua_State *L) {
  int kind = (int)lua_tointeger(L, lua_upvalueindex(2));
  int operation = (int)lua_tointeger(L, lua_upvalueindex(3));
  pg_lua_owner *owner;
  pg_lua_owner *db;
  int args = lua_gettop(L), status;
  luaL_checkstack(L, args + 3, "PostgreSQL protected operation stack");
  owner = pg_lua_check(L, kind);
  db = kind == PG_LUA_DB ? owner : owner->database;
  luaL_argcheck(L, !db->busy && !owner->busy, 1,
                "PostgreSQL receiver is active");
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  db->busy = 1;
  owner->busy = 1;
  if (db->enter != NULL)
    db->enter(db->app_context);
  status = lua_pcall(L, args, LUA_MULTRET, 0);
  if (db->leave != NULL)
    db->leave(db->app_context);
  db->busy = 0;
  owner->busy = 0;
  if (status != LUA_OK) {
    if (kind == PG_LUA_CURSOR && (operation == 1 || operation == 2) &&
        owner->native != NULL) {
      vectis_pg_cursor_close((vectis_pg_cursor *)owner->native, NULL);
      owner->native = NULL;
    }
    return lua_error(L);
  }
  return lua_gettop(L);
}
static void pg_lua_method(lua_State *L, const char *name,
                          lua_CFunction function, int kind, int operation) {
  lua_pushcfunction(L, function);
  lua_pushinteger(L, kind);
  lua_pushinteger(L, operation);
  lua_pushcclosure(L, pg_lua_guard, 3);
  lua_setfield(L, -2, name);
}
static void pg_lua_metatable(lua_State *L, int kind) {
  luaL_newmetatable(L, pg_lua_type(kind));
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushinteger(L, kind);
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, pg_lua_close, 2);
  lua_setfield(L, -2, "close");
  lua_pushinteger(L, kind);
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, pg_lua_close, 2);
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "__gc");
  lua_setfield(L, -2, "__close");
}
int luaopen_vectis_pg_connection_core(lua_State *L) {
  pg_lua_metatable(L, PG_LUA_DB);
  pg_lua_method(L, "poll", pg_lua_poll, PG_LUA_DB, 0);
  pg_lua_method(L, "readiness", pg_lua_readiness, PG_LUA_DB, 0);
  pg_lua_method(L, "query", pg_lua_query, PG_LUA_DB, 0);
  pg_lua_method(L, "exec", pg_lua_exec, PG_LUA_DB, 0);
  pg_lua_method(L, "prepare", pg_lua_prepare, PG_LUA_DB, 0);
  pg_lua_method(L, "transaction", pg_lua_transaction, PG_LUA_DB, 3);
  pg_lua_method(L, "transaction_diagnostic", pg_lua_transaction_diagnostic,
                PG_LUA_DB, 0);
  lua_pop(L, 1);
  pg_lua_metatable(L, PG_LUA_CURSOR);
  pg_lua_method(L, "next", pg_lua_next_blocking, PG_LUA_CURSOR, 1);
  pg_lua_method(L, "next_ready", pg_lua_next_ready, PG_LUA_CURSOR, 1);
  pg_lua_method(L, "columns", pg_lua_columns, PG_LUA_CURSOR, 2);
  pg_lua_method(L, "readiness", pg_lua_readiness, PG_LUA_CURSOR, 0);
  lua_pop(L, 1);
  pg_lua_metatable(L, PG_LUA_STATEMENT);
  pg_lua_method(L, "query", pg_lua_query, PG_LUA_STATEMENT, 0);
  lua_pop(L, 1);
  lua_newtable(L);
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, pg_lua_connect, 1);
  lua_setfield(L, -2, "connect");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, pg_lua_connect, 1);
  lua_setfield(L, -2, "connect_start");
  lua_getfield(L, LUA_REGISTRYINDEX, PG_NULL);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, PG_NULL);
  }
  lua_setfield(L, -2, "NULL");
  return 1;
}
