#define LONEJSON_WITH_OPENSSL 1
#define LONEJSON_WITH_OIDC 1

#include "vectis_internal.h"

#include <vectis/auth.h>

#include <errno.h>
#include <fcntl.h>
#include <lonejson.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <vectis/totp_qr.h>

#define VECTIS_AUTH_PASSWORD_SALT_BYTES 16u
#define VECTIS_AUTH_PASSWORD_HASH_BYTES 32u
#define VECTIS_AUTH_PASSWORD_ITERATIONS 150000u
#define VECTIS_AUTH_RANDOM_PASSWORD_BYTES 24u
#define VECTIS_AUTH_RANDOM_TOTP_BYTES 20u

typedef struct vectis_auth_store_lock {
  int fd;
  char *path;
} vectis_auth_store_lock;

typedef struct vectis_auth_client_id_probe {
  const char *expected;
  char value[256];
  size_t size;
  int matched;
  int overflow;
} vectis_auth_client_id_probe;

typedef struct vectis_auth_revoke_state {
  lonejson *runtime;
  const char *client_id;
  int matched;
} vectis_auth_revoke_state;

typedef struct vectis_auth_claim_string_probe {
  const char *key;
  size_t key_len;
  char *out;
  size_t out_size;
  size_t size;
  int matched;
  int overflow;
} vectis_auth_claim_string_probe;

typedef struct vectis_auth_claim_uint_probe {
  const char *key;
  size_t key_len;
  char value[32];
  size_t size;
  unsigned int out;
  int matched;
  int overflow;
} vectis_auth_claim_uint_probe;

typedef struct vectis_auth_claim_bool_probe {
  const char *key;
  size_t key_len;
  int out;
  int matched;
} vectis_auth_claim_bool_probe;

typedef struct vectis_auth_user_record {
  char username[VECTIS_AUTH_PRINCIPAL_MAX + 1u];
  char password_salt[2u * VECTIS_AUTH_PASSWORD_SALT_BYTES + 1u];
  char password_hash[2u * VECTIS_AUTH_PASSWORD_HASH_BYTES + 1u];
  char password_kdf[64];
  char totp_secret[VECTIS_TOTP_SECRET_MAX];
  unsigned int password_iterations;
  int totp_enabled;
  int found;
} vectis_auth_user_record;

typedef struct vectis_auth_user_find_state {
  lonejson *runtime;
  const char *username;
  vectis_auth_user_record record;
} vectis_auth_user_find_state;

typedef struct vectis_auth_user_drop_state {
  lonejson *runtime;
  const char *username;
  int matched;
} vectis_auth_user_drop_state;

static char *vectis_auth_strdup(const char *value) {
  size_t len;
  char *copy;

  if (value == NULL) {
    return NULL;
  }
  len = strlen(value) + 1u;
  copy = (char *)malloc(len);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value, len);
  return copy;
}

static void vectis_auth_copy_fixed(char *out, size_t out_size,
                                   const char *value) {
  size_t len;

  if (out == NULL || out_size == 0u) {
    return;
  }
  out[0] = '\0';
  if (value == NULL) {
    return;
  }
  len = strlen(value);
  if (len >= out_size) {
    len = out_size - 1u;
  }
  memcpy(out, value, len);
  out[len] = '\0';
}

static void vectis_auth_set_errorf(vectis_error *error, vectis_status code,
                                   const char *format, ...) {
  char message[512];
  va_list args;

  if (error == NULL) {
    return;
  }
  va_start(args, format);
  (void)vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  vectis_set_error(error, code, message);
}

static vectis_status vectis_auth_set_errno(vectis_error *error,
                                           const char *message,
                                           const char *path) {
  if (path != NULL) {
    vectis_auth_set_errorf(error, VECTIS_ERR_STATE, "%s: %s", message, path);
  } else {
    vectis_set_error(error, VECTIS_ERR_STATE, message);
  }
  return VECTIS_ERR_STATE;
}

static vectis_status
vectis_auth_lonejson_error(vectis_error *error, lonejson_status status,
                           const lonejson_error *json_error,
                           const char *message) {
  const char *detail;

  detail = json_error != NULL && json_error->message[0] != '\0'
               ? json_error->message
               : lonejson_status_string(status);
  vectis_auth_set_errorf(error, VECTIS_ERR_INVALID, "%s: %s", message, detail);
  return VECTIS_ERR_INVALID;
}

static int vectis_auth_modes_to_lonejson(unsigned modes) {
  int out;

  out = 0;
  if (modes == VECTIS_AUTH_MODE_DEFAULT) {
    return LONEJSON_M2M_AUTH_DEFAULT;
  }
  if ((modes & VECTIS_AUTH_MODE_BASIC) != 0u) {
    out |= LONEJSON_M2M_AUTH_BASIC;
  }
  if ((modes & VECTIS_AUTH_MODE_BEARER) != 0u) {
    out |= LONEJSON_M2M_AUTH_BEARER;
  }
  return out;
}

static unsigned vectis_auth_mode_from_lonejson(unsigned mode) {
  if ((mode & LONEJSON_M2M_AUTH_BASIC) != 0u) {
    return VECTIS_AUTH_MODE_BASIC;
  }
  if ((mode & LONEJSON_M2M_AUTH_BEARER) != 0u) {
    return VECTIS_AUTH_MODE_BEARER;
  }
  return VECTIS_AUTH_MODE_DEFAULT;
}

static int vectis_auth_parent_dir(const char *path, char *out,
                                  size_t out_size) {
  const char *slash;
  size_t len;

  if (path == NULL || out == NULL || out_size == 0u) {
    return 0;
  }
  slash = strrchr(path, '/');
  if (slash == NULL) {
    if (out_size < 2u) {
      return 0;
    }
    out[0] = '.';
    out[1] = '\0';
    return 1;
  }
  len = slash == path ? 1u : (size_t)(slash - path);
  if (len + 1u > out_size) {
    return 0;
  }
  memcpy(out, path, len);
  out[len] = '\0';
  return 1;
}

static int vectis_auth_mkdir_p(const char *path) {
  char temp[4096];
  char *cursor;
  size_t len;

  if (path == NULL || path[0] == '\0') {
    return 0;
  }
  len = strlen(path);
  if (len >= sizeof(temp)) {
    return 0;
  }
  memcpy(temp, path, len + 1u);
  cursor = temp;
  if (cursor[0] == '/') {
    cursor++;
  }
  for (; *cursor != '\0'; ++cursor) {
    if (*cursor == '/') {
      *cursor = '\0';
      if (temp[0] != '\0' && mkdir(temp, 0700) != 0 && errno != EEXIST) {
        return 0;
      }
      *cursor = '/';
    }
  }
  return mkdir(temp, 0700) == 0 || errno == EEXIST;
}

static vectis_status vectis_auth_prepare_parent(const char *path,
                                                vectis_error *error) {
  char parent[4096];

  if (!vectis_auth_parent_dir(path, parent, sizeof(parent)) ||
      !vectis_auth_mkdir_p(parent)) {
    return vectis_auth_set_errno(error, "failed to create auth store parent",
                                 path);
  }
  return VECTIS_OK;
}

static vectis_status
vectis_auth_lock_open(const vectis_auth_store_config *config,
                      vectis_auth_store_lock *lock, vectis_error *error) {
  size_t path_len;

  if (config == NULL || config->credentials_path == NULL ||
      config->credentials_path[0] == '\0' || lock == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth credentials_path is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_auth_prepare_parent(config->credentials_path, error) !=
      VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  memset(lock, 0, sizeof(*lock));
  lock->fd = -1;
  path_len = strlen(config->credentials_path) + 6u;
  lock->path = (char *)malloc(path_len);
  if (lock->path == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate auth lock path");
    return VECTIS_ERR_NOMEM;
  }
  if ((size_t)snprintf(lock->path, path_len, "%s.lock",
                       config->credentials_path) >= path_len) {
    free(lock->path);
    lock->path = NULL;
    vectis_set_error(error, VECTIS_ERR_INVALID, "auth lock path is too long");
    return VECTIS_ERR_INVALID;
  }
  lock->fd = open(lock->path, O_CREAT | O_RDWR, 0600);
  if (lock->fd < 0 || flock(lock->fd, LOCK_EX) != 0) {
    if (lock->fd >= 0) {
      (void)close(lock->fd);
    }
    free(lock->path);
    lock->path = NULL;
    return vectis_auth_set_errno(error, "failed to lock auth store",
                                 config->credentials_path);
  }
  return VECTIS_OK;
}

static void vectis_auth_lock_close(vectis_auth_store_lock *lock) {
  if (lock == NULL) {
    return;
  }
  if (lock->fd >= 0) {
    (void)flock(lock->fd, LOCK_UN);
    (void)close(lock->fd);
  }
  free(lock->path);
  lock->fd = -1;
  lock->path = NULL;
}

static vectis_status
vectis_auth_read_store_locked(const vectis_auth_store_config *config,
                              char **out, size_t *out_size,
                              vectis_error *error) {
  struct stat st;
  FILE *fp;
  char *buffer;
  size_t nread;
  size_t max_store_bytes;

  if (out == NULL || out_size == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth store output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  *out_size = 0u;
  if (stat(config->credentials_path, &st) != 0) {
    if (errno == ENOENT) {
      return VECTIS_OK;
    }
    return vectis_auth_set_errno(error, "failed to stat auth store",
                                 config->credentials_path);
  }
  if (!S_ISREG(st.st_mode) || st.st_size < 0) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth store must be a regular file");
    return VECTIS_ERR_INVALID;
  }
  max_store_bytes = config->max_store_bytes != 0u
                        ? config->max_store_bytes
                        : VECTIS_AUTH_DEFAULT_MAX_STORE_BYTES;
  if ((size_t)st.st_size > max_store_bytes) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "auth store exceeds limit");
    return VECTIS_ERR_INVALID;
  }
  fp = fopen(config->credentials_path, "rb");
  if (fp == NULL) {
    return vectis_auth_set_errno(error, "failed to open auth store",
                                 config->credentials_path);
  }
  buffer = (char *)malloc((size_t)st.st_size + 1u);
  if (buffer == NULL) {
    (void)fclose(fp);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate auth store");
    return VECTIS_ERR_NOMEM;
  }
  nread = fread(buffer, 1u, (size_t)st.st_size, fp);
  if (nread != (size_t)st.st_size || fclose(fp) != 0) {
    free(buffer);
    return vectis_auth_set_errno(error, "failed to read auth store",
                                 config->credentials_path);
  }
  buffer[nread] = '\0';
  *out = buffer;
  *out_size = nread;
  return VECTIS_OK;
}

