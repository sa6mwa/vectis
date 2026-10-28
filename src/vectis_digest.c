#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include <cpkt/openssl.h>
#include <vectis/digest.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct vectis_digest_impl {
  EVP_MD_CTX *context;
  EVP_MAC_CTX *mac_context;
  int state;
  vectis_digest_result result;
  vectis_error failure;
} vectis_digest_impl;

static vectis_status digest_error(vectis_error *error, vectis_status status,
                                  const char *message) {
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->code = status;
    error->source = VECTIS_ERROR_SOURCE_OPENSSL;
    strncpy(error->message, message, sizeof(error->message) - 1u);
  }
  return status;
}

static vectis_status digest_fail(vectis_digest_impl *impl, vectis_error *error,
                                 const char *message) {
  (void)digest_error(&impl->failure, VECTIS_ERR_STATE, message);
  impl->state = -1;
  if (error != NULL)
    *error = impl->failure;
  return VECTIS_ERR_STATE;
}

vectis_status vectis_digest_new(const char *algorithm, vectis_digest **out,
                                vectis_error *error) {
  const EVP_MD *method;
  vectis_digest *receiver;
  vectis_digest_impl *impl;
  int size;
  if (out != NULL)
    *out = NULL;
  if (out == NULL || algorithm == NULL || algorithm[0] == '\0')
    return digest_error(error, VECTIS_ERR_INVALID,
                        "digest algorithm is required");
  method = EVP_get_digestbyname(algorithm);
  size = method == NULL ? 0 : EVP_MD_get_size(method);
  if (size <= 0 || size > (int)VECTIS_DIGEST_MAX_BYTES ||
      (EVP_MD_get_flags(method) & EVP_MD_FLAG_XOF) != 0)
    return digest_error(error, VECTIS_ERR_INVALID,
                        "unsupported fixed-output digest algorithm");
  receiver = (vectis_digest *)calloc(1u, sizeof(*receiver));
  impl = (vectis_digest_impl *)calloc(1u, sizeof(*impl));
  if (receiver == NULL || impl == NULL) {
    free(receiver);
    free(impl);
    return digest_error(error, VECTIS_ERR_NOMEM, "digest allocation failed");
  }
  receiver->impl = impl;
  receiver->update = vectis_digest_update;
  receiver->final = vectis_digest_final;
  receiver->close = vectis_digest_close;
  impl->context = EVP_MD_CTX_new();
  if (impl->context == NULL) {
    vectis_digest_close(receiver);
    return digest_error(error, VECTIS_ERR_NOMEM,
                        "digest context allocation failed");
  }
  if (EVP_DigestInit_ex(impl->context, method, NULL) != 1) {
    vectis_digest_close(receiver);
    return digest_error(error, VECTIS_ERR_STATE,
                        "digest initialization failed");
  }
  *out = receiver;
  return VECTIS_OK;
}

vectis_status vectis_digest_update(vectis_digest *receiver, const void *data,
                                   size_t size, vectis_error *error) {
  vectis_digest_impl *impl;
  if (receiver == NULL || receiver->impl == NULL)
    return digest_error(error, VECTIS_ERR_INVALID,
                        "digest receiver is required");
  impl = (vectis_digest_impl *)receiver->impl;
  if (impl->state < 0) {
    if (error != NULL)
      *error = impl->failure;
    return impl->failure.code;
  }
  if (impl->state != 0)
    return digest_error(error, VECTIS_ERR_STATE, "digest is finalized");
  if (data == NULL && size != 0u)
    return digest_fail(impl, error, "digest input is NULL");
  if ((uint64_t)size > UINT64_MAX - impl->result.bytes)
    return digest_fail(impl, error, "digest byte count exceeds UINT64_MAX");
  if (size != 0u && (impl->mac_context != NULL
                         ? EVP_MAC_update(impl->mac_context,
                                          (const unsigned char *)data, size)
                         : EVP_DigestUpdate(impl->context, data, size)) != 1)
    return digest_fail(impl, error, "digest update failed");
  impl->result.bytes += (uint64_t)size;
  return VECTIS_OK;
}

