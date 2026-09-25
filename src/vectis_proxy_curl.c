#include "vectis_proxy_curl.h"

#include "vectis_internal.h"
#include "vectis_proxy_events.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <kore/kore.h>
#pragma GCC diagnostic pop

#include <stdlib.h>
#include <string.h>

/* The tested 256 MiB worker planning target reserves 11 MiB for each active
 * exchange and 80 MiB for the worker plus both idle connection caches. */
#define VECTIS_PROXY_CURL_ACTIVE_LIMIT 16u
#define VECTIS_PROXY_CURL_IDLE_LIMIT 4L

typedef struct vectis_proxy_curl_pool vectis_proxy_curl_pool;

typedef struct vectis_proxy_curl_watch {
  struct kore_event event;
  vectis_proxy_curl_pool *pool;
  struct vectis_proxy_curl_watch *next;
  struct vectis_proxy_curl_watch *retired_next;
  curl_socket_t fd;
  int interest;
  int retired;
  int handed_off;
} vectis_proxy_curl_watch;

struct vectis_proxy_curl_transfer {
  struct vectis_proxy_curl_transfer *next;
  vectis_proxy_curl_pool *pool;
  vectis_proxy_curl_done_fn done;
  void *userdata;
  CURL *easy;
  int active;
  int retain_completed;
  int completed;
};

struct vectis_proxy_curl_pool {
  CURLM *multi;
  struct kore_timer *timer;
  vectis_proxy_curl_transfer *transfers;
  vectis_proxy_curl_watch *watches;
  int running;
};

static vectis_proxy_curl_pool vectis_proxy_curl_pools[2];
static vectis_proxy_curl_watch *vectis_proxy_curl_retired;
static struct kore_timer *vectis_proxy_curl_retire_timer;
static size_t vectis_proxy_curl_active;
static int vectis_proxy_curl_ready;
static int vectis_proxy_curl_shutting_down;

vectis_status vectis_proxy_curl_set_ca(CURL *easy, char *pem, size_t pem_length,
                                       vectis_error *error) {
  struct curl_blob blob;

  if (easy == NULL || (pem == NULL && pem_length != 0u) ||
      (pem != NULL && pem_length == 0u)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "invalid proxy TLS CA bundle configuration");
    return VECTIS_ERR_INVALID;
  }
  if (pem == NULL) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  blob.data = pem;
  blob.len = pem_length;
  blob.flags = CURL_BLOB_COPY;
  if (curl_easy_setopt(easy, CURLOPT_CAINFO_BLOB, &blob) != CURLE_OK) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to configure proxy TLS CA bundle");
    return VECTIS_ERR_STATE;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_proxy_curl_set_client_identity(CURL *easy, char *cert_pem,
                                                    size_t cert_length,
                                                    char *key_pem,
                                                    size_t key_length,
                                                    vectis_error *error) {
  struct curl_blob blob;

  if (easy == NULL || (cert_pem == NULL) != (key_pem == NULL) ||
      (cert_pem == NULL && (cert_length != 0u || key_length != 0u)) ||
      (cert_pem != NULL && (cert_length == 0u || key_length == 0u))) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "invalid proxy TLS client identity configuration");
    return VECTIS_ERR_INVALID;
  }
  if (cert_pem == NULL) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  blob.data = cert_pem;
  blob.len = cert_length;
  blob.flags = CURL_BLOB_COPY;
  if (curl_easy_setopt(easy, CURLOPT_SSLCERT_BLOB, &blob) != CURLE_OK ||
      curl_easy_setopt(easy, CURLOPT_SSLCERTTYPE, "PEM") != CURLE_OK) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to configure proxy TLS client certificate");
    return VECTIS_ERR_STATE;
  }
  blob.data = key_pem;
  blob.len = key_length;
  if (curl_easy_setopt(easy, CURLOPT_SSLKEY_BLOB, &blob) != CURLE_OK ||
      curl_easy_setopt(easy, CURLOPT_SSLKEYTYPE, "PEM") != CURLE_OK) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to configure proxy TLS client key");
    return VECTIS_ERR_STATE;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

