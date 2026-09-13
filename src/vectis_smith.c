#include "vectis_internal.h"

#include <cai/agent_runtime.h>
#include <cai/session_store.h>
#include <lc/lc.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VECTIS_SMITH_STORE_OWNER_PREFIX "vectis-smith-"
#define VECTIS_SMITH_STORE_DEFAULT_TTL_SECONDS 30L
#define VECTIS_SMITH_STORE_KEY_PREFIX "vectis/smith/"
#define VECTIS_SMITH_ATTACHMENT_CHECKPOINT "checkpoint"
#define VECTIS_SMITH_ATTACHMENT_LATEST "latest"

typedef struct vectis_smith_record {
  char session_id[CAI_AGENT_SESSION_ID_MAX];
  lonejson_uint64 applied_event_sequence;
} vectis_smith_record;

typedef struct vectis_smith_event_record {
  lonejson_uint64 sequence;
  char *type;
  char *data;
} vectis_smith_event_record;

typedef struct vectis_smith_cai_source {
  cai_source *source;
} vectis_smith_cai_source;

typedef struct vectis_smith_memory_source {
  unsigned char *bytes;
  size_t length;
  size_t offset;
} vectis_smith_memory_source;

struct vectis_smith_store {
  struct lc_client *client;
  char *owner;
  char *diagnostic_endpoint;
  char *diagnostic_namespace;
  long lease_ttl_seconds;
  pthread_mutex_t mutex;
  cai_agent_session_store callbacks;
};

struct vectis_smith {
  cai_client *client;
  int owns_client;
  cai_agent_runtime *runtime;
  vectis_smith_store *store;
  cai_agent_session_store named_store;
  char requested_session[CAI_AGENT_SESSION_ID_MAX];
};

static const lonejson_field vectis_smith_record_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(vectis_smith_record, session_id,
                                    "session_id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_U64_REQ(vectis_smith_record, applied_event_sequence,
                           "applied_event_sequence")};

LONEJSON_MAP_DEFINE(vectis_smith_record_map, vectis_smith_record,
                    vectis_smith_record_fields);

static const lonejson_field vectis_smith_event_record_fields[] = {
    LONEJSON_FIELD_U64_REQ(vectis_smith_event_record, sequence, "sequence"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(vectis_smith_event_record, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(vectis_smith_event_record, data, "data")};

LONEJSON_MAP_DEFINE(vectis_smith_event_record_map, vectis_smith_event_record,
                    vectis_smith_event_record_fields);

static char *vectis_smith_strdup(const char *value) {
  size_t length;
  char *copy;

  if (value == NULL) {
    return NULL;
  }
  length = strlen(value);
  copy = (char *)malloc(length + 1u);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value, length + 1u);
  return copy;
}

static void vectis_smith_set_cai_error(cai_error *error, int code,
                                       const char *message) {
  if (error == NULL) {
    return;
  }
  cai_error_cleanup(error);
  cai_error_init(error);
  error->code = code;
  error->message = vectis_smith_strdup(message);
  if (error->message == NULL) {
    error->code = CAI_ERR_NOMEM;
  }
}

static void vectis_smith_set_error(vectis_error *error, vectis_status status,
                                   const char *message) {
  if (error == NULL) {
    return;
  }
  error->code = status;
  error->message[0] = '\0';
  if (message != NULL) {
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
  }
}

void vectis_smith_lockdc_diagnostic_message(const char *message, char *out,
                                            size_t out_capacity) {
  if (out == NULL || out_capacity == 0u) {
    return;
  }
  if (message == NULL || message[0] == '\0') {
    (void)snprintf(out, out_capacity, "%s", "unknown lockdc error");
    return;
  }
  if (strstr(message, "://") != NULL || strstr(message, "crypto_key") != NULL ||
      strstr(message, "token") != NULL || strstr(message, "secret") != NULL ||
      strstr(message, "password") != NULL ||
      strstr(message, "authorization") != NULL) {
    (void)snprintf(out, out_capacity, "%s",
                   "lockdc returned a redacted dependency error");
    return;
  }
  (void)snprintf(out, out_capacity, "%s", message);
}

static int vectis_smith_hash(const char *value,
                             char out[SHA256_DIGEST_LENGTH * 2u + 1u]) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  unsigned int digest_length;
  size_t i;

  if (value == NULL || out == NULL ||
      EVP_Digest((const unsigned char *)value, strlen(value), digest,
                 &digest_length, EVP_sha256(), NULL) != 1 ||
      digest_length != SHA256_DIGEST_LENGTH) {
    return 0;
  }
  for (i = 0u; i < SHA256_DIGEST_LENGTH; ++i) {
    (void)snprintf(out + i * 2u, 3u, "%02x", (unsigned int)digest[i]);
  }
  out[SHA256_DIGEST_LENGTH * 2u] = '\0';
  return 1;
}

static int vectis_smith_key(const char *kind, const char *scope,
                            const char *session_id, char *out,
                            size_t out_capacity) {
  char scope_hash[SHA256_DIGEST_LENGTH * 2u + 1u];
  char session_hash[SHA256_DIGEST_LENGTH * 2u + 1u];
  int written;

  if (kind == NULL || scope == NULL || out == NULL || out_capacity == 0u ||
      !vectis_smith_hash(scope, scope_hash)) {
    return 0;
  }
  if (session_id == NULL) {
    written = snprintf(out, out_capacity, VECTIS_SMITH_STORE_KEY_PREFIX "%s/%s",
                       kind, scope_hash);
  } else {
    if (!vectis_smith_hash(session_id, session_hash)) {
      return 0;
    }
    written =
        snprintf(out, out_capacity, VECTIS_SMITH_STORE_KEY_PREFIX "%s/%s/%s",
                 kind, scope_hash, session_hash);
  }
  return written >= 0 && (size_t)written < out_capacity;
}

