#include "vectis_lua_digest.h"
#include <lauxlib.h>
#include <string.h>
#include <vectis/digest.h>

typedef struct lua_digest_owner {
  vectis_digest *receiver;
} lua_digest_owner;

static void digest_owner_close(lua_digest_owner *owner) {
  vectis_digest_close(owner->receiver);
  owner->receiver = NULL;
}

static int digest_owner_gc(lua_State *lua) {
  lua_digest_owner *owner;
  owner = (lua_digest_owner *)luaL_testudata(lua, 1, "vectis.digest.owner");
  if (owner != NULL)
    digest_owner_close(owner);
  return 0;
}

static lua_digest_owner *digest_owner_check(lua_State *lua) {
  lua_digest_owner *owner;
  owner = (lua_digest_owner *)luaL_checkudata(lua, 1, "vectis.digest.owner");
  if (owner->receiver == NULL)
    luaL_error(lua, "digest owner is closed");
  return owner;
}

static int digest_owner_update(lua_State *lua) {
  lua_digest_owner *owner;
  const char *chunk;
  size_t size;
  vectis_error error;
  owner = digest_owner_check(lua);
  luaL_checktype(lua, 2, LUA_TSTRING);
  chunk = lua_tolstring(lua, 2, &size);
  if (size > VECTIS_DIGEST_CHUNK_BYTES)
    return luaL_error(lua, "digest update chunk exceeds 65536 bytes");
  if (owner->receiver->update(owner->receiver, chunk, size, &error) !=
      VECTIS_OK)
    return luaL_error(lua, "%s", error.message);
  lua_settop(lua, 1);
  return 1;
}

static int digest_owner_create(lua_State *lua, int hmac) {
  lua_digest_owner *owner;
  const char *algorithm;
  size_t size;
  size_t key_size;
  const char *key;
  vectis_error error;
  algorithm = luaL_checklstring(lua, 1, &size);
  if (strlen(algorithm) != size)
    return luaL_error(lua, "digest algorithm contains NUL");
  key = hmac ? luaL_checklstring(lua, 2, &key_size) : NULL;
  if (!hmac)
    key_size = 0u;
  owner = (lua_digest_owner *)lua_newuserdatauv(lua, sizeof(*owner), 0);
  owner->receiver = NULL;
  luaL_getmetatable(lua, "vectis.digest.owner");
  lua_setmetatable(lua, -2);
  if ((hmac
           ? vectis_hmac_new(algorithm, key, key_size, &owner->receiver, &error)
           : vectis_digest_new(algorithm, &owner->receiver, &error)) !=
      VECTIS_OK)
    return luaL_error(lua, "%s", error.message);
  return 1;
}

static int digest_owner_new(lua_State *lua) {
  return digest_owner_create(lua, 0);
}

static int hmac_owner_new(lua_State *lua) {
  return digest_owner_create(lua, 1);
}

static void digest_push(lua_State *lua, const vectis_digest_result *result,
                        int hex_output) {
  static const char hex[] = "0123456789abcdef";
  char buffer[VECTIS_DIGEST_MAX_BYTES * 2u];
  size_t i;
  if (!hex_output) {
    lua_pushlstring(lua, (const char *)result->data, result->size);
    return;
  }
  for (i = 0u; i < result->size; ++i) {
    buffer[i * 2u] = hex[result->data[i] >> 4u];
    buffer[i * 2u + 1u] = hex[result->data[i] & 15u];
  }
  lua_pushlstring(lua, buffer, result->size * 2u);
}

static int digest_owner_final(lua_State *lua) {
  lua_digest_owner *owner;
  vectis_digest_result result;
  vectis_error error;
  int hex_output;
  owner = digest_owner_check(lua);
  hex_output = lua_toboolean(lua, lua_upvalueindex(1));
  if (owner->receiver->final(owner->receiver, &result, &error) != VECTIS_OK)
    return luaL_error(lua, "%s", error.message);
  digest_push(lua, &result, hex_output);
  lua_createtable(lua, 0, 2);
  lua_pushinteger(lua, (lua_Integer)(result.bytes >> 32u));
  lua_setfield(lua, -2, "high");
  lua_pushinteger(lua, (lua_Integer)(result.bytes & 0xffffffffu));
  lua_setfield(lua, -2, "low");
  return 2;
}

/* mode: materialized=0, reader=1, file=2. Upvalues select SHA-256 versus
 * named algorithm, mode and hexadecimal versus binary output. */
