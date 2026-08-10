#define LONEJSON_WITH_OPENSSL 1
#define LONEJSON_WITH_OIDC 1

#include "vectis_internal.h"

#include <vectis/auth.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
#define VECTIS_AUTH_RANDOM_OIDC_BYTES 16u
#define VECTIS_AUTH_RANDOM_EMAIL_TOKEN_BYTES 16u

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

typedef struct vectis_auth_claim_string_alloc_probe {
  const char *key;
  size_t key_len;
  char *data;
  size_t size;
  size_t capacity;
  int matched;
  int failed;
} vectis_auth_claim_string_alloc_probe;

typedef struct vectis_auth_claim_uint_probe {
  const char *key;
  size_t key_len;
  char value[32];
  size_t size;
  unsigned int out;
  int matched;
  int overflow;
} vectis_auth_claim_uint_probe;

typedef struct vectis_auth_claim_i64_probe {
  const char *key;
  size_t key_len;
  char value[32];
  size_t size;
  int64_t out;
  int matched;
  int overflow;
} vectis_auth_claim_i64_probe;

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

typedef struct vectis_auth_oauth2_flow_record {
  char flow_id[256];
  char subject[VECTIS_AUTH_PRINCIPAL_MAX + 1u];
  char webdav_client_id[256];
  vectis_auth_oauth2_token_flow flow;
  int found;
} vectis_auth_oauth2_flow_record;

typedef struct vectis_auth_email_token_record {
  char transaction_id[256];
  char username[VECTIS_AUTH_PRINCIPAL_MAX + 1u];
  char realm[128];
  char email[320];
  char pending_transaction_id[256];
  char token_hash[2u * VECTIS_AUTH_PASSWORD_HASH_BYTES + 1u];
  int64_t expires_at;
  unsigned int failed_attempts;
  unsigned int max_attempts;
  int found;
} vectis_auth_email_token_record;

typedef struct vectis_auth_pending_login_record {
  char transaction_id[256];
  char username[VECTIS_AUTH_PRINCIPAL_MAX + 1u];
  char realm[128];
  int64_t expires_at;
  int totp_required;
  int found;
} vectis_auth_pending_login_record;

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

typedef struct vectis_auth_oauth2_flow_find_state {
  lonejson *runtime;
  const char *flow_id;
  vectis_auth_oauth2_flow_record record;
} vectis_auth_oauth2_flow_find_state;

typedef struct vectis_auth_email_token_find_state {
  lonejson *runtime;
  const char *transaction_id;
  vectis_auth_email_token_record record;
} vectis_auth_email_token_find_state;

typedef struct vectis_auth_pending_login_find_state {
  lonejson *runtime;
  const char *transaction_id;
  vectis_auth_pending_login_record record;
} vectis_auth_pending_login_find_state;

typedef struct vectis_auth_oauth2_flow_drop_state {
  lonejson *runtime;
  const char *flow_id;
  int matched;
} vectis_auth_oauth2_flow_drop_state;

typedef struct vectis_auth_email_token_drop_state {
  lonejson *runtime;
  const char *transaction_id;
  int matched;
} vectis_auth_email_token_drop_state;

typedef struct vectis_auth_pending_login_drop_state {
  lonejson *runtime;
  const char *transaction_id;
  int matched;
} vectis_auth_pending_login_drop_state;

typedef struct vectis_auth_oauth2_webdav_revoke_state {
  lonejson *runtime;
  const char *flow_id;
  int matched;
} vectis_auth_oauth2_webdav_revoke_state;

typedef struct vectis_auth_oauth2_http_adapter {
  const vectis_auth_oauth2_transport_config *config;
  vectis_error error;
} vectis_auth_oauth2_http_adapter;

typedef struct vectis_auth_oauth2_response_buffer {
  unsigned char *data;
  size_t size;
  size_t capacity;
  size_t max_size;
  int failed;
} vectis_auth_oauth2_response_buffer;

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

static void
vectis_auth_oauth2_flow_record_init(vectis_auth_oauth2_flow_record *record) {
  if (record == NULL) {
    return;
  }
  memset(record, 0, sizeof(*record));
  vectis_auth_oauth2_token_flow_init(&record->flow);
}