static int vectis_smith_acquire(vectis_smith_store *store, const char *key,
                                lc_lease **out, cai_error *error) {
  lc_acquire_req request;
  lc_error lcerr;
  char message[512];
  char dependency_message[256];
  const char *endpoint;
  const char *namespace_name;
  int rc;

  lc_acquire_req_init(&request);
  lc_error_init(&lcerr);
  request.key = key;
  request.owner = store->owner;
  request.ttl_seconds = store->lease_ttl_seconds;
  rc = lc_acquire(store->client, &request, out, &lcerr);
  if (rc != LC_OK) {
    vectis_smith_lockdc_diagnostic_message(lcerr.message, dependency_message,
                                           sizeof(dependency_message));
    endpoint = store->diagnostic_endpoint != NULL
                   ? store->diagnostic_endpoint
                   : "configured lockdc endpoint";
    namespace_name = store->diagnostic_namespace != NULL
                         ? store->diagnostic_namespace
                         : "client default namespace";
    (void)snprintf(message, sizeof(message),
                   "lockdc error: unable to acquire Smith state "
                   "(endpoint=%s namespace=%s code=%d): %s",
                   endpoint, namespace_name, lcerr.code, dependency_message);
    vectis_smith_set_cai_error(error, CAI_ERR_TRANSPORT, message);
  }
  lc_error_cleanup(&lcerr);
  return rc == LC_OK ? CAI_OK : error->code;
}

static int vectis_smith_release(lc_lease *lease, int rollback,
                                cai_error *error) {
  lc_release_req request;
  lc_error lcerr;
  int rc;

  lc_release_req_init(&request);
  request.rollback = rollback;
  lc_error_init(&lcerr);
  rc = lease->release(lease, &request, &lcerr);
  if (rc != LC_OK) {
    lc_lease_close(lease);
    vectis_smith_set_cai_error(error, CAI_ERR_TRANSPORT,
                               "failed to release lockdc Smith state");
  }
  lc_error_cleanup(&lcerr);
  return rc == LC_OK ? CAI_OK : error->code;
}

static size_t vectis_smith_lc_source_read(void *context, void *buffer,
                                          size_t count, lc_error *error) {
  vectis_smith_cai_source *adapter;
  cai_error caierr;
  size_t result;

  adapter = (vectis_smith_cai_source *)context;
  cai_error_init(&caierr);
  result = adapter->source->read(adapter->source, buffer, count, &caierr);
  if (result == 0u && caierr.code != CAI_OK) {
    error->code = LC_ERR_TRANSPORT;
  }
  cai_error_cleanup(&caierr);
  return result;
}

static int vectis_smith_lc_source_reset(void *context, lc_error *error) {
  vectis_smith_cai_source *adapter;
  cai_error caierr;
  int rc;

  adapter = (vectis_smith_cai_source *)context;
  cai_error_init(&caierr);
  rc = adapter->source->reset(adapter->source, &caierr);
  if (rc != CAI_OK) {
    error->code = LC_ERR_TRANSPORT;
  }
  cai_error_cleanup(&caierr);
  return rc == CAI_OK ? LC_OK : LC_ERR_TRANSPORT;
}

static void vectis_smith_lc_source_close(void *context) { free(context); }

static int vectis_smith_attachment_cai_source(lc_lease *lease, const char *name,
                                              cai_source *state,
                                              cai_error *error) {
  vectis_smith_cai_source *adapter;
  lc_source *source;
  lc_attach_req request;
  lc_attach_res response;
  lc_error lcerr;
  int rc;

  adapter = (vectis_smith_cai_source *)calloc(1u, sizeof(*adapter));
  if (adapter == NULL) {
    vectis_smith_set_cai_error(error, CAI_ERR_NOMEM,
                               "failed to allocate Smith checkpoint stream");
    return CAI_ERR_NOMEM;
  }
  adapter->source = state;
  source = NULL;
  lc_error_init(&lcerr);
  rc = lc_source_from_callbacks(
      vectis_smith_lc_source_read, vectis_smith_lc_source_reset,
      vectis_smith_lc_source_close, adapter, &source, &lcerr);
  if (rc != LC_OK) {
    free(adapter);
    lc_error_cleanup(&lcerr);
    vectis_smith_set_cai_error(error, CAI_ERR_NOMEM,
                               "failed to create Smith checkpoint stream");
    return CAI_ERR_NOMEM;
  }
  memset(&request, 0, sizeof(request));
  memset(&response, 0, sizeof(response));
  request.name = name;
  request.content_type = "application/json";
  rc = lease->attach(lease, &request, source, &response, &lcerr);
  lc_source_close(source);
  lc_attach_res_cleanup(&response);
  if (rc != LC_OK) {
    vectis_smith_set_cai_error(error, CAI_ERR_TRANSPORT,
                               "failed to save Smith checkpoint attachment");
  }
  lc_error_cleanup(&lcerr);
  return rc == LC_OK ? CAI_OK : error->code;
}

static int vectis_smith_save_record(lc_lease *lease,
                                    const vectis_smith_record *record,
                                    cai_error *error) {
  lc_error lcerr;
  int rc;

  lc_error_init(&lcerr);
  rc = lease->save(lease, &vectis_smith_record_map, record, &lcerr);
  if (rc != LC_OK) {
    vectis_smith_set_cai_error(error, CAI_ERR_TRANSPORT,
                               "failed to save Smith session metadata");
  }
  lc_error_cleanup(&lcerr);
  return rc == LC_OK ? CAI_OK : error->code;
}

