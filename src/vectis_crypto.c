#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vectis/crypto.h>
typedef struct crypto_key_state {
  vectis_crypto_key receiver;
  EVP_PKEY *native;
  int private_key;
  pid_t process;
  pthread_t thread;
} crypto_key_state;
typedef struct crypto_password {
  const void *data;
  size_t size;
} crypto_password;
static vectis_status crypto_error(vectis_error *error, vectis_status status,
                                  const char *message, int native) {
  unsigned long code = native ? ERR_get_error() : 0u;
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->code = status;
    error->source = VECTIS_ERROR_SOURCE_OPENSSL;
    error->dependency_code =
        code > (unsigned long)LONG_MAX ? LONG_MAX : (long)code;
    strncpy(error->message, message, sizeof(error->message) - 1u);
    if (code != 0u)
      ERR_error_string_n(code, error->detail, sizeof(error->detail));
  }
  if (native)
    ERR_clear_error();
  return status;
}
static int crypto_password_copy(char *buffer, int capacity, int writing,
                                void *context) {
  crypto_password *password = (crypto_password *)context;
  (void)writing;
  if (capacity <= 0 || password->size >= (size_t)capacity)
    return 0;
  if (password->size != 0u)
    memcpy(buffer, password->data, password->size);
  buffer[password->size] = '\0';
  return (int)password->size;
}
static vectis_status crypto_key_check(vectis_crypto_key *receiver,
                                      crypto_key_state **out,
                                      vectis_error *error) {
  crypto_key_state *state =
      receiver == NULL ? NULL : (crypto_key_state *)receiver->impl;
  if (state == NULL)
    return crypto_error(error, VECTIS_ERR_INVALID, "missing key receiver", 0);
  if (state->process != getpid() ||
      !pthread_equal(state->thread, pthread_self()))
    return crypto_error(error, VECTIS_ERR_STATE,
                        "key belongs to another process/thread", 0);
  *out = state;
  return VECTIS_OK;
}
vectis_status vectis_crypto_key_load(vectis_crypto_key_source source,
                                     const void *pem, size_t size,
                                     const void *passphrase,
                                     size_t passphrase_size,
                                     vectis_crypto_key **out,
                                     vectis_error *error) {
  crypto_key_state *state;
  BIO *reader;
  EVP_PKEY *key = NULL;
  X509 *certificate;
  crypto_password password;
  if (out != NULL)
    *out = NULL;
  if (out == NULL || pem == NULL || size == 0u ||
      size > VECTIS_CRYPTO_MAX_BYTES || passphrase_size > 1023u ||
      (passphrase == NULL && passphrase_size != 0u) ||
      (source != VECTIS_CRYPTO_PRIVATE_KEY &&
       source != VECTIS_CRYPTO_PUBLIC_KEY &&
       source != VECTIS_CRYPTO_CERTIFICATE))
    return crypto_error(error, VECTIS_ERR_INVALID,
                        "invalid PEM/key source/passphrase", 0);
  ERR_clear_error();
  reader = BIO_new_mem_buf(pem, (int)size);
  if (reader == NULL)
    return crypto_error(error, VECTIS_ERR_NOMEM, "allocating PEM reader", 1);
  password.data = passphrase;
  password.size = passphrase_size;
  if (source == VECTIS_CRYPTO_PRIVATE_KEY)
    key =
        PEM_read_bio_PrivateKey(reader, NULL, crypto_password_copy, &password);
  else if (source == VECTIS_CRYPTO_PUBLIC_KEY)
    key = PEM_read_bio_PUBKEY(reader, NULL, crypto_password_copy, &password);
  else {
    certificate =
        PEM_read_bio_X509(reader, NULL, crypto_password_copy, &password);
    if (certificate != NULL) {
      key = X509_get_pubkey(certificate);
      X509_free(certificate);
    }
  }
  BIO_free(reader);
  if (key == NULL)
    return crypto_error(error, VECTIS_ERR_STATE, "loading PEM key failed", 1);
  state = (crypto_key_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    EVP_PKEY_free(key);
    return crypto_error(error, VECTIS_ERR_NOMEM, "allocating key receiver", 0);
  }
  state->native = key;
  state->private_key = source == VECTIS_CRYPTO_PRIVATE_KEY;
  state->process = getpid();
  state->thread = pthread_self();
  state->receiver.sign = vectis_crypto_key_sign;
  state->receiver.verify = vectis_crypto_key_verify;
  state->receiver.info = vectis_crypto_key_get_info;
  state->receiver.close = vectis_crypto_key_close;
  state->receiver.impl = state;
  *out = &state->receiver;
  return VECTIS_OK;
}
vectis_status
vectis_crypto_key_load_file(vectis_crypto_key_source source, const char *path,
                            const void *passphrase, size_t passphrase_size,
                            vectis_crypto_key **out, vectis_error *error) {
  FILE *file;
  long length;
  unsigned char *data;
  size_t size;
  int extra, closed, read_error;
  vectis_status status;
  if (out != NULL)
    *out = NULL;
  if (path == NULL || out == NULL)
    return crypto_error(error, VECTIS_ERR_INVALID, "missing key path/output",
                        0);
  file = fopen(path, "rb");
  if (file == NULL)
    return crypto_error(error, VECTIS_ERR_STATE, "opening key file failed", 0);
  if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
      length > (long)VECTIS_CRYPTO_MAX_BYTES || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return crypto_error(error, VECTIS_ERR_INVALID,
                        "key file must be seekable, nonempty and <=16 MiB", 0);
  }
  size = (size_t)length;
  data = (unsigned char *)malloc(size);
  if (data == NULL) {
    fclose(file);
    return crypto_error(error, VECTIS_ERR_NOMEM, "allocating key file input",
                        0);
  }
  if (fread(data, 1u, size, file) != size) {
    OPENSSL_cleanse(data, size);
    free(data);
    fclose(file);
    return crypto_error(error, VECTIS_ERR_STATE, "reading key file failed", 0);
  }
  extra = fgetc(file);
  read_error = ferror(file);
  closed = fclose(file);
  if (extra != EOF || read_error || closed != 0)
    status = crypto_error(error, VECTIS_ERR_STATE,
                          "key file changed or close failed", 0);
  else
    status = vectis_crypto_key_load(source, data, size, passphrase,
                                    passphrase_size, out, error);
  OPENSSL_cleanse(data, size);
  free(data);
  return status;
}
vectis_status vectis_crypto_key_get_info(vectis_crypto_key *receiver,
                                         vectis_crypto_key_info *out,
                                         vectis_error *error) {
  crypto_key_state *state;
  int size;
  vectis_status status = crypto_key_check(receiver, &state, error);
  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (status != VECTIS_OK)
    return status;
  if (out == NULL)
    return crypto_error(error, VECTIS_ERR_INVALID, "missing key info output",
                        0);
  out->type = EVP_PKEY_get0_type_name(state->native);
  out->bits = EVP_PKEY_get_bits(state->native);
  size = EVP_PKEY_get_size(state->native);
  if (out->type == NULL || out->bits <= 0 || size <= 0)
    return crypto_error(error, VECTIS_ERR_STATE, "reading key metadata failed",
                        1);
  out->max_signature_bytes = (size_t)size;
  out->private_key = state->private_key;
  return VECTIS_OK;
}
vectis_status vectis_crypto_key_close(vectis_crypto_key *receiver,
                                      vectis_error *error) {
  crypto_key_state *state;
  vectis_status status;
  if (receiver == NULL)
    return VECTIS_OK;
  status = crypto_key_check(receiver, &state, error);
  if (status != VECTIS_OK)
    return status;
  EVP_PKEY_free(state->native);
  free(state);
  return VECTIS_OK;
}
static vectis_status crypto_digest(const char *algorithm, const EVP_MD **digest,
                                   vectis_error *error) {
  *digest = EVP_get_digestbyname(algorithm == NULL ? "sha256" : algorithm);
  if (*digest == NULL)
    return crypto_error(error, VECTIS_ERR_INVALID, "unknown signature digest",
                        0);
  return VECTIS_OK;
}
vectis_status vectis_crypto_key_sign(vectis_crypto_key *receiver,
                                     const char *algorithm, const void *data,
                                     size_t size, vectis_mutable_bytes *out,
                                     vectis_error *error) {
  crypto_key_state *state;
  EVP_MD_CTX *context;
  const EVP_MD *digest;
  unsigned char *signature = NULL;
  size_t signature_size = 0u;
  vectis_status status = crypto_key_check(receiver, &state, error);
  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (status != VECTIS_OK)
    return status;
  if (out == NULL || (data == NULL && size != 0u) ||
      size > VECTIS_CRYPTO_MAX_BYTES || !state->private_key)
    return crypto_error(error, VECTIS_ERR_INVALID,
                        "sign requires private key and <=16 MiB data/output",
                        0);
  status = crypto_digest(algorithm, &digest, error);
  if (status != VECTIS_OK)
    return status;
  ERR_clear_error();
  context = EVP_MD_CTX_new();
  if (context == NULL)
    return crypto_error(error, VECTIS_ERR_NOMEM, "allocating signature context",
                        1);
  if (EVP_DigestSignInit(context, NULL, digest, NULL, state->native) != 1 ||
      (size != 0u && EVP_DigestSignUpdate(context, data, size) != 1) ||
      EVP_DigestSignFinal(context, NULL, &signature_size) != 1 ||
      signature_size == 0u || signature_size > VECTIS_CRYPTO_MAX_BYTES) {
    EVP_MD_CTX_free(context);
    return crypto_error(error, VECTIS_ERR_STATE,
                        "initializing signature failed", 1);
  }
  signature = (unsigned char *)malloc(signature_size);
  if (signature == NULL) {
    EVP_MD_CTX_free(context);
    return crypto_error(error, VECTIS_ERR_NOMEM, "allocating signature output",
                        0);
  }
  if (EVP_DigestSignFinal(context, signature, &signature_size) != 1) {
    free(signature);
    EVP_MD_CTX_free(context);
    return crypto_error(error, VECTIS_ERR_STATE, "signing failed", 1);
  }
  EVP_MD_CTX_free(context);
  out->data = signature;
  out->size = signature_size;
  return VECTIS_OK;
}
vectis_status vectis_crypto_key_verify(vectis_crypto_key *receiver,
                                       const char *algorithm, const void *data,
                                       size_t size, const void *signature,
                                       size_t signature_size, int *verified,
                                       vectis_error *error) {
  crypto_key_state *state;
  EVP_MD_CTX *context;
  const EVP_MD *digest;
  int result;
  vectis_status status = crypto_key_check(receiver, &state, error);
  if (verified != NULL)
    *verified = 0;
  if (status != VECTIS_OK)
    return status;
  if (verified == NULL || (data == NULL && size != 0u) || signature == NULL ||
      size > VECTIS_CRYPTO_MAX_BYTES ||
      signature_size > VECTIS_CRYPTO_MAX_BYTES)
    return crypto_error(error, VECTIS_ERR_INVALID,
                        "verify requires signature and <=16 MiB input", 0);
  status = crypto_digest(algorithm, &digest, error);
  if (status != VECTIS_OK)
    return status;
  ERR_clear_error();
  context = EVP_MD_CTX_new();
  if (context == NULL)
    return crypto_error(error, VECTIS_ERR_NOMEM,
                        "allocating verification context", 1);
  if (EVP_DigestVerifyInit(context, NULL, digest, NULL, state->native) != 1 ||
      (size != 0u && EVP_DigestVerifyUpdate(context, data, size) != 1)) {
    EVP_MD_CTX_free(context);
    return crypto_error(error, VECTIS_ERR_STATE,
                        "initializing verification failed", 1);
  }
  result = EVP_DigestVerifyFinal(context, (const unsigned char *)signature,
                                 signature_size);
  EVP_MD_CTX_free(context);
  if (result < 0)
    return crypto_error(error, VECTIS_ERR_STATE, "verification failed", 1);
  *verified = result == 1;
  ERR_clear_error();
  return VECTIS_OK;
}
void vectis_crypto_gcm_result_cleanup(vectis_crypto_gcm_result *result) {
  if (result != NULL) {
    vectis_mutable_bytes_cleanup(&result->ciphertext);
    memset(result, 0, sizeof(*result));
  }
}
static const EVP_CIPHER *crypto_gcm_cipher(size_t size) {
  return size == 16u   ? EVP_aes_128_gcm()
         : size == 24u ? EVP_aes_192_gcm()
         : size == 32u ? EVP_aes_256_gcm()
                       : NULL;
}
static vectis_status crypto_gcm(int encrypt, const void *key, size_t key_size,
                                const void *nonce, size_t nonce_size,
                                const void *data, size_t size, const void *aad,
                                size_t aad_size, unsigned char *tag,
                                vectis_mutable_bytes *out,
                                vectis_error *error) {
  const EVP_CIPHER *cipher = crypto_gcm_cipher(key_size);
  EVP_CIPHER_CTX *context;
  unsigned char *output;
  size_t capacity;
  int written = 0, final = 0, ignored = 0, ok;
  vectis_status status = VECTIS_OK;
  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (out == NULL || cipher == NULL || key == NULL || nonce == NULL ||
      nonce_size != 12u || tag == NULL || (data == NULL && size != 0u) ||
      (aad == NULL && aad_size != 0u) || size > VECTIS_CRYPTO_MAX_BYTES ||
      aad_size > VECTIS_CRYPTO_MAX_BYTES)
    return crypto_error(error, VECTIS_ERR_INVALID,
                        "AES-GCM requires 16/24/32-byte key, 12-byte nonce and "
                        "<=16 MiB data/AAD",
                        0);
  ERR_clear_error();
  context = EVP_CIPHER_CTX_new();
  if (context == NULL)
    return crypto_error(error, VECTIS_ERR_NOMEM, "allocating AES-GCM context",
                        1);
  capacity = size + EVP_MAX_BLOCK_LENGTH;
  output = (unsigned char *)malloc(capacity);
  if (output == NULL) {
    EVP_CIPHER_CTX_free(context);
    return crypto_error(error, VECTIS_ERR_NOMEM, "allocating AES-GCM output",
                        0);
  }
  if (encrypt) {
    ok = EVP_EncryptInit_ex(context, cipher, NULL, (const unsigned char *)key,
                            (const unsigned char *)nonce) == 1;
    if (ok && aad_size != 0u)
      ok = EVP_EncryptUpdate(context, NULL, &ignored,
                             (const unsigned char *)aad, (int)aad_size) == 1;
    if (ok && size != 0u)
      ok = EVP_EncryptUpdate(context, output, &written,
                             (const unsigned char *)data, (int)size) == 1;
    if (ok)
      ok = EVP_EncryptFinal_ex(context, output + written, &final) == 1;
    if (ok)
      ok = EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
  } else {
    ok = EVP_DecryptInit_ex(context, cipher, NULL, (const unsigned char *)key,
                            (const unsigned char *)nonce) == 1;
    if (ok && aad_size != 0u)
      ok = EVP_DecryptUpdate(context, NULL, &ignored,
                             (const unsigned char *)aad, (int)aad_size) == 1;
    if (ok && size != 0u)
      ok = EVP_DecryptUpdate(context, output, &written,
                             (const unsigned char *)data, (int)size) == 1;
    if (ok)
      ok = EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, 16, tag) == 1;
    if (ok)
      ok = EVP_DecryptFinal_ex(context, output + written, &final) == 1;
  }
  if (!ok || written < 0 || final < 0 ||
      (size_t)written + (size_t) final != size) {
    status = crypto_error(error, VECTIS_ERR_STATE,
                          encrypt ? "AES-GCM encryption failed"
                                  : "AES-GCM authentication failed",
                          1);
    OPENSSL_cleanse(output, capacity);
    free(output);
  } else {
    out->data = output;
    out->size = size;
  }
  EVP_CIPHER_CTX_free(context);
  return status;
}
vectis_status vectis_crypto_gcm_encrypt_materialized(
    const void *key, size_t key_size, const void *nonce, size_t nonce_size,
    const void *data, size_t size, const void *aad, size_t aad_size,
    vectis_crypto_gcm_result *out, vectis_error *error) {
  vectis_status status;
  if (out == NULL)
    return crypto_error(error, VECTIS_ERR_INVALID, "missing AES-GCM result", 0);
  memset(out, 0, sizeof(*out));
  status = crypto_gcm(1, key, key_size, nonce, nonce_size, data, size, aad,
                      aad_size, out->tag, &out->ciphertext, error);
  if (status != VECTIS_OK)
    memset(out->tag, 0, sizeof(out->tag));
  return status;
}
vectis_status vectis_crypto_gcm_decrypt_materialized(
    const void *key, size_t key_size, const void *nonce, size_t nonce_size,
    const void *data, size_t size, const void *aad, size_t aad_size,
    const void *tag, size_t tag_size, vectis_mutable_bytes *out,
    vectis_error *error) {
  unsigned char tag_copy[16];
  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (tag == NULL || tag_size != 16u)
    return crypto_error(error, VECTIS_ERR_INVALID,
                        "AES-GCM tag must be 16 bytes", 0);
  memcpy(tag_copy, tag, sizeof(tag_copy));
  return crypto_gcm(0, key, key_size, nonce, nonce_size, data, size, aad,
                    aad_size, tag_copy, out, error);
}