static int digest_call(lua_State *lua) {
  const char *algorithm;
  const char *input;
  const char *key;
  size_t size;
  size_t key_size;
  int sha256;
  int hmac;
  int mode;
  int hex_output;
  int input_index;
  lua_digest_owner *owner;
  vectis_digest_result result;
  vectis_error error;
  vectis_status status;

  sha256 = lua_toboolean(lua, lua_upvalueindex(1));
  mode = (int)lua_tointeger(lua, lua_upvalueindex(2));
  hex_output = lua_toboolean(lua, lua_upvalueindex(3));
  hmac = lua_toboolean(lua, lua_upvalueindex(4));
  input_index = sha256 ? 1 : 2;
  algorithm = "sha256";
  if (!sha256) {
    algorithm = luaL_checklstring(lua, 1, &size);
    if (strlen(algorithm) != size)
      return luaL_error(lua, "digest algorithm contains NUL");
  }
  key = NULL;
  key_size = 0u;
  if (hmac) {
    key = luaL_checklstring(lua, input_index, &key_size);
    ++input_index;
  }
  memset(&error, 0, sizeof(error));
  if (mode != 1) {
    input = luaL_checklstring(lua, input_index, &size);
    if (mode == 2 && strlen(input) != size)
      return luaL_error(lua, "digest file path contains NUL");
    if (hmac)
      status = mode == 2
                   ? vectis_hmac_file(algorithm, key, key_size, input, &result,
                                      &error)
                   : vectis_hmac_materialized(algorithm, key, key_size, input,
                                              size, &result, &error);
    else
      status = mode == 2 ? vectis_digest_file(algorithm, input, &result, &error)
                         : vectis_digest_materialized(algorithm, input, size,
                                                      &result, &error);
    if (status != VECTIS_OK)
      return luaL_error(lua, "%s", error.message);
    digest_push(lua, &result, hex_output);
    return 1;
  }
  luaL_checktype(lua, input_index, LUA_TFUNCTION);
  owner = (lua_digest_owner *)lua_newuserdatauv(lua, sizeof(*owner), 0);
  owner->receiver = NULL;
  luaL_getmetatable(lua, "vectis.digest.owner");
  lua_setmetatable(lua, -2);
  status =
      hmac ? vectis_hmac_new(algorithm, key, key_size, &owner->receiver, &error)
           : vectis_digest_new(algorithm, &owner->receiver, &error);
  if (status != VECTIS_OK)
    return luaL_error(lua, "%s", error.message);
  for (;;) {
    lua_pushvalue(lua, input_index);
    lua_pushinteger(lua, VECTIS_DIGEST_CHUNK_BYTES);
    if (lua_pcall(lua, 1, 2, 0) != LUA_OK) {
      digest_owner_close(owner);
      return lua_error(lua);
    }
    if (!lua_isnil(lua, -1)) {
      digest_owner_close(owner);
      return lua_error(lua);
    }
    if (owner->receiver == NULL)
      return luaL_error(lua, "digest owner is closed");
    if (lua_isnil(lua, -2)) {
      lua_pop(lua, 2);
      break;
    }
    if (lua_type(lua, -2) != LUA_TSTRING) {
      digest_owner_close(owner);
      return luaL_error(lua,
                        "digest reader must return a string or nil at EOF");
    }
    input = lua_tolstring(lua, -2, &size);
    if (size == 0u || size > VECTIS_DIGEST_CHUNK_BYTES) {
      digest_owner_close(owner);
      return luaL_error(lua, "digest reader must return 1 through 65536 bytes");
    }
    status = owner->receiver->update(owner->receiver, input, size, &error);
    lua_pop(lua, 2);
    if (status != VECTIS_OK) {
      digest_owner_close(owner);
      return luaL_error(lua, "%s", error.message);
    }
  }
  status = owner->receiver->final(owner->receiver, &result, &error);
  digest_owner_close(owner);
  if (status != VECTIS_OK)
    return luaL_error(lua, "%s", error.message);
  digest_push(lua, &result, hex_output);
  return 1;
}

void vectis_lua_digest_register(lua_State *lua) {
  static const char *const names[] = {"digest_materialized",
                                      "digest_materialized_hex",
                                      "digest_stream",
                                      "digest_stream_hex",
                                      "digest_file",
                                      "digest_file_hex",
                                      "sha256_materialized",
                                      "sha256_materialized_hex",
                                      "sha256_stream",
                                      "sha256_stream_hex",
                                      "sha256_file",
                                      "sha256_file_hex",
                                      "hmac_materialized",
                                      "hmac_materialized_hex",
                                      "hmac_stream",
                                      "hmac_stream_hex",
                                      "hmac_file",
                                      "hmac_file_hex",
                                      "hmac_sha256_materialized",
                                      "hmac_sha256_materialized_hex",
                                      "hmac_sha256_stream",
                                      "hmac_sha256_stream_hex",
                                      "hmac_sha256_file",
                                      "hmac_sha256_file_hex"};
  unsigned i;
  if (luaL_newmetatable(lua, "vectis.digest.owner")) {
    lua_pushcfunction(lua, digest_owner_gc);
    lua_setfield(lua, -2, "__gc");
    lua_pushcfunction(lua, digest_owner_gc);
    lua_setfield(lua, -2, "__close");
    lua_newtable(lua);
    lua_pushcfunction(lua, digest_owner_gc);
    lua_setfield(lua, -2, "close");
    lua_pushcfunction(lua, digest_owner_update);
    lua_setfield(lua, -2, "update");
    lua_pushboolean(lua, 0);
    lua_pushcclosure(lua, digest_owner_final, 1);
    lua_setfield(lua, -2, "final");
    lua_pushboolean(lua, 1);
    lua_pushcclosure(lua, digest_owner_final, 1);
    lua_setfield(lua, -2, "final_hex");
    lua_setfield(lua, -2, "__index");
  }
  lua_pop(lua, 1);
  lua_pushcfunction(lua, digest_owner_new);
  lua_setfield(lua, -2, "digest_stream_new");
  lua_pushcfunction(lua, hmac_owner_new);
  lua_setfield(lua, -2, "hmac_stream_new");
  lua_pushinteger(lua, VECTIS_DIGEST_MAX_BYTES);
  lua_setfield(lua, -2, "DIGEST_MAX_BYTES");
  lua_pushinteger(lua, VECTIS_DIGEST_CHUNK_BYTES);
  lua_setfield(lua, -2, "DIGEST_CHUNK_BYTES");
  for (i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
    lua_pushboolean(lua, (i % 12u) >= 6u);
    lua_pushinteger(lua, (i % 6u) / 2u);
    lua_pushboolean(lua, (i % 2u) != 0u);
    lua_pushboolean(lua, i >= 12u);
    lua_pushcclosure(lua, digest_call, 4);
    lua_setfield(lua, -2, names[i]);
  }
}

int luaopen_vectis_digest_core(lua_State *lua) {
  lua_newtable(lua);
  vectis_lua_digest_register(lua);
  return 1;
}