vectis_status vectis_digest_final(vectis_digest *receiver,
                                  vectis_digest_result *out,
                                  vectis_error *error) {
  vectis_digest_impl *impl;
  unsigned int size;
  size_t mac_size;
  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (receiver == NULL || receiver->impl == NULL || out == NULL)
    return digest_error(error, VECTIS_ERR_INVALID,
                        "digest receiver and result are required");
  impl = (vectis_digest_impl *)receiver->impl;
  if (impl->state < 0) {
    if (error != NULL)
      *error = impl->failure;
    return impl->failure.code;
  }
  if (impl->state == 0) {
    size = 0u;
    if (impl->mac_context != NULL) {
      mac_size = 0u;
      if (EVP_MAC_final(impl->mac_context, impl->result.data, &mac_size,
                        sizeof(impl->result.data)) != 1 ||
          mac_size == 0u || mac_size > VECTIS_DIGEST_MAX_BYTES)
        return digest_fail(impl, error, "HMAC finalization failed");
      size = (unsigned int)mac_size;
    } else if (EVP_DigestFinal_ex(impl->context, impl->result.data, &size) !=
                   1 ||
               size == 0u || size > VECTIS_DIGEST_MAX_BYTES)
      return digest_fail(impl, error, "digest finalization failed");
    impl->result.size = size;
    impl->state = 1;
  }
  *out = impl->result;
  return VECTIS_OK;
}

void vectis_digest_close(vectis_digest *receiver) {
  vectis_digest_impl *impl;
  if (receiver == NULL)
    return;
  impl = (vectis_digest_impl *)receiver->impl;
  if (impl != NULL) {
    EVP_MD_CTX_free(impl->context);
    EVP_MAC_CTX_free(impl->mac_context);
    OPENSSL_cleanse(impl->result.data, sizeof(impl->result.data));
    free(impl);
  }
  free(receiver);
}

vectis_status vectis_digest_materialized(const char *algorithm,
                                         const void *data, size_t size,
                                         vectis_digest_result *out,
                                         vectis_error *error) {
  vectis_digest *receiver;
  vectis_status status;
  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (out == NULL)
    return digest_error(error, VECTIS_ERR_INVALID, "digest result is required");
  status = vectis_digest_new(algorithm, &receiver, error);
  if (status != VECTIS_OK)
    return status;
  status = receiver->update(receiver, data, size, error);
  if (status == VECTIS_OK)
    status = receiver->final(receiver, out, error);
  receiver->close(receiver);
  return status;
}

static vectis_status digest_read_all(vectis_digest *receiver,
                                     vectis_digest_reader reader,
                                     void *userdata, vectis_digest_result *out,
                                     vectis_error *error) {
  unsigned char buffer[VECTIS_DIGEST_CHUNK_BYTES];
  vectis_status status;
  size_t size;
  for (;;) {
    size = 0u;
    status = reader(userdata, buffer, sizeof(buffer), &size, error);
    if (status != VECTIS_OK)
      break;
    if (size > sizeof(buffer)) {
      status = digest_error(error, VECTIS_ERR_INVALID,
                            "digest reader exceeded capacity");
      break;
    }
    if (size == 0u) {
      status = receiver->final(receiver, out, error);
      break;
    }
    status = receiver->update(receiver, buffer, size, error);
    if (status != VECTIS_OK)
      break;
  }
  receiver->close(receiver);
  return status;
}

vectis_status vectis_digest_stream(const char *algorithm,
                                   vectis_digest_reader reader, void *userdata,
                                   vectis_digest_result *out,
                                   vectis_error *error) {
  vectis_digest *receiver;
  vectis_status status;
  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (reader == NULL || out == NULL)
    return digest_error(error, VECTIS_ERR_INVALID,
                        "digest reader and result are required");
  status = vectis_digest_new(algorithm, &receiver, error);
  return status == VECTIS_OK
             ? digest_read_all(receiver, reader, userdata, out, error)
             : status;
}

static vectis_status digest_file_read(void *userdata, unsigned char *buffer,
                                      size_t capacity, size_t *size,
                                      vectis_error *error) {
  FILE *file;
  file = (FILE *)userdata;
  *size = fread(buffer, 1u, capacity, file);
  if (ferror(file))
    return digest_error(error, VECTIS_ERR_STATE, "digest file read failed");
  return VECTIS_OK;
}

vectis_status vectis_digest_file(const char *algorithm, const char *path,
                                 vectis_digest_result *out,
                                 vectis_error *error) {
  FILE *file;
  vectis_status status;
  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (path == NULL || out == NULL)
    return digest_error(error, VECTIS_ERR_INVALID,
                        "digest file path and result are required");
  file = fopen(path, "rb");
  if (file == NULL)
    return digest_error(error, VECTIS_ERR_STATE, "digest file open failed");
  status = vectis_digest_stream(algorithm, digest_file_read, file, out, error);
  if (fclose(file) != 0 && status == VECTIS_OK)
    status = digest_error(error, VECTIS_ERR_STATE, "digest file close failed");
  if (status != VECTIS_OK)
    memset(out, 0, sizeof(*out));
  return status;
}