int vectis_proxy_curl_configure_http(CURL *easy, int force_http1) {
  if (easy == NULL || (force_http1 != 0 && force_http1 != 1))
    return 0;
  return curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,
                          force_http1 ? CURL_HTTP_VERSION_1_1
                                      : CURL_HTTP_VERSION_2TLS) == CURLE_OK &&
         (force_http1 ||
          curl_easy_setopt(easy, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2) ==
              CURLE_OK) &&
         curl_easy_setopt(easy, CURLOPT_NOPROXY, "*") == CURLE_OK &&
         curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
         curl_easy_setopt(easy, CURLOPT_HTTP_CONTENT_DECODING, 0L) == CURLE_OK;
}

static void vectis_proxy_curl_drive(vectis_proxy_curl_pool *pool,
                                    curl_socket_t fd, int flags);

static void vectis_proxy_curl_watch_unlink(vectis_proxy_curl_watch *watch) {
  vectis_proxy_curl_watch **slot;

  slot = &watch->pool->watches;
  while (*slot != NULL && *slot != watch)
    slot = &(*slot)->next;
  if (*slot == watch)
    *slot = watch->next;
  watch->next = NULL;
}

static void vectis_proxy_curl_reap(void *arg, u_int64_t now) {
  vectis_proxy_curl_watch *watch;

  (void)arg;
  (void)now;
  vectis_proxy_curl_retire_timer = NULL;
  while ((watch = vectis_proxy_curl_retired) != NULL) {
    vectis_proxy_curl_retired = watch->retired_next;
    free(watch);
  }
}

static void vectis_proxy_curl_event(void *arg, int error) {
  vectis_proxy_curl_watch *watch;
  int flags;

  watch = (vectis_proxy_curl_watch *)arg;
  if (watch->retired || watch->handed_off || vectis_proxy_curl_shutting_down) {
    return;
  }
  flags = error ? CURL_CSELECT_ERR : 0;
  if ((watch->event.flags & KORE_EVENT_READ) != 0) {
    flags |= CURL_CSELECT_IN;
  }
  if ((watch->event.flags & KORE_EVENT_WRITE) != 0) {
    flags |= CURL_CSELECT_OUT;
  }
  watch->event.flags &= ~(KORE_EVENT_READ | KORE_EVENT_WRITE);
  vectis_proxy_curl_drive(watch->pool, watch->fd, flags);
}

static int vectis_proxy_curl_socket(CURL *easy, curl_socket_t fd, int what,
                                    void *userdata, void *socket_userdata) {
  vectis_proxy_curl_pool *pool;
  vectis_proxy_curl_watch *watch;
  int desired;

  (void)easy;
  pool = (vectis_proxy_curl_pool *)userdata;
  watch = (vectis_proxy_curl_watch *)socket_userdata;
  if (what == CURL_POLL_REMOVE) {
    if (watch != NULL) {
      if (!watch->handed_off)
        vectis_proxy_event_update((int)fd, &watch->event, watch->interest, 0,
                                  0);
      watch->interest = 0;
      vectis_proxy_curl_watch_unlink(watch);
      watch->retired = 1;
      watch->retired_next = vectis_proxy_curl_retired;
      vectis_proxy_curl_retired = watch;
      if (!vectis_proxy_curl_shutting_down &&
          vectis_proxy_curl_retire_timer == NULL) {
        vectis_proxy_curl_retire_timer = kore_timer_add(
            vectis_proxy_curl_reap, 1u, NULL, KORE_TIMER_ONESHOT);
      }
    }
    return 0;
  }
  if (watch == NULL) {
    watch = (vectis_proxy_curl_watch *)calloc(1u, sizeof(*watch));
    if (watch == NULL) {
      return -1;
    }
    watch->event.type = KORE_TYPE_CONNECTION;
    watch->event.handle = vectis_proxy_curl_event;
    watch->pool = pool;
    watch->fd = fd;
    if (curl_multi_assign(pool->multi, fd, watch) != CURLM_OK) {
      free(watch);
      return -1;
    }
    watch->next = pool->watches;
    pool->watches = watch;
  }
  if (watch->handed_off)
    return 0;
  desired = (what & CURL_POLL_IN ? VECTIS_PROXY_EVENT_READ : 0) |
            (what & CURL_POLL_OUT ? VECTIS_PROXY_EVENT_WRITE : 0);
  vectis_proxy_event_update((int)fd, &watch->event, watch->interest, desired,
                            0);
  watch->interest = desired;
  return 0;
}

