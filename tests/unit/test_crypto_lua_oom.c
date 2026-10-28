#include "vectis_lua_crypto.h"
#include <assert.h>
#include <lauxlib.h>
#include <lualib.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct crypto_fault {
  size_t remaining;
  int armed, fired;
} crypto_fault;
static void *crypto_allocate(void *context, void *pointer, size_t previous,
                             size_t size) {
  crypto_fault *fault = (crypto_fault *)context;
  if (size == 0u) {
    free(pointer);
    return NULL;
  }
  if (fault->armed && (pointer == NULL || size > previous)) {
    if (fault->remaining == 0u) {
      fault->fired = 1;
      return NULL;
    }
    --fault->remaining;
  }
  return realloc(pointer, size);
}
int main(int argc, char **argv) {
  EVP_PKEY_CTX *generator = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
  EVP_PKEY *key = NULL;
  BIO *private_bio = BIO_new(BIO_s_mem()), *public_bio = BIO_new(BIO_s_mem());
  char *private_pem, *public_pem;
  long private_size, public_size;
  lua_State *L;
  crypto_fault fault;
  size_t i;
  int status, failures = 0, completed = 0;
  assert(argc == 2 && generator != NULL && private_bio != NULL &&
         public_bio != NULL);
  assert(EVP_PKEY_keygen_init(generator) == 1 &&
         EVP_PKEY_CTX_set_rsa_keygen_bits(generator, 2048) == 1 &&
         EVP_PKEY_keygen(generator, &key) == 1);
  assert(PEM_write_bio_PrivateKey(private_bio, key, NULL, NULL, 0, NULL,
                                  NULL) == 1 &&
         PEM_write_bio_PUBKEY(public_bio, key) == 1);
  private_size = BIO_get_mem_data(private_bio, &private_pem);
  public_size = BIO_get_mem_data(public_bio, &public_pem);
  assert(private_size > 0 && public_size > 0);
  EVP_PKEY_free(key);
  EVP_PKEY_CTX_free(generator);
  for (i = 0u; i < 1024u; ++i) {
    fault.remaining = i;
    fault.armed = fault.fired = 0;
    L = lua_newstate(crypto_allocate, &fault, 0u);
    assert(L != NULL);
    luaL_openlibs(L);
    luaL_requiref(L, "vectis.crypto.core", luaopen_vectis_crypto_core, 0);
    lua_pop(L, 1);
    lua_pushlstring(L, private_pem, (size_t)private_size);
    lua_setglobal(L, "CRYPTO_PRIVATE_PEM");
    lua_pushlstring(L, public_pem, (size_t)public_size);
    lua_setglobal(L, "CRYPTO_PUBLIC_PEM");
    assert(luaL_loadfile(L, argv[1]) == LUA_OK);
    fault.armed = 1;
    status = lua_pcall(L, 0, 1, 0);
    fault.armed = 0;
    if (fault.fired) {
      ++failures;
      assert(status != LUA_OK);
    } else {
      if (status != LUA_OK)
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
      assert(status == LUA_OK && lua_toboolean(L, -1));
      completed = 1;
    }
    lua_close(L);
    if (completed)
      break;
  }
  BIO_free(private_bio);
  BIO_free(public_bio);
  assert(failures > 0 && completed);
  printf("Crypto Lua allocation failures: %d; clean execution completed\n",
         failures);
  return 0;
}