static void
vectis_auth_oauth2_flow_record_cleanup(vectis_auth_oauth2_flow_record *record) {
  if (record == NULL) {
    return;
  }
  vectis_auth_oauth2_token_flow_cleanup(&record->flow);
  vectis_auth_oauth2_flow_record_init(record);
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

static int vectis_auth_path_matches_key2(const lonejson_value_path *path,
                                         const char *key1, size_t key1_len,
                                         const char *key2, size_t key2_len) {
  return path != NULL && key1 != NULL && key2 != NULL &&
         path->segment_count == 2u && path->segments[0].len == key1_len &&
         memcmp(path->segments[0].data, key1, key1_len) == 0 &&
         path->segments[1].len == key2_len &&
         memcmp(path->segments[1].data, key2, key2_len) == 0;
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

static lonejson_status vectis_auth_claim_string_alloc_chunk(
    void *user, const lonejson_value_path *path, const char *data, size_t len,
    lonejson_error *error) {
  vectis_auth_claim_string_alloc_probe *probe;
  char *grown;
  size_t new_size;
  size_t new_capacity;

  (void)error;
  probe = (vectis_auth_claim_string_alloc_probe *)user;
  if (probe == NULL || probe->matched || probe->failed ||
      !vectis_auth_path_matches_key(path, probe->key, probe->key_len)) {
    return LONEJSON_STATUS_OK;
  }
  if (len == 0u) {
    return LONEJSON_STATUS_OK;
  }
  if (probe->size + len < probe->size) {
    probe->failed = 1;
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  new_size = probe->size + len;
  if (new_size + 1u < new_size) {
    probe->failed = 1;
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  if (new_size + 1u > probe->capacity) {
    new_capacity = probe->capacity != 0u ? probe->capacity : 128u;
    while (new_capacity < new_size + 1u) {
      if (new_capacity > ((size_t)-1) / 2u) {
        probe->failed = 1;
        return LONEJSON_STATUS_ALLOCATION_FAILED;
      }
      new_capacity *= 2u;
    }
    grown = (char *)realloc(probe->data, new_capacity);
    if (grown == NULL) {
      probe->failed = 1;
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    probe->data = grown;
    probe->capacity = new_capacity;
  }
  memcpy(probe->data + probe->size, data, len);
  probe->size = new_size;
  probe->data[probe->size] = '\0';
  return LONEJSON_STATUS_OK;
}

static lonejson_status
vectis_auth_claim_string_alloc_end(void *user, const lonejson_value_path *path,
                                   lonejson_error *error) {
  vectis_auth_claim_string_alloc_probe *probe;

  (void)error;
  probe = (vectis_auth_claim_string_alloc_probe *)user;
  if (probe != NULL &&
      vectis_auth_path_matches_key(path, probe->key, probe->key_len) &&
      !probe->failed) {
    if (probe->data == NULL) {
      probe->data = vectis_auth_strdup("");
      if (probe->data == NULL) {
        probe->failed = 1;
        return LONEJSON_STATUS_ALLOCATION_FAILED;
      }
    }
    probe->matched = 1;
  }
  return LONEJSON_STATUS_OK;
}

static char *vectis_auth_claim_string_alloc(lonejson *runtime,
                                            const char *claim_json,
                                            const char *key, int *failed) {
  vectis_auth_claim_string_alloc_probe probe;
  lonejson_path_value_visitor visitor;
  lonejson_error json_error;
  lonejson_status status;

  if (failed != NULL) {
    *failed = 0;
  }
  if (runtime == NULL || claim_json == NULL || key == NULL) {
    return NULL;
  }
  memset(&probe, 0, sizeof(probe));
  probe.key = key;
  probe.key_len = strlen(key);
  visitor = lonejson_default_path_value_visitor();
  visitor.string_chunk = vectis_auth_claim_string_alloc_chunk;
  visitor.string_end = vectis_auth_claim_string_alloc_end;
  lonejson_error_init(&json_error);
  status = lonejson_visit_path_value_buffer(
      runtime, claim_json, strlen(claim_json), &visitor, &probe, &json_error);
  if (status != LONEJSON_STATUS_OK || probe.failed) {
    free(probe.data);
    if (failed != NULL) {
      *failed = 1;
    }
    return NULL;
  }
  if (!probe.matched) {
    free(probe.data);
    return NULL;
  }
  return probe.data;
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
vectis_auth_claim_i64_chunk(void *user, const lonejson_value_path *path,
                            const char *data, size_t len,
                            lonejson_error *error) {
  vectis_auth_claim_i64_probe *probe;

  (void)error;
  probe = (vectis_auth_claim_i64_probe *)user;
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
vectis_auth_claim_i64_end(void *user, const lonejson_value_path *path,
                          lonejson_error *error) {
  vectis_auth_claim_i64_probe *probe;
  char *end;
  long long value;

  (void)error;
  probe = (vectis_auth_claim_i64_probe *)user;
  if (probe == NULL ||
      !vectis_auth_path_matches_key(path, probe->key, probe->key_len) ||
      probe->overflow || probe->value[0] == '\0') {
    return LONEJSON_STATUS_OK;
  }
  errno = 0;
  end = NULL;
  value = strtoll(probe->value, &end, 10);
  if (errno == 0 && end != probe->value && end != NULL && *end == '\0') {
    probe->out = (int64_t)value;
    probe->matched = 1;
  }
  return LONEJSON_STATUS_OK;
}

static int vectis_auth_claim_i64(lonejson *runtime, const char *claim_json,
                                 const char *key, int64_t *out) {
  vectis_auth_claim_i64_probe probe;
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
  visitor.number_chunk = vectis_auth_claim_i64_chunk;
  visitor.number_end = vectis_auth_claim_i64_end;
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

static int
vectis_auth_oauth2_flow_record_from_json(lonejson *runtime, const char *json,
                                         size_t len,
                                         vectis_auth_oauth2_flow_record *out) {
  char *buffer;
  int64_t expires_at;
  int has_expires_at;
  int failed;
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
  vectis_auth_oauth2_flow_record_init(out);
  ok = vectis_auth_claim_string(runtime, buffer, "flow_id", out->flow_id,
                                sizeof(out->flow_id));
  if (ok) {
    (void)vectis_auth_claim_string(runtime, buffer, "subject", out->subject,
                                   sizeof(out->subject));
    (void)vectis_auth_claim_string(runtime, buffer, "webdav_client_id",
                                   out->webdav_client_id,
                                   sizeof(out->webdav_client_id));
    failed = 0;
    out->flow.access_token = vectis_auth_claim_string_alloc(
        runtime, buffer, "access_token", &failed);
    ok = !failed;
    if (ok) {
      out->flow.token_type = vectis_auth_claim_string_alloc(
          runtime, buffer, "token_type", &failed);
      ok = !failed;
    }
    if (ok) {
      out->flow.refresh_token = vectis_auth_claim_string_alloc(
          runtime, buffer, "refresh_token", &failed);
      ok = !failed;
    }
    if (ok) {
      out->flow.scope =
          vectis_auth_claim_string_alloc(runtime, buffer, "scope", &failed);
      ok = !failed;
    }
    if (ok) {
      out->flow.id_token =
          vectis_auth_claim_string_alloc(runtime, buffer, "id_token", &failed);
      ok = !failed;
    }
  }
  if (ok) {
    has_expires_at = vectis_auth_claim_bool(runtime, buffer, "has_expires_at",
                                            &has_expires_at)
                         ? has_expires_at
                         : 0;
    if (has_expires_at) {
      expires_at = 0;
      ok = vectis_auth_claim_i64(runtime, buffer, "expires_at", &expires_at);
      out->flow.expires_at = expires_at;
      out->flow.has_expires_at = ok;
    }
  }
  out->found = ok;
  if (!ok) {
    vectis_auth_oauth2_flow_record_cleanup(out);
  }
  free(buffer);
  return ok;
}

static int
vectis_auth_email_token_record_from_json(lonejson *runtime, const char *json,
                                         size_t len,
                                         vectis_auth_email_token_record *out) {
  char *buffer;
  int64_t expires_at;
  int64_t failed_attempts;
  int64_t max_attempts;
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
  expires_at = 0;
  failed_attempts = 0;
  max_attempts = VECTIS_AUTH_EMAIL_TOKEN_DEFAULT_MAX_ATTEMPTS;
  ok = vectis_auth_claim_string(runtime, buffer, "transaction_id",
                                out->transaction_id,
                                sizeof(out->transaction_id)) &&
       vectis_auth_claim_string(runtime, buffer, "username", out->username,
                                sizeof(out->username)) &&
       vectis_auth_claim_string(runtime, buffer, "realm", out->realm,
                                sizeof(out->realm)) &&
       vectis_auth_claim_string(runtime, buffer, "email", out->email,
                                sizeof(out->email)) &&
       vectis_auth_claim_string(runtime, buffer, "token_hash", out->token_hash,
                                sizeof(out->token_hash)) &&
       vectis_auth_claim_i64(runtime, buffer, "expires_at", &expires_at);
  if (ok) {
    (void)vectis_auth_claim_string(runtime, buffer, "pending_transaction_id",
                                   out->pending_transaction_id,
                                   sizeof(out->pending_transaction_id));
    (void)vectis_auth_claim_i64(runtime, buffer, "failed_attempts",
                                &failed_attempts);
    (void)vectis_auth_claim_i64(runtime, buffer, "max_attempts", &max_attempts);
    if (failed_attempts < 0) {
      failed_attempts = 0;
    }
    if (max_attempts <= 0) {
      max_attempts = VECTIS_AUTH_EMAIL_TOKEN_DEFAULT_MAX_ATTEMPTS;
    }
    out->expires_at = expires_at;
    out->failed_attempts = failed_attempts > (int64_t)UINT_MAX
                               ? UINT_MAX
                               : (unsigned int)failed_attempts;
    out->max_attempts = max_attempts > (int64_t)UINT_MAX
                            ? UINT_MAX
                            : (unsigned int)max_attempts;
    out->found = 1;
  }
  free(buffer);
  return ok;
}

static int vectis_auth_pending_login_record_from_json(
    lonejson *runtime, const char *json, size_t len,
    vectis_auth_pending_login_record *out) {
  char *buffer;
  int ok;
  int64_t expires_at;
  int totp_required;

  if (runtime == NULL || json == NULL || out == NULL || len == 0u) {
    return 0;
  }
  buffer = (char *)malloc(len + 1u);
  if (buffer == NULL) {
    return 0;
  }
  memcpy(buffer, json, len);
  buffer[len] = '\0';
  memset(out, 0, sizeof(*out));
  expires_at = 0;
  totp_required = 0;
  ok = vectis_auth_claim_string(runtime, buffer, "transaction_id",
                                out->transaction_id,
                                sizeof(out->transaction_id)) &&
       vectis_auth_claim_string(runtime, buffer, "username", out->username,
                                sizeof(out->username)) &&
       vectis_auth_claim_string(runtime, buffer, "realm", out->realm,
                                sizeof(out->realm)) &&
       vectis_auth_claim_i64(runtime, buffer, "expires_at", &expires_at);
  if (ok) {
    (void)vectis_auth_claim_bool(runtime, buffer, "totp_required",
                                 &totp_required);
    out->expires_at = expires_at;
    out->totp_required = totp_required ? 1 : 0;
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

static lonejson_status vectis_auth_oauth2_flow_find_item(
    void *user, const lonejson_array_rewrite_context *context, void *item,
    lonejson_array_rewrite_result *result, lonejson_error *error) {
  vectis_auth_oauth2_flow_find_state *state;
  lonejson_json_value *value;
  lonejson_owned_buffer json;
  vectis_auth_oauth2_flow_record record;
  lonejson_status status;

  (void)context;
  (void)result;
  state = (vectis_auth_oauth2_flow_find_state *)user;
  value = (lonejson_json_value *)item;
  if (state == NULL || state->record.found || state->flow_id == NULL) {
    return LONEJSON_STATUS_OK;
  }
  lonejson_owned_buffer_init(&json);
  vectis_auth_oauth2_flow_record_init(&record);
  status = value->methods->write_to_sink(value, lonejson_owned_buffer_sink,
                                         &json, error);
  if (status == LONEJSON_STATUS_OK &&
      vectis_auth_oauth2_flow_record_from_json(state->runtime, json.data,
                                               json.len, &record) &&
      strcmp(record.flow_id, state->flow_id) == 0) {
    state->record = record;
    state->record.found = 1;
  } else {
    vectis_auth_oauth2_flow_record_cleanup(&record);
  }
  lonejson_owned_buffer_free(&json);
  return status;
}

static lonejson_status vectis_auth_oauth2_flow_drop_item(
    void *user, const lonejson_array_rewrite_context *context, void *item,
    lonejson_array_rewrite_result *result, lonejson_error *error) {
  vectis_auth_oauth2_flow_drop_state *state;
  lonejson_json_value *value;
  lonejson_owned_buffer json;
  vectis_auth_oauth2_flow_record record;
  lonejson_status status;

  (void)context;
  state = (vectis_auth_oauth2_flow_drop_state *)user;
  value = (lonejson_json_value *)item;
  if (state == NULL || state->flow_id == NULL) {
    return LONEJSON_STATUS_OK;
  }
  lonejson_owned_buffer_init(&json);
  vectis_auth_oauth2_flow_record_init(&record);
  status = value->methods->write_to_sink(value, lonejson_owned_buffer_sink,
                                         &json, error);
  if (status == LONEJSON_STATUS_OK &&
      vectis_auth_oauth2_flow_record_from_json(state->runtime, json.data,
                                               json.len, &record) &&
      strcmp(record.flow_id, state->flow_id) == 0) {
    result->action = LONEJSON_ARRAY_REWRITE_DROP;
    state->matched = 1;
  }
  vectis_auth_oauth2_flow_record_cleanup(&record);
  lonejson_owned_buffer_free(&json);
  return status;
}

static lonejson_status vectis_auth_email_token_find_item(
    void *user, const lonejson_array_rewrite_context *context, void *item,
    lonejson_array_rewrite_result *result, lonejson_error *error) {
  vectis_auth_email_token_find_state *state;
  lonejson_json_value *value;
  lonejson_owned_buffer json;
  vectis_auth_email_token_record record;
  lonejson_status status;

  (void)context;
  (void)result;
  state = (vectis_auth_email_token_find_state *)user;
  value = (lonejson_json_value *)item;
  if (state == NULL || state->record.found || state->transaction_id == NULL) {
    return LONEJSON_STATUS_OK;
  }
  lonejson_owned_buffer_init(&json);
  status = value->methods->write_to_sink(value, lonejson_owned_buffer_sink,
                                         &json, error);
  if (status == LONEJSON_STATUS_OK &&
      vectis_auth_email_token_record_from_json(state->runtime, json.data,
                                               json.len, &record) &&
      strcmp(record.transaction_id, state->transaction_id) == 0) {
    state->record = record;
    state->record.found = 1;
  }
  lonejson_owned_buffer_free(&json);
  return status;
}

static lonejson_status vectis_auth_email_token_drop_item(
    void *user, const lonejson_array_rewrite_context *context, void *item,
    lonejson_array_rewrite_result *result, lonejson_error *error) {
  vectis_auth_email_token_drop_state *state;
  lonejson_json_value *value;
  lonejson_owned_buffer json;
  vectis_auth_email_token_record record;
  lonejson_status status;

  (void)context;
  state = (vectis_auth_email_token_drop_state *)user;
  value = (lonejson_json_value *)item;
  if (state == NULL || state->transaction_id == NULL) {
    return LONEJSON_STATUS_OK;
  }
  lonejson_owned_buffer_init(&json);
  status = value->methods->write_to_sink(value, lonejson_owned_buffer_sink,
                                         &json, error);
  if (status == LONEJSON_STATUS_OK &&
      vectis_auth_email_token_record_from_json(state->runtime, json.data,
                                               json.len, &record) &&
      strcmp(record.transaction_id, state->transaction_id) == 0) {
    result->action = LONEJSON_ARRAY_REWRITE_DROP;
    state->matched = 1;
  }
  lonejson_owned_buffer_free(&json);
  return status;
}

static lonejson_status vectis_auth_pending_login_find_item(
    void *user, const lonejson_array_rewrite_context *context, void *item,
    lonejson_array_rewrite_result *result, lonejson_error *error) {
  vectis_auth_pending_login_find_state *state;
  lonejson_json_value *value;
  lonejson_owned_buffer json;
  vectis_auth_pending_login_record record;
  lonejson_status status;

  (void)context;
  (void)result;
  state = (vectis_auth_pending_login_find_state *)user;
  value = (lonejson_json_value *)item;
  if (state == NULL || state->record.found || state->transaction_id == NULL) {
    return LONEJSON_STATUS_OK;
  }
  lonejson_owned_buffer_init(&json);
  status = value->methods->write_to_sink(value, lonejson_owned_buffer_sink,
                                         &json, error);
  if (status == LONEJSON_STATUS_OK &&
      vectis_auth_pending_login_record_from_json(state->runtime, json.data,
                                                 json.len, &record) &&
      strcmp(record.transaction_id, state->transaction_id) == 0) {
    state->record = record;
    state->record.found = 1;
  }
  lonejson_owned_buffer_free(&json);
  return status;
}

static lonejson_status vectis_auth_pending_login_drop_item(
    void *user, const lonejson_array_rewrite_context *context, void *item,
    lonejson_array_rewrite_result *result, lonejson_error *error) {
  vectis_auth_pending_login_drop_state *state;
  lonejson_json_value *value;
  lonejson_owned_buffer json;
  vectis_auth_pending_login_record record;
  lonejson_status status;

  (void)context;
  state = (vectis_auth_pending_login_drop_state *)user;
  value = (lonejson_json_value *)item;
  if (state == NULL || state->transaction_id == NULL) {
    return LONEJSON_STATUS_OK;
  }
  lonejson_owned_buffer_init(&json);
  status = value->methods->write_to_sink(value, lonejson_owned_buffer_sink,
                                         &json, error);
  if (status == LONEJSON_STATUS_OK &&
      vectis_auth_pending_login_record_from_json(state->runtime, json.data,
                                                 json.len, &record) &&
      strcmp(record.transaction_id, state->transaction_id) == 0) {
    result->action = LONEJSON_ARRAY_REWRITE_DROP;
    state->matched = 1;
  }
  lonejson_owned_buffer_free(&json);
  return status;
}

static lonejson_status
vectis_auth_oauth2_flow_claim_chunk(void *user, const lonejson_value_path *path,
                                    const char *data, size_t len,
                                    lonejson_error *error) {
  vectis_auth_client_id_probe *probe;

  (void)error;
  probe = (vectis_auth_client_id_probe *)user;
  if (probe == NULL || probe->matched || probe->overflow ||
      !vectis_auth_path_matches_key2(path, "claim", 5u, "oauth2_flow_id",
                                     14u)) {
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
vectis_auth_oauth2_flow_claim_end(void *user, const lonejson_value_path *path,
                                  lonejson_error *error) {
  vectis_auth_client_id_probe *probe;

  (void)error;
  probe = (vectis_auth_client_id_probe *)user;
  if (probe != NULL &&
      vectis_auth_path_matches_key2(path, "claim", 5u, "oauth2_flow_id", 14u) &&
      !probe->overflow && probe->expected != NULL &&
      strcmp(probe->value, probe->expected) == 0) {
    probe->matched = 1;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status vectis_auth_oauth2_webdav_revoke_item(
    void *user, const lonejson_array_rewrite_context *context, void *item,
    lonejson_array_rewrite_result *result, lonejson_error *error) {
  vectis_auth_oauth2_webdav_revoke_state *state;
  lonejson_json_value *value;
  lonejson_owned_buffer json;
  lonejson_path_value_visitor visitor;
  vectis_auth_client_id_probe probe;
  lonejson_status status;

  (void)context;
  state = (vectis_auth_oauth2_webdav_revoke_state *)user;
  value = (lonejson_json_value *)item;
  if (state == NULL || state->flow_id == NULL) {
    return LONEJSON_STATUS_OK;
  }
  memset(&probe, 0, sizeof(probe));
  probe.expected = state->flow_id;
  lonejson_owned_buffer_init(&json);
  status = value->methods->write_to_sink(value, lonejson_owned_buffer_sink,
                                         &json, error);
  if (status == LONEJSON_STATUS_OK) {
    visitor = lonejson_default_path_value_visitor();
    visitor.string_chunk = vectis_auth_oauth2_flow_claim_chunk;
    visitor.string_end = vectis_auth_oauth2_flow_claim_end;
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
      "{\"credentials\":[],\"signups\":[],\"users\":[],\"oauth2_flows\":[],"
      "\"email_tokens\":[],\"pending_logins\":[]}\n";

  return vectis_auth_write_store_locked(config, empty_store,
                                        sizeof(empty_store) - 1u, error);
}

static vectis_status vectis_auth_writer_copy_store_arrays(
    lonejson_writer *writer, const char *store_json, size_t store_len,
    const char *extra_record_json, size_t extra_record_len,
    const char *extra_user_json, size_t extra_user_len,
    const char *extra_flow_json, size_t extra_flow_len,
    const char *extra_email_token_json, size_t extra_email_token_len,
    const char *extra_pending_login_json, size_t extra_pending_login_len,
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
    status = lonejson_writer_key(writer, "oauth2_flows", 12u, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_array(writer, json_error);
  }
  if (status == LONEJSON_STATUS_OK && store_json != NULL && store_len > 0u) {
    status = lonejson_writer_array_items_buffer(
        writer, "oauth2_flows", store_json, store_len, json_error);
  }
  if (status == LONEJSON_STATUS_OK && extra_flow_json != NULL) {
    status = lonejson_writer_json_value_buffer(writer, extra_flow_json,
                                               extra_flow_len, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_array(writer, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(writer, "email_tokens", 12u, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_array(writer, json_error);
  }
  if (status == LONEJSON_STATUS_OK && store_json != NULL && store_len > 0u) {
    status = lonejson_writer_array_items_buffer(
        writer, "email_tokens", store_json, store_len, json_error);
  }
  if (status == LONEJSON_STATUS_OK && extra_email_token_json != NULL) {
    status = lonejson_writer_json_value_buffer(
        writer, extra_email_token_json, extra_email_token_len, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_array(writer, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(writer, "pending_logins", 14u, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_array(writer, json_error);
  }
  if (status == LONEJSON_STATUS_OK && store_json != NULL && store_len > 0u) {
    status = lonejson_writer_array_items_buffer(
        writer, "pending_logins", store_json, store_len, json_error);
  }
  if (status == LONEJSON_STATUS_OK && extra_pending_login_json != NULL) {
    status = lonejson_writer_json_value_buffer(
        writer, extra_pending_login_json, extra_pending_login_len, json_error);
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
        &writer, store_json, store_len, record_json, record_len, NULL, 0u, NULL,
        0u, NULL, 0u, NULL, 0u, &json_error);
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

void vectis_auth_routes_config_init(vectis_auth_routes_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->path_prefix = "/_vectis/auth";
  vectis_auth_store_config_init(&config->store);
  config->realm = "vectis";
  config->login_title = "Vectis Login";
  config->max_body_bytes = 8192u;
  config->required_factors = VECTIS_AUTH_ROUTE_FACTOR_PASSWORD;
  config->email_token_ttl_seconds = VECTIS_AUTH_EMAIL_TOKEN_DEFAULT_TTL_SECONDS;
  config->email_token_max_attempts =
      VECTIS_AUTH_EMAIL_TOKEN_DEFAULT_MAX_ATTEMPTS;
  config->pending_login_ttl_seconds =
      VECTIS_AUTH_PENDING_LOGIN_DEFAULT_TTL_SECONDS;
  vectis_auth_smtp_config_init(&config->email_smtp);
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

void vectis_auth_password_check_config_init(
    vectis_auth_password_check_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  vectis_auth_store_config_init(&config->store);
}

void vectis_auth_password_check_result_init(
    vectis_auth_password_check_result *result) {
  if (result == NULL) {
    return;
  }
  memset(result, 0, sizeof(*result));
}

void vectis_auth_pending_login_issue_config_init(
    vectis_auth_pending_login_issue_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  vectis_auth_store_config_init(&config->store);
  config->ttl_seconds = VECTIS_AUTH_PENDING_LOGIN_DEFAULT_TTL_SECONDS;
}

void vectis_auth_pending_login_init(vectis_auth_pending_login *pending) {
  if (pending == NULL) {
    return;
  }
  memset(pending, 0, sizeof(*pending));
}

void vectis_auth_pending_login_cleanup(vectis_auth_pending_login *pending) {
  if (pending == NULL) {
    return;
  }
  free(pending->transaction_id);
  free(pending->username);
  free(pending->realm);
  vectis_auth_pending_login_init(pending);
}

void vectis_auth_pending_login_consume_config_init(
    vectis_auth_pending_login_consume_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  vectis_auth_store_config_init(&config->store);
  config->totp_window = 1u;
}

void vectis_auth_pending_login_result_init(
    vectis_auth_pending_login_result *result) {
  if (result == NULL) {
    return;
  }
  memset(result, 0, sizeof(*result));
}

void vectis_auth_pending_login_result_cleanup(
    vectis_auth_pending_login_result *result) {
  if (result == NULL) {
    return;
  }
  free(result->username);
  free(result->realm);
  vectis_auth_pending_login_result_init(result);
}

void vectis_auth_email_token_issue_config_init(
    vectis_auth_email_token_issue_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  vectis_auth_store_config_init(&config->store);
  config->realm = "vectis";
  config->ttl_seconds = VECTIS_AUTH_EMAIL_TOKEN_DEFAULT_TTL_SECONDS;
  config->max_attempts = VECTIS_AUTH_EMAIL_TOKEN_DEFAULT_MAX_ATTEMPTS;
}

void vectis_auth_email_token_init(vectis_auth_email_token *token) {
  if (token == NULL) {
    return;
  }
  memset(token, 0, sizeof(*token));
}

void vectis_auth_email_token_cleanup(vectis_auth_email_token *token) {
  if (token == NULL) {
    return;
  }
  free(token->transaction_id);
  free(token->token);
  vectis_auth_email_token_init(token);
}

void vectis_auth_email_token_verify_config_init(
    vectis_auth_email_token_verify_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  vectis_auth_store_config_init(&config->store);
  config->realm = "vectis";
}

void vectis_auth_email_token_result_init(
    vectis_auth_email_token_result *result) {
  if (result == NULL) {
    return;
  }
  memset(result, 0, sizeof(*result));
}

void vectis_auth_email_token_result_cleanup(
    vectis_auth_email_token_result *result) {
  if (result == NULL) {
    return;
  }
  free(result->username);
  free(result->realm);
  free(result->email);
  free(result->pending_transaction_id);
  vectis_auth_email_token_result_init(result);
}

void vectis_auth_smtp_config_init(vectis_auth_smtp_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->subject = "Vectis login token";
  config->timeout_ms = 30000L;
  config->connect_timeout_ms = 10000L;
}

void vectis_auth_oauth2_http_response_init(
    vectis_auth_oauth2_http_response *response) {
  if (response == NULL) {
    return;
  }
  memset(response, 0, sizeof(*response));
}

void vectis_auth_oauth2_http_response_cleanup(
    vectis_auth_oauth2_http_response *response) {
  if (response == NULL) {
    return;
  }
  free(response->content_type);
  free(response->body);
  vectis_auth_oauth2_http_response_init(response);
}

void vectis_auth_oauth2_transport_config_init(
    vectis_auth_oauth2_transport_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->user_agent = "vectis";
}

void vectis_auth_oauth2_client_credentials_config_init(
    vectis_auth_oauth2_client_credentials_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  vectis_auth_oauth2_transport_config_init(&config->transport);
}

void vectis_auth_oauth2_token_response_init(
    vectis_auth_oauth2_token_response *response) {
  if (response == NULL) {
    return;
  }
  memset(response, 0, sizeof(*response));
}

void vectis_auth_oauth2_token_response_cleanup(
    vectis_auth_oauth2_token_response *response) {
  if (response == NULL) {
    return;
  }
  free(response->access_token);
  free(response->token_type);
  free(response->refresh_token);
  free(response->scope);
  free(response->id_token);
  vectis_auth_oauth2_token_response_init(response);
}

void vectis_auth_oauth2_token_flow_init(vectis_auth_oauth2_token_flow *flow) {
  if (flow == NULL) {
    return;
  }
  memset(flow, 0, sizeof(*flow));
}

void vectis_auth_oauth2_token_flow_cleanup(
    vectis_auth_oauth2_token_flow *flow) {
  if (flow == NULL) {
    return;
  }
  free(flow->access_token);
  free(flow->token_type);
  free(flow->refresh_token);
  free(flow->scope);
  free(flow->id_token);
  vectis_auth_oauth2_token_flow_init(flow);
}

void vectis_auth_oauth2_token_flow_policy_init(
    vectis_auth_oauth2_token_flow_policy *policy) {
  if (policy == NULL) {
    return;
  }
  memset(policy, 0, sizeof(*policy));
  vectis_auth_oauth2_transport_config_init(&policy->transport);
  policy->refresh_skew_seconds = 60;
  policy->max_retries = 2u;
}

void vectis_auth_oauth2_token_flow_result_init(
    vectis_auth_oauth2_token_flow_result *result) {
  if (result == NULL) {
    return;
  }
  memset(result, 0, sizeof(*result));
}

void vectis_auth_oauth2_stored_token_flow_init(
    vectis_auth_oauth2_stored_token_flow *flow) {
  if (flow == NULL) {
    return;
  }
  memset(flow, 0, sizeof(*flow));
  vectis_auth_oauth2_token_flow_init(&flow->flow);
}

void vectis_auth_oauth2_stored_token_flow_cleanup(
    vectis_auth_oauth2_stored_token_flow *flow) {
  if (flow == NULL) {
    return;
  }
  free(flow->flow_id);
  free(flow->subject);
  free(flow->webdav_client_id);
  vectis_auth_oauth2_token_flow_cleanup(&flow->flow);
  vectis_auth_oauth2_stored_token_flow_init(flow);
}

void vectis_auth_oauth2_token_flow_store_config_init(
    vectis_auth_oauth2_token_flow_store_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  vectis_auth_store_config_init(&config->store);
  vectis_auth_oauth2_token_flow_init(&config->flow);
}

void vectis_auth_oauth2_stored_token_flow_policy_init(
    vectis_auth_oauth2_stored_token_flow_policy *policy) {
  if (policy == NULL) {
    return;
  }
  memset(policy, 0, sizeof(*policy));
  vectis_auth_store_config_init(&policy->store);
  vectis_auth_oauth2_token_flow_policy_init(&policy->flow_policy);
  policy->revoke_webdav_keys_on_failure = 1;
}

void vectis_auth_oauth2_webdav_key_config_init(
    vectis_auth_oauth2_webdav_key_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  vectis_auth_store_config_init(&config->store);
}

void vectis_auth_oidc_authorization_config_init(
    vectis_auth_oidc_authorization_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->scope = "openid";
}

void vectis_auth_oidc_authorization_init(
    vectis_auth_oidc_authorization *authorization) {
  if (authorization == NULL) {
    return;
  }
  memset(authorization, 0, sizeof(*authorization));
}

void vectis_auth_oidc_authorization_cleanup(
    vectis_auth_oidc_authorization *authorization) {
  if (authorization == NULL) {
    return;
  }
  free(authorization->authorization_url);
  free(authorization->code_verifier);
  free(authorization->code_challenge);
  free(authorization->state);
  free(authorization->nonce);
  vectis_auth_oidc_authorization_init(authorization);
}

void vectis_auth_oidc_token_exchange_config_init(
    vectis_auth_oidc_token_exchange_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  vectis_auth_oauth2_transport_config_init(&config->transport);
}

void vectis_auth_oidc_token_exchange_init(
    vectis_auth_oidc_token_exchange *exchange) {
  if (exchange == NULL) {
    return;
  }
  memset(exchange, 0, sizeof(*exchange));
  vectis_auth_oauth2_token_response_init(&exchange->token);
  vectis_auth_oauth2_token_flow_init(&exchange->flow);
}

void vectis_auth_oidc_token_exchange_cleanup(
    vectis_auth_oidc_token_exchange *exchange) {
  if (exchange == NULL) {
    return;
  }
  free(exchange->code);
  free(exchange->state);
  vectis_auth_oauth2_token_response_cleanup(&exchange->token);
  vectis_auth_oauth2_token_flow_cleanup(&exchange->flow);
  vectis_auth_oidc_token_exchange_init(exchange);
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

static vectis_status vectis_auth_generate_oidc_value(char *out, size_t out_size,
                                                     vectis_error *error) {
  unsigned char bytes[VECTIS_AUTH_RANDOM_OIDC_BYTES];

  if (out == NULL || out_size < VECTIS_AUTH_RANDOM_OIDC_BYTES * 2u + 1u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OIDC random value buffer is too small");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_auth_random_bytes(bytes, sizeof(bytes), error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  vectis_auth_hex_encode(bytes, sizeof(bytes), out);
  OPENSSL_cleanse(bytes, sizeof(bytes));
  return VECTIS_OK;
}

static vectis_status
vectis_auth_generate_email_token_value(char *out, size_t out_size,
                                       vectis_error *error) {
  unsigned char bytes[VECTIS_AUTH_RANDOM_EMAIL_TOKEN_BYTES];

  if (out == NULL ||
      out_size < VECTIS_AUTH_RANDOM_EMAIL_TOKEN_BYTES * 2u + 1u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "email token buffer is too small");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_auth_random_bytes(bytes, sizeof(bytes), error) != VECTIS_OK) {
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  vectis_auth_hex_encode(bytes, sizeof(bytes), out);
  OPENSSL_cleanse(bytes, sizeof(bytes));
  return VECTIS_OK;
}

static vectis_status vectis_auth_token_sha256_hex(const char *token, char *out,
                                                  size_t out_size,
                                                  vectis_error *error) {
  unsigned char digest[VECTIS_AUTH_PASSWORD_HASH_BYTES];
  unsigned int digest_len;

  if (token == NULL || token[0] == '\0' || out == NULL ||
      out_size < sizeof(digest) * 2u + 1u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth token and output are required");
    return VECTIS_ERR_INVALID;
  }
  digest_len = 0u;
  if (EVP_Digest(token, strlen(token), digest, &digest_len, EVP_sha256(),
                 NULL) != 1 ||
      digest_len != sizeof(digest)) {
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to hash auth token");
    OPENSSL_cleanse(digest, sizeof(digest));
    return VECTIS_ERR_STATE;
  }
  vectis_auth_hex_encode(digest, sizeof(digest), out);
  OPENSSL_cleanse(digest, sizeof(digest));
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

static void vectis_auth_oauth2_lonejson_error(lonejson_error *error,
                                              lonejson_status status,
                                              const char *message) {
  if (error == NULL) {
    return;
  }
  lonejson_error_init(error);
  error->code = status;
  if (message != NULL) {
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
  }
}

static int vectis_auth_oauth2_http_method(const char *method,
                                          vectis_http_method *out) {
  if (out == NULL) {
    return 0;
  }
  if (method == NULL || strcmp(method, "GET") == 0) {
    *out = VECTIS_HTTP_GET;
    return 1;
  }
  if (strcmp(method, "POST") == 0) {
    *out = VECTIS_HTTP_POST;
    return 1;
  }
  if (strcmp(method, "DELETE") == 0) {
    *out = VECTIS_HTTP_DELETE;
    return 1;
  }
  if (strcmp(method, "HEAD") == 0) {
    *out = VECTIS_HTTP_HEAD;
    return 1;
  }
  if (strcmp(method, "OPTIONS") == 0) {
    *out = VECTIS_HTTP_OPTIONS;
    return 1;
  }
  if (strcmp(method, "PUT") == 0) {
    *out = VECTIS_HTTP_PUT;
    return 1;
  }
  if (strcmp(method, "PATCH") == 0) {
    *out = VECTIS_HTTP_PATCH;
    return 1;
  }
  return 0;
}

static vectis_status vectis_auth_oauth2_response_append(const void *data,
                                                        size_t size,
                                                        void *userdata,
                                                        vectis_error *error) {
  vectis_auth_oauth2_response_buffer *buffer;
  unsigned char *grown;
  size_t new_size;
  size_t new_capacity;

  buffer = (vectis_auth_oauth2_response_buffer *)userdata;
  if (buffer == NULL || (data == NULL && size > 0u)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 HTTP response buffer is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (size == 0u) {
    return VECTIS_OK;
  }
  if (buffer->max_size != 0u && size > buffer->max_size - buffer->size) {
    buffer->failed = 1;
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 HTTP response exceeds configured limit");
    return VECTIS_ERR_INVALID;
  }
  new_size = buffer->size + size;
  if (new_size + 1u < new_size) {
    buffer->failed = 1;
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "OAuth2 HTTP response size overflow");
    return VECTIS_ERR_NOMEM;
  }
  if (new_size + 1u > buffer->capacity) {
    new_capacity = buffer->capacity != 0u ? buffer->capacity : 256u;
    while (new_capacity < new_size + 1u) {
      if (new_capacity > ((size_t)-1) / 2u) {
        buffer->failed = 1;
        vectis_set_error(error, VECTIS_ERR_NOMEM,
                         "OAuth2 HTTP response capacity overflow");
        return VECTIS_ERR_NOMEM;
      }
      new_capacity *= 2u;
    }
    grown = (unsigned char *)realloc(buffer->data, new_capacity);
    if (grown == NULL) {
      buffer->failed = 1;
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to allocate OAuth2 HTTP response buffer");
      return VECTIS_ERR_NOMEM;
    }
    buffer->data = grown;
    buffer->capacity = new_capacity;
  }
  memcpy(buffer->data + buffer->size, data, size);
  buffer->size = new_size;
  buffer->data[buffer->size] = '\0';
  return VECTIS_OK;
}

static char *vectis_auth_oauth2_header(const char *name, const char *value) {
  char *out;
  size_t name_len;
  size_t value_len;
  size_t len;

  if (name == NULL || value == NULL || value[0] == '\0') {
    return NULL;
  }
  name_len = strlen(name);
  value_len = strlen(value);
  len = name_len + 2u + value_len + 1u;
  out = (char *)malloc(len);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, name, name_len);
  memcpy(out + name_len, ": ", 2u);
  memcpy(out + name_len + 2u, value, value_len + 1u);
  return out;
}

static vectis_status vectis_auth_oauth2_http_execute(
    const vectis_auth_oauth2_transport_config *config,
    const lonejson_http_request *request, vectis_auth_oauth2_http_response *out,
    vectis_error *error) {
  vectis_http_request http_request;
  vectis_http_response http_response;
  vectis_auth_oauth2_response_buffer body;
  const char *headers[2];
  char *authorization_header;
  char *user_agent_header;
  vectis_status status;
  size_t header_count;

  vectis_http_request_init(&http_request);
  memset(&http_response, 0, sizeof(http_response));
  memset(&body, 0, sizeof(body));
  headers[0] = NULL;
  headers[1] = NULL;
  authorization_header = NULL;
  user_agent_header = NULL;
  if (request->authorization != NULL) {
    authorization_header =
        vectis_auth_oauth2_header("Authorization", request->authorization);
    if (authorization_header == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to allocate OAuth2 authorization header");
      return VECTIS_ERR_NOMEM;
    }
  }
  if (request->user_agent != NULL) {
    user_agent_header =
        vectis_auth_oauth2_header("User-Agent", request->user_agent);
    if (user_agent_header == NULL) {
      free(authorization_header);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to allocate OAuth2 user-agent header");
      return VECTIS_ERR_NOMEM;
    }
  }
  header_count = 0u;
  if (authorization_header != NULL) {
    headers[header_count++] = authorization_header;
  }
  if (user_agent_header != NULL) {
    headers[header_count++] = user_agent_header;
  }
  body.max_size = request->max_response_bytes;
  if (!vectis_auth_oauth2_http_method(request->method, &http_request.method)) {
    free(authorization_header);
    free(user_agent_header);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 HTTP request method is unsupported");
    return VECTIS_ERR_INVALID;
  }
  http_request.url = request->url;
  http_request.headers = headers;
  http_request.header_count = header_count;
  http_request.body = request->body;
  http_request.body_size = request->body_len;
  http_request.content_type = request->content_type;
  http_request.response_body = vectis_auth_oauth2_response_append;
  http_request.response_body_userdata = &body;
  status = vectis_http_execute(config != NULL ? config->http_client : NULL,
                               &http_request, &http_response, error);
  free(authorization_header);
  free(user_agent_header);
  if (status != VECTIS_OK) {
    free(body.data);
    vectis_http_response_cleanup(&http_response);
    return status;
  }
  out->status_code = http_response.status_code;
  out->content_type = vectis_auth_strdup(http_response.content_type);
  out->body = body.data;
  out->body_size = body.size;
  body.data = NULL;
  if (http_response.content_type != NULL && out->content_type == NULL) {
    vectis_http_response_cleanup(&http_response);
    vectis_auth_oauth2_http_response_cleanup(out);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy OAuth2 response content type");
    return VECTIS_ERR_NOMEM;
  }
  vectis_http_response_cleanup(&http_response);
  return VECTIS_OK;
}

static lonejson_status vectis_auth_oauth2_lonejson_http_request(
    void *user_data, const lonejson_http_request *request,
    lonejson_http_response *response, lonejson_error *error) {
  vectis_auth_oauth2_http_adapter *adapter;
  vectis_auth_oauth2_http_request public_request;
  vectis_auth_oauth2_http_response public_response;
  vectis_status status;
  lonejson_status json_status;

  adapter = (vectis_auth_oauth2_http_adapter *)user_data;
  if (adapter == NULL || request == NULL || response == NULL) {
    vectis_auth_oauth2_lonejson_error(error, LONEJSON_STATUS_INVALID_ARGUMENT,
                                      "OAuth2 HTTP adapter input is required");
    return LONEJSON_STATUS_INVALID_ARGUMENT;
  }
  vectis_auth_oauth2_http_response_init(&public_response);
  if (adapter->config != NULL && adapter->config->request != NULL) {
    memset(&public_request, 0, sizeof(public_request));
    public_request.method = request->method;
    public_request.url = request->url;
    public_request.content_type = request->content_type;
    public_request.authorization = request->authorization;
    public_request.user_agent = request->user_agent;
    public_request.body = request->body;
    public_request.body_size = request->body_len;
    public_request.max_response_bytes = request->max_response_bytes;
    status = adapter->config->request(&public_request, &public_response,
                                      adapter->config->request_userdata,
                                      &adapter->error);
  } else {
    status = vectis_auth_oauth2_http_execute(adapter->config, request,
                                             &public_response, &adapter->error);
  }
  if (status != VECTIS_OK) {
    vectis_auth_oauth2_lonejson_error(error, LONEJSON_STATUS_CALLBACK_FAILED,
                                      adapter->error.message[0] != '\0'
                                          ? adapter->error.message
                                          : "OAuth2 HTTP request failed");
    vectis_auth_oauth2_http_response_cleanup(&public_response);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (request->max_response_bytes != 0u &&
      public_response.body_size > request->max_response_bytes) {
    vectis_auth_oauth2_lonejson_error(
        error, LONEJSON_STATUS_OVERFLOW,
        "OAuth2 HTTP response exceeds configured limit");
    vectis_auth_oauth2_http_response_cleanup(&public_response);
    return LONEJSON_STATUS_OVERFLOW;
  }
  lonejson_http_response_init(response);
  response->status_code = public_response.status_code;
  json_status = lonejson_owned_buffer_sink(
      &response->body, public_response.body, public_response.body_size, error);
  vectis_auth_oauth2_http_response_cleanup(&public_response);
  return json_status;
}

static vectis_status vectis_auth_oauth2_install_http_provider(
    lonejson *runtime, const vectis_auth_oauth2_transport_config *config,
    vectis_auth_oauth2_http_adapter *adapter, lonejson_http_provider *provider,
    vectis_error *error) {
  lonejson_error json_error;
  lonejson_status json_status;

  if (runtime == NULL || adapter == NULL || provider == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 HTTP provider input is required");
    return VECTIS_ERR_INVALID;
  }
  memset(adapter, 0, sizeof(*adapter));
  adapter->config = config;
  vectis_error_clear(&adapter->error);
  lonejson_error_init(&json_error);
  json_status = lonejson_http_provider_init_simple(
      provider, adapter,
      config != NULL && config->user_agent != NULL ? config->user_agent
                                                   : "vectis",
      vectis_auth_oauth2_lonejson_http_request, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    json_status = lonejson_set_http_provider(runtime, provider, &json_error);
  }
  if (json_status != LONEJSON_STATUS_OK) {
    return vectis_auth_lonejson_error(error, json_status, &json_error,
                                      "failed to install OAuth2 HTTP provider");
  }
  return VECTIS_OK;
}

static vectis_status vectis_auth_oauth2_copy_token_response(
    const lonejson_oauth2_token_response *source,
    vectis_auth_oauth2_token_response *out, vectis_error *error) {
  if (source == NULL || out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 token response output is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_oauth2_token_response_cleanup(out);
  out->access_token = vectis_auth_strdup(source->access_token);
  out->token_type = vectis_auth_strdup(source->token_type);
  out->refresh_token = vectis_auth_strdup(source->refresh_token);
  out->scope = vectis_auth_strdup(source->scope);
  out->id_token = vectis_auth_strdup(source->id_token);
  out->expires_in = (int64_t)source->expires_in;
  out->has_expires_in = source->has_expires_in;
  if ((source->access_token != NULL && out->access_token == NULL) ||
      (source->token_type != NULL && out->token_type == NULL) ||
      (source->refresh_token != NULL && out->refresh_token == NULL) ||
      (source->scope != NULL && out->scope == NULL) ||
      (source->id_token != NULL && out->id_token == NULL)) {
    vectis_auth_oauth2_token_response_cleanup(out);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy OAuth2 token response");
    return VECTIS_ERR_NOMEM;
  }
  return VECTIS_OK;
}

static vectis_status vectis_auth_oauth2_copy_token_flow_from_lonejson(
    const lonejson_oauth2_token_flow *source,
    vectis_auth_oauth2_token_flow *out, vectis_error *error) {
  if (source == NULL || out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 token flow output is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_oauth2_token_flow_cleanup(out);
  out->access_token = vectis_auth_strdup(source->access_token);
  out->token_type = vectis_auth_strdup(source->token_type);
  out->refresh_token = vectis_auth_strdup(source->refresh_token);
  out->scope = vectis_auth_strdup(source->scope);
  out->id_token = vectis_auth_strdup(source->id_token);
  out->expires_at = (int64_t)source->expires_at;
  out->has_expires_at = source->has_expires_at;
  if ((source->access_token != NULL && out->access_token == NULL) ||
      (source->token_type != NULL && out->token_type == NULL) ||
      (source->refresh_token != NULL && out->refresh_token == NULL) ||
      (source->scope != NULL && out->scope == NULL) ||
      (source->id_token != NULL && out->id_token == NULL)) {
    vectis_auth_oauth2_token_flow_cleanup(out);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy OAuth2 token flow");
    return VECTIS_ERR_NOMEM;
  }
  return VECTIS_OK;
}

static vectis_status vectis_auth_oauth2_copy_token_flow_public(
    const vectis_auth_oauth2_token_flow *source,
    vectis_auth_oauth2_token_flow *out, vectis_error *error) {
  if (source == NULL || out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 token flow output is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_oauth2_token_flow_cleanup(out);
  out->access_token = vectis_auth_strdup(source->access_token);
  out->token_type = vectis_auth_strdup(source->token_type);
  out->refresh_token = vectis_auth_strdup(source->refresh_token);
  out->scope = vectis_auth_strdup(source->scope);
  out->id_token = vectis_auth_strdup(source->id_token);
  out->expires_at = source->expires_at;
  out->has_expires_at = source->has_expires_at;
  if ((source->access_token != NULL && out->access_token == NULL) ||
      (source->token_type != NULL && out->token_type == NULL) ||
      (source->refresh_token != NULL && out->refresh_token == NULL) ||
      (source->scope != NULL && out->scope == NULL) ||
      (source->id_token != NULL && out->id_token == NULL)) {
    vectis_auth_oauth2_token_flow_cleanup(out);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy OAuth2 token flow");
    return VECTIS_ERR_NOMEM;
  }
  return VECTIS_OK;
}

static vectis_status vectis_auth_oauth2_copy_stored_flow(
    const vectis_auth_oauth2_flow_record *source,
    vectis_auth_oauth2_stored_token_flow *out, vectis_error *error) {
  vectis_status status;

  if (source == NULL || out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 stored token flow output is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_oauth2_stored_token_flow_cleanup(out);
  out->found = source->found;
  if (!source->found) {
    return VECTIS_OK;
  }
  out->flow_id = vectis_auth_strdup(source->flow_id);
  if (source->subject[0] != '\0') {
    out->subject = vectis_auth_strdup(source->subject);
  }
  if (source->webdav_client_id[0] != '\0') {
    out->webdav_client_id = vectis_auth_strdup(source->webdav_client_id);
  }
  status = vectis_auth_oauth2_copy_token_flow_public(&source->flow, &out->flow,
                                                     error);
  if (out->flow_id == NULL ||
      (source->subject[0] != '\0' && out->subject == NULL) ||
      (source->webdav_client_id[0] != '\0' && out->webdav_client_id == NULL) ||
      status != VECTIS_OK) {
    vectis_auth_oauth2_stored_token_flow_cleanup(out);
    if (status == VECTIS_OK) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy OAuth2 stored token flow");
      status = VECTIS_ERR_NOMEM;
    }
  }
  return status;
}

static vectis_status vectis_auth_oauth2_copy_stored_flow_public(
    const vectis_auth_oauth2_stored_token_flow *source,
    vectis_auth_oauth2_stored_token_flow *out, vectis_error *error) {
  vectis_status status;

  if (source == NULL || out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 stored token flow output is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_oauth2_stored_token_flow_cleanup(out);
  out->found = source->found;
  if (!source->found) {
    return VECTIS_OK;
  }
  out->flow_id = vectis_auth_strdup(source->flow_id);
  if (source->subject != NULL) {
    out->subject = vectis_auth_strdup(source->subject);
  }
  if (source->webdav_client_id != NULL) {
    out->webdav_client_id = vectis_auth_strdup(source->webdav_client_id);
  }
  status = vectis_auth_oauth2_copy_token_flow_public(&source->flow, &out->flow,
                                                     error);
  if (out->flow_id == NULL ||
      (source->subject != NULL && out->subject == NULL) ||
      (source->webdav_client_id != NULL && out->webdav_client_id == NULL) ||
      status != VECTIS_OK) {
    vectis_auth_oauth2_stored_token_flow_cleanup(out);
    if (status == VECTIS_OK) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy OAuth2 stored token flow");
      status = VECTIS_ERR_NOMEM;
    }
  }
  return status;
}

static void vectis_auth_oauth2_lonejson_flow_from_public(
    const vectis_auth_oauth2_token_flow *source,
    lonejson_oauth2_token_flow *out) {
  lonejson_oauth2_token_flow_init(out);
  if (source == NULL) {
    return;
  }
  out->access_token = source->access_token;
  out->token_type = source->token_type;
  out->refresh_token = source->refresh_token;
  out->scope = source->scope;
  out->id_token = source->id_token;
  out->expires_at = (lonejson_int64)source->expires_at;
  out->has_expires_at = source->has_expires_at;
}

static vectis_auth_oauth2_token_flow_state
vectis_auth_oauth2_flow_state_from_lonejson(
    lonejson_oauth2_token_flow_state state) {
  switch (state) {
  case LONEJSON_OAUTH2_TOKEN_FLOW_REFRESHED:
    return VECTIS_AUTH_OAUTH2_TOKEN_FLOW_REFRESHED;
  case LONEJSON_OAUTH2_TOKEN_FLOW_NEEDS_INTERACTION:
    return VECTIS_AUTH_OAUTH2_TOKEN_FLOW_NEEDS_INTERACTION;
  case LONEJSON_OAUTH2_TOKEN_FLOW_FAILED:
    return VECTIS_AUTH_OAUTH2_TOKEN_FLOW_FAILED;
  case LONEJSON_OAUTH2_TOKEN_FLOW_READY:
  default:
    return VECTIS_AUTH_OAUTH2_TOKEN_FLOW_READY;
  }
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
  if (status == LONEJSON_STATUS_OK && cfg->oauth2_flow_id != NULL &&
      cfg->oauth2_flow_id[0] != '\0') {
    status = lonejson_writer_key(&writer, "oauth2_flow_id", 14u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && cfg->oauth2_flow_id != NULL &&
      cfg->oauth2_flow_id[0] != '\0') {
    status = lonejson_writer_string(&writer, cfg->oauth2_flow_id,
                                    strlen(cfg->oauth2_flow_id), &json_error);
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
  memset(&lock, 0, sizeof(lock));
  lock.fd = -1;
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

static vectis_status vectis_auth_build_oauth2_flow_record_json(
    lonejson *runtime, const vectis_auth_oauth2_token_flow_store_config *config,
    lonejson_owned_buffer *out, vectis_error *error) {
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;

  if (runtime == NULL || config == NULL || out == NULL ||
      config->flow_id == NULL || config->flow_id[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 token flow_id is required");
    return VECTIS_ERR_INVALID;
  }
  lonejson_error_init(&json_error);
  status = lonejson_writer_init_sink(
      runtime, &writer, lonejson_owned_buffer_sink, out, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "flow_id", 7u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, config->flow_id,
                                    strlen(config->flow_id), &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->subject != NULL &&
      config->subject[0] != '\0') {
    status = lonejson_writer_key(&writer, "subject", 7u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->subject != NULL &&
      config->subject[0] != '\0') {
    status = lonejson_writer_string(&writer, config->subject,
                                    strlen(config->subject), &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->webdav_client_id != NULL &&
      config->webdav_client_id[0] != '\0') {
    status = lonejson_writer_key(&writer, "webdav_client_id", 16u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->webdav_client_id != NULL &&
      config->webdav_client_id[0] != '\0') {
    status =
        lonejson_writer_string(&writer, config->webdav_client_id,
                               strlen(config->webdav_client_id), &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->flow.access_token != NULL) {
    status = lonejson_writer_key(&writer, "access_token", 12u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->flow.access_token != NULL) {
    status =
        lonejson_writer_string(&writer, config->flow.access_token,
                               strlen(config->flow.access_token), &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->flow.token_type != NULL) {
    status = lonejson_writer_key(&writer, "token_type", 10u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->flow.token_type != NULL) {
    status =
        lonejson_writer_string(&writer, config->flow.token_type,
                               strlen(config->flow.token_type), &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->flow.refresh_token != NULL) {
    status = lonejson_writer_key(&writer, "refresh_token", 13u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->flow.refresh_token != NULL) {
    status =
        lonejson_writer_string(&writer, config->flow.refresh_token,
                               strlen(config->flow.refresh_token), &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->flow.scope != NULL) {
    status = lonejson_writer_key(&writer, "scope", 5u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->flow.scope != NULL) {
    status = lonejson_writer_string(&writer, config->flow.scope,
                                    strlen(config->flow.scope), &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->flow.id_token != NULL) {
    status = lonejson_writer_key(&writer, "id_token", 8u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->flow.id_token != NULL) {
    status = lonejson_writer_string(&writer, config->flow.id_token,
                                    strlen(config->flow.id_token), &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "has_expires_at", 14u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status =
        lonejson_writer_bool(&writer, config->flow.has_expires_at, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->flow.has_expires_at) {
    status = lonejson_writer_key(&writer, "expires_at", 10u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->flow.has_expires_at) {
    status = lonejson_writer_i64(&writer, config->flow.expires_at, &json_error);
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
                                      "failed to build OAuth2 token flow");
  }
  return VECTIS_OK;
}

static vectis_status vectis_auth_build_email_token_record_json(
    lonejson *runtime, const char *transaction_id, const char *username,
    const char *realm, const char *email, const char *pending_transaction_id,
    const char *token_hash, int64_t expires_at, unsigned int failed_attempts,
    unsigned int max_attempts, lonejson_owned_buffer *out,
    vectis_error *error) {
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;

  if (runtime == NULL || transaction_id == NULL || transaction_id[0] == '\0' ||
      username == NULL || username[0] == '\0' || realm == NULL ||
      realm[0] == '\0' || email == NULL || email[0] == '\0' ||
      token_hash == NULL || token_hash[0] == '\0' || out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth email token record fields are required");
    return VECTIS_ERR_INVALID;
  }
  lonejson_error_init(&json_error);
  status = lonejson_writer_init_sink(
      runtime, &writer, lonejson_owned_buffer_sink, out, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "transaction_id", 14u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, transaction_id,
                                    strlen(transaction_id), &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "username", 8u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, username, strlen(username),
                                    &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "realm", 5u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, realm, strlen(realm), &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "email", 5u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, email, strlen(email), &json_error);
  }
  if (status == LONEJSON_STATUS_OK && pending_transaction_id != NULL &&
      pending_transaction_id[0] != '\0') {
    status = lonejson_writer_key(&writer, "pending_transaction_id", 22u,
                                 &json_error);
  }
  if (status == LONEJSON_STATUS_OK && pending_transaction_id != NULL &&
      pending_transaction_id[0] != '\0') {
    status =
        lonejson_writer_string(&writer, pending_transaction_id,
                               strlen(pending_transaction_id), &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "token_hash", 10u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, token_hash, strlen(token_hash),
                                    &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "expires_at", 10u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_i64(&writer, expires_at, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "failed_attempts", 15u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status =
        lonejson_writer_i64(&writer, (int64_t)failed_attempts, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "max_attempts", 12u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_i64(&writer, (int64_t)max_attempts, &json_error);
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
                                      "failed to build auth email token");
  }
  return VECTIS_OK;
}

static vectis_status vectis_auth_build_pending_login_record_json(
    lonejson *runtime, const char *transaction_id, const char *username,
    const char *realm, int totp_required, int64_t expires_at,
    lonejson_owned_buffer *out, vectis_error *error) {
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;

  if (runtime == NULL || transaction_id == NULL || transaction_id[0] == '\0' ||
      username == NULL || username[0] == '\0' || realm == NULL ||
      realm[0] == '\0' || out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth pending login record fields are required");
    return VECTIS_ERR_INVALID;
  }
  lonejson_error_init(&json_error);
  status = lonejson_writer_init_sink(
      runtime, &writer, lonejson_owned_buffer_sink, out, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "transaction_id", 14u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, transaction_id,
                                    strlen(transaction_id), &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "username", 8u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, username, strlen(username),
                                    &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "realm", 5u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, realm, strlen(realm), &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "totp_required", 13u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_bool(&writer, totp_required ? 1 : 0, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "expires_at", 10u, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_i64(&writer, expires_at, &json_error);
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
                                      "failed to build auth pending login");
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
        &writer, store_json, store_len, NULL, 0u, user_json, user_len, NULL, 0u,
        NULL, 0u, NULL, 0u, &json_error);
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

static vectis_status vectis_auth_write_store_with_flow_locked(
    const vectis_auth_store_config *config, const char *store_json,
    size_t store_len, const char *flow_json, size_t flow_len,
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
        &writer, store_json, store_len, NULL, 0u, NULL, 0u, flow_json, flow_len,
        NULL, 0u, NULL, 0u, &json_error);
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

static vectis_status vectis_auth_write_store_with_email_token_locked(
    const vectis_auth_store_config *config, const char *store_json,
    size_t store_len, const char *token_json, size_t token_len,
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
        &writer, store_json, store_len, NULL, 0u, NULL, 0u, NULL, 0u,
        token_json, token_len, NULL, 0u, &json_error);
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

static vectis_status vectis_auth_write_store_with_pending_login_locked(
    const vectis_auth_store_config *config, const char *store_json,
    size_t store_len, const char *pending_json, size_t pending_len,
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
        &writer, store_json, store_len, NULL, 0u, NULL, 0u, NULL, 0u, NULL, 0u,
        pending_json, pending_len, &json_error);
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

static vectis_status vectis_auth_drop_oauth2_flow_to_temp_locked(
    const vectis_auth_store_config *store_config, lonejson *runtime,
    const char *flow_id, char *temp_path, size_t temp_path_size,
    vectis_error *error) {
  lonejson_json_value item_value;
  lonejson_array_rewrite_options options;
  vectis_auth_oauth2_flow_drop_state state;
  lonejson_error json_error;
  lonejson_status json_status;
  int written;

  written = snprintf(temp_path, temp_path_size, "%s.oauth2.%ld",
                     store_config->credentials_path, (long)getpid());
  if (written < 0 || (size_t)written >= temp_path_size) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 token flow temp path is too long");
    return VECTIS_ERR_INVALID;
  }
  memset(&state, 0, sizeof(state));
  state.runtime = runtime;
  state.flow_id = flow_id;
  memset(&options, 0, sizeof(options));
  lonejson_json_value_init(runtime, &item_value);
  lonejson_error_init(&json_error);
  json_status =
      lonejson_json_value_enable_parse_capture(&item_value, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    options.item_value = &item_value;
    options.item = vectis_auth_oauth2_flow_drop_item;
    options.user = &state;
    json_status = lonejson_array_rewrite_path(runtime, "oauth2_flows",
                                              store_config->credentials_path,
                                              temp_path, &options, &json_error);
  }
  lonejson_json_value_cleanup(&item_value);
  if (json_status != LONEJSON_STATUS_OK) {
    (void)unlink(temp_path);
    return vectis_auth_lonejson_error(error, json_status, &json_error,
                                      "failed to rewrite OAuth2 token flows");
  }
  return VECTIS_OK;
}

static vectis_status vectis_auth_find_oauth2_flow_locked(
    const vectis_auth_store_config *store_config, lonejson *runtime,
    const char *flow_id, vectis_auth_oauth2_flow_record *out,
    vectis_error *error) {
  lonejson_json_value item_value;
  lonejson_array_rewrite_options options;
  vectis_auth_oauth2_flow_find_state state;
  lonejson_error json_error;
  lonejson_status json_status;
  char temp_path[4096];
  int written;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 token flow output is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_oauth2_flow_record_init(out);
  written = snprintf(temp_path, sizeof(temp_path), "%s.oauth2.find.%ld",
                     store_config->credentials_path, (long)getpid());
  if (written < 0 || (size_t)written >= sizeof(temp_path)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 token flow lookup temp path is too long");
    return VECTIS_ERR_INVALID;
  }
  memset(&state, 0, sizeof(state));
  state.runtime = runtime;
  state.flow_id = flow_id;
  vectis_auth_oauth2_flow_record_init(&state.record);
  memset(&options, 0, sizeof(options));
  lonejson_json_value_init(runtime, &item_value);
  lonejson_error_init(&json_error);
  json_status =
      lonejson_json_value_enable_parse_capture(&item_value, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    options.item_value = &item_value;
    options.item = vectis_auth_oauth2_flow_find_item;
    options.user = &state;
    json_status = lonejson_array_rewrite_path(runtime, "oauth2_flows",
                                              store_config->credentials_path,
                                              temp_path, &options, &json_error);
  }
  lonejson_json_value_cleanup(&item_value);
  (void)unlink(temp_path);
  if (json_status != LONEJSON_STATUS_OK) {
    vectis_auth_oauth2_flow_record_cleanup(&state.record);
    return vectis_auth_lonejson_error(error, json_status, &json_error,
                                      "failed to read OAuth2 token flows");
  }
  *out = state.record;
  return VECTIS_OK;
}

static vectis_status vectis_auth_revoke_oauth2_flow_credentials_locked(
    const vectis_auth_store_config *store_config, lonejson *runtime,
    const char *flow_id, vectis_error *error) {
  lonejson_json_value item_value;
  lonejson_array_rewrite_options options;
  vectis_auth_oauth2_webdav_revoke_state state;
  lonejson_error json_error;
  lonejson_status json_status;
  char temp_path[4096];
  int written;

  written = snprintf(temp_path, sizeof(temp_path), "%s.oauth2.revoke.%ld",
                     store_config->credentials_path, (long)getpid());
  if (written < 0 || (size_t)written >= sizeof(temp_path)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 credential revoke temp path is too long");
    return VECTIS_ERR_INVALID;
  }
  memset(&state, 0, sizeof(state));
  state.runtime = runtime;
  state.flow_id = flow_id;
  memset(&options, 0, sizeof(options));
  lonejson_json_value_init(runtime, &item_value);
  lonejson_error_init(&json_error);
  json_status =
      lonejson_json_value_enable_parse_capture(&item_value, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    options.item_value = &item_value;
    options.item = vectis_auth_oauth2_webdav_revoke_item;
    options.user = &state;
    json_status = lonejson_array_rewrite_path(runtime, "credentials",
                                              store_config->credentials_path,
                                              temp_path, &options, &json_error);
  }
  lonejson_json_value_cleanup(&item_value);
  if (json_status != LONEJSON_STATUS_OK) {
    (void)unlink(temp_path);
    return vectis_auth_lonejson_error(error, json_status, &json_error,
                                      "failed to revoke OAuth2 credentials");
  }
  if (!state.matched) {
    (void)unlink(temp_path);
    return VECTIS_OK;
  }
  if (rename(temp_path, store_config->credentials_path) != 0) {
    (void)unlink(temp_path);
    return vectis_auth_set_errno(error, "failed to replace auth store",
                                 store_config->credentials_path);
  }
  return VECTIS_OK;
}

static vectis_status vectis_auth_drop_email_token_to_temp_locked(
    const vectis_auth_store_config *store_config, lonejson *runtime,
    const char *transaction_id, char *temp_path, size_t temp_path_size,
    vectis_error *error) {
  lonejson_json_value item_value;
  lonejson_array_rewrite_options options;
  vectis_auth_email_token_drop_state state;
  lonejson_error json_error;
  lonejson_status json_status;
  int written;

  written = snprintf(temp_path, temp_path_size, "%s.email_tokens.%ld",
                     store_config->credentials_path, (long)getpid());
  if (written < 0 || (size_t)written >= temp_path_size) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth email-token temp path is too long");
    return VECTIS_ERR_INVALID;
  }
  memset(&state, 0, sizeof(state));
  state.runtime = runtime;
  state.transaction_id = transaction_id;
  memset(&options, 0, sizeof(options));
  lonejson_json_value_init(runtime, &item_value);
  lonejson_error_init(&json_error);
  json_status =
      lonejson_json_value_enable_parse_capture(&item_value, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    options.item_value = &item_value;
    options.item = vectis_auth_email_token_drop_item;
    options.user = &state;
    json_status = lonejson_array_rewrite_path(runtime, "email_tokens",
                                              store_config->credentials_path,
                                              temp_path, &options, &json_error);
  }
  lonejson_json_value_cleanup(&item_value);
  if (json_status != LONEJSON_STATUS_OK) {
    (void)unlink(temp_path);
    return vectis_auth_lonejson_error(error, json_status, &json_error,
                                      "failed to rewrite auth email tokens");
  }
  return VECTIS_OK;
}

static vectis_status vectis_auth_find_email_token_locked(
    const vectis_auth_store_config *store_config, lonejson *runtime,
    const char *transaction_id, vectis_auth_email_token_record *out,
    vectis_error *error) {
  lonejson_json_value item_value;
  lonejson_array_rewrite_options options;
  vectis_auth_email_token_find_state state;
  lonejson_error json_error;
  lonejson_status json_status;
  char temp_path[4096];
  int written;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth email-token output is required");
    return VECTIS_ERR_INVALID;
  }
  memset(out, 0, sizeof(*out));
  written = snprintf(temp_path, sizeof(temp_path), "%s.email_tokens.find.%ld",
                     store_config->credentials_path, (long)getpid());
  if (written < 0 || (size_t)written >= sizeof(temp_path)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth email-token lookup temp path is too long");
    return VECTIS_ERR_INVALID;
  }
  memset(&state, 0, sizeof(state));
  state.runtime = runtime;
  state.transaction_id = transaction_id;
  memset(&options, 0, sizeof(options));
  lonejson_json_value_init(runtime, &item_value);
  lonejson_error_init(&json_error);
  json_status =
      lonejson_json_value_enable_parse_capture(&item_value, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    options.item_value = &item_value;
    options.item = vectis_auth_email_token_find_item;
    options.user = &state;
    json_status = lonejson_array_rewrite_path(runtime, "email_tokens",
                                              store_config->credentials_path,
                                              temp_path, &options, &json_error);
  }
  lonejson_json_value_cleanup(&item_value);
  (void)unlink(temp_path);
  if (json_status != LONEJSON_STATUS_OK) {
    return vectis_auth_lonejson_error(error, json_status, &json_error,
                                      "failed to read auth email tokens");
  }
  *out = state.record;
  return VECTIS_OK;
}

static vectis_status vectis_auth_drop_pending_login_to_temp_locked(
    const vectis_auth_store_config *store_config, lonejson *runtime,
    const char *transaction_id, char *temp_path, size_t temp_path_size,
    vectis_error *error) {
  lonejson_json_value item_value;
  lonejson_array_rewrite_options options;
  vectis_auth_pending_login_drop_state state;
  lonejson_error json_error;
  lonejson_status json_status;
  int written;

  written = snprintf(temp_path, temp_path_size, "%s.pending_logins.%ld",
                     store_config->credentials_path, (long)getpid());
  if (written < 0 || (size_t)written >= temp_path_size) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth pending-login temp path is too long");
    return VECTIS_ERR_INVALID;
  }
  memset(&state, 0, sizeof(state));
  state.runtime = runtime;
  state.transaction_id = transaction_id;
  memset(&options, 0, sizeof(options));
  lonejson_json_value_init(runtime, &item_value);
  lonejson_error_init(&json_error);
  json_status =
      lonejson_json_value_enable_parse_capture(&item_value, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    options.item_value = &item_value;
    options.item = vectis_auth_pending_login_drop_item;
    options.user = &state;
    json_status = lonejson_array_rewrite_path(runtime, "pending_logins",
                                              store_config->credentials_path,
                                              temp_path, &options, &json_error);
  }
  lonejson_json_value_cleanup(&item_value);
  if (json_status != LONEJSON_STATUS_OK) {
    (void)unlink(temp_path);
    return vectis_auth_lonejson_error(error, json_status, &json_error,
                                      "failed to rewrite auth pending logins");
  }
  return VECTIS_OK;
}

static vectis_status vectis_auth_find_pending_login_locked(
    const vectis_auth_store_config *store_config, lonejson *runtime,
    const char *transaction_id, vectis_auth_pending_login_record *out,
    vectis_error *error) {
  lonejson_json_value item_value;
  lonejson_array_rewrite_options options;
  vectis_auth_pending_login_find_state state;
  lonejson_error json_error;
  lonejson_status json_status;
  char temp_path[4096];
  int written;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth pending-login output is required");
    return VECTIS_ERR_INVALID;
  }
  memset(out, 0, sizeof(*out));
  written = snprintf(temp_path, sizeof(temp_path), "%s.pending_logins.find.%ld",
                     store_config->credentials_path, (long)getpid());
  if (written < 0 || (size_t)written >= sizeof(temp_path)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth pending-login lookup temp path is too long");
    return VECTIS_ERR_INVALID;
  }
  memset(&state, 0, sizeof(state));
  state.runtime = runtime;
  state.transaction_id = transaction_id;
  memset(&options, 0, sizeof(options));
  lonejson_json_value_init(runtime, &item_value);
  lonejson_error_init(&json_error);
  json_status =
      lonejson_json_value_enable_parse_capture(&item_value, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    options.item_value = &item_value;
    options.item = vectis_auth_pending_login_find_item;
    options.user = &state;
    json_status = lonejson_array_rewrite_path(runtime, "pending_logins",
                                              store_config->credentials_path,
                                              temp_path, &options, &json_error);
  }
  lonejson_json_value_cleanup(&item_value);
  (void)unlink(temp_path);
  if (json_status != LONEJSON_STATUS_OK) {
    return vectis_auth_lonejson_error(error, json_status, &json_error,
                                      "failed to read auth pending logins");
  }
  *out = state.record;
  return VECTIS_OK;
}

vectis_status vectis_auth_email_token_issue(
    const vectis_auth_email_token_issue_config *config,
    vectis_auth_email_token *out, vectis_error *error) {
  vectis_auth_store_lock lock;
  vectis_auth_store_config temp_config;
  lonejson *runtime;
  lonejson_owned_buffer token_json;
  vectis_status status;
  char *store_json;
  char transaction_id[2u * VECTIS_AUTH_RANDOM_OIDC_BYTES + 1u];
  char token[2u * VECTIS_AUTH_RANDOM_EMAIL_TOKEN_BYTES + 1u];
  char token_hash[2u * VECTIS_AUTH_PASSWORD_HASH_BYTES + 1u];
  char temp_path[4096];
  const char *effective_realm;
  uint64_t now;
  uint64_t ttl;
  uint64_t expires_at;
  size_t store_len;
  unsigned int max_attempts;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth email token output is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_email_token_init(out);
  memset(&lock, 0, sizeof(lock));
  lock.fd = -1;
  if (config == NULL || config->username == NULL ||
      config->username[0] == '\0' || config->email == NULL ||
      config->email[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth email token username and email are required");
    return VECTIS_ERR_INVALID;
  }
  effective_realm = config->realm != NULL && config->realm[0] != '\0'
                        ? config->realm
                        : "vectis";
  transaction_id[0] = '\0';
  token[0] = '\0';
  if (config->transaction_id != NULL && config->transaction_id[0] != '\0') {
    vectis_auth_copy_fixed(transaction_id, sizeof(transaction_id),
                           config->transaction_id);
  } else {
    status = vectis_auth_generate_oidc_value(transaction_id,
                                             sizeof(transaction_id), error);
    if (status != VECTIS_OK) {
      return status;
    }
  }
  if (config->token != NULL && config->token[0] != '\0') {
    vectis_auth_copy_fixed(token, sizeof(token), config->token);
  } else {
    status =
        vectis_auth_generate_email_token_value(token, sizeof(token), error);
    if (status != VECTIS_OK) {
      return status;
    }
  }
  status = vectis_auth_token_sha256_hex(token, token_hash, sizeof(token_hash),
                                        error);
  if (status != VECTIS_OK) {
    OPENSSL_cleanse(token, sizeof(token));
    return status;
  }
  now = config->now_seconds != 0u ? config->now_seconds : (uint64_t)time(NULL);
  ttl = config->ttl_seconds != 0u ? config->ttl_seconds
                                  : VECTIS_AUTH_EMAIL_TOKEN_DEFAULT_TTL_SECONDS;
  max_attempts = config->max_attempts != 0u
                     ? config->max_attempts
                     : VECTIS_AUTH_EMAIL_TOKEN_DEFAULT_MAX_ATTEMPTS;
  if (ttl > (uint64_t)INT64_MAX || now > (uint64_t)INT64_MAX - ttl) {
    OPENSSL_cleanse(token, sizeof(token));
    OPENSSL_cleanse(token_hash, sizeof(token_hash));
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth email token expiry overflows");
    return VECTIS_ERR_INVALID;
  }
  expires_at = now + ttl;
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status != VECTIS_OK) {
    OPENSSL_cleanse(token, sizeof(token));
    OPENSSL_cleanse(token_hash, sizeof(token_hash));
    return status;
  }
  lonejson_owned_buffer_init(&token_json);
  status = vectis_auth_build_email_token_record_json(
      runtime, transaction_id, config->username, effective_realm, config->email,
      config->pending_transaction_id, token_hash, (int64_t)expires_at, 0u,
      max_attempts, &token_json, error);
  if (status != VECTIS_OK) {
    lonejson_owned_buffer_free(&token_json);
    lonejson_free(runtime);
    OPENSSL_cleanse(token, sizeof(token));
    OPENSSL_cleanse(token_hash, sizeof(token_hash));
    return status;
  }
  status = vectis_auth_lock_open(&config->store, &lock, error);
  if (status == VECTIS_OK) {
    store_json = NULL;
    store_len = 0u;
    status = vectis_auth_read_store_locked(&config->store, &store_json,
                                           &store_len, error);
  } else {
    store_json = NULL;
    store_len = 0u;
  }
  if (status == VECTIS_OK && store_json == NULL) {
    status = vectis_auth_write_empty_store_locked(&config->store, error);
  }
  if (status == VECTIS_OK) {
    status = vectis_auth_drop_email_token_to_temp_locked(
        &config->store, runtime, transaction_id, temp_path, sizeof(temp_path),
        error);
  }
  if (status == VECTIS_OK) {
    temp_config = config->store;
    temp_config.credentials_path = temp_path;
    free(store_json);
    store_json = NULL;
    store_len = 0u;
    status = vectis_auth_read_store_locked(&temp_config, &store_json,
                                           &store_len, error);
  }
  if (status == VECTIS_OK) {
    status = vectis_auth_write_store_with_email_token_locked(
        &config->store, store_json, store_len, token_json.data, token_json.len,
        error);
  }
  if (status == VECTIS_OK) {
    out->transaction_id = vectis_auth_strdup(transaction_id);
    out->token = vectis_auth_strdup(token);
    out->expires_at = expires_at;
    if (out->transaction_id == NULL || out->token == NULL) {
      vectis_auth_email_token_cleanup(out);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy auth email token");
      status = VECTIS_ERR_NOMEM;
    }
  }
  if (lock.fd >= 0) {
    vectis_auth_lock_close(&lock);
  }
  lonejson_owned_buffer_free(&token_json);
  lonejson_free(runtime);
  free(store_json);
  OPENSSL_cleanse(token, sizeof(token));
  OPENSSL_cleanse(token_hash, sizeof(token_hash));
  return status;
}

vectis_status vectis_auth_email_token_verify(
    const vectis_auth_email_token_verify_config *config,
    vectis_auth_email_token_result *out, vectis_error *error) {
  vectis_auth_store_lock lock;
  vectis_auth_store_config temp_config;
  vectis_auth_email_token_record record;
  lonejson *runtime;
  lonejson_owned_buffer token_json;
  vectis_status status;
  char token_hash[2u * VECTIS_AUTH_PASSWORD_HASH_BYTES + 1u];
  char temp_path[4096];
  char *store_json;
  const char *effective_realm;
  const char *required_pending_transaction_id;
  uint64_t now;
  size_t store_len;
  unsigned int next_failed_attempts;
  int drop_record;
  int replace_record;
  int token_matches;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth email token result is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_email_token_result_init(out);
  lonejson_owned_buffer_init(&token_json);
  memset(&lock, 0, sizeof(lock));
  lock.fd = -1;
  temp_path[0] = '\0';
  store_json = NULL;
  if (config == NULL || config->transaction_id == NULL ||
      config->transaction_id[0] == '\0' || config->username == NULL ||
      config->username[0] == '\0' || config->token == NULL ||
      config->token[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth email token transaction, username, and token are "
                     "required");
    return VECTIS_ERR_INVALID;
  }
  effective_realm = config->realm != NULL && config->realm[0] != '\0'
                        ? config->realm
                        : "vectis";
  required_pending_transaction_id =
      config->pending_transaction_id != NULL &&
              config->pending_transaction_id[0] != '\0'
          ? config->pending_transaction_id
          : "";
  status = vectis_auth_token_sha256_hex(config->token, token_hash,
                                        sizeof(token_hash), error);
  if (status != VECTIS_OK) {
    return status;
  }
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status != VECTIS_OK) {
    OPENSSL_cleanse(token_hash, sizeof(token_hash));
    return status;
  }
  status = vectis_auth_lock_open(&config->store, &lock, error);
  if (status == VECTIS_OK) {
    status = vectis_auth_find_email_token_locked(
        &config->store, runtime, config->transaction_id, &record, error);
  }
  drop_record = 0;
  replace_record = 0;
  next_failed_attempts = 0u;
  if (status == VECTIS_OK && record.found &&
      strcmp(record.username, config->username) == 0 &&
      strcmp(record.realm, effective_realm) == 0 &&
      strcmp(record.pending_transaction_id, required_pending_transaction_id) ==
          0) {
    now =
        config->now_seconds != 0u ? config->now_seconds : (uint64_t)time(NULL);
    token_matches = strcmp(record.token_hash, token_hash) == 0;
    out->failed_attempts = record.failed_attempts;
    out->max_attempts = record.max_attempts;
    if (now > (uint64_t)record.expires_at) {
      out->expired = 1;
      drop_record = 1;
    } else if (token_matches) {
      out->verified = 1;
      drop_record = 1;
    } else {
      next_failed_attempts = record.failed_attempts == UINT_MAX
                                 ? UINT_MAX
                                 : record.failed_attempts + 1u;
      out->failed_attempts = next_failed_attempts;
      if (next_failed_attempts >= record.max_attempts) {
        drop_record = 1;
      } else {
        replace_record = 1;
      }
    }
    out->username = vectis_auth_strdup(record.username);
    out->realm = vectis_auth_strdup(record.realm);
    out->email = vectis_auth_strdup(record.email);
    out->pending_transaction_id =
        record.pending_transaction_id[0] != '\0'
            ? vectis_auth_strdup(record.pending_transaction_id)
            : NULL;
    if (out->username == NULL || out->realm == NULL || out->email == NULL ||
        (record.pending_transaction_id[0] != '\0' &&
         out->pending_transaction_id == NULL)) {
      vectis_auth_email_token_result_cleanup(out);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy auth email token result");
      status = VECTIS_ERR_NOMEM;
    }
  }
  if (status == VECTIS_OK && replace_record) {
    status = vectis_auth_build_email_token_record_json(
        runtime, record.transaction_id, record.username, record.realm,
        record.email, record.pending_transaction_id, record.token_hash,
        record.expires_at, next_failed_attempts, record.max_attempts,
        &token_json, error);
    if (status == VECTIS_OK) {
      status = vectis_auth_drop_email_token_to_temp_locked(
          &config->store, runtime, config->transaction_id, temp_path,
          sizeof(temp_path), error);
    }
    if (status == VECTIS_OK) {
      temp_config = config->store;
      temp_config.credentials_path = temp_path;
      store_len = 0u;
      status = vectis_auth_read_store_locked(&temp_config, &store_json,
                                             &store_len, error);
    }
    if (status == VECTIS_OK) {
      status = vectis_auth_write_store_with_email_token_locked(
          &config->store, store_json, store_len, token_json.data,
          token_json.len, error);
    }
    if (temp_path[0] != '\0') {
      (void)unlink(temp_path);
      temp_path[0] = '\0';
    }
  }
  if (status == VECTIS_OK && drop_record) {
    status = vectis_auth_drop_email_token_to_temp_locked(
        &config->store, runtime, config->transaction_id, temp_path,
        sizeof(temp_path), error);
    if (status == VECTIS_OK &&
        rename(temp_path, config->store.credentials_path) != 0) {
      (void)unlink(temp_path);
      status =
          vectis_auth_set_errno(error, "failed to consume auth email token",
                                config->store.credentials_path);
    }
  }
  if (lock.fd >= 0) {
    vectis_auth_lock_close(&lock);
  }
  lonejson_owned_buffer_free(&token_json);
  lonejson_free(runtime);
  free(store_json);
  OPENSSL_cleanse(token_hash, sizeof(token_hash));
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
vectis_auth_user_exists(const vectis_auth_store_config *store_config,
                        const char *username, int *out_exists,
                        vectis_error *error) {
  vectis_auth_store_lock lock;
  vectis_auth_user_record record;
  lonejson *runtime;
  vectis_status status;
  char *store_json;
  size_t store_len;

  if (out_exists == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth user exists output is required");
    return VECTIS_ERR_INVALID;
  }
  *out_exists = 0;
  if (username == NULL || username[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "auth username is required");
    return VECTIS_ERR_INVALID;
  }
  memset(&lock, 0, sizeof(lock));
  lock.fd = -1;
  status = vectis_auth_lock_open(store_config, &lock, error);
  if (status != VECTIS_OK) {
    return status;
  }
  store_json = NULL;
  store_len = 0u;
  status = vectis_auth_read_store_locked(store_config, &store_json, &store_len,
                                         error);
  free(store_json);
  if (status == VECTIS_OK && store_len == 0u) {
    vectis_auth_lock_close(&lock);
    return VECTIS_OK;
  }
  runtime = NULL;
  if (status == VECTIS_OK) {
    status = vectis_auth_lonejson_runtime(&runtime, error);
  }
  if (status == VECTIS_OK) {
    status = vectis_auth_find_user_locked(store_config, runtime, username,
                                          &record, error);
  }
  vectis_auth_lock_close(&lock);
  if (runtime != NULL) {
    lonejson_free(runtime);
  }
  if (status == VECTIS_OK) {
    *out_exists = record.found ? 1 : 0;
  }
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

vectis_status
vectis_auth_user_password_check(const vectis_auth_password_check_config *config,
                                vectis_auth_password_check_result *out,
                                vectis_error *error) {
  vectis_auth_store_lock lock;
  vectis_auth_user_record record;
  lonejson *runtime;
  vectis_status status;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth password-check result is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_password_check_result_init(out);
  if (config == NULL || config->username == NULL ||
      config->username[0] == '\0' || config->password == NULL ||
      config->password[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth password-check username and password are required");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_auth_lock_open(&config->store, &lock, error);
  if (status != VECTIS_OK) {
    return status;
  }
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status == VECTIS_OK) {
    status = vectis_auth_find_user_locked(&config->store, runtime,
                                          config->username, &record, error);
  }
  vectis_auth_lock_close(&lock);
  if (runtime != NULL) {
    lonejson_free(runtime);
  }
  if (status != VECTIS_OK) {
    return status;
  }
  if (record.found && vectis_auth_verify_password(&record, config->password)) {
    out->authenticated = 1;
    out->totp_required = record.totp_enabled ? 1 : 0;
  }
  return VECTIS_OK;
}

vectis_status vectis_auth_pending_login_issue(
    const vectis_auth_pending_login_issue_config *config,
    vectis_auth_pending_login *out, vectis_error *error) {
  vectis_auth_store_lock lock;
  vectis_auth_store_config temp_config;
  vectis_auth_user_record record;
  lonejson *runtime;
  lonejson_owned_buffer pending_json;
  vectis_status status;
  char *store_json;
  char transaction_id[2u * VECTIS_AUTH_RANDOM_OIDC_BYTES + 1u];
  char temp_path[4096];
  const char *effective_realm;
  uint64_t now;
  uint64_t ttl;
  uint64_t expires_at;
  size_t store_len;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth pending login output is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_pending_login_init(out);
  memset(&lock, 0, sizeof(lock));
  lock.fd = -1;
  if (config == NULL || config->username == NULL ||
      config->username[0] == '\0' || config->password == NULL ||
      config->password[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth pending login username and password are required");
    return VECTIS_ERR_INVALID;
  }
  effective_realm = config->realm != NULL && config->realm[0] != '\0'
                        ? config->realm
                        : "vectis";
  transaction_id[0] = '\0';
  if (config->transaction_id != NULL && config->transaction_id[0] != '\0') {
    vectis_auth_copy_fixed(transaction_id, sizeof(transaction_id),
                           config->transaction_id);
  } else {
    status = vectis_auth_generate_oidc_value(transaction_id,
                                             sizeof(transaction_id), error);
    if (status != VECTIS_OK) {
      return status;
    }
  }
  now = config->now_seconds != 0u ? config->now_seconds : (uint64_t)time(NULL);
  ttl = config->ttl_seconds != 0u
            ? config->ttl_seconds
            : VECTIS_AUTH_PENDING_LOGIN_DEFAULT_TTL_SECONDS;
  if (ttl > (uint64_t)INT64_MAX || now > (uint64_t)INT64_MAX - ttl) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth pending login expiry overflows");
    return VECTIS_ERR_INVALID;
  }
  expires_at = now + ttl;
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status != VECTIS_OK) {
    return status;
  }
  store_json = NULL;
  store_len = 0u;
  lonejson_owned_buffer_init(&pending_json);
  status = vectis_auth_lock_open(&config->store, &lock, error);
  if (status == VECTIS_OK) {
    status = vectis_auth_read_store_locked(&config->store, &store_json,
                                           &store_len, error);
  }
  if (status == VECTIS_OK && store_json == NULL) {
    status = vectis_auth_write_empty_store_locked(&config->store, error);
  }
  if (status == VECTIS_OK) {
    status = vectis_auth_find_user_locked(&config->store, runtime,
                                          config->username, &record, error);
  }
  if (status == VECTIS_OK &&
      (!record.found ||
       !vectis_auth_verify_password(&record, config->password))) {
    vectis_auth_lock_close(&lock);
    lock.fd = -1;
    lonejson_owned_buffer_free(&pending_json);
    lonejson_free(runtime);
    free(store_json);
    return VECTIS_OK;
  }
  if (status == VECTIS_OK) {
    status = vectis_auth_build_pending_login_record_json(
        runtime, transaction_id, config->username, effective_realm,
        record.totp_enabled, (int64_t)expires_at, &pending_json, error);
  }
  if (status == VECTIS_OK) {
    status = vectis_auth_drop_pending_login_to_temp_locked(
        &config->store, runtime, transaction_id, temp_path, sizeof(temp_path),
        error);
  }
  if (status == VECTIS_OK) {
    temp_config = config->store;
    temp_config.credentials_path = temp_path;
    free(store_json);
    store_json = NULL;
    store_len = 0u;
    status = vectis_auth_read_store_locked(&temp_config, &store_json,
                                           &store_len, error);
  }
  if (status == VECTIS_OK) {
    status = vectis_auth_write_store_with_pending_login_locked(
        &config->store, store_json, store_len, pending_json.data,
        pending_json.len, error);
  }
  if (status == VECTIS_OK) {
    out->transaction_id = vectis_auth_strdup(transaction_id);
    out->username = vectis_auth_strdup(config->username);
    out->realm = vectis_auth_strdup(effective_realm);
    out->expires_at = expires_at;
    out->totp_required = record.totp_enabled ? 1 : 0;
    out->authenticated = out->transaction_id != NULL && out->username != NULL &&
                         out->realm != NULL;
    if (!out->authenticated) {
      vectis_auth_pending_login_cleanup(out);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy auth pending login");
      status = VECTIS_ERR_NOMEM;
    }
  }
  if (lock.fd >= 0) {
    vectis_auth_lock_close(&lock);
  }
  lonejson_owned_buffer_free(&pending_json);
  lonejson_free(runtime);
  free(store_json);
  return status;
}

static vectis_status vectis_auth_pending_login_check(
    const vectis_auth_pending_login_consume_config *config,
    vectis_auth_pending_login_result *out, int consume_record,
    vectis_error *error) {
  vectis_auth_store_lock lock;
  vectis_auth_pending_login_record record;
  vectis_auth_user_record user;
  lonejson *runtime;
  vectis_totp totp;
  vectis_status status;
  char temp_path[4096];
  const char *effective_realm;
  uint64_t now;
  int drop_record;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "auth pending login result is required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_pending_login_result_init(out);
  memset(&lock, 0, sizeof(lock));
  lock.fd = -1;
  if (config == NULL || config->transaction_id == NULL ||
      config->transaction_id[0] == '\0' || config->username == NULL ||
      config->username[0] == '\0') {
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "auth pending login transaction and username are required");
    return VECTIS_ERR_INVALID;
  }
  effective_realm = config->realm != NULL && config->realm[0] != '\0'
                        ? config->realm
                        : "vectis";
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_auth_lock_open(&config->store, &lock, error);
  if (status == VECTIS_OK) {
    status = vectis_auth_find_pending_login_locked(
        &config->store, runtime, config->transaction_id, &record, error);
  }
  drop_record = 0;
  if (status == VECTIS_OK && record.found &&
      strcmp(record.username, config->username) == 0 &&
      strcmp(record.realm, effective_realm) == 0) {
    now =
        config->now_seconds != 0u ? config->now_seconds : (uint64_t)time(NULL);
    out->totp_required = record.totp_required ? 1 : 0;
    out->username = vectis_auth_strdup(record.username);
    out->realm = vectis_auth_strdup(record.realm);
    if (out->username == NULL || out->realm == NULL) {
      vectis_auth_pending_login_result_cleanup(out);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy auth pending login result");
      status = VECTIS_ERR_NOMEM;
    } else if (now > (uint64_t)record.expires_at) {
      out->expired = 1;
      drop_record = 1;
    } else if (!record.totp_required) {
      out->authenticated = 1;
      drop_record = 1;
    }
  }
  if (status == VECTIS_OK && record.found && !out->authenticated &&
      !out->expired && record.totp_required &&
      strcmp(record.username, config->username) == 0 &&
      strcmp(record.realm, effective_realm) == 0 && config->totp_code != NULL &&
      config->totp_code[0] != '\0') {
    status = vectis_auth_find_user_locked(&config->store, runtime,
                                          config->username, &user, error);
    if (status == VECTIS_OK && user.found && user.totp_enabled &&
        vectis_totp_init(&totp, user.totp_secret) == VECTIS_TOTP_QR_OK &&
        vectis_totp_validate(&totp, config->totp_code,
                             config->now_seconds != 0u ? config->now_seconds
                                                       : (uint64_t)time(NULL),
                             config->totp_window != 0u ? config->totp_window
                                                       : 1u)) {
      out->authenticated = 1;
      drop_record = 1;
    }
  }
  if (status == VECTIS_OK && record.found && record.totp_required &&
      !out->authenticated && !out->expired && config->totp_code != NULL &&
      config->totp_code[0] != '\0') {
    drop_record = 1;
  }
  if (status == VECTIS_OK && consume_record && drop_record) {
    status = vectis_auth_drop_pending_login_to_temp_locked(
        &config->store, runtime, config->transaction_id, temp_path,
        sizeof(temp_path), error);
    if (status == VECTIS_OK &&
        rename(temp_path, config->store.credentials_path) != 0) {
      (void)unlink(temp_path);
      status =
          vectis_auth_set_errno(error, "failed to consume auth pending login",
                                config->store.credentials_path);
    }
  }
  if (lock.fd >= 0) {
    vectis_auth_lock_close(&lock);
  }
  lonejson_free(runtime);
  return status;
}

vectis_status vectis_auth_pending_login_verify(
    const vectis_auth_pending_login_consume_config *config,
    vectis_auth_pending_login_result *out, vectis_error *error) {
  return vectis_auth_pending_login_check(config, out, 0, error);
}

vectis_status vectis_auth_pending_login_consume(
    const vectis_auth_pending_login_consume_config *config,
    vectis_auth_pending_login_result *out, vectis_error *error) {
  return vectis_auth_pending_login_check(config, out, 1, error);
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

vectis_status vectis_auth_oauth2_client_credentials_request(
    const vectis_auth_oauth2_client_credentials_config *config,
    vectis_auth_oauth2_token_response *out, vectis_error *error) {
  lonejson *runtime;
  lonejson_http_provider provider;
  vectis_auth_oauth2_http_adapter adapter;
  lonejson_oauth2_client_credentials request;
  lonejson_oauth2_token_response response;
  lonejson_error json_error;
  lonejson_status json_status;
  vectis_status status;

  if (config == NULL || out == NULL || config->token_endpoint == NULL ||
      config->token_endpoint[0] == '\0' || config->client_id == NULL ||
      config->client_id[0] == '\0' || config->client_secret == NULL ||
      config->client_secret[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 client credentials require token endpoint, "
                     "client_id, and client_secret");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_oauth2_token_response_init(out);
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_auth_oauth2_install_http_provider(runtime, &config->transport,
                                                    &adapter, &provider, error);
  if (status != VECTIS_OK) {
    lonejson_free(runtime);
    return status;
  }
  memset(&request, 0, sizeof(request));
  request.client_id = config->client_id;
  request.client_secret = config->client_secret;
  request.scope = config->scope;
  request.audience = config->audience;
  request.resource = config->resource;
  request.max_body_bytes = config->max_body_bytes;
  lonejson_oauth2_token_response_init(&response);
  lonejson_error_init(&json_error);
  json_status = lonejson_oauth2_client_credentials_request(
      runtime, config->token_endpoint, &request, config->max_response_bytes,
      &response, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    status = vectis_auth_oauth2_copy_token_response(&response, out, error);
  } else {
    status =
        vectis_auth_lonejson_error(error, json_status, &json_error,
                                   "OAuth2 client credentials request failed");
  }
  lonejson_oauth2_token_response_cleanup(&response);
  lonejson_free(runtime);
  return status;
}

vectis_status vectis_auth_oauth2_token_flow_ensure(
    vectis_auth_oauth2_token_flow *flow,
    const vectis_auth_oauth2_token_flow_policy *policy,
    vectis_auth_oauth2_token_flow_result *result, vectis_error *error) {
  lonejson *runtime;
  lonejson_http_provider provider;
  vectis_auth_oauth2_http_adapter adapter;
  lonejson_oauth2_token_flow source_flow;
  lonejson_oauth2_token_flow working_flow;
  lonejson_oauth2_token_flow_policy lonejson_policy;
  lonejson_oauth2_token_flow_result lonejson_result;
  lonejson_error json_error;
  lonejson_status json_status;
  vectis_status status;

  if (flow == NULL || policy == NULL || result == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 token flow, policy, and result are required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_oauth2_token_flow_result_init(result);
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_auth_oauth2_install_http_provider(runtime, &policy->transport,
                                                    &adapter, &provider, error);
  if (status != VECTIS_OK) {
    lonejson_free(runtime);
    return status;
  }
  vectis_auth_oauth2_lonejson_flow_from_public(flow, &source_flow);
  lonejson_oauth2_token_flow_init(&working_flow);
  lonejson_error_init(&json_error);
  json_status = lonejson_oauth2_token_flow_assign(&working_flow, &source_flow,
                                                  &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    memset(&lonejson_policy, 0, sizeof(lonejson_policy));
    lonejson_policy.token_endpoint = policy->token_endpoint;
    lonejson_policy.client_id = policy->client_id;
    lonejson_policy.client_secret = policy->client_secret;
    lonejson_policy.scope = policy->scope;
    lonejson_policy.now = (lonejson_int64)policy->now;
    lonejson_policy.refresh_skew_seconds =
        (lonejson_int64)policy->refresh_skew_seconds;
    lonejson_policy.max_response_bytes = policy->max_response_bytes;
    lonejson_policy.max_retries = policy->max_retries;
    lonejson_policy.disable_refresh = policy->disable_refresh;
    lonejson_policy.disable_retry = policy->disable_retry;
    memset(&lonejson_result, 0, sizeof(lonejson_result));
    json_status = lonejson_oauth2_token_flow_ensure(
        runtime, &working_flow, &lonejson_policy, &lonejson_result,
        &json_error);
  }
  if (json_status == LONEJSON_STATUS_OK) {
    result->state =
        vectis_auth_oauth2_flow_state_from_lonejson(lonejson_result.state);
    result->attempts = lonejson_result.attempts;
    result->refreshed = lonejson_result.refreshed;
    status = vectis_auth_oauth2_copy_token_flow_from_lonejson(&working_flow,
                                                              flow, error);
  } else {
    status = vectis_auth_lonejson_error(error, json_status, &json_error,
                                        "OAuth2 token flow ensure failed");
  }
  lonejson_oauth2_token_flow_cleanup(&working_flow);
  lonejson_free(runtime);
  return status;
}

vectis_status vectis_auth_oauth2_token_flow_upsert(
    const vectis_auth_oauth2_token_flow_store_config *config,
    vectis_error *error) {
  vectis_auth_store_lock lock;
  vectis_auth_store_config temp_config;
  lonejson *runtime;
  lonejson_owned_buffer flow_json;
  vectis_status status;
  char temp_path[4096];
  char *store_json;
  size_t store_len;

  if (config == NULL || config->flow_id == NULL || config->flow_id[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 token flow_id is required");
    return VECTIS_ERR_INVALID;
  }
  lock.fd = -1;
  lock.path = NULL;
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status != VECTIS_OK) {
    return status;
  }
  lonejson_owned_buffer_init(&flow_json);
  status = vectis_auth_build_oauth2_flow_record_json(runtime, config,
                                                     &flow_json, error);
  if (status != VECTIS_OK) {
    lonejson_owned_buffer_free(&flow_json);
    lonejson_free(runtime);
    return status;
  }
  status = vectis_auth_lock_open(&config->store, &lock, error);
  if (status == VECTIS_OK) {
    store_json = NULL;
    store_len = 0u;
    status = vectis_auth_read_store_locked(&config->store, &store_json,
                                           &store_len, error);
    if (status == VECTIS_OK && store_json == NULL) {
      status = vectis_auth_write_empty_store_locked(&config->store, error);
    }
    free(store_json);
  }
  if (status == VECTIS_OK) {
    status = vectis_auth_drop_oauth2_flow_to_temp_locked(
        &config->store, runtime, config->flow_id, temp_path, sizeof(temp_path),
        error);
  }
  if (status == VECTIS_OK) {
    temp_config = config->store;
    temp_config.credentials_path = temp_path;
    store_json = NULL;
    store_len = 0u;
    status = vectis_auth_read_store_locked(&temp_config, &store_json,
                                           &store_len, error);
    if (status == VECTIS_OK) {
      status = vectis_auth_write_store_with_flow_locked(
          &config->store, store_json, store_len, flow_json.data, flow_json.len,
          error);
    }
    free(store_json);
    (void)unlink(temp_path);
  }
  if (lock.fd >= 0) {
    vectis_auth_lock_close(&lock);
  }
  lonejson_owned_buffer_free(&flow_json);
  lonejson_free(runtime);
  return status;
}

vectis_status vectis_auth_oauth2_token_flow_load(
    const vectis_auth_store_config *store_config, const char *flow_id,
    vectis_auth_oauth2_stored_token_flow *out, vectis_error *error) {
  vectis_auth_store_lock lock;
  vectis_auth_oauth2_flow_record record;
  lonejson *runtime;
  vectis_status status;

  if (flow_id == NULL || flow_id[0] == '\0' || out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 token flow_id and output are required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_oauth2_stored_token_flow_init(out);
  status = vectis_auth_lock_open(store_config, &lock, error);
  if (status != VECTIS_OK) {
    return status;
  }
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status == VECTIS_OK) {
    vectis_auth_oauth2_flow_record_init(&record);
    status = vectis_auth_find_oauth2_flow_locked(store_config, runtime, flow_id,
                                                 &record, error);
    if (status == VECTIS_OK) {
      status = vectis_auth_oauth2_copy_stored_flow(&record, out, error);
    }
    vectis_auth_oauth2_flow_record_cleanup(&record);
  }
  if (runtime != NULL) {
    lonejson_free(runtime);
  }
  vectis_auth_lock_close(&lock);
  return status;
}

vectis_status vectis_auth_oauth2_stored_token_flow_ensure(
    const vectis_auth_oauth2_stored_token_flow_policy *policy,
    vectis_auth_oauth2_stored_token_flow *out,
    vectis_auth_oauth2_token_flow_result *result, vectis_error *error) {
  vectis_auth_oauth2_stored_token_flow loaded;
  vectis_auth_oauth2_token_flow_store_config store_update;
  vectis_auth_store_lock lock;
  lonejson *runtime;
  vectis_status status;
  vectis_status revoke_status;
  int revoke;

  if (policy == NULL || policy->flow_id == NULL || policy->flow_id[0] == '\0' ||
      out == NULL || result == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 stored token flow policy, id, and outputs are "
                     "required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_oauth2_stored_token_flow_init(out);
  vectis_auth_oauth2_token_flow_result_init(result);
  vectis_auth_oauth2_stored_token_flow_init(&loaded);
  status = vectis_auth_oauth2_token_flow_load(&policy->store, policy->flow_id,
                                              &loaded, error);
  if (status != VECTIS_OK) {
    vectis_auth_oauth2_stored_token_flow_cleanup(&loaded);
    return status;
  }
  if (!loaded.found) {
    vectis_auth_oauth2_stored_token_flow_cleanup(&loaded);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 token flow was not found");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_auth_oauth2_token_flow_ensure(
      &loaded.flow, &policy->flow_policy, result, error);
  revoke = status != VECTIS_OK ||
           (result->state != VECTIS_AUTH_OAUTH2_TOKEN_FLOW_READY &&
            result->state != VECTIS_AUTH_OAUTH2_TOKEN_FLOW_REFRESHED);
  if (!revoke) {
    vectis_auth_oauth2_token_flow_store_config_init(&store_update);
    store_update.store = policy->store;
    store_update.flow_id = loaded.flow_id;
    store_update.subject = loaded.subject;
    store_update.webdav_client_id = loaded.webdav_client_id;
    store_update.flow = loaded.flow;
    status = vectis_auth_oauth2_token_flow_upsert(&store_update, error);
    if (status == VECTIS_OK) {
      status = vectis_auth_oauth2_copy_stored_flow_public(&loaded, out, error);
    }
    vectis_auth_oauth2_stored_token_flow_cleanup(&loaded);
    return status;
  }
  if (policy->revoke_webdav_keys_on_failure) {
    revoke_status = vectis_auth_lock_open(&policy->store, &lock, error);
    if (revoke_status == VECTIS_OK) {
      runtime = NULL;
      revoke_status = vectis_auth_lonejson_runtime(&runtime, error);
      if (revoke_status == VECTIS_OK) {
        revoke_status = vectis_auth_revoke_oauth2_flow_credentials_locked(
            &policy->store, runtime, policy->flow_id, error);
        lonejson_free(runtime);
      }
      vectis_auth_lock_close(&lock);
    }
    if (revoke_status != VECTIS_OK) {
      vectis_auth_oauth2_stored_token_flow_cleanup(&loaded);
      return revoke_status;
    }
  }
  vectis_auth_oauth2_stored_token_flow_cleanup(&loaded);
  return status;
}

vectis_status vectis_auth_issue_webdav_key_for_oauth2_flow(
    const vectis_auth_oauth2_webdav_key_config *config,
    vectis_auth_issued_credential *out, vectis_error *error) {
  vectis_auth_issue_config issue;
  vectis_status status;

  if (config == NULL || config->flow_id == NULL || config->flow_id[0] == '\0' ||
      config->subject == NULL || config->subject[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 WebDAV key flow_id and subject are required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_issue_config_init(&issue);
  issue.subject = config->subject;
  issue.purpose = "webdav";
  issue.oauth2_flow_id = config->flow_id;
  issue.auth_modes = VECTIS_AUTH_MODE_BASIC;
  issue.max_record_bytes = config->max_record_bytes;
  status = vectis_auth_issue_credential(&config->store, &issue, out, error);
  return status;
}

vectis_status vectis_auth_oidc_authorization_start(
    const vectis_auth_oidc_authorization_config *config,
    vectis_auth_oidc_authorization *out, vectis_error *error) {
  lonejson *runtime;
  lonejson_oidc_pkce pkce;
  lonejson_oidc_authorization_request request;
  lonejson_owned_buffer challenge;
  lonejson_owned_buffer url;
  lonejson_error json_error;
  lonejson_status json_status;
  vectis_status status;
  char state[VECTIS_AUTH_RANDOM_OIDC_BYTES * 2u + 1u];
  char nonce[VECTIS_AUTH_RANDOM_OIDC_BYTES * 2u + 1u];
  const char *state_value;
  const char *nonce_value;
  const char *verifier_value;
  const char *challenge_value;

  if (config == NULL || out == NULL || config->authorization_endpoint == NULL ||
      config->authorization_endpoint[0] == '\0' || config->client_id == NULL ||
      config->client_id[0] == '\0' || config->redirect_uri == NULL ||
      config->redirect_uri[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OIDC authorization endpoint, client_id, and "
                     "redirect_uri are required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_oidc_authorization_init(out);
  state[0] = '\0';
  nonce[0] = '\0';
  state_value = config->state;
  nonce_value = config->nonce;
  if (state_value == NULL || state_value[0] == '\0') {
    status = vectis_auth_generate_oidc_value(state, sizeof(state), error);
    if (status != VECTIS_OK) {
      return status;
    }
    state_value = state;
  }
  if (nonce_value == NULL || nonce_value[0] == '\0') {
    status = vectis_auth_generate_oidc_value(nonce, sizeof(nonce), error);
    if (status != VECTIS_OK) {
      return status;
    }
    nonce_value = nonce;
  }
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status != VECTIS_OK) {
    return status;
  }
  lonejson_oidc_pkce_init(&pkce);
  lonejson_owned_buffer_init(&challenge);
  lonejson_owned_buffer_init(&url);
  lonejson_error_init(&json_error);
  verifier_value = config->code_verifier;
  challenge_value = config->code_challenge;
  if (verifier_value == NULL || verifier_value[0] == '\0') {
    if (challenge_value == NULL || challenge_value[0] == '\0') {
      json_status = lonejson_oidc_pkce_generate_with_runtime(
          runtime, config->verifier_bytes, &pkce, &json_error);
      if (json_status != LONEJSON_STATUS_OK) {
        lonejson_free(runtime);
        return vectis_auth_lonejson_error(error, json_status, &json_error,
                                          "failed to generate OIDC PKCE");
      }
      verifier_value = pkce.code_verifier;
      challenge_value = pkce.code_challenge;
    }
  } else if (challenge_value == NULL || challenge_value[0] == '\0') {
    json_status = lonejson_oidc_pkce_challenge_with_runtime(
        runtime, verifier_value, &challenge, &json_error);
    if (json_status != LONEJSON_STATUS_OK) {
      lonejson_oidc_pkce_cleanup(&pkce);
      lonejson_free(runtime);
      return vectis_auth_lonejson_error(error, json_status, &json_error,
                                        "failed to compute OIDC PKCE "
                                        "challenge");
    }
    challenge_value = challenge.data;
  }
  memset(&request, 0, sizeof(request));
  request.authorization_endpoint = config->authorization_endpoint;
  request.client_id = config->client_id;
  request.redirect_uri = config->redirect_uri;
  request.scope = config->scope;
  request.state = state_value;
  request.nonce = nonce_value;
  request.code_challenge = challenge_value;
  request.audience = config->audience;
  request.resource = config->resource;
  request.max_url_bytes = config->max_url_bytes;
  json_status = lonejson_oidc_authorization_url(&request, &url, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    out->authorization_url = vectis_auth_strdup(url.data);
    if (verifier_value != NULL) {
      out->code_verifier = vectis_auth_strdup(verifier_value);
    }
    out->code_challenge = vectis_auth_strdup(challenge_value);
    out->state = vectis_auth_strdup(state_value);
    out->nonce = vectis_auth_strdup(nonce_value);
    if (out->authorization_url == NULL ||
        (verifier_value != NULL && out->code_verifier == NULL) ||
        out->code_challenge == NULL || out->state == NULL ||
        out->nonce == NULL) {
      vectis_auth_oidc_authorization_cleanup(out);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy OIDC authorization result");
      status = VECTIS_ERR_NOMEM;
    } else {
      status = VECTIS_OK;
    }
  } else {
    status = vectis_auth_lonejson_error(error, json_status, &json_error,
                                        "failed to build OIDC authorization "
                                        "URL");
  }
  lonejson_owned_buffer_free(&url);
  lonejson_owned_buffer_free(&challenge);
  lonejson_oidc_pkce_cleanup(&pkce);
  lonejson_free(runtime);
  return status;
}

vectis_status vectis_auth_oidc_exchange_callback(
    const vectis_auth_oidc_token_exchange_config *config,
    vectis_auth_oidc_token_exchange *out, vectis_error *error) {
  lonejson *runtime;
  lonejson_http_provider provider;
  vectis_auth_oauth2_http_adapter adapter;
  lonejson_oidc_authorization_callback callback;
  lonejson_oidc_authorization_code_token request;
  lonejson_oauth2_token_response token;
  lonejson_oauth2_token_flow flow;
  lonejson_error json_error;
  lonejson_status json_status;
  vectis_status status;
  int64_t now;

  if (config == NULL || out == NULL || config->token_endpoint == NULL ||
      config->token_endpoint[0] == '\0' || config->client_id == NULL ||
      config->client_id[0] == '\0' || config->redirect_uri == NULL ||
      config->redirect_uri[0] == '\0' || config->code_verifier == NULL ||
      config->code_verifier[0] == '\0' || config->callback_query == NULL ||
      config->expected_state == NULL || config->expected_state[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OIDC token endpoint, client_id, redirect_uri, "
                     "code_verifier, callback_query, and expected_state are "
                     "required");
    return VECTIS_ERR_INVALID;
  }
  vectis_auth_oidc_token_exchange_init(out);
  runtime = NULL;
  status = vectis_auth_lonejson_runtime(&runtime, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_auth_oauth2_install_http_provider(runtime, &config->transport,
                                                    &adapter, &provider, error);
  if (status != VECTIS_OK) {
    lonejson_free(runtime);
    return status;
  }
  lonejson_oidc_authorization_callback_init(&callback);
  lonejson_oauth2_token_response_init(&token);
  lonejson_oauth2_token_flow_init(&flow);
  lonejson_error_init(&json_error);
  json_status = lonejson_oidc_authorization_callback_parse_query(
      config->callback_query, strlen(config->callback_query),
      config->expected_state, config->max_query_bytes, &callback, &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    memset(&request, 0, sizeof(request));
    request.client_id = config->client_id;
    request.code = callback.code;
    request.redirect_uri = config->redirect_uri;
    request.code_verifier = config->code_verifier;
    request.client_secret = config->client_secret;
    request.max_body_bytes = config->max_body_bytes;
    json_status = lonejson_oidc_authorization_code_token_request(
        runtime, config->token_endpoint, &request, config->max_response_bytes,
        &token, &json_error);
  }
  if (json_status == LONEJSON_STATUS_OK) {
    now = config->now != 0 ? config->now : (int64_t)time(NULL);
    json_status = lonejson_oauth2_token_flow_update_response(&flow, &token, now,
                                                             &json_error);
  }
  if (json_status == LONEJSON_STATUS_OK) {
    out->code = vectis_auth_strdup(callback.code);
    out->state = vectis_auth_strdup(callback.state);
    status = vectis_auth_oauth2_copy_token_response(&token, &out->token, error);
    if (status == VECTIS_OK) {
      status = vectis_auth_oauth2_copy_token_flow_from_lonejson(
          &flow, &out->flow, error);
    }
    if (status != VECTIS_OK || out->code == NULL || out->state == NULL) {
      vectis_auth_oidc_token_exchange_cleanup(out);
      if (status == VECTIS_OK) {
        vectis_set_error(error, VECTIS_ERR_NOMEM,
                         "failed to copy OIDC token exchange result");
        status = VECTIS_ERR_NOMEM;
      }
    }
  } else {
    status = vectis_auth_lonejson_error(error, json_status, &json_error,
                                        "OIDC callback token exchange failed");
  }
  lonejson_oauth2_token_flow_cleanup(&flow);
  lonejson_oauth2_token_response_cleanup(&token);
  lonejson_oidc_authorization_callback_cleanup(&callback);
  lonejson_free(runtime);
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
