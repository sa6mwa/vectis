#include "vectis_lua_zlib_stream.h"
#include <lauxlib.h>
#include <string.h>
#include <vectis/zlib.h>

#define ZLIB_OWNER "vectis.zlib.stream.owner"

typedef struct lua_zlib_owner {
  vectis_zlib *receiver;
  vectis_zlib_options options;
  int busy;
  int cancelled;
} lua_zlib_owner;

static void owner_release(lua_zlib_owner *owner) {
  vectis_zlib *receiver;
  receiver = owner->receiver;
  owner->receiver = NULL;
  vectis_zlib_close(receiver);
}

static int owner_close(lua_State *lua) {
  lua_zlib_owner *owner;
  owner = (lua_zlib_owner *)luaL_checkudata(lua, 1, ZLIB_OWNER);
  if (owner->busy)
    owner->cancelled = 1;
  else
    owner_release(owner);
  return 0;
}

static int stream_error(lua_State *lua, vectis_status status,
                        const char *message) {
  lua_pushnil(lua);
  lua_createtable(lua, 0, 3);
  lua_pushinteger(lua, status);
  lua_setfield(lua, -2, "status");
  lua_pushstring(lua, vectis_status_string(status));
  lua_setfield(lua, -2, "status_string");
  lua_pushstring(lua, message);
  lua_setfield(lua, -2, "message");
  return 2;
}

static uint64_t exact_count(lua_State *lua, int index) {
  lua_Integer high;
  lua_Integer low;
  if (lua_isinteger(lua, index)) {
    low = lua_tointeger(lua, index);
    if (low < 0)
      luaL_error(lua, "zlib count must be non-negative");
    return (uint64_t)low;
  }
  luaL_checktype(lua, index, LUA_TTABLE);
  index = lua_absindex(lua, index);
  lua_pushliteral(lua, "high");
  lua_rawget(lua, index);
  if (!lua_isinteger(lua, -1))
    luaL_error(lua, "zlib count high must be a uint32 integer");
  high = luaL_checkinteger(lua, -1);
  lua_pop(lua, 1);
  lua_pushliteral(lua, "low");
  lua_rawget(lua, index);
  if (!lua_isinteger(lua, -1))
    luaL_error(lua, "zlib count low must be a uint32 integer");
  low = luaL_checkinteger(lua, -1);
  lua_pop(lua, 1);
  if (high < 0 || (uint64_t)high > 0xffffffffu || low < 0 ||
      (uint64_t)low > 0xffffffffu)
    luaL_error(lua, "zlib count high/low must be uint32 integers");
  return ((uint64_t)high << 32u) | (uint64_t)low;
}

static void push_count(lua_State *lua, uint64_t value) {
  if (value <= (uint64_t)LUA_MAXINTEGER) {
    lua_pushinteger(lua, (lua_Integer)value);
    return;
  }
  lua_createtable(lua, 0, 2);
  lua_pushinteger(lua, (lua_Integer)(value >> 32u));
  lua_setfield(lua, -2, "high");
  lua_pushinteger(lua, (lua_Integer)(value & 0xffffffffu));
  lua_setfield(lua, -2, "low");
}

static void push_result(lua_State *lua, const vectis_zlib_result *result) {
  lua_createtable(lua, 0, 5);
  lua_pushboolean(lua, result->finished);
  lua_setfield(lua, -2, "ok");
  lua_pushboolean(lua, result->finished);
  lua_setfield(lua, -2, "finished");
  push_count(lua, result->input_bytes);
  lua_setfield(lua, -2, "input_bytes");
  push_count(lua, result->output_bytes);
  lua_setfield(lua, -2, "output_bytes");
  push_count(lua, result->members);
  lua_setfield(lua, -2, "members");
}