vectis_status vectis_hmac_new(const char *algorithm, const void *key,
                              size_t key_size, vectis_digest **out,
                              vectis_error *error) {
  static const unsigned char empty_key = 0u;
  vectis_digest *receiver;
  vectis_digest_impl *impl;
  vectis_status status;
  EVP_MAC *method;
  OSSL_PARAM params[2];
  char *name;
  size_t name_size;
  int initialized;
  if (out != NULL)
    *out = NULL;
  if (out == NULL || (key == NULL && key_size != 0u))
    return digest_error(error, VECTIS_ERR_INVALID,
                        "invalid HMAC key or output");
  /* Reuse the fixed-output algorithm validation and receiver ownership. */
  status = vectis_digest_new(algorithm, &receiver, error);
  if (status != VECTIS_OK)
    return status;
  impl = (vectis_digest_impl *)receiver->impl;
  EVP_MD_CTX_free(impl->context);
  impl->context = NULL;
  method = EVP_MAC_fetch(NULL, "HMAC", NULL);
  if (method == NULL) {
    vectis_digest_close(receiver);
    return digest_error(error, VECTIS_ERR_STATE, "HMAC provider unavailable");
  }
  impl->mac_context = EVP_MAC_CTX_new(method);
  EVP_MAC_free(method);
  name_size = strlen(algorithm) + 1u;
  name = (char *)malloc(name_size);
  if (name == NULL || impl->mac_context == NULL) {
    free(name);
    vectis_digest_close(receiver);
    return digest_error(error, VECTIS_ERR_NOMEM, "HMAC allocation failed");
  }
  memcpy(name, algorithm, name_size);
  params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, name, 0u);
  params[1] = OSSL_PARAM_construct_end();
  initialized = EVP_MAC_init(
      impl->mac_context, key == NULL ? &empty_key : (const unsigned char *)key,
      key_size, params);
  free(name);
  if (initialized != 1) {
    vectis_digest_close(receiver);
    return digest_error(error, VECTIS_ERR_STATE, "HMAC initialization failed");
  }
  *out = receiver;
  return VECTIS_OK;
}

vectis_status vectis_hmac_materialized(const char *algorithm, const void *key,
                                       size_t key_size, const void *data,
                                       size_t size, vectis_digest_result *out,
                                       vectis_error *error) {
  vectis_digest *receiver;
  vectis_status status;
  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (out == NULL)
    return digest_error(error, VECTIS_ERR_INVALID, "HMAC result is required");
  status = vectis_hmac_new(algorithm, key, key_size, &receiver, error);
  if (status != VECTIS_OK)
    return status;
  status = receiver->update(receiver, data, size, error);
  if (status == VECTIS_OK)
    status = receiver->final(receiver, out, error);
  receiver->close(receiver);
  return status;
}

vectis_status vectis_hmac_stream(const char *algorithm, const void *key,
                                 size_t key_size, vectis_digest_reader reader,
                                 void *userdata, vectis_digest_result *out,
                                 vectis_error *error) {
  vectis_digest *receiver;
  vectis_status status;
  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (reader == NULL || out == NULL)
    return digest_error(error, VECTIS_ERR_INVALID,
                        "HMAC reader and result are required");
  status = vectis_hmac_new(algorithm, key, key_size, &receiver, error);
  return status == VECTIS_OK
             ? digest_read_all(receiver, reader, userdata, out, error)
             : status;
}

vectis_status vectis_hmac_file(const char *algorithm, const void *key,
                               size_t key_size, const char *path,
                               vectis_digest_result *out, vectis_error *error) {
  FILE *file;
  vectis_status status;
  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (path == NULL || out == NULL)
    return digest_error(error, VECTIS_ERR_INVALID,
                        "HMAC file path and result are required");
  file = fopen(path, "rb");
  if (file == NULL)
    return digest_error(error, VECTIS_ERR_STATE, "HMAC file open failed");
  status = vectis_hmac_stream(algorithm, key, key_size, digest_file_read, file,
                              out, error);
  if (fclose(file) != 0 && status == VECTIS_OK)
    status = digest_error(error, VECTIS_ERR_STATE, "HMAC file close failed");
  if (status != VECTIS_OK)
    memset(out, 0, sizeof(*out));
  return status;
}