static void vectis_proxy_curl_timeout(void *arg, u_int64_t now) {
  vectis_proxy_curl_pool *pool;

  (void)now;
  pool = (vectis_proxy_curl_pool *)arg;
  pool->timer = NULL;
  vectis_proxy_curl_drive(pool, CURL_SOCKET_TIMEOUT, 0);
}

static int vectis_proxy_curl_timer(CURLM *multi, long timeout_ms,
                                   void *userdata) {
  vectis_proxy_curl_pool *pool;

  (void)multi;
  pool = (vectis_proxy_curl_pool *)userdata;
  if (pool->timer != NULL) {
    kore_timer_remove(pool->timer);
    pool->timer = NULL;
  }
  if (timeout_ms >= 0L && !vectis_proxy_curl_shutting_down) {
    pool->timer = kore_timer_add(vectis_proxy_curl_timeout,
                                 (u_int64_t)(timeout_ms > 0L ? timeout_ms : 1L),
                                 pool, KORE_TIMER_ONESHOT);
  }
  return 0;
}

static void vectis_proxy_curl_unlink(vectis_proxy_curl_transfer *transfer) {
  vectis_proxy_curl_transfer **slot;

  slot = &transfer->pool->transfers;
  while (*slot != NULL && *slot != transfer) {
    slot = &(*slot)->next;
  }
  if (*slot == transfer) {
    *slot = transfer->next;
  }
  transfer->next = NULL;
}

static void vectis_proxy_curl_drive(vectis_proxy_curl_pool *pool,
                                    curl_socket_t fd, int flags) {
  vectis_proxy_curl_transfer *transfer;
  CURLMsg *message;
  CURLcode result;
  int pending;

  if (pool->multi == NULL || vectis_proxy_curl_shutting_down) {
    return;
  }
  (void)curl_multi_socket_action(pool->multi, fd, flags, &pool->running);
  while ((message = curl_multi_info_read(pool->multi, &pending)) != NULL) {
    if (message->msg != CURLMSG_DONE) {
      continue;
    }
    transfer = NULL;
    (void)curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &transfer);
    if (transfer == NULL || !transfer->active) {
      continue;
    }
    result = message->data.result;
    if (transfer->retain_completed && result == CURLE_OK) {
      transfer->completed = 1;
      transfer->done(transfer->easy, result, transfer->userdata);
      continue;
    }
    transfer->active = 0;
    vectis_proxy_curl_unlink(transfer);
    (void)curl_multi_remove_handle(pool->multi, transfer->easy);
    vectis_proxy_curl_active--;
    transfer->done(transfer->easy, result, transfer->userdata);
    curl_easy_cleanup(transfer->easy);
    free(transfer);
  }
}

static int vectis_proxy_curl_pool_init(vectis_proxy_curl_pool *pool) {
  pool->multi = curl_multi_init();
  if (pool->multi == NULL) {
    return 0;
  }
  if (curl_multi_setopt(pool->multi, CURLMOPT_SOCKETFUNCTION,
                        vectis_proxy_curl_socket) != CURLM_OK ||
      curl_multi_setopt(pool->multi, CURLMOPT_SOCKETDATA, pool) != CURLM_OK ||
      curl_multi_setopt(pool->multi, CURLMOPT_TIMERFUNCTION,
                        vectis_proxy_curl_timer) != CURLM_OK ||
      curl_multi_setopt(pool->multi, CURLMOPT_TIMERDATA, pool) != CURLM_OK ||
      curl_multi_setopt(pool->multi, CURLMOPT_PIPELINING, CURLPIPE_NOTHING) !=
          CURLM_OK ||
      curl_multi_setopt(pool->multi, CURLMOPT_MAX_CONCURRENT_STREAMS, 1L) !=
          CURLM_OK ||
      curl_multi_setopt(pool->multi, CURLMOPT_MAX_TOTAL_CONNECTIONS,
                        (long)VECTIS_PROXY_CURL_ACTIVE_LIMIT) != CURLM_OK ||
      curl_multi_setopt(pool->multi, CURLMOPT_MAXCONNECTS,
                        VECTIS_PROXY_CURL_IDLE_LIMIT) != CURLM_OK) {
    curl_multi_cleanup(pool->multi);
    pool->multi = NULL;
    return 0;
  }
  return 1;
}