static int vectis_smith_load_record(lc_lease *lease, vectis_smith_record *out,
                                    int *missing, cai_error *error) {
  lc_get_res response;
  lc_error lcerr;
  int rc;

  memset(out, 0, sizeof(*out));
  memset(&response, 0, sizeof(response));
  lc_error_init(&lcerr);
  rc = lease->load(lease, &vectis_smith_record_map, out, NULL, &response,
                   &lcerr);
  if (rc == LC_OK) {
    *missing = response.no_content;
  } else {
    vectis_smith_set_cai_error(error, CAI_ERR_PROTOCOL,
                               "failed to load Smith session metadata");
  }
  lc_get_res_cleanup(&response);
  lc_error_cleanup(&lcerr);
  return rc == LC_OK ? CAI_OK : error->code;
}

static size_t vectis_smith_memory_source_read(void *context, void *buffer,
                                              size_t count, cai_error *error) {
  vectis_smith_memory_source *source;
  size_t available;

  (void)error;
  source = (vectis_smith_memory_source *)context;
  available = source->length - source->offset;
  if (count > available) {
    count = available;
  }
  if (count != 0u) {
    memcpy(buffer, source->bytes + source->offset, count);
    source->offset += count;
  }
  return count;
}

static int vectis_smith_memory_source_reset(void *context, cai_error *error) {
  vectis_smith_memory_source *source;

  (void)error;
  source = (vectis_smith_memory_source *)context;
  source->offset = 0u;
  return CAI_OK;
}

static void vectis_smith_memory_source_close(void *context) {
  vectis_smith_memory_source *source;

  source = (vectis_smith_memory_source *)context;
  if (source != NULL) {
    free(source->bytes);
    free(source);
  }
}

static int vectis_smith_attachment_memory(lc_lease *lease, const char *name,
                                          unsigned char **bytes, size_t *length,
                                          cai_error *error) {
  lc_attachment_get_req request;
  lc_attachment_get_res response;
  lc_sink *sink;
  lc_error lcerr;
  const void *contents;
  size_t contents_length;
  int rc;

  *bytes = NULL;
  *length = 0u;
  sink = NULL;
  lc_error_init(&lcerr);
  rc = lc_sink_to_memory(&sink, &lcerr);
  if (rc == LC_OK) {
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    request.selector.name = name;
    rc = lease->get_attachment(lease, &request, sink, &response, &lcerr);
    lc_attachment_get_res_cleanup(&response);
  }
  if (rc == LC_OK) {
    rc = lc_sink_memory_bytes(sink, &contents, &contents_length, &lcerr);
  }
  if (rc == LC_OK) {
    *bytes = (unsigned char *)malloc(contents_length + 1u);
    if (*bytes == NULL) {
      rc = LC_ERR_NOMEM;
    } else {
      memcpy(*bytes, contents, contents_length);
      (*bytes)[contents_length] = '\0';
      *length = contents_length;
    }
  }
  if (sink != NULL) {
    lc_sink_close(sink);
  }
  if (rc != LC_OK) {
    free(*bytes);
    *bytes = NULL;
    *length = 0u;
    vectis_smith_set_cai_error(
        error, rc == LC_ERR_NOMEM ? CAI_ERR_NOMEM : CAI_ERR_TRANSPORT,
        "failed to load Smith session attachment");
  }
  lc_error_cleanup(&lcerr);
  return rc == LC_OK ? CAI_OK : error->code;
}

static int vectis_smith_store_checkpoint(void *context, const char *scope,
                                         const char *session_id,
                                         cai_source *state,
                                         unsigned long long watermark,
                                         cai_error *error) {
  vectis_smith_store *store;
  vectis_smith_record record;
  lc_lease *lease;
  char key[256];
  int result;

  store = (vectis_smith_store *)context;
  if (store == NULL || scope == NULL || scope[0] == '\0' ||
      session_id == NULL || session_id[0] == '\0' ||
      strlen(session_id) >= sizeof(record.session_id) || state == NULL ||
      !vectis_smith_key("session", scope, session_id, key, sizeof(key))) {
    vectis_smith_set_cai_error(error, CAI_ERR_INVALID,
                               "invalid Smith checkpoint arguments");
    return CAI_ERR_INVALID;
  }
  (void)pthread_mutex_lock(&store->mutex);
  lease = NULL;
  result = vectis_smith_acquire(store, key, &lease, error);
  if (result == CAI_OK) {
    memset(&record, 0, sizeof(record));
    (void)snprintf(record.session_id, sizeof(record.session_id), "%s",
                   session_id);
    record.applied_event_sequence = (lonejson_uint64)watermark;
    result = vectis_smith_save_record(lease, &record, error);
  }
  if (result == CAI_OK) {
    result = vectis_smith_attachment_cai_source(
        lease, VECTIS_SMITH_ATTACHMENT_CHECKPOINT, state, error);
  }
  if (result == CAI_OK) {
    if (!vectis_smith_key("scope", scope, NULL, key, sizeof(key))) {
      vectis_smith_set_cai_error(error, CAI_ERR_INVALID,
                                 "invalid Smith session scope");
      result = CAI_ERR_INVALID;
    } else {
      result = vectis_smith_release(lease, 0, error);
      lease = NULL;
      if (result == CAI_OK) {
        result = vectis_smith_acquire(store, key, &lease, error);
      }
      if (result == CAI_OK) {
        result = vectis_smith_save_record(lease, &record, error);
      }
    }
  }
  if (lease != NULL) {
    if (result == CAI_OK) {
      result = vectis_smith_release(lease, 0, error);
    } else {
      cai_error rollback_error;

      /* Never commit metadata without its attachment. Preserve the original
       * persistence error even if rollback also fails. */
      cai_error_init(&rollback_error);
      (void)vectis_smith_release(lease, 1, &rollback_error);
      cai_error_cleanup(&rollback_error);
    }
  }
  (void)pthread_mutex_unlock(&store->mutex);
  return result;
}