static vectis_status
vectis_auth_write_store_locked(const vectis_auth_store_config *config,
                               const char *data, size_t len,
                               vectis_error *error) {
  char temp_path[4096];
  int fd;
  int written;
  size_t offset;
  ssize_t nwrite;

  written = snprintf(temp_path, sizeof(temp_path), "%s.tmp.%ld",
                     config->credentials_path, (long)getpid());
  if (written < 0 || (size_t)written >= sizeof(temp_path)) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "auth temp path is too long");
    return VECTIS_ERR_INVALID;
  }
  fd = open(temp_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (fd < 0) {
    return vectis_auth_set_errno(error, "failed to create auth temp store",
                                 temp_path);
  }
  offset = 0u;
  while (offset < len) {
    nwrite = write(fd, data + offset, len - offset);
    if (nwrite <= 0) {
      (void)close(fd);
      (void)unlink(temp_path);
      return vectis_auth_set_errno(error, "failed to write auth temp store",
                                   temp_path);
    }
    offset += (size_t)nwrite;
  }
  if (fsync(fd) != 0 || close(fd) != 0) {
    (void)unlink(temp_path);
    return vectis_auth_set_errno(error, "failed to flush auth temp store",
                                 temp_path);
  }
  if (rename(temp_path, config->credentials_path) != 0) {
    (void)unlink(temp_path);
    return vectis_auth_set_errno(error, "failed to replace auth store",
                                 config->credentials_path);
  }
  return VECTIS_OK;
}

static int vectis_auth_path_is_client_id(const lonejson_value_path *path) {
  return path != NULL && path->segment_count == 1u &&
         path->segments[0].len == 9u &&
         memcmp(path->segments[0].data, "client_id", 9u) == 0;
}

static int vectis_auth_path_matches_key(const lonejson_value_path *path,
                                        const char *key, size_t key_len) {
  return path != NULL && key != NULL && path->segment_count == 1u &&
         path->segments[0].len == key_len &&
         memcmp(path->segments[0].data, key, key_len) == 0;
}

static lonejson_status
vectis_auth_claim_string_chunk(void *user, const lonejson_value_path *path,
                               const char *data, size_t len,
                               lonejson_error *error) {
  vectis_auth_claim_string_probe *probe;

  (void)error;
  probe = (vectis_auth_claim_string_probe *)user;
  if (probe == NULL || probe->matched || probe->overflow ||
      !vectis_auth_path_matches_key(path, probe->key, probe->key_len)) {
    return LONEJSON_STATUS_OK;
  }
  if (probe->size + len >= probe->out_size) {
    probe->overflow = 1;
    return LONEJSON_STATUS_OK;
  }
  memcpy(probe->out + probe->size, data, len);
  probe->size += len;
  probe->out[probe->size] = '\0';
  return LONEJSON_STATUS_OK;
}

static lonejson_status
vectis_auth_claim_string_end(void *user, const lonejson_value_path *path,
                             lonejson_error *error) {
  vectis_auth_claim_string_probe *probe;

  (void)error;
  probe = (vectis_auth_claim_string_probe *)user;
  if (probe != NULL &&
      vectis_auth_path_matches_key(path, probe->key, probe->key_len) &&
      !probe->overflow) {
    probe->matched = 1;
  }
  return LONEJSON_STATUS_OK;
}

static int vectis_auth_claim_string(lonejson *runtime, const char *claim_json,
                                    const char *key, char *out,
                                    size_t out_size) {
  vectis_auth_claim_string_probe probe;
  lonejson_path_value_visitor visitor;
  lonejson_error json_error;
  lonejson_status status;

  if (runtime == NULL || claim_json == NULL || key == NULL || out == NULL ||
      out_size == 0u) {
    return 0;
  }
  memset(&probe, 0, sizeof(probe));
  probe.key = key;
  probe.key_len = strlen(key);
  probe.out = out;
  probe.out_size = out_size;
  out[0] = '\0';
  visitor = lonejson_default_path_value_visitor();
  visitor.string_chunk = vectis_auth_claim_string_chunk;
  visitor.string_end = vectis_auth_claim_string_end;
  lonejson_error_init(&json_error);
  status = lonejson_visit_path_value_buffer(
      runtime, claim_json, strlen(claim_json), &visitor, &probe, &json_error);
  return status == LONEJSON_STATUS_OK && probe.matched && !probe.overflow;
}

static lonejson_status
vectis_auth_claim_uint_chunk(void *user, const lonejson_value_path *path,
                             const char *data, size_t len,
                             lonejson_error *error) {
  vectis_auth_claim_uint_probe *probe;

  (void)error;
  probe = (vectis_auth_claim_uint_probe *)user;
  if (probe == NULL || probe->matched || probe->overflow ||
      !vectis_auth_path_matches_key(path, probe->key, probe->key_len)) {
    return LONEJSON_STATUS_OK;
  }
  if (probe->size + len >= sizeof(probe->value)) {
    probe->overflow = 1;
    return LONEJSON_STATUS_OK;
  }
  memcpy(probe->value + probe->size, data, len);
  probe->size += len;
  probe->value[probe->size] = '\0';
  return LONEJSON_STATUS_OK;
}

static lonejson_status
vectis_auth_claim_uint_end(void *user, const lonejson_value_path *path,
                           lonejson_error *error) {
  vectis_auth_claim_uint_probe *probe;
  char *end;
  unsigned long value;

  (void)error;
  probe = (vectis_auth_claim_uint_probe *)user;
  if (probe == NULL ||
      !vectis_auth_path_matches_key(path, probe->key, probe->key_len) ||
      probe->overflow || probe->value[0] == '\0') {
    return LONEJSON_STATUS_OK;
  }
  errno = 0;
  end = NULL;
  value = strtoul(probe->value, &end, 10);
  if (errno == 0 && end != probe->value && end != NULL && *end == '\0' &&
      value <= 0xfffffffful) {
    probe->out = (unsigned int)value;
    probe->matched = 1;
  }
  return LONEJSON_STATUS_OK;
}

static int vectis_auth_claim_uint(lonejson *runtime, const char *claim_json,
                                  const char *key, unsigned int *out) {
  vectis_auth_claim_uint_probe probe;
  lonejson_path_value_visitor visitor;
  lonejson_error json_error;
  lonejson_status status;

  if (runtime == NULL || claim_json == NULL || key == NULL || out == NULL) {
    return 0;
  }
  memset(&probe, 0, sizeof(probe));
  probe.key = key;
  probe.key_len = strlen(key);
  visitor = lonejson_default_path_value_visitor();
  visitor.number_chunk = vectis_auth_claim_uint_chunk;
  visitor.number_end = vectis_auth_claim_uint_end;
  lonejson_error_init(&json_error);
  status = lonejson_visit_path_value_buffer(
      runtime, claim_json, strlen(claim_json), &visitor, &probe, &json_error);
  if (status == LONEJSON_STATUS_OK && probe.matched && !probe.overflow) {
    *out = probe.out;
    return 1;
  }
  return 0;
}

static lonejson_status
vectis_auth_claim_bool_value(void *user, const lonejson_value_path *path,
                             int value, lonejson_error *error) {
  vectis_auth_claim_bool_probe *probe;

  (void)error;
  probe = (vectis_auth_claim_bool_probe *)user;
  if (probe != NULL &&
      vectis_auth_path_matches_key(path, probe->key, probe->key_len)) {
    probe->out = value != 0;
    probe->matched = 1;
  }
  return LONEJSON_STATUS_OK;
}