static int vectis_proxy_curl_worker_init(vectis_error *error) {
  const curl_version_info_data *version;

  if (vectis_proxy_curl_ready) {
    return 1;
  }
  version = curl_version_info(CURLVERSION_NOW);
  if (version == NULL || (version->features & CURL_VERSION_ASYNCHDNS) == 0) {
    vectis_set_error(error, VECTIS_ERR_NOT_IMPLEMENTED,
                     "proxy requires asynchronous libcurl DNS");
    return 0;
  }
  if (!vectis_proxy_curl_pool_init(&vectis_proxy_curl_pools[0]) ||
      !vectis_proxy_curl_pool_init(&vectis_proxy_curl_pools[1])) {
    vectis_proxy_curl_worker_cleanup();
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to initialize proxy curl pools");
    return 0;
  }
  vectis_proxy_curl_ready = 1;
  return 1;
}

static vectis_status vectis_proxy_curl_submit_internal(
    CURL *easy, int force_http1, int retain_completed,
    vectis_proxy_curl_done_fn done, void *userdata,
    vectis_proxy_curl_transfer **out, vectis_error *error) {
  vectis_proxy_curl_transfer *transfer;
  vectis_proxy_curl_pool *pool;

  if (out != NULL) {
    *out = NULL;
  }
  if (easy == NULL || done == NULL || out == NULL ||
      (force_http1 != 0 && force_http1 != 1)) {
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "proxy curl submission requires easy, callback and output");
    return VECTIS_ERR_INVALID;
  }
  if (!vectis_proxy_curl_admission_available()) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "proxy worker exchange limit reached");
    return VECTIS_ERR_STATE;
  }
  if (!vectis_proxy_curl_worker_init(error)) {
    return error != NULL ? error->code : VECTIS_ERR_NOMEM;
  }
  pool = &vectis_proxy_curl_pools[force_http1 ? 1 : 0];
  transfer = (vectis_proxy_curl_transfer *)calloc(1u, sizeof(*transfer));
  if (transfer == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate proxy curl transfer");
    return VECTIS_ERR_NOMEM;
  }
  transfer->pool = pool;
  transfer->done = done;
  transfer->userdata = userdata;
  transfer->easy = easy;
  transfer->retain_completed = retain_completed;
  if (curl_easy_setopt(easy, CURLOPT_PRIVATE, transfer) != CURLE_OK ||
      !vectis_proxy_curl_configure_http(easy, force_http1)) {
    (void)curl_easy_setopt(easy, CURLOPT_PRIVATE, NULL);
    free(transfer);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to configure proxy curl transfer");
    return VECTIS_ERR_STATE;
  }
  transfer->active = 1;
  transfer->next = pool->transfers;
  pool->transfers = transfer;
  vectis_proxy_curl_active++;
  if (curl_multi_add_handle(pool->multi, easy) != CURLM_OK) {
    vectis_proxy_curl_unlink(transfer);
    vectis_proxy_curl_active--;
    (void)curl_easy_setopt(easy, CURLOPT_PRIVATE, NULL);
    free(transfer);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to add proxy curl transfer");
    return VECTIS_ERR_STATE;
  }
  *out = transfer;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_proxy_curl_submit(CURL *easy, int force_http1,
                                       vectis_proxy_curl_done_fn done,
                                       void *userdata,
                                       vectis_proxy_curl_transfer **out,
                                       vectis_error *error) {
  return vectis_proxy_curl_submit_internal(easy, force_http1, 0, done, userdata,
                                           out, error);
}

