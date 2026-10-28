#include "vectis_lua_sqlite.h"
#include <lauxlib.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <vectis/sqlite.h>

#define DB "vectis.sqlite.database"
#define STMT "vectis.sqlite.statement"
#define BLOB "vectis.sqlite.blob"
#define NULL_KEY "vectis.pg.NULL"
typedef struct lua_database {
  vectis_sqlite *native;
  pid_t process;
  int busy;
  int declaration;
  int transaction;
  void *app_context;
  void (*enter)(void *);
  void (*leave)(void *);
} lua_database;
typedef struct lua_statement {
  vectis_sqlite_statement *native;
  lua_database *owner;
  int busy;
} lua_statement;

static int push_error(lua_State *L, const vectis_error *error,
                      const char *operation) {
  lua_pushnil(L);
  lua_createtable(L, 0, 8);
  lua_pushinteger(L, error->code);
  lua_setfield(L, -2, "status");
  lua_pushstring(L, vectis_status_string(error->code));
  lua_setfield(L, -2, "status_string");
  lua_pushstring(L, vectis_error_source_string(error->source));
  lua_setfield(L, -2, "source");
  lua_pushinteger(L, error->source);
  lua_setfield(L, -2, "source_code");
  lua_pushinteger(L, error->dependency_code);
  lua_setfield(L, -2, "dependency_code");
  lua_pushstring(L, error->message);
  lua_setfield(L, -2, "message");
  lua_pushstring(L, operation);
  lua_setfield(L, -2, "operation");
  if (error->detail[0]) {
    lua_pushstring(L, error->detail);
    lua_setfield(L, -2, "transaction_outcome");
  }
  return 2;
}
static int is_null(lua_State *L, int index) {
  int equal;
  lua_getfield(L, LUA_REGISTRYINDEX, NULL_KEY);
  equal = lua_rawequal(L, index, -1);
  lua_pop(L, 1);
  return equal;
}
static size_t parameters(lua_State *L, int index, vectis_sql_value *values) {
  size_t count, i;
  luaL_checktype(L, index, LUA_TTABLE);
  count = lua_rawlen(L, index);
  luaL_argcheck(L, count <= VECTIS_SQL_MAX_PARAMETERS, index,
                "too many SQL parameters");
  lua_pushnil(L);
  while (lua_next(L, index) != 0) {
    lua_Integer key;
    luaL_argcheck(L, lua_isinteger(L, -2), index,
                  "parameters must be a dense array");
    key = lua_tointeger(L, -2);
    luaL_argcheck(L, key >= 1 && (lua_Unsigned)key <= count, index,
                  "parameters must be a dense array");
    lua_pop(L, 1);
  }
  memset(values, 0, count * sizeof(*values));
  for (i = 0; i < count; ++i) {
    vectis_sql_value *value = &values[i];
    lua_rawgeti(L, index, (lua_Integer)i + 1);
    if (is_null(L, lua_gettop(L))) {
      value->type = VECTIS_SQL_NULL;
    } else if (lua_isinteger(L, -1)) {
      value->type = VECTIS_SQL_INTEGER;
      value->integer = (int64_t)lua_tointeger(L, -1);
    } else if (lua_type(L, -1) == LUA_TNUMBER) {
      value->type = VECTIS_SQL_REAL;
      value->real = lua_tonumber(L, -1);
    } else if (lua_type(L, -1) == LUA_TBOOLEAN) {
      value->type = VECTIS_SQL_INTEGER;
      value->integer = lua_toboolean(L, -1);
    } else if (lua_type(L, -1) == LUA_TSTRING) {
      value->type = VECTIS_SQL_TEXT;
      value->data = lua_tolstring(L, -1, &value->size);
    } else if (luaL_testudata(L, -1, BLOB) != NULL) {
      lua_getiuservalue(L, -1, 1);
      value->type = VECTIS_SQL_BLOB;
      value->data = lua_tolstring(L, -1, &value->size);
      lua_pop(L, 1);
    } else {
      return (size_t)luaL_argerror(
          L, index, "SQL parameters require values, NULL or sqlite.blob");
    }
    lua_pop(L, 1);
  }
  return count;
}
static const char *checked_string(lua_State *L, int index, size_t *size) {
  const char *value;
  luaL_checktype(L, index, LUA_TSTRING);
  value = lua_tolstring(L, index, size);
  luaL_argcheck(L, memchr(value, 0, *size) == NULL, index,
                "string cannot contain NUL");
  return value;
}
static lua_database *database(lua_State *L) {
  lua_database *owner = (lua_database *)luaL_checkudata(L, 1, DB);
  luaL_argcheck(L, owner->declaration || owner->process == getpid(), 1,
                "foreign-process SQLite database");
  luaL_argcheck(L, owner->native != NULL, 1, "SQLite database is closed");
  return owner;
}
static lua_statement *statement(lua_State *L) {
  lua_statement *owner = (lua_statement *)luaL_checkudata(L, 1, STMT);
  luaL_argcheck(L,
                owner->owner->declaration || owner->owner->process == getpid(),
                1, "foreign-process SQLite statement");
  luaL_argcheck(L, owner->native != NULL, 1, "SQLite statement is closed");
  return owner;
}
static int db_close(lua_State *L) {
  lua_database *owner = (lua_database *)luaL_checkudata(L, 1, DB);
  int finalizer = lua_toboolean(L, lua_upvalueindex(1));
  vectis_error error;
  if (owner->busy || (!owner->declaration && owner->process != getpid())) {
    if (owner->busy && owner->transaction && owner->native != NULL)
      (void)vectis_sqlite_close(owner->native, &error);
    if (finalizer)
      return 0;
    return luaL_error(L,
                      "cannot close active or foreign-process SQLite database");
  }
  if (vectis_sqlite_close(owner->native, &error) != VECTIS_OK) {
    if (finalizer)
      return 0;
    return push_error(L, &error, "close");
  }
  owner->native = NULL;
  lua_pushboolean(L, 1);
  return 1;
}
static int stmt_close(lua_State *L) {
  lua_statement *owner = (lua_statement *)luaL_checkudata(L, 1, STMT);
  int finalizer = lua_toboolean(L, lua_upvalueindex(1));
  if (owner->busy ||
      (!owner->owner->declaration && owner->owner->process != getpid())) {
    if (finalizer)
      return 0;
    return luaL_error(
        L, "cannot close active or foreign-process SQLite statement");
  }
  vectis_sqlite_statement_close(owner->native);
  owner->native = NULL;
  lua_pushboolean(L, 1);
  return 1;
}
static void parse_options(lua_State *L, vectis_sqlite_config *config,
                          const char **name, const char **path) {
  size_t size;
  vectis_sqlite_config_init(config);
  if (!lua_isnoneornil(L, 2)) {
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
      const char *key;
      luaL_checktype(L, -2, LUA_TSTRING);
      key = lua_tostring(L, -2);
      if (strcmp(key, "mode") == 0)
        config->mode = checked_string(L, lua_gettop(L), &size);
      else if (strcmp(key, "uri") == 0) {
        luaL_checktype(L, -1, LUA_TBOOLEAN);
        config->uri = lua_toboolean(L, -1);
      } else if (strcmp(key, "busy_timeout_ms") == 0) {
        lua_Integer ms = luaL_checkinteger(L, -1);
        luaL_argcheck(L, ms >= 0 && ms <= INT_MAX, 2, "invalid busy timeout");
        config->busy_timeout_ms = (int)ms;
      } else if (name != NULL && strcmp(key, "name") == 0) {
        *name = checked_string(L, lua_gettop(L), &size);
      } else if (path != NULL && strcmp(key, "path") == 0) {
        *path = checked_string(L, lua_gettop(L), &size);
      } else
        (void)luaL_argerror(L, 2, "unknown SQLite option");
      lua_pop(L, 1);
    }
  }
}
static int open_database(lua_State *L) {
  vectis_sqlite_config config;
  vectis_error error;
  lua_database *owner;
  size_t size;
  const char *path = checked_string(L, 1, &size);
  lua_settop(L, 2);
  owner = (lua_database *)lua_newuserdatauv(L, sizeof(*owner), 0);
  memset(owner, 0, sizeof(*owner));
  owner->process = getpid();
  luaL_setmetatable(L, DB);
  /* Parse borrowed option strings after the last allocating Lua setup call. */
  parse_options(L, &config, NULL, NULL);
  if (vectis_sqlite_open(path, &config, &owner->native, &error) != VECTIS_OK)
    return push_error(L, &error, "open");
  return 1;
}
int vectis_lua_sqlite_declare(lua_State *L, vectis_app *app, void *context,
                              void (*enter)(void *), void (*leave)(void *)) {
  vectis_sqlite_config config;
  vectis_error error;
  lua_database *owner;
  const char *name = NULL, *path = NULL;
  lua_settop(L, 2);
  luaL_requiref(L, "vectis.sqlite.core", luaopen_vectis_sqlite_core, 0);
  lua_pop(L, 1);
  owner = (lua_database *)lua_newuserdatauv(L, sizeof(*owner), 1);
  memset(owner, 0, sizeof(*owner));
  owner->process = getpid();
  owner->declaration = 1;
  owner->app_context = context;
  owner->enter = enter;
  owner->leave = leave;
  luaL_setmetatable(L, DB);
  lua_pushvalue(L, 1);
  lua_setiuservalue(L, -2, 1);
  parse_options(L, &config, &name, &path);
  luaL_argcheck(L, name != NULL && name[0] != '\0' && path != NULL, 2,
                "SQLite declaration requires name and path");
  if (vectis_app_sqlite(app, name, path, &config, &owner->native, &error) !=
      VECTIS_OK)
    return push_error(L, &error, "declare");
  return 1;
}
static int blob(lua_State *L) {
  luaL_checktype(L, 1, LUA_TSTRING);
  lua_newuserdatauv(L, 1u, 1);
  luaL_setmetatable(L, BLOB);
  lua_pushvalue(L, 1);
  lua_setiuservalue(L, -2, 1);
  return 1;
}
static int prepare(lua_State *L) {
  lua_database *db = database(L);
  lua_statement *owner;
  vectis_sql_value values[VECTIS_SQL_MAX_PARAMETERS];
  vectis_error error;
  int cursor = lua_toboolean(L, lua_upvalueindex(1));
  size_t size, count = 0u;
  const char *sql = checked_string(L, 2, &size);
  owner = (lua_statement *)lua_newuserdatauv(L, sizeof(*owner), 1);
  memset(owner, 0, sizeof(*owner));
  owner->owner = db;
  luaL_setmetatable(L, STMT);
  lua_pushvalue(L, 1);
  lua_setiuservalue(L, -2, 1);
  if (cursor)
    count = parameters(L, 3, values);
  if (vectis_sqlite_prepare(db->native, sql, size, cursor, &owner->native,
                            &error) != VECTIS_OK)
    return push_error(L, &error, "prepare");
  if (cursor &&
      vectis_sqlite_bind(owner->native, values, count, &error) != VECTIS_OK) {
    vectis_sqlite_statement_close(owner->native);
    owner->native = NULL;
    return push_error(L, &error, "bind");
  }
  return 1;
}
static int bind_values(lua_State *L) {
  lua_statement *owner = statement(L);
  vectis_sql_value values[VECTIS_SQL_MAX_PARAMETERS];
  vectis_error error;
  size_t count = parameters(L, 2, values);
  if (vectis_sqlite_bind(owner->native, values, count, &error) != VECTIS_OK)
    return push_error(L, &error, "bind");
  lua_settop(L, 1);
  return 1;
}
static int next_row(lua_State *L) {
  lua_statement *owner = statement(L);
  vectis_error error;
  vectis_sql_value value;
  size_t count, i;
  int row;
  if (vectis_sqlite_next(owner->native, &row, &error) != VECTIS_OK)
    return push_error(L, &error, "next");
  if (!row) {
    lua_pushnil(L);
    lua_pushnil(L);
    return 2;
  }
  if (vectis_sqlite_columns(owner->native, &count, &error) != VECTIS_OK)
    return push_error(L, &error, "columns");
  lua_createtable(L, (int)count, 0);
  for (i = 0; i < count; ++i) {
    if (vectis_sqlite_value(owner->native, i, &value, &error) != VECTIS_OK)
      return push_error(L, &error, "value");
    if (value.type == VECTIS_SQL_NULL)
      lua_getfield(L, LUA_REGISTRYINDEX, NULL_KEY);
    else if (value.type == VECTIS_SQL_INTEGER)
      lua_pushinteger(L, (lua_Integer)value.integer);
    else if (value.type == VECTIS_SQL_REAL)
      lua_pushnumber(L, value.real);
    else
      lua_pushlstring(L, value.data == NULL ? "" : (const char *)value.data,
                      value.size);
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}
static int columns(lua_State *L) {
  lua_statement *owner = statement(L);
  vectis_error error;
  vectis_sql_column column;
  size_t count, i;
  if (vectis_sqlite_columns(owner->native, &count, &error) != VECTIS_OK)
    return push_error(L, &error, "columns");
  lua_createtable(L, (int)count, 0);
  for (i = 0; i < count; ++i) {
    if (vectis_sqlite_column(owner->native, i, &column, &error) != VECTIS_OK)
      return push_error(L, &error, "columns");
    lua_createtable(L, 0, 2);
    lua_pushstring(L, column.name);
    lua_setfield(L, -2, "name");
    lua_pushstring(L, column.declared_type);
    lua_setfield(L, -2, "declared_type");
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}
static int reset(lua_State *L) {
  lua_statement *owner = statement(L);
  vectis_error error;
  if (vectis_sqlite_reset(owner->native, &error) != VECTIS_OK)
    return push_error(L, &error, "reset");
  lua_settop(L, 1);
  return 1;
}
static int exec(lua_State *L) {
  lua_database *owner = database(L);
  vectis_sql_value values[VECTIS_SQL_MAX_PARAMETERS];
  vectis_sqlite_exec_result result;
  vectis_error error;
  size_t size, count;
  const char *sql = checked_string(L, 2, &size);
  count = parameters(L, 3, values);
  if (vectis_sqlite_exec(owner->native, sql, size, values, count, &result,
                         &error) != VECTIS_OK)
    return push_error(L, &error, "exec");
  lua_createtable(L, 0, 2);
  lua_pushinteger(L, (lua_Integer)result.changes);
  lua_setfield(L, -2, "changes");
  lua_pushinteger(L, (lua_Integer)result.last_insert_rowid);
  lua_setfield(L, -2, "last_insert_rowid");
  return 1;
}
typedef struct transaction_call {
  lua_State *lua;
  int base;
  int results;
  int raised;
  int rejected;
} transaction_call;
static vectis_status transaction_callback(vectis_sqlite *db, void *context,
                                          vectis_error *error) {
  transaction_call *call = (transaction_call *)context;
  lua_State *L = call->lua;
  int status;
  (void)db;
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 1);
  status = lua_pcall(L, 1, LUA_MULTRET, 0);
  call->results = lua_gettop(L) - call->base;
  call->raised = status != LUA_OK;
  call->rejected =
      !call->raised && call->results >= 2 && lua_isnil(L, call->base + 1);
  if (call->raised || call->rejected) {
    memset(error, 0, sizeof(*error));
    error->code = VECTIS_ERR_STATE;
    error->source = VECTIS_ERROR_SOURCE_CPKT;
    strcpy(error->message, "SQLite transaction callback failed");
    return VECTIS_ERR_STATE;
  }
  return VECTIS_OK;
}
static int transaction(lua_State *L) {
  lua_database *owner = database(L);
  vectis_error error;
  vectis_status status;
  const char *mode = "DEFERRED";
  transaction_call call;
  luaL_checktype(L, 2, LUA_TFUNCTION);
  if (!lua_isnoneornil(L, 3)) {
    luaL_checktype(L, 3, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, 3)) {
      luaL_argcheck(L,
                    lua_type(L, -2) == LUA_TSTRING &&
                        strcmp(lua_tostring(L, -2), "mode") == 0,
                    3, "unknown SQLite transaction option");
      luaL_checktype(L, -1, LUA_TSTRING);
      if (strcmp(lua_tostring(L, -1), "DEFERRED") == 0)
        mode = "DEFERRED";
      else if (strcmp(lua_tostring(L, -1), "IMMEDIATE") == 0)
        mode = "IMMEDIATE";
      else if (strcmp(lua_tostring(L, -1), "EXCLUSIVE") == 0)
        mode = "EXCLUSIVE";
      else
        return luaL_argerror(L, 3, "invalid SQLite transaction mode");
      lua_pop(L, 1);
    }
  }
  luaL_checkstack(L, 8, "transaction callback stack");
  memset(&call, 0, sizeof(call));
  call.lua = L;
  call.base = lua_gettop(L);
  {
    int previous = owner->transaction;
    owner->transaction = 1;
    status = vectis_sqlite_transaction(owner->native, mode,
                                       transaction_callback, &call, &error);
    owner->transaction = previous;
  }
  if (call.raised)
    return lua_error(L);
  if (status != VECTIS_OK) {
    int result = push_error(L, &error, "transaction");
    const vectis_sqlite_diagnostic *diagnostic =
        vectis_sqlite_get_transaction_diagnostic(owner->native);
    if (diagnostic != NULL && diagnostic->cleanup_failed) {
      (void)push_error(L, &diagnostic->cleanup, "rollback");
      lua_remove(L, -2);
      lua_setfield(L, -2, "cleanup_error");
    }
    if (call.rejected) {
      lua_pushvalue(L, call.base + 2);
      lua_setfield(L, -2, "cause");
    }
    return result;
  }
  lua_pushboolean(L, 1);
  lua_insert(L, call.base + 1);
  return call.results + 1;
}
static int transaction_diagnostic(lua_State *L) {
  lua_database *owner = database(L);
  const vectis_sqlite_diagnostic *diagnostic =
      vectis_sqlite_get_transaction_diagnostic(owner->native);
  if (diagnostic == NULL || diagnostic->primary.detail[0] == '\0') {
    lua_pushnil(L);
    return 1;
  }
  (void)push_error(L, &diagnostic->primary, "transaction");
  if (diagnostic->cleanup_failed) {
    (void)push_error(L, &diagnostic->cleanup, "rollback");
    lua_remove(L, -2);
    lua_setfield(L, -2, "cleanup_error");
  }
  return 1;
}

