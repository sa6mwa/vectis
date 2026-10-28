#include "vectis_lua_pouch.h"
#include <lauxlib.h>
#include <stddef.h>
#include <string.h>

typedef struct pouch_field {
  const char *name;
  uint64_t mask;
  size_t offset;
  int kind;
} pouch_field;
#define POUCH_FIELD(name, bit, member, kind)                                   \
  {name, bit, offsetof(lc_pouch_settings, member), kind}
static const pouch_field fields[] = {
    POUCH_FIELD("single_writer", LC_POUCH_SETTING_SINGLE_WRITER, single_writer,
                0),
    POUCH_FIELD("durable_sync", LC_POUCH_SETTING_DURABLE_SYNC, durable_sync, 0),
    POUCH_FIELD("fsync_batch_max_ops", LC_POUCH_SETTING_FSYNC_BATCH_MAX_OPS,
                fsync_batch_max_ops, 1),
    POUCH_FIELD("segment_target_bytes", LC_POUCH_SETTING_SEGMENT_TARGET_BYTES,
                segment_target_bytes, 1),
    POUCH_FIELD("indexer_flush_docs", LC_POUCH_SETTING_INDEXER_FLUSH_DOCS,
                indexer_flush_docs, 1),
    POUCH_FIELD("indexer_flush_interval_seconds",
                LC_POUCH_SETTING_INDEXER_FLUSH_INTERVAL_SECONDS,
                indexer_flush_interval_seconds, 1),
    POUCH_FIELD("background_compaction", LC_POUCH_SETTING_BACKGROUND_COMPACTION,
                background_compaction_enabled, 0),
    POUCH_FIELD("disable_compaction_throttling",
                LC_POUCH_SETTING_DISABLE_COMPACTION_THROTTLING,
                compaction_throttling_disabled, 0),
    POUCH_FIELD("terminal_reclaim_min_bytes",
                LC_POUCH_SETTING_TERMINAL_RECLAIM_MIN_BYTES,
                terminal_reclaim_min_bytes, 1),
    POUCH_FIELD("queue_watch", LC_POUCH_SETTING_QUEUE_WATCH, queue_watch, 0),
    POUCH_FIELD("query_engine", LC_POUCH_SETTING_QUERY_ENGINE, query_engine, 2),
    POUCH_FIELD("query_fallback_engine", LC_POUCH_SETTING_QUERY_FALLBACK_ENGINE,
                query_fallback_engine, 2),
    POUCH_FIELD("query_indexing", LC_POUCH_SETTING_QUERY_INDEXING,
                query_indexing_enabled, 0),
    POUCH_FIELD("crypto_key", LC_POUCH_SETTING_CRYPTO_KEY, crypto_key, 2),
    POUCH_FIELD("crypto_key_file", LC_POUCH_SETTING_CRYPTO_KEY_FILE,
                crypto_key_file, 2),
    POUCH_FIELD("crypto_generate_key_file",
                LC_POUCH_SETTING_CRYPTO_GENERATE_KEY_FILE,
                crypto_generate_key_file, 0),
    POUCH_FIELD("compression", LC_POUCH_SETTING_COMPRESSION, compression, 2)};
#undef POUCH_FIELD

static uint64_t pouch_u64(lua_State *lua, int index) {
  lua_Integer value, high, low;
  if (lua_isinteger(lua, index)) {
    value = lua_tointeger(lua, index);
    if (value < 0)
      luaL_error(lua, "Pouch integer must be nonnegative");
    return (uint64_t)value;
  }
  luaL_checktype(lua, index, LUA_TTABLE);
  index = lua_absindex(lua, index);
  lua_pushnil(lua);
  while (lua_next(lua, index)) {
    const char *name;
    size_t size;
    name =
        lua_type(lua, -2) == LUA_TSTRING ? lua_tolstring(lua, -2, &size) : NULL;
    if (name == NULL || strlen(name) != size ||
        (strcmp(name, "high") != 0 && strcmp(name, "low") != 0))
      luaL_error(lua, "Pouch uint64 words require only high and low");
    lua_pop(lua, 1);
  }
  lua_pushliteral(lua, "high");
  lua_rawget(lua, index);
  if (!lua_isinteger(lua, -1))
    luaL_error(lua, "Pouch high word must be an integer");
  high = lua_tointeger(lua, -1);
  lua_pop(lua, 1);
  lua_pushliteral(lua, "low");
  lua_rawget(lua, index);
  if (!lua_isinteger(lua, -1))
    luaL_error(lua, "Pouch low word must be an integer");
  low = lua_tointeger(lua, -1);
  lua_pop(lua, 1);
  if (high < 0 || low < 0 || (uint64_t)high > 0xffffffffu ||
      (uint64_t)low > 0xffffffffu)
    luaL_error(lua, "Pouch uint64 words must be unsigned 32-bit integers");
  return ((uint64_t)high << 32u) | (uint64_t)low;
}

void vectis_lua_pouch_parse(lua_State *lua, int index, lc_pouch_settings *out,
                            int pins) {
  size_t i;
  lc_pouch_settings_init(out);
  if (lua_isnil(lua, index))
    return;
  luaL_checktype(lua, index, LUA_TTABLE);
  index = lua_absindex(lua, index);
  pins = lua_absindex(lua, pins);
  lua_pushnil(lua);
  while (lua_next(lua, index)) {
    const char *name;
    size_t size;
    name =
        lua_type(lua, -2) == LUA_TSTRING ? lua_tolstring(lua, -2, &size) : NULL;
    if (name == NULL || strlen(name) != size)
      luaL_error(lua, "Pouch setting names must be NUL-free strings");
    for (i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i)
      if (strcmp(name, fields[i].name) == 0)
        break;
    if (i == sizeof(fields) / sizeof(fields[0]))
      luaL_error(lua, "unknown Pouch setting: %s", name);
    lua_pop(lua, 1);
  }
  for (i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
    unsigned char *target;
    lua_pushstring(lua, fields[i].name);
    lua_rawget(lua, index);
    if (lua_isnil(lua, -1)) {
      lua_pop(lua, 1);
      continue;
    }
    target = (unsigned char *)out + fields[i].offset;
    if (fields[i].kind == 0) {
      luaL_checktype(lua, -1, LUA_TBOOLEAN);
      *(int *)target = lua_toboolean(lua, -1);
    } else if (fields[i].kind == 1) {
      *(uint64_t *)target = pouch_u64(lua, -1);
    } else {
      const char *value;
      size_t size;
      luaL_checktype(lua, -1, LUA_TSTRING);
      value = lua_tolstring(lua, -1, &size);
      if (size == 0u || memchr(value, 0, size) != NULL)
        luaL_error(lua, "Pouch strings must be nonempty and NUL-free");
      *(const char **)target = value;
      lua_pushvalue(lua, -1);
      lua_rawseti(lua, pins, (lua_Integer)i + 1);
    }
    out->set_mask |= fields[i].mask;
    lua_pop(lua, 1);
  }
}