static int vectis_smith_store_load_selected(
    void *context, const char *scope, const char *requested, char *session_id,
    size_t session_id_capacity, cai_source **out,
    unsigned long long *out_applied_event_sequence, cai_error *error) {
  vectis_smith_store *store;
  vectis_smith_record record;
  vectis_smith_memory_source *source;
  cai_source_callbacks callbacks;
  lc_lease *lease;
  unsigned char *bytes;
  size_t length;
  char key[256];
  int missing;
  int result;

  if (out == NULL || session_id == NULL || session_id_capacity == 0u ||
      out_applied_event_sequence == NULL) {
    vectis_smith_set_cai_error(error, CAI_ERR_INVALID,
                               "invalid Smith checkpoint load arguments");
    return CAI_ERR_INVALID;
  }
  *out = NULL;
  session_id[0] = '\0';
  *out_applied_event_sequence = 0u;
  store = (vectis_smith_store *)context;
  if (store == NULL || scope == NULL || scope[0] == '\0' ||
      !vectis_smith_key(requested == NULL ? "scope" : "session", scope,
                        requested, key, sizeof(key))) {
    vectis_smith_set_cai_error(error, CAI_ERR_INVALID,
                               "invalid Smith session scope");
    return CAI_ERR_INVALID;
  }
  (void)pthread_mutex_lock(&store->mutex);
  lease = NULL;
  result = vectis_smith_acquire(store, key, &lease, error);
  if (result == CAI_OK) {
    result = vectis_smith_load_record(lease, &record, &missing, error);
    if (result == CAI_OK && !missing && requested != NULL &&
        strcmp(requested, record.session_id) != 0) {
      vectis_smith_set_cai_error(
          error, CAI_ERR_PROTOCOL,
          "Smith checkpoint metadata does not match requested session");
      result = CAI_ERR_PROTOCOL;
    }
  } else {
    missing = 1;
  }
  if (lease != NULL) {
    if (vectis_smith_release(lease, 0, error) != CAI_OK && result == CAI_OK) {
      result = error->code;
    }
    lease = NULL;
  }
  if (result == CAI_OK && !missing) {
    if (strlen(record.session_id) + 1u > session_id_capacity ||
        !vectis_smith_key("session", scope, record.session_id, key,
                          sizeof(key))) {
      vectis_smith_set_cai_error(error, CAI_ERR_INVALID,
                                 "invalid stored Smith session id");
      result = CAI_ERR_INVALID;
    } else {
      result = vectis_smith_acquire(store, key, &lease, error);
    }
    if (result == CAI_OK) {
      vectis_smith_record session_record;
      result =
          vectis_smith_load_record(lease, &session_record, &missing, error);
      if (result == CAI_OK && (missing || strcmp(session_record.session_id,
                                                 record.session_id) != 0)) {
        vectis_smith_set_cai_error(
            error, CAI_ERR_PROTOCOL,
            "Smith checkpoint metadata does not match key");
        result = CAI_ERR_PROTOCOL;
      }
      if (result == CAI_OK) {
        record = session_record;
      }
    }
    if (result == CAI_OK) {
      result = vectis_smith_attachment_memory(
          lease, VECTIS_SMITH_ATTACHMENT_CHECKPOINT, &bytes, &length, error);
    }
    if (lease != NULL) {
      if (vectis_smith_release(lease, 0, error) != CAI_OK && result == CAI_OK) {
        result = error->code;
      }
      lease = NULL;
    }
    if (result == CAI_OK) {
      source = (vectis_smith_memory_source *)calloc(1u, sizeof(*source));
      if (source == NULL) {
        free(bytes);
        vectis_smith_set_cai_error(
            error, CAI_ERR_NOMEM, "failed to allocate Smith checkpoint source");
        result = CAI_ERR_NOMEM;
      } else {
        source->bytes = bytes;
        source->length = length;
        memset(&callbacks, 0, sizeof(callbacks));
        callbacks.read = vectis_smith_memory_source_read;
        callbacks.reset = vectis_smith_memory_source_reset;
        callbacks.close = vectis_smith_memory_source_close;
        callbacks.context = source;
        result = cai_source_from_callbacks(&callbacks, out, error);
        if (result != CAI_OK) {
          vectis_smith_memory_source_close(source);
        }
      }
      if (result == CAI_OK) {
        (void)snprintf(session_id, session_id_capacity, "%s",
                       record.session_id);
        *out_applied_event_sequence =
            (unsigned long long)record.applied_event_sequence;
      }
    }
  }
  (void)pthread_mutex_unlock(&store->mutex);
  return result;
}

static int vectis_smith_store_load_latest(void *context, const char *scope,
                                          char *session_id, size_t capacity,
                                          cai_source **out,
                                          unsigned long long *watermark,
                                          cai_error *error) {
  return vectis_smith_store_load_selected(context, scope, NULL, session_id,
                                          capacity, out, watermark, error);
}