/* Keep the owner alive and repair allocation-failed row/metadata copies before
 * rethrowing. No provider stack is active across this protected Lua call. */
static int guarded(lua_State *L) {
  int args = lua_gettop(L), status,
      index = (int)lua_tointeger(L, lua_upvalueindex(1));
  lua_statement *stmt = (lua_statement *)luaL_testudata(L, 1, STMT);
  lua_database *db = stmt != NULL ? stmt->owner : database(L);
  lua_CFunction method;
  luaL_argcheck(L, !db->busy && (stmt == NULL || !stmt->busy), 1,
                "SQLite receiver is active");
  switch (index) {
  case 0:
    method = exec;
    break;
  case 1:
    method = bind_values;
    break;
  case 2:
    method = next_row;
    break;
  case 3:
    method = columns;
    break;
  case 4:
    method = reset;
    break;
  case 6:
    method = transaction_diagnostic;
    break;
  default:
    method = transaction;
    break;
  }
  luaL_checkstack(L, args + 2, "SQLite operation stack");
  lua_pushcfunction(L, method);
  lua_insert(L, 1);
  if (index != 5)
    db->busy = 1;
  if (stmt != NULL)
    stmt->busy = 1;
  if (db->enter != NULL)
    db->enter(db->app_context);
  status = lua_pcall(L, args, LUA_MULTRET, 0);
  if (db->leave != NULL)
    db->leave(db->app_context);
  db->busy = 0;
  if (stmt != NULL) {
    stmt->busy = 0;
    if (status != LUA_OK && (index == 2 || index == 3)) {
      vectis_sqlite_statement_close(stmt->native);
      stmt->native = NULL;
    }
  }
  if (status != LUA_OK)
    return lua_error(L);
  return lua_gettop(L);
}
static void set_guarded(lua_State *L, const char *name, int operation) {
  lua_pushinteger(L, operation);
  lua_pushcclosure(L, guarded, 1);
  lua_setfield(L, -2, name);
}
static void set_close(lua_State *L, lua_CFunction function) {
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, function, 1);
  lua_setfield(L, -2, "close");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, function, 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "__gc");
  lua_setfield(L, -2, "__close");
}
int luaopen_vectis_sqlite_core(lua_State *L) {
  luaL_newmetatable(L, BLOB);
  lua_pop(L, 1);
  luaL_newmetatable(L, DB);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  set_close(L, db_close);
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, prepare, 1);
  lua_setfield(L, -2, "prepare");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, prepare, 1);
  lua_setfield(L, -2, "query");
  set_guarded(L, "exec", 0);
  set_guarded(L, "transaction", 5);
  set_guarded(L, "transaction_diagnostic", 6);
  lua_pop(L, 1);
  luaL_newmetatable(L, STMT);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  set_close(L, stmt_close);
  set_guarded(L, "bind", 1);
  set_guarded(L, "next", 2);
  set_guarded(L, "columns", 3);
  set_guarded(L, "reset", 4);
  lua_pop(L, 1);
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
  lua_pushcfunction(L, open_database);
  lua_setfield(L, -2, "open");
  lua_pushcfunction(L, blob);
  lua_setfield(L, -2, "blob");
  return 1;
}