/* Raw options avoid executing metamethods during validation/acquisition. */
static void read_options(lua_State *lua, int index, int compress,
                         vectis_zlib_format format,
                         vectis_zlib_options *options, int *sync_flush) {
  const char *key;
  size_t key_size;
  lua_Integer level;
  int limits;
  vectis_zlib_options_init(options);
  options->compress = compress;
  options->format = format;
  *sync_flush = 0;
  limits = 0;
  if (lua_isnoneornil(lua, index))
    return;
  luaL_checktype(lua, index, LUA_TTABLE);
  lua_pushnil(lua);
  while (lua_next(lua, index) != 0) {
    if (lua_type(lua, -2) != LUA_TSTRING)
      luaL_error(lua, "zlib option names must be strings");
    key = lua_tolstring(lua, -2, &key_size);
    if (strlen(key) != key_size)
      luaL_error(lua, "zlib option name contains NUL");
    if (strcmp(key, "level") == 0) {
      if (!compress)
        luaL_error(lua, "zlib level is a compression option");
      level = luaL_checkinteger(lua, -1);
      if (level < -1 || level > 9)
        luaL_error(lua, "zlib level must be -1..9");
      options->level = (int)level;
    } else if (strcmp(key, "max_output_bytes") == 0 ||
               strcmp(key, "max_output") == 0) {
      if (++limits > 1)
        luaL_error(lua, "use only one zlib output limit option");
      options->max_output_bytes = exact_count(lua, -1);
    } else if (strcmp(key, "sync_flush") == 0) {
      if (!compress)
        luaL_error(lua, "zlib sync_flush is a compression option");
      luaL_checktype(lua, -1, LUA_TBOOLEAN);
      *sync_flush = lua_toboolean(lua, -1);
    } else {
      luaL_error(lua, "unknown zlib streaming option: %s", key);
    }
    lua_pop(lua, 1);
  }
}

static lua_zlib_owner *new_owner(lua_State *lua,
                                 const vectis_zlib_options *options) {
  lua_zlib_owner *owner;
  owner = (lua_zlib_owner *)lua_newuserdatauv(lua, sizeof(*owner), 0);
  memset(owner, 0, sizeof(*owner));
  owner->options = *options;
  luaL_setmetatable(lua, ZLIB_OWNER);
  return owner;
}

static int owner_check_live(lua_State *lua, lua_zlib_owner *owner) {
  if (owner->cancelled || owner->receiver == NULL)
    return luaL_error(lua, "zlib stream owner is closed");
  return 0;
}

/* All Lua allocation and callbacks occur inside this protected driver. No
 * Lua call occurs in a libvectis/provider frame. Output is one bounded chunk.
 */
static int stream_driver(lua_State *lua) {
  lua_zlib_owner *owner;
  vectis_error error;
  vectis_zlib_result result;
  unsigned char output[VECTIS_ZLIB_CHUNK_BYTES];
  const char *input;
  size_t size;
  size_t offset;
  size_t consumed;
  size_t produced;
  int eof;
  int sync_flush;
  vectis_zlib_flush flush;
  vectis_status status;
  owner = (lua_zlib_owner *)luaL_checkudata(lua, 1, ZLIB_OWNER);
  sync_flush = lua_toboolean(lua, 4);
  status = vectis_zlib_new(&owner->options, &owner->receiver, &error);
  if (status != VECTIS_OK)
    return stream_error(lua, status, error.message);
  for (;;) {
    lua_settop(lua, 4);
    lua_pushvalue(lua, 2);
    lua_pushinteger(lua, VECTIS_ZLIB_CHUNK_BYTES);
    if (lua_pcall(lua, 1, 2, 0) != LUA_OK)
      return lua_error(lua);
    if (!lua_isnil(lua, -1))
      return lua_error(lua);
    (void)owner_check_live(lua, owner);
    eof = lua_isnil(lua, -2);
    input = NULL;
    size = 0u;
    if (!eof) {
      if (lua_type(lua, -2) != LUA_TSTRING)
        return stream_error(lua, VECTIS_ERR_INVALID,
                            "zlib reader must return a string or nil at EOF");
      input = lua_tolstring(lua, -2, &size);
      if (size == 0u || size > VECTIS_ZLIB_CHUNK_BYTES)
        return stream_error(lua, VECTIS_ERR_INVALID,
                            "zlib reader must return 1 through 65536 bytes");
    }
    flush = eof          ? VECTIS_ZLIB_FINISH
            : sync_flush ? VECTIS_ZLIB_SYNC_FLUSH
                         : VECTIS_ZLIB_NO_FLUSH;
    offset = 0u;
    for (;;) {
      status = vectis_zlib_process(
          owner->receiver, input == NULL ? NULL : input + offset, size - offset,
          &consumed, output, sizeof(output), &produced, flush, &error);
      if (status != VECTIS_OK)
        return stream_error(lua, status, error.message);
      offset += consumed;
      if (produced != 0u) {
        lua_pushvalue(lua, 3);
        lua_pushlstring(lua, (const char *)output, produced);
        if (lua_pcall(lua, 1, 2, 0) != LUA_OK)
          return lua_error(lua);
        if (!lua_isnil(lua, -1))
          return lua_error(lua);
        (void)owner_check_live(lua, owner);
        if (!lua_isboolean(lua, -2) || !lua_toboolean(lua, -2))
          return stream_error(lua, VECTIS_ERR_STATE,
                              "zlib writer must return true");
        lua_pop(lua, 2);
      }
      status = vectis_zlib_get_result(owner->receiver, &result, &error);
      if (status != VECTIS_OK)
        return stream_error(lua, status, error.message);
      if (eof && result.finished) {
        push_result(lua, &result);
        return 1;
      }
      if (!eof && offset == size && produced < sizeof(output))
        break;
    }
  }
}