static int vectis_smith_attachment_text(lc_lease *lease, const char *name,
                                        const char *text, cai_error *error) {
  lc_attach_req request;
  lc_attach_res response;
  lc_source *source;
  lc_error lcerr;
  int rc;

  source = NULL;
  lc_error_init(&lcerr);
  rc = lc_source_from_memory(text, strlen(text), &source, &lcerr);
  if (rc == LC_OK) {
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    request.name = name;
    request.content_type = "application/json";
    request.prevent_overwrite = 1;
    rc = lease->attach(lease, &request, source, &response, &lcerr);
    lc_attach_res_cleanup(&response);
  }
  if (source != NULL) {
    lc_source_close(source);
  }
  if (rc != LC_OK) {
    vectis_smith_set_cai_error(error, CAI_ERR_TRANSPORT,
                               "failed to append Smith session event");
  }
  lc_error_cleanup(&lcerr);
  return rc == LC_OK ? CAI_OK : error->code;
}

static int vectis_smith_store_append_event(void *context, const char *scope,
                                           const char *session_id,
                                           const cai_agent_session_event *event,
                                           cai_error *error) {
  vectis_smith_store *store;
  vectis_smith_record record;
  vectis_smith_event_record event_record;
  lonejson *json;
  lonejson_error json_error;
  lc_lease *lease;
  char key[256];
  char attachment[64];
  char *encoded;
  size_t encoded_length;
  int missing;
  int result;

  store = (vectis_smith_store *)context;
  if (store == NULL || scope == NULL || scope[0] == '\0' ||
      session_id == NULL || session_id[0] == '\0' || event == NULL ||
      event->type == NULL || strlen(session_id) >= sizeof(record.session_id) ||
      !vectis_smith_key("session", scope, session_id, key, sizeof(key))) {
    vectis_smith_set_cai_error(error, CAI_ERR_INVALID,
                               "invalid Smith event arguments");
    return CAI_ERR_INVALID;
  }
  (void)pthread_mutex_lock(&store->mutex);
  lease = NULL;
  result = vectis_smith_acquire(store, key, &lease, error);
  if (result == CAI_OK) {
    result = vectis_smith_load_record(lease, &record, &missing, error);
  } else {
    missing = 0;
  }
  if (result == CAI_OK && missing) {
    memset(&record, 0, sizeof(record));
    (void)snprintf(record.session_id, sizeof(record.session_id), "%s",
                   session_id);
    result = vectis_smith_save_record(lease, &record, error);
  }
  if (result == CAI_OK && strcmp(record.session_id, session_id) != 0) {
    vectis_smith_set_cai_error(error, CAI_ERR_PROTOCOL,
                               "Smith session metadata does not match key");
    result = CAI_ERR_PROTOCOL;
  }
  encoded = NULL;
  if (result == CAI_OK) {
    memset(&event_record, 0, sizeof(event_record));
    event_record.sequence = (lonejson_uint64)event->sequence;
    event_record.type = vectis_smith_strdup(event->type);
    event_record.data =
        vectis_smith_strdup(event->data == NULL ? "" : event->data);
    if (event_record.type == NULL || event_record.data == NULL) {
      vectis_smith_set_cai_error(error, CAI_ERR_NOMEM,
                                 "failed to copy Smith session event");
      result = CAI_ERR_NOMEM;
    }
    lonejson_error_init(&json_error);
    json = result == CAI_OK ? lonejson_new(NULL, &json_error) : NULL;
    if (result == CAI_OK && json == NULL) {
      vectis_smith_set_cai_error(error, CAI_ERR_NOMEM,
                                 "failed to allocate Smith event serializer");
      result = CAI_ERR_NOMEM;
    } else if (result == CAI_OK) {
      encoded =
          lonejson_serialize_alloc(json, &vectis_smith_event_record_map,
                                   &event_record, &encoded_length, &json_error);
      lonejson_free(json);
      if (encoded == NULL) {
        vectis_smith_set_cai_error(error, CAI_ERR_PROTOCOL,
                                   "failed to encode Smith session event");
        result = CAI_ERR_PROTOCOL;
      }
    }
    free(event_record.type);
    free(event_record.data);
  }
  if (result == CAI_OK &&
      snprintf(attachment, sizeof(attachment), "event-%020llu",
               event->sequence) >= (int)sizeof(attachment)) {
    vectis_smith_set_cai_error(error, CAI_ERR_INVALID,
                               "Smith event sequence is not representable");
    result = CAI_ERR_INVALID;
  }
  if (result == CAI_OK) {
    result = vectis_smith_attachment_text(lease, attachment, encoded, error);
  }
  free(encoded);
  if (lease != NULL) {
    if (vectis_smith_release(lease, 0, error) != CAI_OK && result == CAI_OK) {
      result = error->code;
    }
  }
  (void)pthread_mutex_unlock(&store->mutex);
  return result;
}

static int vectis_smith_attachment_event_sequence(const char *name,
                                                  unsigned long long *out) {
  const char *prefix;
  char *end;
  unsigned long long value;

  prefix = "event-";
  if (name == NULL || strncmp(name, prefix, strlen(prefix)) != 0 ||
      name[strlen(prefix)] == '\0') {
    return 0;
  }
  value = strtoull(name + strlen(prefix), &end, 10);
  if (*end != '\0') {
    return 0;
  }
  *out = value;
  return 1;
}