static int vectis_auth_claim_bool(lonejson *runtime, const char *claim_json,
                                  const char *key, int *out) {
  vectis_auth_claim_bool_probe probe;
  lonejson_path_value_visitor visitor;
  lonejson_error json_error;
  lonejson_status status;

  if (runtime == NULL || claim_json == NULL || key == NULL || out == NULL) {
    return 0;
  }
  memset(&probe, 0, sizeof(probe));
  probe.key = key;
  probe.key_len = strlen(key);
  visitor = lonejson_default_path_value_visitor();
  visitor.boolean_value = vectis_auth_claim_bool_value;
  lonejson_error_init(&json_error);
  status = lonejson_visit_path_value_buffer(
      runtime, claim_json, strlen(claim_json), &visitor, &probe, &json_error);
  if (status == LONEJSON_STATUS_OK && probe.matched) {
    *out = probe.out;
    return 1;
  }
  return 0;
}

static int vectis_auth_user_record_from_json(lonejson *runtime,
                                             const char *json, size_t len,
                                             vectis_auth_user_record *out) {
  char *buffer;
  int ok;

  if (runtime == NULL || json == NULL || out == NULL) {
    return 0;
  }
  buffer = (char *)malloc(len + 1u);
  if (buffer == NULL) {
    return 0;
  }
  memcpy(buffer, json, len);
  buffer[len] = '\0';
  memset(out, 0, sizeof(*out));
  ok = vectis_auth_claim_string(runtime, buffer, "username", out->username,
                                sizeof(out->username)) &&
       vectis_auth_claim_string(runtime, buffer, "password_salt",
                                out->password_salt,
                                sizeof(out->password_salt)) &&
       vectis_auth_claim_string(runtime, buffer, "password_hash",
                                out->password_hash,
                                sizeof(out->password_hash)) &&
       vectis_auth_claim_string(runtime, buffer, "password_kdf",
                                out->password_kdf, sizeof(out->password_kdf)) &&
       vectis_auth_claim_uint(runtime, buffer, "password_iterations",
                              &out->password_iterations);
  if (ok) {
    out->totp_enabled = vectis_auth_claim_bool(runtime, buffer, "totp_enabled",
                                               &out->totp_enabled)
                            ? out->totp_enabled
                            : 0;
    if (out->totp_enabled) {
      ok = vectis_auth_claim_string(runtime, buffer, "totp_secret",
                                    out->totp_secret, sizeof(out->totp_secret));
    }
    out->found = ok;
  }
  free(buffer);
  return ok;
}

static lonejson_status
vectis_auth_probe_client_id_chunk(void *user, const lonejson_value_path *path,
                                  const char *data, size_t len,
                                  lonejson_error *error) {
  vectis_auth_client_id_probe *probe;

  (void)error;
  probe = (vectis_auth_client_id_probe *)user;
  if (!vectis_auth_path_is_client_id(path) || probe == NULL ||
      probe->overflow) {
    return LONEJSON_STATUS_OK;
  }
  if (probe->size + len >= sizeof(probe->value)) {
    probe->overflow = 1;
    return LONEJSON_STATUS_OK;
  }
  memcpy(probe->value + probe->size, data, len);
  probe->size += len;
  probe->value[probe->size] = '\0';
  return LONEJSON_STATUS_OK;
}