static int stream_call(lua_State *lua) {
  vectis_zlib_options options;
  lua_zlib_owner *owner;
  int sync_flush;
  int status;
  luaL_checktype(lua, 1, LUA_TFUNCTION);
  luaL_checktype(lua, 2, LUA_TFUNCTION);
  read_options(lua, 3, lua_toboolean(lua, lua_upvalueindex(1)),
               (vectis_zlib_format)lua_tointeger(lua, lua_upvalueindex(2)),
               &options, &sync_flush);
  lua_settop(lua, 3);
  owner = new_owner(lua, &options);
  lua_pushcfunction(lua, stream_driver);
  lua_pushvalue(lua, 4);
  lua_pushvalue(lua, 1);
  lua_pushvalue(lua, 2);
  lua_pushboolean(lua, sync_flush);
  owner->busy = 1;
  status = lua_pcall(lua, 4, LUA_MULTRET, 0);
  owner->busy = 0;
  owner_release(owner);
  if (status != LUA_OK)
    return lua_error(lua);
  return lua_gettop(lua) - 4;
}

/* Optional incremental receiver mirrors the public bounded process method.
 * It emits at most one chunk per call; the caller must resubmit any suffix. */
static int step_driver(lua_State *lua) {
  lua_zlib_owner *owner;
  const char *input;
  const char *flush_name;
  size_t size;
  size_t consumed;
  size_t produced;
  unsigned char output[VECTIS_ZLIB_CHUNK_BYTES];
  vectis_zlib_flush flush;
  vectis_zlib_result result;
  vectis_error error;
  vectis_status status;
  owner = (lua_zlib_owner *)luaL_checkudata(lua, 1, ZLIB_OWNER);
  (void)owner_check_live(lua, owner);
  luaL_checktype(lua, 2, LUA_TSTRING);
  input = lua_tolstring(lua, 2, &size);
  flush_name = luaL_optstring(lua, 3, "none");
  flush = VECTIS_ZLIB_NO_FLUSH;
  if (strcmp(flush_name, "sync") == 0)
    flush = VECTIS_ZLIB_SYNC_FLUSH;
  else if (strcmp(flush_name, "finish") == 0)
    flush = VECTIS_ZLIB_FINISH;
  else if (strcmp(flush_name, "none") != 0)
    return luaL_error(lua, "zlib flush must be none, sync or finish");
  status = vectis_zlib_process(owner->receiver, input, size, &consumed, output,
                               sizeof(output), &produced, flush, &error);
  if (status != VECTIS_OK) {
    owner->cancelled = 1;
    return stream_error(lua, status, error.message);
  }
  status = vectis_zlib_get_result(owner->receiver, &result, &error);
  if (status != VECTIS_OK) {
    owner->cancelled = 1;
    return stream_error(lua, status, error.message);
  }
  lua_pushlstring(lua, (const char *)output, produced);
  lua_pushinteger(lua, (lua_Integer)consumed);
  lua_pushboolean(lua, result.finished);
  return 3;
}

