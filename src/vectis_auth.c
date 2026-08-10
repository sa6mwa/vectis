#define LONEJSON_WITH_OPENSSL 1
#define LONEJSON_WITH_OIDC 1

#include "vectis_internal.h"

#include <vectis/auth.h>

#include <errno.h>
#include <fcntl.h>
#include <lonejson.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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
  static const char empty_store[] = "{\"credentials\":[],\"signups\":[]}\n";

  return vectis_auth_write_store_locked(config, empty_store,
                                        sizeof(empty_store) - 1u, error);
}

static vectis_status vectis_auth_writer_copy_store_arrays(
    lonejson_writer *writer, const char *store_json, size_t store_len,
    const char *extra_record_json, size_t extra_record_len,
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
        &writer, store_json, store_len, record_json, record_len, &json_error);
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