static int vectis_smith_event_compare(const void *left, const void *right) {
  const lc_attachment_info *a;
  const lc_attachment_info *b;
  unsigned long long a_sequence;
  unsigned long long b_sequence;
  int a_is_event;
  int b_is_event;

  a = (const lc_attachment_info *)left;
  b = (const lc_attachment_info *)right;
  a_sequence = 0u;
  b_sequence = 0u;
  a_is_event = vectis_smith_attachment_event_sequence(a->name, &a_sequence);
  b_is_event = vectis_smith_attachment_event_sequence(b->name, &b_sequence);
  if (!a_is_event || !b_is_event) {
    if (a_is_event != b_is_event) {
      return a_is_event ? -1 : 1;
    }
    if (a->name == NULL || b->name == NULL) {
      return a->name == b->name ? 0 : (a->name == NULL ? -1 : 1);
    }
    return strcmp(a->name, b->name);
  }
  return a_sequence < b_sequence ? -1 : (a_sequence > b_sequence ? 1 : 0);
}

static int vectis_smith_store_load_events_after(
    void *context, const char *scope, const char *session_id,
    unsigned long long after_sequence, cai_agent_session_event_fn callback,
    void *callback_context, cai_error *error) {
  vectis_smith_store *store;
  lc_attachment_list attachments;
  vectis_smith_event_record event_record;
  cai_agent_session_event event;
  lonejson *json;
  lonejson_error json_error;
  lc_lease *lease;
  unsigned char *bytes;
  size_t length;
  char key[256];
  unsigned long long sequence;
  size_t i;
  int result;

  store = (vectis_smith_store *)context;
  if (store == NULL || scope == NULL || scope[0] == '\0' ||
      session_id == NULL || session_id[0] == '\0' || callback == NULL ||
      !vectis_smith_key("session", scope, session_id, key, sizeof(key))) {
    vectis_smith_set_cai_error(error, CAI_ERR_INVALID,
                               "invalid Smith event replay arguments");
    return CAI_ERR_INVALID;
  }
  memset(&attachments, 0, sizeof(attachments));
  (void)pthread_mutex_lock(&store->mutex);
  lease = NULL;
  result = vectis_smith_acquire(store, key, &lease, error);
  if (result == CAI_OK) {
    lc_error lcerr;
    lc_error_init(&lcerr);
    if (lease->list_attachments(lease, &attachments, &lcerr) != LC_OK) {
      vectis_smith_set_cai_error(error, CAI_ERR_TRANSPORT,
                                 "failed to list Smith session events");
      result = error->code;
    }
    lc_error_cleanup(&lcerr);
  }
  if (result == CAI_OK) {
    qsort(attachments.items, attachments.count, sizeof(*attachments.items),
          vectis_smith_event_compare);
    for (i = 0u; i < attachments.count && result == CAI_OK; ++i) {
      if (!vectis_smith_attachment_event_sequence(attachments.items[i].name,
                                                  &sequence) ||
          sequence <= after_sequence) {
        continue;
      }
      bytes = NULL;
      length = 0u;
      result = vectis_smith_attachment_memory(lease, attachments.items[i].name,
                                              &bytes, &length, error);
      if (result != CAI_OK) {
        break;
      }
      memset(&event_record, 0, sizeof(event_record));
      lonejson_error_init(&json_error);
      json = lonejson_new(NULL, &json_error);
      if (json == NULL ||
          lonejson_parse_buffer(json, &vectis_smith_event_record_map,
                                &event_record, (const char *)bytes, length,
                                &json_error) != LONEJSON_STATUS_OK) {
        if (json != NULL) {
          lonejson_free(json);
        }
        free(bytes);
        vectis_smith_set_cai_error(error, CAI_ERR_PROTOCOL,
                                   "invalid Smith session event attachment");
        result = CAI_ERR_PROTOCOL;
        break;
      }
      free(bytes);
      memset(&event, 0, sizeof(event));
      event.sequence = (unsigned long long)event_record.sequence;
      event.type = event_record.type;
      event.data = event_record.data;
      result = callback(callback_context, &event, error);
      lonejson_cleanup(&vectis_smith_event_record_map, &event_record);
      lonejson_free(json);
    }
  }
  lc_attachment_list_cleanup(&attachments);
  if (lease != NULL) {
    if (vectis_smith_release(lease, 0, error) != CAI_OK && result == CAI_OK) {
      result = error->code;
    }
  }
  (void)pthread_mutex_unlock(&store->mutex);
  return result;
}

void vectis_smith_store_config_init(vectis_smith_store_config *config) {
  if (config != NULL) {
    memset(config, 0, sizeof(*config));
  }
}

int vectis_smith_store_set_diagnostic_context(vectis_smith_store *store,
                                              const char *endpoint,
                                              const char *namespace_name) {
  char *endpoint_copy;
  char *namespace_copy;

  if (store == NULL) {
    return -1;
  }
  endpoint_copy = endpoint == NULL ? NULL : vectis_smith_strdup(endpoint);
  namespace_copy =
      namespace_name == NULL ? NULL : vectis_smith_strdup(namespace_name);
  if ((endpoint != NULL && endpoint_copy == NULL) ||
      (namespace_name != NULL && namespace_copy == NULL)) {
    free(namespace_copy);
    free(endpoint_copy);
    return -1;
  }
  free(store->diagnostic_namespace);
  free(store->diagnostic_endpoint);
  store->diagnostic_endpoint = endpoint_copy;
  store->diagnostic_namespace = namespace_copy;
  return 0;
}