static int owner_step(lua_State *lua) {
  lua_zlib_owner *owner;
  int status;
  owner = (lua_zlib_owner *)luaL_checkudata(lua, 1, ZLIB_OWNER);
  if (owner->busy)
    return luaL_error(lua, "zlib stream is busy");
  lua_settop(lua, 3);
  lua_pushcfunction(lua, step_driver);
  lua_pushvalue(lua, 1);
  lua_pushvalue(lua, 2);
  lua_pushvalue(lua, 3);
  owner->busy = 1;
  status = lua_pcall(lua, 3, LUA_MULTRET, 0);
  owner->busy = 0;
  if (status != LUA_OK || owner->cancelled)
    owner_release(owner);
  if (status != LUA_OK)
    return lua_error(lua);
  return lua_gettop(lua) - 3;
}

static int owner_result(lua_State *lua) {
  lua_zlib_owner *owner;
  vectis_zlib_result result;
  vectis_error error;
  vectis_status status;
  owner = (lua_zlib_owner *)luaL_checkudata(lua, 1, ZLIB_OWNER);
  if (owner->busy)
    return luaL_error(lua, "zlib stream is busy");
  (void)owner_check_live(lua, owner);
  status = vectis_zlib_get_result(owner->receiver, &result, &error);
  if (status != VECTIS_OK)
    return stream_error(lua, status, error.message);
  push_result(lua, &result);
  return 1;
}

static int owner_new(lua_State *lua) {
  vectis_zlib_options options;
  lua_zlib_owner *owner;
  vectis_error error;
  vectis_status status;
  int sync_flush;
  read_options(lua, 1, lua_toboolean(lua, lua_upvalueindex(1)),
               (vectis_zlib_format)lua_tointeger(lua, lua_upvalueindex(2)),
               &options, &sync_flush);
  if (sync_flush)
    return luaL_error(lua,
                      "receiver selects sync flush in process(chunk, 'sync')");
  owner = new_owner(lua, &options);
  status = vectis_zlib_new(&options, &owner->receiver, &error);
  if (status != VECTIS_OK)
    return stream_error(lua, status, error.message);
  return 1;
}

static uint32_t checksum_seed(lua_State *lua, int index) {
  lua_Integer value;
  if (!lua_isinteger(lua, index))
    luaL_error(lua, "checksum value must be a uint32 integer");
  value = lua_tointeger(lua, index);
  if (value < 0 || (uint64_t)value > 0xffffffffu)
    luaL_error(lua, "checksum value must be a uint32 integer");
  return (uint32_t)value;
}

static int checksum_call(lua_State *lua) {
  vectis_zlib_checksum_kind kind;
  vectis_status status;
  vectis_error error;
  const char *data;
  size_t size;
  uint32_t seed;
  uint32_t second;
  uint32_t result;
  int mode;
  int input;
  kind = (vectis_zlib_checksum_kind)lua_tointeger(lua, lua_upvalueindex(1));
  mode = (int)lua_tointeger(lua, lua_upvalueindex(2));
  if (mode >= 3) {
    if (mode == 4)
      status =
          vectis_zlib_crc32_combine_gen(exact_count(lua, 1), &result, &error);
    else {
      seed = checksum_seed(lua, 1);
      second = checksum_seed(lua, 2);
      if (mode == 5) {
        result =
            vectis_zlib_crc32_combine_op(seed, second, checksum_seed(lua, 3));
        status = VECTIS_OK;
      } else
        status = vectis_zlib_checksum_combine(
            kind, seed, second, exact_count(lua, 3), &result, &error);
    }
  } else {
    data = NULL;
    size = 0u;
    seed = kind == VECTIS_ZLIB_CRC32 ? 0u : 1u;
    if (mode != 2) {
      input = mode == 1 ? 2 : 1;
      if (mode == 1)
        seed = checksum_seed(lua, 1);
      else if (!lua_isnoneornil(lua, 2))
        seed = checksum_seed(lua, 2);
      luaL_checktype(lua, input, LUA_TSTRING);
      data = lua_tolstring(lua, input, &size);
      if (mode == 1 && size > VECTIS_ZLIB_CHUNK_BYTES)
        return luaL_error(lua, "checksum update chunk exceeds 65536 bytes");
    }
    status = vectis_zlib_checksum(kind, seed, data, size, &result, &error);
  }
  if (status != VECTIS_OK)
    return luaL_error(lua, "%s", error.message);
  lua_pushinteger(lua, (lua_Integer)result);
  return 1;
}