vectis_status vectis_proxy_curl_submit_connect(CURL *easy,
                                               vectis_proxy_curl_done_fn done,
                                               void *userdata,
                                               vectis_proxy_curl_transfer **out,
                                               vectis_error *error) {
  if (easy == NULL ||
      curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 1L) != CURLE_OK ||
      curl_easy_setopt(easy, CURLOPT_SSL_ENABLE_ALPN, 0L) != CURLE_OK ||
      curl_easy_setopt(easy, CURLOPT_FRESH_CONNECT, 1L) != CURLE_OK) {
    if (out != NULL)
      *out = NULL;
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "failed to configure raw proxy connection");
    return VECTIS_ERR_INVALID;
  }
  return vectis_proxy_curl_submit_internal(easy, 1, 1, done, userdata, out,
                                           error);
}

vectis_status
vectis_proxy_curl_handoff_socket(vectis_proxy_curl_transfer *transfer,
                                 curl_socket_t *out, vectis_error *error) {
  vectis_proxy_curl_watch *watch;
  curl_socket_t fd;

  if (out != NULL)
    *out = CURL_SOCKET_BAD;
  if (transfer == NULL || out == NULL || !transfer->active ||
      !transfer->completed || !transfer->retain_completed ||
      curl_easy_getinfo(transfer->easy, CURLINFO_ACTIVESOCKET, &fd) !=
          CURLE_OK ||
      fd == CURL_SOCKET_BAD) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "raw proxy socket is not ready for handoff");
    return VECTIS_ERR_STATE;
  }
  for (watch = transfer->pool->watches; watch != NULL; watch = watch->next) {
    if (watch->fd != fd)
      continue;
    if (!watch->handed_off) {
      vectis_proxy_event_update((int)fd, &watch->event, watch->interest, 0, 0);
      watch->interest = 0;
      watch->handed_off = 1;
    }
    break;
  }
  *out = fd;
  vectis_error_clear(error);
  return VECTIS_OK;
}

void vectis_proxy_curl_cancel(vectis_proxy_curl_transfer *transfer) {
  if (transfer == NULL || !transfer->active) {
    return;
  }
  transfer->active = 0;
  vectis_proxy_curl_unlink(transfer);
  (void)curl_easy_setopt(transfer->easy, CURLOPT_FORBID_REUSE, 1L);
  (void)curl_multi_remove_handle(transfer->pool->multi, transfer->easy);
  vectis_proxy_curl_active--;
  curl_easy_cleanup(transfer->easy);
  free(transfer);
}

void vectis_proxy_curl_worker_cleanup(void) {
  size_t i;
  vectis_proxy_curl_transfer *transfer;
  vectis_proxy_curl_watch *watch;

  vectis_proxy_curl_shutting_down = 1;
  for (i = 0u; i < 2u; ++i) {
    while ((transfer = vectis_proxy_curl_pools[i].transfers) != NULL) {
      vectis_proxy_curl_cancel(transfer);
    }
    if (vectis_proxy_curl_pools[i].timer != NULL) {
      kore_timer_remove(vectis_proxy_curl_pools[i].timer);
      vectis_proxy_curl_pools[i].timer = NULL;
    }
    if (vectis_proxy_curl_pools[i].multi != NULL) {
      curl_multi_cleanup(vectis_proxy_curl_pools[i].multi);
      vectis_proxy_curl_pools[i].multi = NULL;
    }
    while ((watch = vectis_proxy_curl_pools[i].watches) != NULL) {
      vectis_proxy_curl_pools[i].watches = watch->next;
      free(watch);
    }
  }
  if (vectis_proxy_curl_retire_timer != NULL) {
    kore_timer_remove(vectis_proxy_curl_retire_timer);
    vectis_proxy_curl_retire_timer = NULL;
  }
  vectis_proxy_curl_reap(NULL, 0u);
  vectis_proxy_curl_active = 0u;
  vectis_proxy_curl_ready = 0;
  vectis_proxy_curl_shutting_down = 0;
}

size_t vectis_proxy_curl_active_count(void) { return vectis_proxy_curl_active; }

int vectis_proxy_curl_admission_available(void) {
  return vectis_proxy_curl_active < VECTIS_PROXY_CURL_ACTIVE_LIMIT;
}
