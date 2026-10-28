#include "vectis_lua_crypto.h"
#include <lauxlib.h>
#include <string.h>
#include <unistd.h>
#include <vectis/crypto.h>
#define CRYPTO_KEY "vectis.crypto.key"
#define CRYPTO_BYTES "vectis.crypto.bytes"
typedef struct crypto_lua_key {
  vectis_crypto_key *native;
  int busy;
  pid_t process;
} crypto_lua_key;
typedef struct crypto_lua_bytes {
  vectis_mutable_bytes bytes;
  vectis_crypto_gcm_result encrypted;
} crypto_lua_bytes;
static int crypto_lua_error(lua_State *L, const vectis_error *error,
                            const char *operation) {
  lua_pushnil(L);
  lua_createtable(L, 0, 8);
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
  lua_pushstring(L, error->detail);
  lua_setfield(L, -2, "detail");
  lua_pushstring(L, operation);
  lua_setfield(L, -2, "operation");
  return 2;
}
static const char *crypto_lua_string(lua_State *L, int index, size_t *size,
                                     int text) {
  const char *value;
  luaL_checktype(L, index, LUA_TSTRING);
  value = lua_tolstring(L, index, size);
  if (text && memchr(value, 0, *size) != NULL)
    luaL_argerror(L, index, "string contains NUL");
  return value;
}
static void crypto_lua_field(lua_State *L, int index, const char *name) {
  if (lua_isnoneornil(L, index)) {
    lua_pushnil(L);
    return;
  }
  index = lua_absindex(L, index);
  lua_pushstring(L, name);
  lua_rawget(L, index);
}
static void crypto_lua_fields(lua_State *L, int index,
                              const char *const *fields) {
  const char *key;
  size_t size;
  int i;
  index = lua_absindex(L, index);
  if (lua_isnoneornil(L, index))
    return;
  luaL_checktype(L, index, LUA_TTABLE);
  lua_pushnil(L);
  while (lua_next(L, index)) {
    key = crypto_lua_string(L, -2, &size, 1);
    for (i = 0; fields[i] != NULL && strcmp(key, fields[i]) != 0; ++i) {
    }
    if (fields[i] == NULL)
      luaL_error(L, "unknown crypto option '%s'", key);
    lua_pop(L, 1);
  }
}
static int crypto_lua_bytes_gc(lua_State *L) {
  crypto_lua_bytes *owner =
      (crypto_lua_bytes *)luaL_testudata(L, 1, CRYPTO_BYTES);
  if (owner != NULL) {
    vectis_mutable_bytes_cleanup(&owner->bytes);
    vectis_crypto_gcm_result_cleanup(&owner->encrypted);
  }
  return 0;
}
static crypto_lua_bytes *crypto_lua_bytes_new(lua_State *L) {
  crypto_lua_bytes *owner =
      (crypto_lua_bytes *)lua_newuserdatauv(L, sizeof(*owner), 0);
  memset(owner, 0, sizeof(*owner));
  luaL_setmetatable(L, CRYPTO_BYTES);
  lua_toclose(L, -1);
  return owner;
}
static crypto_lua_key *crypto_lua_key_check(lua_State *L) {
  crypto_lua_key *owner = (crypto_lua_key *)luaL_checkudata(L, 1, CRYPTO_KEY);
  luaL_argcheck(L, owner->native != NULL, 1, "key is closed");
  luaL_argcheck(L, owner->process == getpid(), 1,
                "key belongs to another process");
  return owner;
}
static int crypto_lua_key_close(lua_State *L) {
  crypto_lua_key *owner = (crypto_lua_key *)luaL_checkudata(L, 1, CRYPTO_KEY);
  int finalizer = lua_toboolean(L, lua_upvalueindex(1));
  vectis_error error;
  if (owner->busy || owner->process != getpid()) {
    if (finalizer)
      return 0;
    return luaL_error(L, "cannot close active or foreign-process key");
  }
  if (vectis_crypto_key_close(owner->native, &error) != VECTIS_OK) {
    if (finalizer)
      return 0;
    return crypto_lua_error(L, &error, "close");
  }
  owner->native = NULL;
  if (!finalizer)
    lua_pushboolean(L, 1);
  return !finalizer;
}
static int crypto_lua_load_key(lua_State *L) {
  static const char *const fields[] = {"private_key_pem", "private_key_path",
                                       "public_key_pem",  "public_key_path",
                                       "certificate_pem", "certificate_path",
                                       "passphrase",      NULL};
  crypto_lua_key *owner;
  vectis_crypto_key *key = NULL;
  vectis_crypto_key_source source = VECTIS_CRYPTO_PRIVATE_KEY;
  vectis_error error;
  vectis_status status;
  const char *value = NULL, *passphrase = NULL;
  size_t size = 0u, passphrase_size = 0u;
  int i, count = 0, file = 0, result;
  lua_settop(L, 1);
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checkstack(L, 24, "key constructor stack");
  owner = (crypto_lua_key *)lua_newuserdatauv(L, sizeof(*owner), 0);
  memset(owner, 0, sizeof(*owner));
  owner->process = getpid();
  result = lua_gettop(L);
  luaL_setmetatable(L, CRYPTO_KEY);
  crypto_lua_fields(L, 1, fields);
  for (i = 0; i < 6; ++i) {
    crypto_lua_field(L, 1, fields[i]);
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      continue;
    }
    ++count;
    file = i % 2;
    source = (vectis_crypto_key_source)(i / 2 + 1);
    value = crypto_lua_string(L, -1, &size, file); /* Retained on the stack. */
  }
  luaL_argcheck(L, count == 1, 1,
                "load_key requires exactly one PEM/path source");
  crypto_lua_field(L, 1, "passphrase");
  if (!lua_isnil(L, -1))
    passphrase = crypto_lua_string(L, -1, &passphrase_size, 0);
  if (file)
    status = vectis_crypto_key_load_file(source, value, passphrase,
                                         passphrase_size, &key, &error);
  else
    status = vectis_crypto_key_load(source, value, size, passphrase,
                                    passphrase_size, &key, &error);
  owner->native = key;
  if (status != VECTIS_OK)
    return crypto_lua_error(L, &error, "load_key");
  lua_pushvalue(L, result);
  return 1;
}
static const char *crypto_lua_algorithm(lua_State *L, int index) {
  static const char *const fields[] = {"algorithm", "digest", NULL};
  size_t size;
  crypto_lua_fields(L, index, fields);
  crypto_lua_field(L, index, "algorithm");
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    crypto_lua_field(L, index, "digest");
  }
  if (lua_isnil(L, -1))
    return NULL;
  return crypto_lua_string(L, -1, &size, 1);
}
static int crypto_lua_sign(lua_State *L) {
  crypto_lua_key *owner = crypto_lua_key_check(L);
  crypto_lua_bytes *output;
  const char *data, *algorithm;
  size_t size;
  vectis_error error;
  lua_settop(L, 3);
  output = crypto_lua_bytes_new(L);
  data = crypto_lua_string(L, 2, &size, 0);
  algorithm = crypto_lua_algorithm(L, 3);
  if (vectis_crypto_key_sign(owner->native, algorithm, data, size,
                             &output->bytes, &error) != VECTIS_OK)
    return crypto_lua_error(L, &error, "sign");
  lua_pushlstring(L, (const char *)output->bytes.data, output->bytes.size);
  return 1;
}
static int crypto_lua_verify(lua_State *L) {
  crypto_lua_key *owner = crypto_lua_key_check(L);
  const char *data, *signature, *algorithm;
  size_t size, signature_size;
  vectis_error error;
  int verified;
  data = crypto_lua_string(L, 2, &size, 0);
  signature = crypto_lua_string(L, 3, &signature_size, 0);
  algorithm = crypto_lua_algorithm(L, 4);
  if (vectis_crypto_key_verify(owner->native, algorithm, data, size, signature,
                               signature_size, &verified, &error) != VECTIS_OK)
    return crypto_lua_error(L, &error, "verify");
  lua_pushboolean(L, verified);
  return 1;
}
static int crypto_lua_info(lua_State *L) {
  crypto_lua_key *owner = crypto_lua_key_check(L);
  vectis_crypto_key_info info;
  vectis_error error;
  if (vectis_crypto_key_get_info(owner->native, &info, &error) != VECTIS_OK)
    return crypto_lua_error(L, &error, "info");
  lua_createtable(L, 0, 4);
  lua_pushstring(L, info.type);
  lua_setfield(L, -2, "type");
  lua_pushinteger(L, info.bits);
  lua_setfield(L, -2, "bits");
  lua_pushinteger(L, (lua_Integer)info.max_signature_bytes);
  lua_setfield(L, -2, "max_signature_bytes");
  lua_pushboolean(L, info.private_key);
  lua_setfield(L, -2, "private_key");
  return 1;
}
static int crypto_lua_key_guard(lua_State *L) {
  int args = lua_gettop(L), status;
  crypto_lua_key *owner;
  luaL_checkstack(L, args + 3, "protected key operation stack");
  owner = crypto_lua_key_check(L);
  luaL_argcheck(L, !owner->busy, 1, "key is active");
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  owner->busy = 1;
  status = lua_pcall(L, args, LUA_MULTRET, 0);
  owner->busy = 0;
  if (status != LUA_OK)
    return lua_error(L);
  return lua_gettop(L);
}
static int crypto_lua_gcm(lua_State *L, int encrypt) {
  static const char *const encrypt_fields[] = {"key", "nonce", "data", "aad",
                                               NULL};
  static const char *const decrypt_fields[] = {"key", "nonce", "ciphertext",
                                               "tag", "aad",   NULL};
  crypto_lua_bytes *owner;
  const char *key, *nonce, *data, *aad = NULL, *tag = NULL;
  size_t key_size, nonce_size, size, aad_size = 0u, tag_size = 0u;
  vectis_error error;
  vectis_status status;
  lua_settop(L, 1);
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checkstack(L, 20, "AES-GCM stack");
  owner = crypto_lua_bytes_new(L);
  crypto_lua_fields(L, 1, encrypt ? encrypt_fields : decrypt_fields);
  crypto_lua_field(L, 1, "key");
  key = crypto_lua_string(L, -1, &key_size, 0);
  crypto_lua_field(L, 1, "nonce");
  nonce = crypto_lua_string(L, -1, &nonce_size, 0);
  crypto_lua_field(L, 1, encrypt ? "data" : "ciphertext");
  data = crypto_lua_string(L, -1, &size, 0);
  crypto_lua_field(L, 1, "aad");
  if (!lua_isnil(L, -1))
    aad = crypto_lua_string(L, -1, &aad_size, 0);
  if (encrypt)
    status = vectis_crypto_gcm_encrypt_materialized(
        key, key_size, nonce, nonce_size, data, size, aad, aad_size,
        &owner->encrypted, &error);
  else {
    crypto_lua_field(L, 1, "tag");
    tag = crypto_lua_string(L, -1, &tag_size, 0);
    status = vectis_crypto_gcm_decrypt_materialized(
        key, key_size, nonce, nonce_size, data, size, aad, aad_size, tag,
        tag_size, &owner->bytes, &error);
  }
  if (status != VECTIS_OK)
    return crypto_lua_error(L, &error,
                            encrypt ? "aes_gcm_encrypt_materialized"
                                    : "aes_gcm_decrypt_materialized");
  if (!encrypt) {
    lua_pushlstring(L, (const char *)owner->bytes.data, owner->bytes.size);
    return 1;
  }
  lua_createtable(L, 0, 2);
  lua_pushlstring(L, (const char *)owner->encrypted.ciphertext.data,
                  owner->encrypted.ciphertext.size);
  lua_setfield(L, -2, "ciphertext");
  lua_pushlstring(L, (const char *)owner->encrypted.tag, 16u);
  lua_setfield(L, -2, "tag");
  return 1;
}
static int crypto_lua_encrypt(lua_State *L) { return crypto_lua_gcm(L, 1); }
static int crypto_lua_decrypt(lua_State *L) { return crypto_lua_gcm(L, 0); }
static void crypto_lua_method(lua_State *L, const char *name,
                              lua_CFunction function) {
  lua_pushcfunction(L, function);
  lua_pushcclosure(L, crypto_lua_key_guard, 1);
  lua_setfield(L, -2, name);
}
void vectis_lua_crypto_register(lua_State *L) {
  int module = lua_absindex(L, -1);
  if (luaL_newmetatable(L, CRYPTO_KEY)) {
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    crypto_lua_method(L, "sign", crypto_lua_sign);
    crypto_lua_method(L, "verify", crypto_lua_verify);
    crypto_lua_method(L, "info", crypto_lua_info);
    lua_pushboolean(L, 0);
    lua_pushcclosure(L, crypto_lua_key_close, 1);
    lua_setfield(L, -2, "close");
    lua_pushboolean(L, 1);
    lua_pushcclosure(L, crypto_lua_key_close, 1);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, "__gc");
    lua_setfield(L, -2, "__close");
  }
  lua_pop(L, 1);
  if (luaL_newmetatable(L, CRYPTO_BYTES)) {
    lua_pushcfunction(L, crypto_lua_bytes_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, crypto_lua_bytes_gc);
    lua_setfield(L, -2, "__close");
  }
  lua_pop(L, 1);
  lua_pushcfunction(L, crypto_lua_load_key);
  lua_setfield(L, module, "load_key");
  lua_pushcfunction(L, crypto_lua_encrypt);
  lua_setfield(L, module, "aes_gcm_encrypt_materialized");
  lua_pushcfunction(L, crypto_lua_decrypt);
  lua_setfield(L, module, "aes_gcm_decrypt_materialized");
}
int luaopen_vectis_crypto_core(lua_State *L) {
  lua_newtable(L);
  vectis_lua_crypto_register(L);
  return 1;
}