static void checksum_register(lua_State *lua) {
  static const char *const names[] = {
      "adler32_materialized", "crc32_materialized", "adler32_update",
      "crc32_update",         "adler32_initial",    "crc32_initial",
      "adler32_combine",      "crc32_combine",      "adler32_combine64",
      "crc32_combine64",      "crc32_combine_gen",  "crc32_combine_gen64",
      "crc32_combine_op"};
  static const int modes[] = {0, 0, 1, 1, 2, 2, 3, 3, 3, 3, 4, 4, 5};
  size_t i;
  for (i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
    lua_pushinteger(lua,
                    (i % 2u) == 0u ? VECTIS_ZLIB_ADLER32 : VECTIS_ZLIB_CRC32);
    lua_pushinteger(lua, modes[i]);
    lua_pushcclosure(lua, checksum_call, 2);
    lua_setfield(lua, -2, names[i]);
  }
}

void vectis_lua_zlib_stream_register(lua_State *lua) {
  static const char *const names[] = {
      "deflate_stream",       "gzip_stream",        "inflate_stream",
      "gunzip_stream",        "decompress_stream",  "deflate_stream_new",
      "gzip_stream_new",      "inflate_stream_new", "gunzip_stream_new",
      "decompress_stream_new"};
  static const int compress[] = {1, 1, 0, 0, 0, 1, 1, 0, 0, 0};
  static const vectis_zlib_format formats[] = {
      VECTIS_ZLIB_ZLIB, VECTIS_ZLIB_GZIP, VECTIS_ZLIB_ZLIB, VECTIS_ZLIB_GZIP,
      VECTIS_ZLIB_AUTO, VECTIS_ZLIB_ZLIB, VECTIS_ZLIB_GZIP, VECTIS_ZLIB_ZLIB,
      VECTIS_ZLIB_GZIP, VECTIS_ZLIB_AUTO};
  size_t i;
  checksum_register(lua);
  if (luaL_newmetatable(lua, ZLIB_OWNER)) {
    lua_pushcfunction(lua, owner_close);
    lua_setfield(lua, -2, "__gc");
    lua_pushcfunction(lua, owner_close);
    lua_setfield(lua, -2, "__close");
    lua_newtable(lua);
    lua_pushcfunction(lua, owner_close);
    lua_setfield(lua, -2, "close");
    lua_pushcfunction(lua, owner_step);
    lua_setfield(lua, -2, "process");
    lua_pushcfunction(lua, owner_result);
    lua_setfield(lua, -2, "result");
    lua_setfield(lua, -2, "__index");
  }
  lua_pop(lua, 1);
  for (i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
    lua_pushboolean(lua, compress[i]);
    lua_pushinteger(lua, formats[i]);
    lua_pushcclosure(lua, i < 5u ? stream_call : owner_new, 2);
    lua_setfield(lua, -2, names[i]);
  }
  lua_getfield(lua, -1, "deflate_stream");
  lua_setfield(lua, -2, "compress_stream");
  lua_pushinteger(lua, VECTIS_ZLIB_CHUNK_BYTES);
  lua_setfield(lua, -2, "STREAM_CHUNK_BYTES");
}