vectis_status vectis_smith_store_new(const vectis_smith_store_config *config,
                                     vectis_smith_store **out,
                                     vectis_error *error) {
  vectis_smith_store *store;
  char owner[64];

  if (out == NULL || config == NULL || config->client == NULL) {
    vectis_smith_set_error(error, VECTIS_ERR_INVALID,
                           "Smith store requires a lockdc client and output");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  store = (vectis_smith_store *)calloc(1u, sizeof(*store));
  if (store == NULL) {
    vectis_smith_set_error(error, VECTIS_ERR_NOMEM,
                           "failed to allocate Smith store");
    return VECTIS_ERR_NOMEM;
  }
  if (config->owner == NULL || config->owner[0] == '\0') {
    (void)snprintf(owner, sizeof(owner), VECTIS_SMITH_STORE_OWNER_PREFIX "%ld",
                   (long)getpid());
    store->owner = vectis_smith_strdup(owner);
  } else {
    store->owner = vectis_smith_strdup(config->owner);
  }
  if (store->owner == NULL || pthread_mutex_init(&store->mutex, NULL) != 0) {
    free(store->diagnostic_namespace);
    free(store->diagnostic_endpoint);
    free(store->owner);
    free(store);
    vectis_smith_set_error(error, VECTIS_ERR_NOMEM,
                           "failed to initialize Smith store");
    return VECTIS_ERR_NOMEM;
  }
  store->client = config->client;
  store->lease_ttl_seconds = config->lease_ttl_seconds > 0L
                                 ? config->lease_ttl_seconds
                                 : VECTIS_SMITH_STORE_DEFAULT_TTL_SECONDS;
  store->callbacks.checkpoint = vectis_smith_store_checkpoint;
  store->callbacks.load_latest = vectis_smith_store_load_latest;
  store->callbacks.append_event = vectis_smith_store_append_event;
  store->callbacks.load_events_after = vectis_smith_store_load_events_after;
  store->callbacks.context = store;
  *out = store;
  vectis_error_clear(error);
  return VECTIS_OK;
}

void vectis_smith_store_destroy(vectis_smith_store *store) {
  if (store != NULL) {
    (void)pthread_mutex_destroy(&store->mutex);
    free(store->diagnostic_namespace);
    free(store->diagnostic_endpoint);
    free(store->owner);
    free(store);
  }
}

const cai_agent_session_store *
vectis_smith_store_session_store(const vectis_smith_store *store) {
  return store == NULL ? NULL : &store->callbacks;
}

void vectis_smith_config_init(vectis_smith_config *config) {
  if (config != NULL) {
    memset(config, 0, sizeof(*config));
    cai_client_config_init(&config->client_config);
    cai_agent_runtime_config_init(&config->runtime);
    config->runtime.preset = "smith";
  }
}

static int vectis_smith_named_load(void *context, const char *scope,
                                   char *session_id, size_t capacity,
                                   cai_source **out,
                                   unsigned long long *watermark,
                                   cai_error *error) {
  vectis_smith *smith = (vectis_smith *)context;
  return vectis_smith_store_load_selected(smith->store, scope,
                                          smith->requested_session, session_id,
                                          capacity, out, watermark, error);
}

static int vectis_smith_named_checkpoint(void *context, const char *scope,
                                         const char *session_id,
                                         cai_source *state,
                                         unsigned long long watermark,
                                         cai_error *error) {
  vectis_smith *smith = (vectis_smith *)context;
  return vectis_smith_store_checkpoint(smith->store, scope, session_id, state,
                                       watermark, error);
}

static int vectis_smith_named_append(void *context, const char *scope,
                                     const char *session_id,
                                     const cai_agent_session_event *event,
                                     cai_error *error) {
  vectis_smith *smith = (vectis_smith *)context;
  return vectis_smith_store_append_event(smith->store, scope, session_id, event,
                                         error);
}

static int vectis_smith_named_events(void *context, const char *scope,
                                     const char *session_id,
                                     unsigned long long after,
                                     cai_agent_session_event_fn callback,
                                     void *callback_context, cai_error *error) {
  vectis_smith *smith = (vectis_smith *)context;
  return vectis_smith_store_load_events_after(smith->store, scope, session_id,
                                              after, callback, callback_context,
                                              error);
}

vectis_status vectis_smith_open(const vectis_smith_config *config,
                                vectis_smith **out, vectis_error *error) {
  vectis_smith *smith;
  cai_agent_runtime_config runtime;
  cai_error caierr;
  int rc;

  if (out == NULL || config == NULL) {
    vectis_smith_set_error(error, VECTIS_ERR_INVALID,
                           "Smith configuration and output are required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  if (config->runtime.session_id != NULL &&
      (config->runtime.session_id[0] == '\0' ||
       strlen(config->runtime.session_id) >= CAI_AGENT_SESSION_ID_MAX)) {
    vectis_smith_set_error(error, VECTIS_ERR_INVALID,
                           "invalid Smith session id");
    return VECTIS_ERR_INVALID;
  }
  if (config->store != NULL && config->runtime.session_store != NULL) {
    vectis_smith_set_error(
        error, VECTIS_ERR_INVALID,
        "Smith store and runtime session_store are exclusive");
    return VECTIS_ERR_INVALID;
  }
  smith = (vectis_smith *)calloc(1u, sizeof(*smith));
  if (smith == NULL) {
    vectis_smith_set_error(error, VECTIS_ERR_NOMEM,
                           "failed to allocate Smith runtime");
    return VECTIS_ERR_NOMEM;
  }
  cai_error_init(&caierr);
  if (config->client != NULL) {
    smith->client = config->client;
  } else {
    rc = cai_client_open(&config->client_config, &smith->client, &caierr);
    if (rc != CAI_OK) {
      free(smith);
      (void)vectis_cai_error(error, &caierr, "failed to open CAI client");
      cai_error_cleanup(&caierr);
      return VECTIS_ERR_STATE;
    }
    smith->owns_client = 1;
  }
  runtime = config->runtime;
  if (runtime.preset == NULL) {
    runtime.preset = "smith";
  }
  if (config->store != NULL) {
    runtime.session_store = vectis_smith_store_session_store(config->store);
    if (runtime.resume_latest && runtime.session_id != NULL) {
      smith->store = config->store;
      strcpy(smith->requested_session, runtime.session_id);
      smith->named_store.context = smith;
      smith->named_store.load_latest = vectis_smith_named_load;
      smith->named_store.checkpoint = vectis_smith_named_checkpoint;
      smith->named_store.append_event = vectis_smith_named_append;
      smith->named_store.load_events_after = vectis_smith_named_events;
      runtime.session_store = &smith->named_store;
    }
  }
  rc =
      cai_agent_runtime_open(smith->client, &runtime, &smith->runtime, &caierr);
  if (rc != CAI_OK) {
    if (smith->owns_client) {
      cai_client_close(smith->client);
    }
    free(smith);
    (void)vectis_cai_error(error, &caierr, "failed to open Smith runtime");
    cai_error_cleanup(&caierr);
    return VECTIS_ERR_STATE;
  }
  cai_error_cleanup(&caierr);
  *out = smith;
  vectis_error_clear(error);
  return VECTIS_OK;
}

void vectis_smith_close(vectis_smith *smith) {
  if (smith != NULL) {
    if (smith->runtime != NULL) {
      cai_agent_runtime_close(smith->runtime);
    }
    if (smith->owns_client && smith->client != NULL) {
      cai_client_close(smith->client);
    }
    free(smith);
  }
}

cai_agent_runtime *vectis_smith_runtime(vectis_smith *smith) {
  return smith == NULL ? NULL : smith->runtime;
}

static vectis_status vectis_smith_call(vectis_smith *smith, const char *text,
                                       int (*operation)(cai_agent_runtime *,
                                                        const char *,
                                                        cai_error *),
                                       vectis_error *error) {
  cai_error caierr;
  int rc;

  if (smith == NULL || smith->runtime == NULL || text == NULL ||
      text[0] == '\0') {
    vectis_smith_set_error(error, VECTIS_ERR_INVALID,
                           "Smith runtime and non-empty text are required");
    return VECTIS_ERR_INVALID;
  }
  cai_error_init(&caierr);
  rc = operation(smith->runtime, text, &caierr);
  if (rc != CAI_OK) {
    (void)vectis_cai_error(error, &caierr, "Smith runtime operation failed");
    cai_error_cleanup(&caierr);
    return VECTIS_ERR_STATE;
  }
  cai_error_cleanup(&caierr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_smith_submit(vectis_smith *smith, const char *text,
                                  vectis_error *error) {
  return vectis_smith_call(smith, text, cai_agent_runtime_submit, error);
}

vectis_status vectis_smith_submit_steering(vectis_smith *smith,
                                           const char *text,
                                           vectis_error *error) {
  return vectis_smith_call(smith, text, cai_agent_runtime_submit_steering,
                           error);
}

vectis_status vectis_smith_submit_queued(vectis_smith *smith, const char *text,
                                         vectis_error *error) {
  return vectis_smith_call(smith, text, cai_agent_runtime_submit_queued, error);
}

vectis_status vectis_smith_pump(vectis_smith *smith, long timeout_ms,
                                vectis_error *error) {
  cai_error caierr;
  int rc;

  if (smith == NULL || smith->runtime == NULL) {
    vectis_smith_set_error(error, VECTIS_ERR_INVALID,
                           "Smith runtime is required");
    return VECTIS_ERR_INVALID;
  }
  cai_error_init(&caierr);
  rc = cai_agent_runtime_pump(smith->runtime, timeout_ms, &caierr);
  if (rc != CAI_OK) {
    (void)vectis_cai_error(error, &caierr, "failed to pump Smith runtime");
    cai_error_cleanup(&caierr);
    return VECTIS_ERR_STATE;
  }
  cai_error_cleanup(&caierr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_smith_wakeup_fd(const vectis_smith *smith, int *out_fd,
                                     vectis_error *error) {
  cai_error caierr;
  int rc;

  if (smith == NULL || smith->runtime == NULL || out_fd == NULL) {
    vectis_smith_set_error(error, VECTIS_ERR_INVALID,
                           "Smith runtime and wakeup-fd output are required");
    return VECTIS_ERR_INVALID;
  }
  cai_error_init(&caierr);
  rc = cai_agent_runtime_wakeup_fd(smith->runtime, out_fd, &caierr);
  if (rc != CAI_OK) {
    (void)vectis_cai_error(error, &caierr, "failed to obtain Smith wakeup fd");
    cai_error_cleanup(&caierr);
    return VECTIS_ERR_STATE;
  }
  cai_error_cleanup(&caierr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_smith_state(vectis_smith *smith, cai_agent_run_state *out,
                                 vectis_error *error) {
  cai_error caierr;
  int rc;

  if (smith == NULL || smith->runtime == NULL || out == NULL) {
    vectis_smith_set_error(error, VECTIS_ERR_INVALID,
                           "Smith runtime and state output are required");
    return VECTIS_ERR_INVALID;
  }
  cai_error_init(&caierr);
  rc = cai_agent_runtime_state(smith->runtime, out, &caierr);
  if (rc != CAI_OK) {
    (void)vectis_cai_error(error, &caierr,
                           "failed to read Smith runtime state");
    cai_error_cleanup(&caierr);
    return VECTIS_ERR_STATE;
  }
  cai_error_cleanup(&caierr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

const char *vectis_smith_session_id(const vectis_smith *smith) {
  return smith == NULL || smith->runtime == NULL
             ? NULL
             : cai_agent_runtime_session_id(smith->runtime);
}