static lonejson_status
vectis_auth_probe_client_id_end(void *user, const lonejson_value_path *path,
                                lonejson_error *error) {
  vectis_auth_client_id_probe *probe;

  (void)error;
  probe = (vectis_auth_client_id_probe *)user;
  if (probe != NULL && vectis_auth_path_is_client_id(path) &&
      !probe->overflow && probe->expected != NULL &&
      strcmp(probe->value, probe->expected) == 0) {
    probe->matched = 1;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status vectis_auth_revoke_item(
    void *user, const lonejson_array_rewrite_context *context, void *item,
    lonejson_array_rewrite_result *result, lonejson_error *error) {
  vectis_auth_revoke_state *state;
  lonejson_json_value *value;
  lonejson_owned_buffer json;
  lonejson_path_value_visitor visitor;
  vectis_auth_client_id_probe probe;
  lonejson_status status;

  (void)context;
  state = (vectis_auth_revoke_state *)user;
  value = (lonejson_json_value *)item;
  memset(&probe, 0, sizeof(probe));
  probe.expected = state != NULL ? state->client_id : NULL;
  lonejson_owned_buffer_init(&json);
  status = value->methods->write_to_sink(value, lonejson_owned_buffer_sink,
                                         &json, error);
  if (status == LONEJSON_STATUS_OK) {
    visitor = lonejson_default_path_value_visitor();
    visitor.string_chunk = vectis_auth_probe_client_id_chunk;
    visitor.string_end = vectis_auth_probe_client_id_end;
    status = lonejson_visit_path_value_buffer(
        state->runtime, json.data, json.len, &visitor, &probe, error);
  }
  if (status == LONEJSON_STATUS_OK && probe.matched) {
    result->action = LONEJSON_ARRAY_REWRITE_DROP;
    state->matched = 1;
  }
  lonejson_owned_buffer_free(&json);
  return status;
}

static lonejson_status vectis_auth_user_find_item(
    void *user, const lonejson_array_rewrite_context *context, void *item,
    lonejson_array_rewrite_result *result, lonejson_error *error) {
  vectis_auth_user_find_state *state;
  lonejson_json_value *value;
  lonejson_owned_buffer json;
  vectis_auth_user_record record;
  lonejson_status status;

  (void)context;
  (void)result;
  state = (vectis_auth_user_find_state *)user;
  value = (lonejson_json_value *)item;
  if (state == NULL || state->record.found || state->username == NULL) {
    return LONEJSON_STATUS_OK;
  }
  lonejson_owned_buffer_init(&json);
  status = value->methods->write_to_sink(value, lonejson_owned_buffer_sink,
                                         &json, error);
  if (status == LONEJSON_STATUS_OK &&
      vectis_auth_user_record_from_json(state->runtime, json.data, json.len,
                                        &record) &&
      strcmp(record.username, state->username) == 0) {
    state->record = record;
    state->record.found = 1;
  }
  lonejson_owned_buffer_free(&json);
  return status;
}

static lonejson_status vectis_auth_user_drop_item(
    void *user, const lonejson_array_rewrite_context *context, void *item,
    lonejson_array_rewrite_result *result, lonejson_error *error) {
  vectis_auth_user_drop_state *state;
  lonejson_json_value *value;
  lonejson_owned_buffer json;
  vectis_auth_user_record record;
  lonejson_status status;

  (void)context;
  state = (vectis_auth_user_drop_state *)user;
  value = (lonejson_json_value *)item;
  if (state == NULL || state->username == NULL) {
    return LONEJSON_STATUS_OK;
  }
  lonejson_owned_buffer_init(&json);
  status = value->methods->write_to_sink(value, lonejson_owned_buffer_sink,
                                         &json, error);
  if (status == LONEJSON_STATUS_OK &&
      vectis_auth_user_record_from_json(state->runtime, json.data, json.len,
                                        &record) &&
      strcmp(record.username, state->username) == 0) {
    result->action = LONEJSON_ARRAY_REWRITE_DROP;
    state->matched = 1;
  }
  lonejson_owned_buffer_free(&json);
  return status;
}

static vectis_status vectis_auth_lonejson_runtime(lonejson **out,
                                                  vectis_error *error) {
  lonejson_auth_provider provider;
  lonejson_error json_error;
  lonejson_status json_status;
  lonejson *runtime;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "lonejson output is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  lonejson_error_init(&json_error);
  runtime = lonejson_new(NULL, &json_error);
  if (runtime == NULL) {
    return vectis_auth_lonejson_error(error, LONEJSON_STATUS_ALLOCATION_FAILED,
                                      &json_error,
                                      "failed to allocate lonejson runtime");
  }
  json_status =
      lonejson_auth_provider_init_openssl(&provider, NULL, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    json_status = lonejson_set_auth_provider(runtime, &provider, &json_error);
  }
  if (json_status != LONEJSON_STATUS_OK) {
    lonejson_free(runtime);
    return vectis_auth_lonejson_error(error, json_status, &json_error,
                                      "failed to initialize auth provider");
  }
  *out = runtime;
  return VECTIS_OK;
}

static vectis_status
vectis_auth_write_empty_store_locked(const vectis_auth_store_config *config,
                                     vectis_error *error) {
  static const char empty_store[] =
      "{\"credentials\":[],\"signups\":[],\"users\":[]}\n";

  return vectis_auth_write_store_locked(config, empty_store,
                                        sizeof(empty_store) - 1u, error);
}

static vectis_status vectis_auth_writer_copy_store_arrays(
    lonejson_writer *writer, const char *store_json, size_t store_len,
    const char *extra_record_json, size_t extra_record_len,
    const char *extra_user_json, size_t extra_user_len,
    lonejson_error *json_error) {
  lonejson_status status;

  status = lonejson_writer_begin_object(writer, json_error);
  if (status != LONEJSON_STATUS_OK) {
    return VECTIS_ERR_INVALID;
  }
  status = lonejson_writer_key(writer, "credentials", 11u, json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_array(writer, json_error);
  }
  if (status == LONEJSON_STATUS_OK && store_json != NULL && store_len > 0u) {
    status = lonejson_writer_array_items_buffer(
        writer, "credentials", store_json, store_len, json_error);
  }
  if (status == LONEJSON_STATUS_OK && extra_record_json != NULL) {
    status = lonejson_writer_json_value_buffer(writer, extra_record_json,
                                               extra_record_len, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_array(writer, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(writer, "signups", 7u, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_array(writer, json_error);
  }
  if (status == LONEJSON_STATUS_OK && store_json != NULL && store_len > 0u) {
    status = lonejson_writer_array_items_buffer(writer, "signups", store_json,
                                                store_len, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_array(writer, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(writer, "users", 5u, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_array(writer, json_error);
  }
  if (status == LONEJSON_STATUS_OK && store_json != NULL && store_len > 0u) {
    status = lonejson_writer_array_items_buffer(writer, "users", store_json,
                                                store_len, json_error);
  }
  if (status == LONEJSON_STATUS_OK && extra_user_json != NULL) {
    status = lonejson_writer_json_value_buffer(writer, extra_user_json,
                                               extra_user_len, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_array(writer, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(writer, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(writer, json_error);
  }
  return status == LONEJSON_STATUS_OK ? VECTIS_OK : VECTIS_ERR_INVALID;
}

static vectis_status
vectis_auth_append_record_locked(const vectis_auth_store_config *config,
                                 const char *record_json, size_t record_len,
                                 vectis_error *error) {
  lonejson *runtime;
  lonejson_writer writer;
  lonejson_owned_buffer out;
  lonejson_error json_error;
  lonejson_status json_status;
  vectis_status status;
  char *store_json;
  size_t store_len;

  store_json = NULL;
  store_len = 0u;
  status =
      vectis_auth_read_store_locked(config, &store_json, &store_len, error);
  if (status != VECTIS_OK) {
    return status;
  }
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status != VECTIS_OK) {
    free(store_json);
    return status;
  }
  lonejson_owned_buffer_init(&out);
  lonejson_error_init(&json_error);
  json_status = lonejson_writer_init_sink(
      runtime, &writer, lonejson_owned_buffer_sink, &out, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    status = vectis_auth_writer_copy_store_arrays(
        &writer, store_json, store_len, record_json, record_len, NULL, 0u,
        &json_error);
    lonejson_writer_cleanup(&writer);
  } else {
    status = vectis_auth_lonejson_error(error, json_status, &json_error,
                                        "failed to initialize auth writer");
  }
  if (status == VECTIS_OK) {
    status = vectis_auth_write_store_locked(config, out.data, out.len, error);
  } else if (error != NULL && error->code == VECTIS_OK) {
    (void)vectis_auth_lonejson_error(error, LONEJSON_STATUS_INVALID_JSON,
                                     &json_error,
                                     "failed to rewrite auth store");
  }
  lonejson_owned_buffer_free(&out);
  lonejson_free(runtime);
  free(store_json);
  return status;
}

void vectis_auth_store_config_init(vectis_auth_store_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->max_store_bytes = VECTIS_AUTH_DEFAULT_MAX_STORE_BYTES;
}

void vectis_auth_issue_config_init(vectis_auth_issue_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->purpose = "api";
  config->auth_modes = VECTIS_AUTH_MODE_BEARER;
}

void vectis_auth_issued_credential_init(
    vectis_auth_issued_credential *credential) {
  if (credential == NULL) {
    return;
  }
  memset(credential, 0, sizeof(*credential));
}

void vectis_auth_issued_credential_cleanup(
    vectis_auth_issued_credential *credential) {
  if (credential == NULL) {
    return;
  }
  free(credential->client_id);
  free(credential->client_secret);
  free(credential->api_key);
  free(credential->claim_json);
  vectis_auth_issued_credential_init(credential);
}

void vectis_auth_result_init(vectis_auth_result *result) {
  if (result == NULL) {
    return;
  }
  memset(result, 0, sizeof(*result));
}

void vectis_auth_result_cleanup(vectis_auth_result *result) {
  if (result == NULL) {
    return;
  }
  free(result->client_id);
  free(result->claim_json);
  vectis_auth_result_init(result);
}

void vectis_auth_provider_request_init(vectis_auth_provider_request *request) {
  if (request == NULL) {
    return;
  }
  memset(request, 0, sizeof(*request));
  request->allowed_auth_modes = VECTIS_AUTH_MODE_DEFAULT;
}

void vectis_auth_provider_response_init(
    vectis_auth_provider_response *response) {
  if (response == NULL) {
    return;
  }
  memset(response, 0, sizeof(*response));
  response->action = VECTIS_AUTH_DENY;
  vectis_auth_result_init(&response->result);
}

void vectis_auth_provider_response_cleanup(
    vectis_auth_provider_response *response) {
  if (response == NULL) {
    return;
  }
  vectis_auth_result_cleanup(&response->result);
  vectis_auth_provider_response_init(response);
}

void vectis_auth_provider_init(vectis_auth_provider *provider) {
  if (provider == NULL) {
    return;
  }
  memset(provider, 0, sizeof(*provider));
}

void vectis_auth_native_provider_config_init(
    vectis_auth_native_provider_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  vectis_auth_store_config_init(&config->store);
  config->realm = "vectis";
  config->allowed_auth_modes = VECTIS_AUTH_MODE_DEFAULT;
}

void vectis_auth_user_config_init(vectis_auth_user_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->totp_issuer = "Vectis";
}

void vectis_auth_user_enrollment_init(vectis_auth_user_enrollment *enrollment) {
  if (enrollment == NULL) {
    return;
  }
  memset(enrollment, 0, sizeof(*enrollment));
}

void vectis_auth_user_enrollment_cleanup(
    vectis_auth_user_enrollment *enrollment) {
  if (enrollment == NULL) {
    return;
  }
  free(enrollment->username);
  free(enrollment->generated_password);
  free(enrollment->totp_secret);
  free(enrollment->totp_uri);
  free(enrollment->totp_qr_ansi);
  vectis_auth_user_enrollment_init(enrollment);
}

void vectis_auth_login_config_init(vectis_auth_login_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->totp_window = 1u;
}

static void vectis_auth_hex_encode(const unsigned char *data, size_t len,
                                   char *out) {
  static const char digits[] = "0123456789abcdef";
  size_t i;

  for (i = 0u; i < len; ++i) {
    out[i * 2u] = digits[data[i] >> 4u];
    out[i * 2u + 1u] = digits[data[i] & 15u];
  }
  out[len * 2u] = '\0';
}

static int vectis_auth_hex_value(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

static int vectis_auth_hex_decode(const char *text, unsigned char *out,
                                  size_t out_len) {
  size_t i;
  int hi;
  int lo;

  if (text == NULL || strlen(text) != out_len * 2u) {
    return 0;
  }
  for (i = 0u; i < out_len; ++i) {
    hi = vectis_auth_hex_value(text[i * 2u]);
    lo = vectis_auth_hex_value(text[i * 2u + 1u]);
    if (hi < 0 || lo < 0) {
      return 0;
    }
    out[i] = (unsigned char)((hi << 4) | lo);
  }
  return 1;
}

static vectis_status vectis_auth_random_bytes(unsigned char *out, size_t len,
                                              vectis_error *error) {
  if (out == NULL || len == 0u || RAND_bytes(out, (int)len) != 1) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to generate auth random bytes");
    return VECTIS_ERR_STATE;
  }
  return VECTIS_OK;
}

static vectis_status vectis_auth_generate_password(char *out, size_t out_size,
                                                   vectis_error *error) {
  unsigned char bytes[VECTIS_AUTH_RANDOM_PASSWORD_BYTES];

  if (out == NULL || out_size < VECTIS_AUTH_RANDOM_PASSWORD_BYTES * 2u + 1u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "generated password buffer is too small");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_auth_random_bytes(bytes, sizeof(bytes), error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  vectis_auth_hex_encode(bytes, sizeof(bytes), out);
  OPENSSL_cleanse(bytes, sizeof(bytes));
  return VECTIS_OK;
}

static vectis_status vectis_auth_generate_totp_secret(char *out,
                                                      size_t out_size,
                                                      vectis_error *error) {
  static const char base32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  unsigned char bytes[VECTIS_AUTH_RANDOM_TOTP_BYTES];
  unsigned int buffer;
  unsigned int bits;
  size_t i;
  size_t n;

  if (out == NULL || out_size < 33u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "TOTP secret buffer is too small");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_auth_random_bytes(bytes, sizeof(bytes), error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  buffer = 0u;
  bits = 0u;
  n = 0u;
  for (i = 0u; i < sizeof(bytes); ++i) {
    buffer = (buffer << 8u) | bytes[i];
    bits += 8u;
    while (bits >= 5u) {
      out[n++] = base32[(buffer >> (bits - 5u)) & 31u];
      bits -= 5u;
    }
  }
  if (bits > 0u) {
    out[n++] = base32[(buffer << (5u - bits)) & 31u];
  }
  out[n] = '\0';
  OPENSSL_cleanse(bytes, sizeof(bytes));
  return VECTIS_OK;
}

static vectis_status vectis_auth_hash_password(
    const char *password,
    char salt_hex[2u * VECTIS_AUTH_PASSWORD_SALT_BYTES + 1u],
    char hash_hex[2u * VECTIS_AUTH_PASSWORD_HASH_BYTES + 1u],
    vectis_error *error) {
  unsigned char salt[VECTIS_AUTH_PASSWORD_SALT_BYTES];
  unsigned char hash[VECTIS_AUTH_PASSWORD_HASH_BYTES];
  int ok;

  if (password == NULL || password[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "password is required");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_auth_random_bytes(salt, sizeof(salt), error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  ok = PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt,
                         (int)sizeof(salt), VECTIS_AUTH_PASSWORD_ITERATIONS,
                         EVP_sha256(), (int)sizeof(hash), hash);
  if (ok != 1) {
    OPENSSL_cleanse(salt, sizeof(salt));
    OPENSSL_cleanse(hash, sizeof(hash));
    vectis_set_error(error, VECTIS_ERR_STATE, "password hashing failed");
    return VECTIS_ERR_STATE;
  }
  vectis_auth_hex_encode(salt, sizeof(salt), salt_hex);
  vectis_auth_hex_encode(hash, sizeof(hash), hash_hex);
  OPENSSL_cleanse(salt, sizeof(salt));
  OPENSSL_cleanse(hash, sizeof(hash));
  return VECTIS_OK;
}

static int vectis_auth_verify_password(const vectis_auth_user_record *record,
                                       const char *password) {
  unsigned char salt[VECTIS_AUTH_PASSWORD_SALT_BYTES];
  unsigned char expected[VECTIS_AUTH_PASSWORD_HASH_BYTES];
  unsigned char actual[VECTIS_AUTH_PASSWORD_HASH_BYTES];
  int ok;

  if (record == NULL || password == NULL ||
      strcmp(record->password_kdf, "pbkdf2-sha256") != 0 ||
      record->password_iterations == 0u ||
      !vectis_auth_hex_decode(record->password_salt, salt, sizeof(salt)) ||
      !vectis_auth_hex_decode(record->password_hash, expected,
                              sizeof(expected))) {
    return 0;
  }
  ok = PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt,
                         (int)sizeof(salt), record->password_iterations,
                         EVP_sha256(), (int)sizeof(actual), actual);
  OPENSSL_cleanse(salt, sizeof(salt));
  if (ok != 1) {
    OPENSSL_cleanse(expected, sizeof(expected));
    OPENSSL_cleanse(actual, sizeof(actual));
    return 0;
  }
  ok = CRYPTO_memcmp(expected, actual, sizeof(expected)) == 0;
  OPENSSL_cleanse(expected, sizeof(expected));
  OPENSSL_cleanse(actual, sizeof(actual));
  return ok;
}

vectis_status vectis_auth_store_init(const vectis_auth_store_config *config,
                                     vectis_error *error) {
  vectis_auth_store_lock lock;
  vectis_status status;
  char *store_json;
  size_t store_len;

  status = vectis_auth_lock_open(config, &lock, error);
  if (status != VECTIS_OK) {
    return status;
  }
  store_json = NULL;
  store_len = 0u;
  status =
      vectis_auth_read_store_locked(config, &store_json, &store_len, error);
  if (status == VECTIS_OK && store_json == NULL) {
    status = vectis_auth_write_empty_store_locked(config, error);
  }
  free(store_json);
  vectis_auth_lock_close(&lock);
  return status;
}

static vectis_status
vectis_auth_build_claim(lonejson *runtime, const vectis_auth_issue_config *cfg,
                        lonejson_owned_buffer *out, vectis_error *error) {
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;

  lonejson_error_init(&json_error);
  status = lonejson_writer_init_sink(
      runtime, &writer, lonejson_owned_buffer_sink, out, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "sub", 3u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, cfg->subject, strlen(cfg->subject),
                                    &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "purpose", 7u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, cfg->purpose, strlen(cfg->purpose),
                                    &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(&writer, &json_error);
  }
  lonejson_writer_cleanup(&writer);
  if (status != LONEJSON_STATUS_OK) {
    return vectis_auth_lonejson_error(error, status, &json_error,
                                      "failed to build auth claim");
  }
  return VECTIS_OK;
}

vectis_status
vectis_auth_issue_credential(const vectis_auth_store_config *store_config,
                             const vectis_auth_issue_config *issue_config,
                             vectis_auth_issued_credential *out,
                             vectis_error *error) {
  lonejson *runtime;
  lonejson_m2m_credential credential;
  lonejson_m2m_credential_request request;
  lonejson_owned_buffer claim;
  lonejson_error json_error;
  lonejson_status json_status;
  vectis_auth_store_lock lock;
  vectis_status status;

  if (issue_config == NULL || out == NULL || issue_config->subject == NULL ||
      issue_config->subject[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth credential subject is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_issued_credential_init(out);
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status != VECTIS_OK) {
    return status;
  }
  lonejson_owned_buffer_init(&claim);
  status = vectis_auth_build_claim(runtime, issue_config, &claim, error);
  if (status != VECTIS_OK) {
    lonejson_free(runtime);
    return status;
  }
  memset(&request, 0, sizeof(request));
  request.claim_json = claim.data;
  request.claim_len = claim.len;
  request.auth_modes =
      (unsigned)vectis_auth_modes_to_lonejson(issue_config->auth_modes);
  request.max_record_bytes = issue_config->max_record_bytes;
  lonejson_m2m_credential_init(&credential);
  lonejson_error_init(&json_error);
  json_status = lonejson_m2m_credential_generate(runtime, &request, &credential,
                                                 &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    status = vectis_auth_lock_open(store_config, &lock, error);
  } else {
    status = vectis_auth_lonejson_error(error, json_status, &json_error,
                                        "failed to generate credential");
  }
  if (status == VECTIS_OK) {
    status = vectis_auth_append_record_locked(
        store_config, credential.record_json.data, credential.record_json.len,
        error);
    vectis_auth_lock_close(&lock);
  }
  if (status == VECTIS_OK) {
    out->client_id = vectis_auth_strdup(credential.client_id);
    out->client_secret = vectis_auth_strdup(credential.client_secret);
    out->api_key = vectis_auth_strdup(credential.api_key);
    out->claim_json = vectis_auth_strdup(claim.data);
    if (out->client_id == NULL ||
        (credential.client_secret != NULL && out->client_secret == NULL) ||
        (credential.api_key != NULL && out->api_key == NULL) ||
        out->claim_json == NULL) {
      vectis_auth_issued_credential_cleanup(out);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy issued credential");
      status = VECTIS_ERR_NOMEM;
    }
  }
  lonejson_m2m_credential_cleanup(&credential);
  lonejson_owned_buffer_free(&claim);
  lonejson_free(runtime);
  return status;
}

static vectis_status vectis_auth_build_user_record_json(
    lonejson *runtime, const char *username, const char *salt_hex,
    const char *hash_hex, int totp_enabled, const char *totp_secret,
    lonejson_owned_buffer *out, vectis_error *error) {
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;

  lonejson_error_init(&json_error);
  status = lonejson_writer_init_sink(
      runtime, &writer, lonejson_owned_buffer_sink, out, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "username", 8u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, username, strlen(username),
                                    &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "password_kdf", 12u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, "pbkdf2-sha256", 13u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status =
        lonejson_writer_key(&writer, "password_iterations", 19u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_u64(&writer, VECTIS_AUTH_PASSWORD_ITERATIONS,
                                 &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "password_salt", 13u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, salt_hex, strlen(salt_hex),
                                    &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "password_hash", 13u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, hash_hex, strlen(hash_hex),
                                    &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "totp_enabled", 12u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_bool(&writer, totp_enabled, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && totp_enabled) {
    status = lonejson_writer_key(&writer, "totp_secret", 11u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && totp_enabled) {
    status = lonejson_writer_string(&writer, totp_secret, strlen(totp_secret),
                                    &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(&writer, &json_error);
  }
  lonejson_writer_cleanup(&writer);
  if (status != LONEJSON_STATUS_OK) {
    return vectis_auth_lonejson_error(error, status, &json_error,
                                      "failed to build auth user record");
  }
  return VECTIS_OK;
}

static vectis_status vectis_auth_write_store_with_user_locked(
    const vectis_auth_store_config *config, const char *store_json,
    size_t store_len, const char *user_json, size_t user_len,
    vectis_error *error) {
  lonejson *runtime;
  lonejson_writer writer;
  lonejson_owned_buffer out;
  lonejson_error json_error;
  lonejson_status json_status;
  vectis_status status;

  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status != VECTIS_OK) {
    return status;
  }
  lonejson_owned_buffer_init(&out);
  lonejson_error_init(&json_error);
  json_status = lonejson_writer_init_sink(
      runtime, &writer, lonejson_owned_buffer_sink, &out, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    status = vectis_auth_writer_copy_store_arrays(
        &writer, store_json, store_len, NULL, 0u, user_json, user_len,
        &json_error);
    lonejson_writer_cleanup(&writer);
  } else {
    status = vectis_auth_lonejson_error(error, json_status, &json_error,
                                        "failed to initialize auth writer");
  }
  if (status == VECTIS_OK) {
    status = vectis_auth_write_store_locked(config, out.data, out.len, error);
  } else if (error != NULL && error->code == VECTIS_OK) {
    (void)vectis_auth_lonejson_error(error, LONEJSON_STATUS_INVALID_JSON,
                                     &json_error,
                                     "failed to rewrite auth store");
  }
  lonejson_owned_buffer_free(&out);
  lonejson_free(runtime);
  return status;
}

static vectis_status vectis_auth_drop_user_to_temp_locked(
    const vectis_auth_store_config *store_config, lonejson *runtime,
    const char *username, char *temp_path, size_t temp_path_size,
    vectis_error *error) {
  lonejson_json_value item_value;
  lonejson_array_rewrite_options options;
  vectis_auth_user_drop_state state;
  lonejson_error json_error;
  lonejson_status json_status;
  int written;

  written = snprintf(temp_path, temp_path_size, "%s.users.%ld",
                     store_config->credentials_path, (long)getpid());
  if (written < 0 || (size_t)written >= temp_path_size) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth users temp path is too long");
    return VECTIS_ERR_INVALID;
  }
  memset(&state, 0, sizeof(state));
  state.runtime = runtime;
  state.username = username;
  memset(&options, 0, sizeof(options));
  lonejson_json_value_init(runtime, &item_value);
  lonejson_error_init(&json_error);
  json_status =
      lonejson_json_value_enable_parse_capture(&item_value, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    options.item_value = &item_value;
    options.item = vectis_auth_user_drop_item;
    options.user = &state;
    json_status = lonejson_array_rewrite_path(runtime, "users",
                                              store_config->credentials_path,
                                              temp_path, &options, &json_error);
  }
  lonejson_json_value_cleanup(&item_value);
  if (json_status != LONEJSON_STATUS_OK) {
    (void)unlink(temp_path);
    return vectis_auth_lonejson_error(error, json_status, &json_error,
                                      "failed to rewrite auth users");
  }
  return VECTIS_OK;
}

static vectis_status vectis_auth_find_user_locked(
    const vectis_auth_store_config *store_config, lonejson *runtime,
    const char *username, vectis_auth_user_record *out, vectis_error *error) {
  lonejson_json_value item_value;
  lonejson_array_rewrite_options options;
  vectis_auth_user_find_state state;
  lonejson_error json_error;
  lonejson_status json_status;
  char temp_path[4096];
  int written;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "auth user output is required");
    return VECTIS_ERR_INVALID;
  }
  memset(out, 0, sizeof(*out));
  written = snprintf(temp_path, sizeof(temp_path), "%s.find.%ld",
                     store_config->credentials_path, (long)getpid());
  if (written < 0 || (size_t)written >= sizeof(temp_path)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth user lookup temp path is too long");
    return VECTIS_ERR_INVALID;
  }
  memset(&state, 0, sizeof(state));
  state.runtime = runtime;
  state.username = username;
  memset(&options, 0, sizeof(options));
  lonejson_json_value_init(runtime, &item_value);
  lonejson_error_init(&json_error);
  json_status =
      lonejson_json_value_enable_parse_capture(&item_value, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    options.item_value = &item_value;
    options.item = vectis_auth_user_find_item;
    options.user = &state;
    json_status = lonejson_array_rewrite_path(runtime, "users",
                                              store_config->credentials_path,
                                              temp_path, &options, &json_error);
  }
  lonejson_json_value_cleanup(&item_value);
  (void)unlink(temp_path);
  if (json_status != LONEJSON_STATUS_OK) {
    return vectis_auth_lonejson_error(error, json_status, &json_error,
                                      "failed to read auth users");
  }
  *out = state.record;
  return VECTIS_OK;
}

static vectis_status vectis_auth_build_login_claim(lonejson *runtime,
                                                   const char *username,
                                                   lonejson_owned_buffer *out,
                                                   vectis_error *error) {
  vectis_auth_issue_config issue;

  vectis_auth_issue_config_init(&issue);
  issue.subject = username;
  issue.purpose = "login";
  return vectis_auth_build_claim(runtime, &issue, out, error);
}

vectis_status
vectis_auth_user_add_or_update(const vectis_auth_store_config *store_config,
                               const vectis_auth_user_config *user_config,
                               vectis_auth_user_enrollment *out,
                               vectis_error *error) {
  vectis_auth_store_lock lock;
  vectis_auth_store_config temp_config;
  lonejson *runtime;
  lonejson_owned_buffer user_json;
  vectis_totp totp;
  vectis_qr qr;
  vectis_status status;
  vectis_totp_qr_status totp_status;
  char password[VECTIS_AUTH_GENERATED_PASSWORD_MAX + 1u];
  char salt_hex[2u * VECTIS_AUTH_PASSWORD_SALT_BYTES + 1u];
  char hash_hex[2u * VECTIS_AUTH_PASSWORD_HASH_BYTES + 1u];
  char secret[VECTIS_TOTP_SECRET_MAX];
  char temp_path[4096];
  char *store_json;
  char *totp_uri;
  char *totp_qr_ansi;
  size_t store_len;
  size_t totp_qr_len;
  const char *password_value;
  const char *secret_value;
  const char *label;
  const char *issuer;

  if (user_config == NULL || user_config->username == NULL ||
      user_config->username[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "auth username is required");
    return VECTIS_ERR_INVALID;
  }
  if (out != NULL) {
    vectis_auth_user_enrollment_init(out);
  }
  lock.fd = -1;
  lock.path = NULL;
  password[0] = '\0';
  secret[0] = '\0';
  password_value = user_config->password;
  if (password_value == NULL || password_value[0] == '\0') {
    status = vectis_auth_generate_password(password, sizeof(password), error);
    if (status != VECTIS_OK) {
      return status;
    }
    password_value = password;
  }
  status = vectis_auth_hash_password(password_value, salt_hex, hash_hex, error);
  if (status != VECTIS_OK) {
    OPENSSL_cleanse(password, sizeof(password));
    return status;
  }
  secret_value = "";
  totp_uri = NULL;
  totp_qr_ansi = NULL;
  totp_qr_len = 0u;
  label = user_config->totp_label != NULL ? user_config->totp_label
                                          : user_config->username;
  issuer =
      user_config->totp_issuer != NULL ? user_config->totp_issuer : "Vectis";
  if (user_config->enable_totp) {
    if (user_config->totp_secret != NULL &&
        user_config->totp_secret[0] != '\0') {
      vectis_auth_copy_fixed(secret, sizeof(secret), user_config->totp_secret);
    } else {
      status = vectis_auth_generate_totp_secret(secret, sizeof(secret), error);
      if (status != VECTIS_OK) {
        OPENSSL_cleanse(password, sizeof(password));
        OPENSSL_cleanse(salt_hex, sizeof(salt_hex));
        OPENSSL_cleanse(hash_hex, sizeof(hash_hex));
        return status;
      }
    }
    totp_status = vectis_totp_init(&totp, secret);
    if (totp_status == VECTIS_TOTP_QR_OK) {
      secret_value = totp.secret;
      totp_status = vectis_totp_uri(&totp, label, issuer, &totp_uri);
    }
    if (totp_status == VECTIS_TOTP_QR_OK) {
      totp_status = vectis_totp_enrollment_qr(&totp, label, issuer, &qr);
    }
    if (totp_status == VECTIS_TOTP_QR_OK) {
      totp_status = vectis_qr_render_ansi(&qr, &totp_qr_ansi, &totp_qr_len);
    }
    if (totp_status != VECTIS_TOTP_QR_OK) {
      OPENSSL_cleanse(password, sizeof(password));
      OPENSSL_cleanse(salt_hex, sizeof(salt_hex));
      OPENSSL_cleanse(hash_hex, sizeof(hash_hex));
      vectis_totp_qr_free(totp_uri);
      vectis_totp_qr_free(totp_qr_ansi);
      vectis_auth_set_errorf(error, VECTIS_ERR_INVALID,
                             "failed to enroll TOTP: %s",
                             vectis_totp_qr_status_string(totp_status));
      return VECTIS_ERR_INVALID;
    }
  }
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status != VECTIS_OK) {
    OPENSSL_cleanse(password, sizeof(password));
    OPENSSL_cleanse(salt_hex, sizeof(salt_hex));
    OPENSSL_cleanse(hash_hex, sizeof(hash_hex));
    vectis_totp_qr_free(totp_uri);
    vectis_totp_qr_free(totp_qr_ansi);
    return status;
  }
  lonejson_owned_buffer_init(&user_json);
  status = vectis_auth_build_user_record_json(
      runtime, user_config->username, salt_hex, hash_hex,
      user_config->enable_totp, secret_value, &user_json, error);
  if (status == VECTIS_OK) {
    status = vectis_auth_lock_open(store_config, &lock, error);
  }
  if (status == VECTIS_OK) {
    store_json = NULL;
    store_len = 0u;
    status = vectis_auth_read_store_locked(store_config, &store_json,
                                           &store_len, error);
    if (status == VECTIS_OK && store_json == NULL) {
      status = vectis_auth_write_empty_store_locked(store_config, error);
    }
    free(store_json);
  }
  if (status == VECTIS_OK) {
    status = vectis_auth_drop_user_to_temp_locked(
        store_config, runtime, user_config->username, temp_path,
        sizeof(temp_path), error);
  }
  if (status == VECTIS_OK) {
    temp_config = *store_config;
    temp_config.credentials_path = temp_path;
    store_json = NULL;
    store_len = 0u;
    status = vectis_auth_read_store_locked(&temp_config, &store_json,
                                           &store_len, error);
    if (status == VECTIS_OK) {
      status = vectis_auth_write_store_with_user_locked(
          store_config, store_json, store_len, user_json.data, user_json.len,
          error);
    }
    free(store_json);
    (void)unlink(temp_path);
  }
  if (status == VECTIS_OK && out != NULL) {
    out->username = vectis_auth_strdup(user_config->username);
    if (user_config->password == NULL || user_config->password[0] == '\0') {
      out->generated_password = vectis_auth_strdup(password);
    }
    if (user_config->enable_totp) {
      out->totp_secret = vectis_auth_strdup(secret_value);
      out->totp_uri = vectis_auth_strdup(totp_uri);
      out->totp_qr_ansi = vectis_auth_strdup(totp_qr_ansi);
    }
    if (out->username == NULL ||
        ((user_config->password == NULL || user_config->password[0] == '\0') &&
         out->generated_password == NULL) ||
        (user_config->enable_totp &&
         (out->totp_secret == NULL || out->totp_uri == NULL ||
          out->totp_qr_ansi == NULL))) {
      vectis_auth_user_enrollment_cleanup(out);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy auth user enrollment");
      status = VECTIS_ERR_NOMEM;
    }
  }
  if (status == VECTIS_OK) {
    vectis_auth_lock_close(&lock);
  } else if (lock.fd >= 0) {
    vectis_auth_lock_close(&lock);
  }
  lonejson_owned_buffer_free(&user_json);
  lonejson_free(runtime);
  vectis_totp_qr_free(totp_uri);
  vectis_totp_qr_free(totp_qr_ansi);
  OPENSSL_cleanse(password, sizeof(password));
  OPENSSL_cleanse(salt_hex, sizeof(salt_hex));
  OPENSSL_cleanse(hash_hex, sizeof(hash_hex));
  return status;
}

vectis_status
vectis_auth_user_login(const vectis_auth_store_config *store_config,
                       const vectis_auth_login_config *login_config,
                       vectis_auth_result *out, vectis_error *error) {
  vectis_auth_store_lock lock;
  vectis_auth_user_record record;
  lonejson *runtime;
  lonejson_owned_buffer claim;
  vectis_totp totp;
  vectis_status status;
  uint64_t now_seconds;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth login result is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_result_init(out);
  if (login_config == NULL || login_config->username == NULL ||
      login_config->username[0] == '\0' || login_config->password == NULL ||
      login_config->password[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth login username and password are required");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_auth_lock_open(store_config, &lock, error);
  if (status != VECTIS_OK) {
    return status;
  }
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status == VECTIS_OK) {
    status = vectis_auth_find_user_locked(
        store_config, runtime, login_config->username, &record, error);
  }
  vectis_auth_lock_close(&lock);
  if (status != VECTIS_OK) {
    if (runtime != NULL) {
      lonejson_free(runtime);
    }
    return status;
  }
  if (!record.found ||
      !vectis_auth_verify_password(&record, login_config->password)) {
    lonejson_free(runtime);
    return VECTIS_OK;
  }
  if (record.totp_enabled) {
    if (login_config->totp_code == NULL ||
        vectis_totp_init(&totp, record.totp_secret) != VECTIS_TOTP_QR_OK) {
      lonejson_free(runtime);
      return VECTIS_OK;
    }
    now_seconds = login_config->unix_seconds != 0u ? login_config->unix_seconds
                                                   : (uint64_t)time(NULL);
    if (!vectis_totp_validate(
            &totp, login_config->totp_code, now_seconds,
            login_config->totp_window != 0u ? login_config->totp_window : 1u)) {
      lonejson_free(runtime);
      return VECTIS_OK;
    }
  }
  lonejson_owned_buffer_init(&claim);
  status = vectis_auth_build_login_claim(runtime, login_config->username,
                                         &claim, error);
  if (status == VECTIS_OK) {
    out->client_id = vectis_auth_strdup(login_config->username);
    out->claim_json = vectis_auth_strdup(claim.data);
    out->authenticated = out->client_id != NULL && out->claim_json != NULL;
    if (!out->authenticated) {
      vectis_auth_result_cleanup(out);
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy login result");
      status = VECTIS_ERR_NOMEM;
    }
  }
  lonejson_owned_buffer_free(&claim);
  lonejson_free(runtime);
  return status;
}

vectis_status vectis_auth_issue_webdav_key_for_login(
    const vectis_auth_store_config *store_config,
    const vectis_auth_login_config *login_config,
    vectis_auth_issued_credential *out, vectis_error *error) {
  vectis_auth_result login;
  vectis_auth_issue_config issue;
  vectis_status status;

  vectis_auth_result_init(&login);
  status = vectis_auth_user_login(store_config, login_config, &login, error);
  if (status != VECTIS_OK) {
    return status;
  }
  if (!login.authenticated) {
    vectis_auth_result_cleanup(&login);
    vectis_set_error(error, VECTIS_ERR_INVALID, "auth login failed");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_issue_config_init(&issue);
  issue.subject = login_config->username;
  issue.purpose = "webdav";
  issue.auth_modes = VECTIS_AUTH_MODE_BASIC;
  status = vectis_auth_issue_credential(store_config, &issue, out, error);
  vectis_auth_result_cleanup(&login);
  return status;
}

vectis_status vectis_auth_verify_authorization(
    const vectis_auth_store_config *store_config, const char *authorization,
    unsigned allowed_auth_modes, vectis_auth_result *out, vectis_error *error) {
  lonejson *runtime;
  lonejson_m2m_store store;
  lonejson_m2m_verify_request request;
  lonejson_m2m_authentication auth;
  lonejson_owned_buffer claim;
  lonejson_error json_error;
  lonejson_status json_status;
  vectis_auth_store_lock lock;
  vectis_status status;
  char *store_json;
  size_t store_len;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "auth result is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_result_init(out);
  status = vectis_auth_lock_open(store_config, &lock, error);
  if (status != VECTIS_OK) {
    return status;
  }
  store_json = NULL;
  store_len = 0u;
  status = vectis_auth_read_store_locked(store_config, &store_json, &store_len,
                                         error);
  vectis_auth_lock_close(&lock);
  if (status != VECTIS_OK) {
    return status;
  }
  if (store_json == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth store is not initialized");
    return VECTIS_ERR_INVALID;
  }
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status != VECTIS_OK) {
    free(store_json);
    return status;
  }
  memset(&store, 0, sizeof(store));
  store.json = store_json;
  store.len = store_len;
  store.max_store_bytes = store_config->max_store_bytes;
  memset(&request, 0, sizeof(request));
  request.store = &store;
  request.authorization_header = authorization;
  request.allowed_auth_modes =
      (unsigned)vectis_auth_modes_to_lonejson(allowed_auth_modes);
  lonejson_m2m_authentication_init(&auth);
  lonejson_error_init(&json_error);
  json_status =
      lonejson_m2m_verify_authorization(runtime, &request, &auth, &json_error);
  if (auth.failure != LONEJSON_AUTH_FAILURE_NONE) {
    out->authenticated = 0;
    status = VECTIS_OK;
  } else if (json_status == LONEJSON_STATUS_OK) {
    lonejson_owned_buffer_init(&claim);
    json_status = auth.claim.methods->write_to_sink(
        &auth.claim, lonejson_owned_buffer_sink, &claim, &json_error);
    if (json_status == LONEJSON_STATUS_OK) {
      out->client_id = vectis_auth_strdup(auth.client_id);
      out->claim_json = vectis_auth_strdup(claim.data);
      out->auth_mode = vectis_auth_mode_from_lonejson(auth.auth_mode);
      out->authenticated = out->client_id != NULL && out->claim_json != NULL;
      if (!out->authenticated) {
        vectis_auth_result_cleanup(out);
        vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy auth result");
        status = VECTIS_ERR_NOMEM;
      }
    } else {
      status = vectis_auth_lonejson_error(error, json_status, &json_error,
                                          "failed to copy auth claim");
    }
    lonejson_owned_buffer_free(&claim);
  } else if (json_status != LONEJSON_STATUS_OK) {
    status = vectis_auth_lonejson_error(error, json_status, &json_error,
                                        "failed to verify authorization");
  }
  lonejson_m2m_authentication_cleanup(&auth);
  lonejson_free(runtime);
  free(store_json);
  return status;
}

vectis_status
vectis_auth_revoke_client(const vectis_auth_store_config *store_config,
                          const char *client_id, vectis_error *error) {
  vectis_auth_store_lock lock;
  lonejson *runtime;
  lonejson_json_value item_value;
  lonejson_array_rewrite_options options;
  vectis_auth_revoke_state state;
  lonejson_error json_error;
  lonejson_status json_status;
  vectis_status status;
  char temp_path[4096];
  int written;

  if (client_id == NULL || client_id[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth revoke client_id is required");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_auth_lock_open(store_config, &lock, error);
  if (status != VECTIS_OK) {
    return status;
  }
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status != VECTIS_OK) {
    vectis_auth_lock_close(&lock);
    return status;
  }
  written = snprintf(temp_path, sizeof(temp_path), "%s.revoke.%ld",
                     store_config->credentials_path, (long)getpid());
  if (written < 0 || (size_t)written >= sizeof(temp_path)) {
    lonejson_free(runtime);
    vectis_auth_lock_close(&lock);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth revoke temp path is too long");
    return VECTIS_ERR_INVALID;
  }
  memset(&state, 0, sizeof(state));
  state.runtime = runtime;
  state.client_id = client_id;
  memset(&options, 0, sizeof(options));
  lonejson_json_value_init(runtime, &item_value);
  lonejson_error_init(&json_error);
  json_status =
      lonejson_json_value_enable_parse_capture(&item_value, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    options.item_value = &item_value;
    options.item = vectis_auth_revoke_item;
    options.user = &state;
    json_status = lonejson_array_rewrite_path(runtime, "credentials",
                                              store_config->credentials_path,
                                              temp_path, &options, &json_error);
  }
  lonejson_json_value_cleanup(&item_value);
  if (json_status != LONEJSON_STATUS_OK) {
    (void)unlink(temp_path);
    lonejson_free(runtime);
    vectis_auth_lock_close(&lock);
    return vectis_auth_lonejson_error(error, json_status, &json_error,
                                      "failed to revoke auth credential");
  }
  if (!state.matched) {
    (void)unlink(temp_path);
    lonejson_free(runtime);
    vectis_auth_lock_close(&lock);
    return VECTIS_OK;
  }
  if (rename(temp_path, store_config->credentials_path) != 0) {
    (void)unlink(temp_path);
    lonejson_free(runtime);
    vectis_auth_lock_close(&lock);
    return vectis_auth_set_errno(error, "failed to replace auth store",
                                 store_config->credentials_path);
  }
  lonejson_free(runtime);
  vectis_auth_lock_close(&lock);
  return VECTIS_OK;
}

vectis_status vectis_auth_provider_response_set_authenticated(
    vectis_auth_provider_response *response, const char *principal,
    const char *client_id, const char *claim_json, unsigned auth_mode,
    vectis_error *error) {
  char *client_id_copy;
  char *claim_copy;

  if (response == NULL || client_id == NULL || claim_json == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth response, client_id, and claim_json are required");
    return VECTIS_ERR_INVALID;
  }
  client_id_copy = vectis_auth_strdup(client_id);
  claim_copy = vectis_auth_strdup(claim_json);
  if (client_id_copy == NULL || claim_copy == NULL) {
    free(client_id_copy);
    free(claim_copy);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy authenticated auth response");
    return VECTIS_ERR_NOMEM;
  }
  vectis_auth_result_cleanup(&response->result);
  response->action = VECTIS_AUTH_ALLOW;
  response->status_code = 0;
  response->location = NULL;
  response->content_type = NULL;
  response->body = NULL;
  response->body_size = 0u;
  response->www_authenticate[0] = '\0';
  vectis_auth_copy_fixed(response->principal, sizeof(response->principal),
                         principal != NULL ? principal : client_id);
  response->result.authenticated = 1;
  response->result.auth_mode = auth_mode;
  response->result.client_id = client_id_copy;
  response->result.claim_json = claim_copy;
  return VECTIS_OK;
}

static int vectis_auth_realm_valid(const char *realm) {
  const unsigned char *cursor;

  if (realm == NULL || realm[0] == '\0') {
    return 0;
  }
  cursor = (const unsigned char *)realm;
  while (*cursor != '\0') {
    if (*cursor < 0x20u || *cursor == '"' || *cursor == '\\') {
      return 0;
    }
    cursor++;
  }
  return 1;
}

static void
vectis_auth_provider_require(const vectis_auth_native_provider_config *config,
                             unsigned modes,
                             vectis_auth_provider_response *response) {
  const char *realm;

  vectis_auth_provider_response_cleanup(response);
  response->action = VECTIS_AUTH_REQUIRED;
  response->status_code = 401;
  realm = config != NULL && vectis_auth_realm_valid(config->realm)
              ? config->realm
              : "vectis";
  if (modes == VECTIS_AUTH_MODE_DEFAULT ||
      (modes & VECTIS_AUTH_MODE_BASIC) != 0u) {
    (void)snprintf(response->www_authenticate,
                   sizeof(response->www_authenticate), "Basic realm=\"%s\"",
                   realm);
  } else if ((modes & VECTIS_AUTH_MODE_BEARER) != 0u) {
    vectis_auth_copy_fixed(response->www_authenticate,
                           sizeof(response->www_authenticate), "Bearer");
  }
}

static vectis_status vectis_auth_native_provider_authenticate(
    const vectis_auth_provider_request *request,
    vectis_auth_provider_response *response, void *userdata,
    vectis_error *error) {
  const vectis_auth_native_provider_config *config;
  const char *authorization;
  const char *purpose;
  vectis_auth_result result;
  lonejson *runtime;
  vectis_status status;
  unsigned allowed_modes;
  char principal[VECTIS_AUTH_PRINCIPAL_MAX + 1u];
  char credential_purpose[128];

  config = (const vectis_auth_native_provider_config *)userdata;
  if (config == NULL || response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "native auth provider config is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_provider_response_init(response);
  authorization = request != NULL ? request->authorization : NULL;
  if ((authorization == NULL || authorization[0] == '\0') && request != NULL &&
      request->request != NULL) {
    authorization = vectis_request_header(request->request, "authorization");
  }
  allowed_modes =
      request != NULL && request->allowed_auth_modes != VECTIS_AUTH_MODE_DEFAULT
          ? request->allowed_auth_modes
          : config->allowed_auth_modes;
  if (authorization == NULL || authorization[0] == '\0') {
    vectis_auth_provider_require(config, allowed_modes, response);
    return VECTIS_OK;
  }
  vectis_auth_result_init(&result);
  status = vectis_auth_verify_authorization(&config->store, authorization,
                                            allowed_modes, &result, error);
  if (status != VECTIS_OK) {
    return status;
  }
  if (!result.authenticated) {
    vectis_auth_result_cleanup(&result);
    vectis_auth_provider_require(config, allowed_modes, response);
    return VECTIS_OK;
  }
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status != VECTIS_OK) {
    vectis_auth_result_cleanup(&result);
    return status;
  }
  purpose = request != NULL && request->purpose != NULL ? request->purpose
                                                        : config->purpose;
  if (purpose != NULL && purpose[0] != '\0') {
    if (!vectis_auth_claim_string(runtime, result.claim_json, "purpose",
                                  credential_purpose,
                                  sizeof(credential_purpose)) ||
        strcmp(credential_purpose, purpose) != 0) {
      lonejson_free(runtime);
      vectis_auth_result_cleanup(&result);
      response->action = VECTIS_AUTH_DENY;
      return VECTIS_OK;
    }
  }
  if (!vectis_auth_claim_string(runtime, result.claim_json, "sub", principal,
                                sizeof(principal))) {
    vectis_auth_copy_fixed(principal, sizeof(principal), result.client_id);
  }
  status = vectis_auth_provider_response_set_authenticated(
      response, principal, result.client_id, result.claim_json,
      result.auth_mode, error);
  lonejson_free(runtime);
  vectis_auth_result_cleanup(&result);
  return status;
}

vectis_status
vectis_auth_provider_from_callback(vectis_auth_provider *provider,
                                   vectis_auth_provider_fn authenticate,
                                   void *userdata, vectis_error *error) {
  if (provider == NULL || authenticate == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth provider callback is required");
    return VECTIS_ERR_INVALID;
  }
  provider->authenticate = authenticate;
  provider->userdata = userdata;
  return VECTIS_OK;
}

vectis_status vectis_auth_provider_from_native_store(
    vectis_auth_provider *provider, vectis_auth_native_provider_config *config,
    vectis_error *error) {
  if (provider == NULL || config == NULL ||
      config->store.credentials_path == NULL ||
      config->store.credentials_path[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "native auth provider store path is required");
    return VECTIS_ERR_INVALID;
  }
  provider->authenticate = vectis_auth_native_provider_authenticate;
  provider->userdata = config;
  return VECTIS_OK;
}

vectis_status
vectis_auth_provider_authenticate(const vectis_auth_provider *provider,
                                  const vectis_auth_provider_request *request,
                                  vectis_auth_provider_response *response,
                                  vectis_error *error) {
  if (provider == NULL || provider->authenticate == NULL || response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth provider authenticate callback is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_provider_response_init(response);
  return provider->authenticate(request, response, provider->userdata, error);
}
