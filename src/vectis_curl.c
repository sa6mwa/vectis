#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vectis/curl.h>
typedef struct curl_owner {
  pid_t process;
  pthread_t thread;
  int busy, violated;
} curl_owner;
typedef struct curl_bytes {
  unsigned char *data;
  size_t size, capacity;
} curl_bytes;
typedef struct curl_transfer {
  CURL *native;
  int closing;
  curl_owner *owner;
  vectis_curl_request request;
  vectis_curl_result result;
  curl_bytes body, headers;
  char message[CURL_ERROR_SIZE];
  struct curl_transfer *next;
} curl_transfer;
typedef struct curl_session_state {
  vectis_curl_session receiver;
  curl_owner owner;
  curl_transfer transfer;
} curl_session_state;
typedef struct curl_multi_state {
  vectis_curl_multi receiver;
  curl_owner owner;
  CURLM *native;
  curl_transfer *active, *head, *tail;
  size_t count, queued;
  unsigned long next_id;
} curl_multi_state;
static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static CURLcode curl_initialization = CURLE_FAILED_INIT;
static void curl_initialize(void) {
  curl_initialization = curl_global_init(CURL_GLOBAL_DEFAULT);
}
static vectis_status curl_failure(vectis_error *error, vectis_status status,
                                  const char *message) {
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->code = status;
    error->source = VECTIS_ERROR_SOURCE_CURL;
    strncpy(error->message, message, sizeof(error->message) - 1u);
  }
  return status;
}
static vectis_status curl_check(curl_owner *owner, vectis_error *error) {
  if (owner == NULL)
    return curl_failure(error, VECTIS_ERR_INVALID, "missing curl receiver");
  if (owner->process != getpid() ||
      !pthread_equal(owner->thread, pthread_self()))
    return curl_failure(error, VECTIS_ERR_STATE,
                        "curl receiver belongs to another process/thread");
  if (owner->busy) {
    owner->violated = 1;
    return curl_failure(error, VECTIS_ERR_STATE,
                        "cannot reenter or close active curl receiver");
  }
  return VECTIS_OK;
}
static void curl_owner_init(curl_owner *owner) {
  owner->process = getpid();
  owner->thread = pthread_self();
}
static void curl_latch(curl_transfer *transfer, vectis_status status,
                       const vectis_error *error) {
  if (transfer->result.failure.code != VECTIS_OK)
    return;
  if (status != VECTIS_OK) {
    if (error != NULL && error->code != VECTIS_OK)
      transfer->result.failure = *error;
    else
      curl_failure(&transfer->result.failure, status, "curl callback failed");
  } else if (transfer->owner->violated)
    curl_failure(&transfer->result.failure, VECTIS_ERR_STATE,
                 "curl callback attempted receiver reentry/close");
  transfer->owner->violated = 0;
}
static int curl_append(curl_transfer *transfer, curl_bytes *buffer,
                       const void *data, size_t size, size_t limit) {
  size_t capacity;
  unsigned char *grown;
  if (size > limit - buffer->size) {
    curl_failure(&transfer->result.failure, VECTIS_ERR_INVALID,
                 "curl response body/header limit exceeded");
    return 0;
  }
  if (buffer->size + size > buffer->capacity) {
    capacity = buffer->capacity == 0u ? 4096u : buffer->capacity;
    while (capacity < buffer->size + size) {
      capacity = capacity > limit / 2u ? limit : capacity * 2u;
    }
    grown = (unsigned char *)realloc(buffer->data, capacity);
    if (grown == NULL) {
      curl_failure(&transfer->result.failure, VECTIS_ERR_NOMEM,
                   "allocating curl response buffer");
      return 0;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
  }
  if (size != 0u)
    memcpy(buffer->data + buffer->size, data, size);
  buffer->size += size;
  return 1;
}
static size_t curl_write_body(char *data, size_t width, size_t count,
                              void *context) {
  curl_transfer *transfer = (curl_transfer *)context;
  vectis_error error;
  vectis_status status;
  size_t size, offset = 0u, chunk;
  if (width != 0u && count > (size_t)-1 / width)
    return 0u;
  size = width * count;
  if (transfer->closing || transfer->result.failure.code != VECTIS_OK)
    return 0u;
  if (transfer->request.write == NULL)
    return curl_append(transfer, &transfer->body, data, size,
                       VECTIS_CURL_BODY_LIMIT)
               ? size
               : 0u;
  while (offset < size) {
    chunk = size - offset > VECTIS_CURL_CHUNK_LIMIT ? VECTIS_CURL_CHUNK_LIMIT
                                                    : size - offset;
    memset(&error, 0, sizeof(error));
    status = transfer->request.write(transfer->request.context, data + offset,
                                     chunk, &error);
    curl_latch(transfer, status, &error);
    if (transfer->result.failure.code != VECTIS_OK)
      return 0u;
    offset += chunk;
  }
  return size;
}
static size_t curl_write_header(char *data, size_t width, size_t count,
                                void *context) {
  curl_transfer *transfer = (curl_transfer *)context;
  size_t size;
  if (width != 0u && count > (size_t)-1 / width)
    return 0u;
  size = width * count;
  if (transfer->closing || transfer->result.failure.code != VECTIS_OK)
    return 0u;
  return curl_append(transfer, &transfer->headers, data, size,
                     VECTIS_CURL_HEADER_LIMIT)
             ? size
             : 0u;
}
static size_t curl_read_body(char *data, size_t width, size_t count,
                             void *context) {
  curl_transfer *transfer = (curl_transfer *)context;
  size_t capacity, size = 0u;
  vectis_error error;
  vectis_status status;
  if (width != 0u && count > (size_t)-1 / width)
    return CURL_READFUNC_ABORT;
  capacity = width * count;
  if (capacity > VECTIS_CURL_CHUNK_LIMIT)
    capacity = VECTIS_CURL_CHUNK_LIMIT;
  if (transfer->closing || transfer->result.failure.code != VECTIS_OK)
    return CURL_READFUNC_ABORT;
  memset(&error, 0, sizeof(error));
  status = transfer->request.read(transfer->request.context, data, capacity,
                                  &size, &error);
  curl_latch(transfer, status, &error);
  if (size > capacity)
    curl_failure(&transfer->result.failure, VECTIS_ERR_INVALID,
                 "curl reader exceeded requested capacity");
  return transfer->result.failure.code == VECTIS_OK ? size
                                                    : CURL_READFUNC_ABORT;
}
static int curl_progress(void *context, curl_off_t total, curl_off_t current,
                         curl_off_t upload_total, curl_off_t uploaded) {
  curl_transfer *transfer = (curl_transfer *)context;
  vectis_curl_progress progress;
  vectis_error error;
  vectis_status status;
  int cancel = 0;
  if (transfer->closing || transfer->result.failure.code != VECTIS_OK)
    return 1;
  if (transfer->request.progress == NULL)
    return 0;
  progress.download_total = total;
  progress.downloaded = current;
  progress.upload_total = upload_total;
  progress.uploaded = uploaded;
  memset(&error, 0, sizeof(error));
  status = transfer->request.progress(transfer->request.context, &progress,
                                      &cancel, &error);
  curl_latch(transfer, status, &error);
  if (cancel && transfer->result.failure.code == VECTIS_OK)
    curl_failure(&transfer->result.failure, VECTIS_ERR_STATE,
                 "curl transfer cancelled by progress callback");
  return transfer->result.failure.code != VECTIS_OK;
}
/* libcurl may call progress/header hooks while disconnecting. Quiesce the
 * engine first, then detach callbacks before releasing borrowed contexts. */
static size_t curl_close_discard(char *data, size_t width, size_t count,
                                 void *context) {
  (void)data;
  (void)context;
  return width != 0u && count > (size_t)-1 / width ? 0u : width * count;
}
static size_t curl_close_read(char *data, size_t width, size_t count,
                              void *context) {
  (void)data;
  (void)width;
  (void)count;
  (void)context;
  return CURL_READFUNC_ABORT;
}
static void curl_transfer_quiesce(curl_transfer *transfer) {
  transfer->closing = 1;
  (void)curl_easy_setopt(transfer->native, CURLOPT_NOPROGRESS, 1L);
  (void)curl_easy_setopt(transfer->native, CURLOPT_XFERINFOFUNCTION, NULL);
  (void)curl_easy_setopt(transfer->native, CURLOPT_XFERINFODATA, NULL);
  (void)curl_easy_setopt(transfer->native, CURLOPT_HEADERFUNCTION,
                         curl_close_discard);
  (void)curl_easy_setopt(transfer->native, CURLOPT_HEADERDATA, NULL);
  (void)curl_easy_setopt(transfer->native, CURLOPT_WRITEFUNCTION,
                         curl_close_discard);
  (void)curl_easy_setopt(transfer->native, CURLOPT_WRITEDATA, NULL);
  (void)curl_easy_setopt(transfer->native, CURLOPT_READFUNCTION,
                         curl_close_read);
  (void)curl_easy_setopt(transfer->native, CURLOPT_READDATA, NULL);
}
static void curl_transfer_clear(curl_transfer *transfer, int destroy) {
  curl_transfer_quiesce(transfer);
  if (destroy)
    curl_easy_cleanup(transfer->native);
  else
    curl_easy_reset(transfer->native);
  free(transfer->body.data);
  free(transfer->headers.data);
  memset(&transfer->body, 0, sizeof(transfer->body));
  memset(&transfer->headers, 0, sizeof(transfer->headers));
  memset(&transfer->result, 0, sizeof(transfer->result));
  memset(&transfer->request, 0, sizeof(transfer->request));
  memset(transfer->message, 0, sizeof(transfer->message));
  transfer->closing = 0;
}
static vectis_status curl_transfer_prepare(curl_transfer *transfer,
                                           const vectis_curl_request *request,
                                           vectis_error *error) {
  vectis_status status;
  CURLcode code;
  vectis_error configured_error;
  if (request == NULL || request->configure == NULL)
    return curl_failure(error, VECTIS_ERR_INVALID,
                        "curl request requires a configure hook");
  curl_transfer_clear(transfer, 0);
  transfer->request = *request;
  memset(&configured_error, 0, sizeof(configured_error));
  status =
      request->configure(transfer->native, request->context, &configured_error);
  if (status != VECTIS_OK) {
    if (configured_error.code != VECTIS_OK) {
      if (error != NULL)
        *error = configured_error;
    } else
      curl_failure(error, status, "curl request configuration failed");
    return status;
  }
  if (transfer->owner->violated)
    return curl_failure(error, VECTIS_ERR_STATE,
                        "curl configuration attempted receiver reentry/close");
#define CURL_SET(option, value)                                                \
  do {                                                                         \
    code = curl_easy_setopt(transfer->native, option, value);                  \
    if (code != CURLE_OK) {                                                    \
      curl_failure(error, VECTIS_ERR_STATE, curl_easy_strerror(code));         \
      if (error != NULL)                                                       \
        error->dependency_code = (int)code;                                    \
      return VECTIS_ERR_STATE;                                                 \
    }                                                                          \
  } while (0)
  CURL_SET(CURLOPT_ERRORBUFFER, transfer->message);
  CURL_SET(CURLOPT_WRITEFUNCTION, curl_write_body);
  CURL_SET(CURLOPT_WRITEDATA, transfer);
  CURL_SET(CURLOPT_HEADERFUNCTION, curl_write_header);
  CURL_SET(CURLOPT_HEADERDATA, transfer);
  if (request->read != NULL) {
    CURL_SET(CURLOPT_READFUNCTION, curl_read_body);
    CURL_SET(CURLOPT_READDATA, transfer);
  }
  CURL_SET(CURLOPT_XFERINFOFUNCTION, curl_progress);
  CURL_SET(CURLOPT_XFERINFODATA, transfer);
  CURL_SET(CURLOPT_NOPROGRESS, 0L);
  CURL_SET(CURLOPT_PRIVATE, transfer);
#undef CURL_SET
  return VECTIS_OK;
}
static void curl_transfer_result(curl_transfer *transfer, CURLcode code) {
  transfer->result.code = code;
  transfer->result.streamed = transfer->request.write != NULL;
  transfer->result.body = transfer->body.data;
  transfer->result.body_size = transfer->body.size;
  transfer->result.headers = transfer->headers.data;
  transfer->result.header_size = transfer->headers.size;
  (void)curl_easy_getinfo(transfer->native, CURLINFO_RESPONSE_CODE,
                          &transfer->result.status);
  (void)curl_easy_getinfo(transfer->native, CURLINFO_EFFECTIVE_URL,
                          &transfer->result.effective_url);
  transfer->result.message = transfer->message[0] == '\0'
                                 ? curl_easy_strerror(code)
                                 : transfer->message;
  transfer->result.context = transfer->request.context;
  if (transfer->result.failure.code != VECTIS_OK && code == CURLE_OK)
    transfer->result.code = CURLE_ABORTED_BY_CALLBACK;
}
vectis_status vectis_curl_session_new(vectis_curl_session **out,
                                      vectis_error *error) {
  curl_session_state *state;
  if (out == NULL)
    return curl_failure(error, VECTIS_ERR_INVALID,
                        "missing curl session output");
  *out = NULL;
  (void)pthread_once(&curl_once, curl_initialize);
  if (curl_initialization != CURLE_OK)
    return curl_failure(error, VECTIS_ERR_STATE,
                        "curl global initialization failed");
  state = (curl_session_state *)calloc(1u, sizeof(*state));
  if (state == NULL)
    return curl_failure(error, VECTIS_ERR_NOMEM, "allocating curl session");
  state->transfer.native = curl_easy_init();
  if (state->transfer.native == NULL) {
    free(state);
    return curl_failure(error, VECTIS_ERR_NOMEM, "allocating curl easy handle");
  }
  curl_owner_init(&state->owner);
  state->transfer.owner = &state->owner;
  state->receiver.perform = vectis_curl_session_perform;
  state->receiver.reset = vectis_curl_session_reset;
  state->receiver.close = vectis_curl_session_close;
  state->receiver.impl = state;
  *out = &state->receiver;
  return VECTIS_OK;
}
vectis_status vectis_curl_session_perform(vectis_curl_session *receiver,
                                          const vectis_curl_request *request,
                                          const vectis_curl_result **out,
                                          vectis_error *error) {
  curl_session_state *state =
      receiver == NULL ? NULL : (curl_session_state *)receiver->impl;
  vectis_status status =
      curl_check(state == NULL ? NULL : &state->owner, error);
  CURLcode code;
  if (out != NULL)
    *out = NULL;
  if (status != VECTIS_OK)
    return status;
  if (out == NULL)
    return curl_failure(error, VECTIS_ERR_INVALID,
                        "missing curl result output");
  state->owner.busy = 1;
  state->owner.violated = 0;
  status = curl_transfer_prepare(&state->transfer, request, error);
  if (status == VECTIS_OK) {
    code = curl_easy_perform(state->transfer.native);
    curl_transfer_result(&state->transfer, code);
    *out = &state->transfer.result;
  }
  state->owner.busy = 0;
  return status;
}
vectis_status vectis_curl_session_reset(vectis_curl_session *receiver,
                                        vectis_error *error) {
  curl_session_state *state =
      receiver == NULL ? NULL : (curl_session_state *)receiver->impl;
  vectis_status status =
      curl_check(state == NULL ? NULL : &state->owner, error);
  if (status != VECTIS_OK)
    return status;
  curl_transfer_clear(&state->transfer, 0);
  return VECTIS_OK;
}
vectis_status vectis_curl_session_close(vectis_curl_session *receiver,
                                        vectis_error *error) {
  curl_session_state *state =
      receiver == NULL ? NULL : (curl_session_state *)receiver->impl;
  vectis_status status;
  if (state == NULL)
    return VECTIS_OK;
  status = curl_check(&state->owner, error);
  if (status != VECTIS_OK)
    return status;
  curl_transfer_clear(&state->transfer, 1);
  free(state);
  return VECTIS_OK;
}
vectis_status vectis_curl_multi_new(vectis_curl_multi **out,
                                    vectis_error *error) {
  curl_multi_state *state;
  if (out == NULL)
    return curl_failure(error, VECTIS_ERR_INVALID, "missing curl multi output");
  *out = NULL;
  (void)pthread_once(&curl_once, curl_initialize);
  if (curl_initialization != CURLE_OK)
    return curl_failure(error, VECTIS_ERR_STATE,
                        "curl global initialization failed");
  state = (curl_multi_state *)calloc(1u, sizeof(*state));
  if (state == NULL)
    return curl_failure(error, VECTIS_ERR_NOMEM, "allocating curl multi");
  state->native = curl_multi_init();
  if (state->native == NULL) {
    free(state);
    return curl_failure(error, VECTIS_ERR_NOMEM,
                        "allocating native curl multi");
  }
  curl_owner_init(&state->owner);
  state->next_id = 1u;
  state->receiver.add = vectis_curl_multi_add;
  state->receiver.step = vectis_curl_multi_step;
  state->receiver.poll = vectis_curl_multi_poll;
  state->receiver.completion = vectis_curl_multi_completion;
  state->receiver.consume = vectis_curl_multi_consume;
  state->receiver.close = vectis_curl_multi_close;
  state->receiver.impl = state;
  *out = &state->receiver;
  return VECTIS_OK;
}
vectis_status vectis_curl_multi_add(vectis_curl_multi *receiver,
                                    const vectis_curl_request *request,
                                    unsigned long *id, vectis_error *error) {
  curl_multi_state *state =
      receiver == NULL ? NULL : (curl_multi_state *)receiver->impl;
  curl_transfer *transfer;
  vectis_status status =
      curl_check(state == NULL ? NULL : &state->owner, error);
  CURLMcode code;
  if (id != NULL)
    *id = 0u;
  if (status != VECTIS_OK)
    return status;
  if (id == NULL || state->count >= VECTIS_CURL_MULTI_LIMIT ||
      state->next_id == ULONG_MAX)
    return curl_failure(
        error, VECTIS_ERR_STATE,
        "curl multi capacity/transfer IDs exhausted; drain completions");
  transfer = (curl_transfer *)calloc(1u, sizeof(*transfer));
  if (transfer == NULL)
    return curl_failure(error, VECTIS_ERR_NOMEM, "allocating curl transfer");
  transfer->native = curl_easy_init();
  if (transfer->native == NULL) {
    free(transfer);
    return curl_failure(error, VECTIS_ERR_NOMEM, "allocating curl easy handle");
  }
  transfer->owner = &state->owner;
  state->owner.busy = 1;
  state->owner.violated = 0;
  status = curl_transfer_prepare(transfer, request, error);
  if (status == VECTIS_OK) {
    code = curl_multi_add_handle(state->native, transfer->native);
    if (code != CURLM_OK) {
      status = curl_failure(error, VECTIS_ERR_STATE, curl_multi_strerror(code));
      if (error != NULL)
        error->dependency_code = (int)code;
    }
  }
  state->owner.busy = 0;
  if (status != VECTIS_OK) {
    curl_transfer_clear(transfer, 1);
    free(transfer);
    return status;
  }
  transfer->result.id = state->next_id++;
  *id = transfer->result.id;
  transfer->next = state->active;
  state->active = transfer;
  ++state->count;
  return VECTIS_OK;
}
vectis_status vectis_curl_multi_step(vectis_curl_multi *receiver,
                                     size_t *running, vectis_error *error) {
  curl_multi_state *state =
      receiver == NULL ? NULL : (curl_multi_state *)receiver->impl;
  vectis_status status =
      curl_check(state == NULL ? NULL : &state->owner, error);
  CURLMcode code;
  CURLMsg *message;
  curl_transfer *transfer, **link;
  int active, left;
  if (running != NULL)
    *running = 0u;
  if (status != VECTIS_OK)
    return status;
  if (running == NULL)
    return curl_failure(error, VECTIS_ERR_INVALID,
                        "missing curl running output");
  state->owner.busy = 1;
  state->owner.violated = 0;
  code = curl_multi_perform(state->native, &active);
  if (code != CURLM_OK) {
    state->owner.busy = 0;
    status = curl_failure(error, VECTIS_ERR_STATE, curl_multi_strerror(code));
    if (error != NULL)
      error->dependency_code = (int)code;
    return status;
  }
  while ((message = curl_multi_info_read(state->native, &left)) != NULL) {
    if (message->msg != CURLMSG_DONE)
      continue;
    link = &state->active;
    while (*link != NULL && (*link)->native != message->easy_handle)
      link = &(*link)->next;
    if (*link == NULL) {
      state->owner.busy = 0;
      return curl_failure(error, VECTIS_ERR_STATE,
                          "curl completed an unknown transfer");
    }
    transfer = *link;
    curl_transfer_result(transfer, message->data.result);
    curl_transfer_quiesce(transfer);
    code = curl_multi_remove_handle(state->native, transfer->native);
    if (code != CURLM_OK) {
      state->owner.busy = 0;
      return curl_failure(error, VECTIS_ERR_STATE, curl_multi_strerror(code));
    }
    *link = transfer->next;
    transfer->next = NULL;
    if (state->tail == NULL)
      state->head = transfer;
    else
      state->tail->next = transfer;
    state->tail = transfer;
    ++state->queued;
  }
  state->owner.busy = 0;
  *running = (size_t)active;
  return VECTIS_OK;
}
vectis_status vectis_curl_multi_poll(vectis_curl_multi *receiver,
                                     int timeout_ms, int *ready,
                                     vectis_error *error) {
  curl_multi_state *state =
      receiver == NULL ? NULL : (curl_multi_state *)receiver->impl;
  vectis_status status =
      curl_check(state == NULL ? NULL : &state->owner, error);
  CURLMcode code;
  if (ready != NULL)
    *ready = 0;
  if (status != VECTIS_OK)
    return status;
  if (ready == NULL || timeout_ms < 0 || timeout_ms > 60000)
    return curl_failure(error, VECTIS_ERR_INVALID,
                        "curl poll timeout must be 0..60000 ms");
  if (state->queued != 0u || state->active == NULL)
    return VECTIS_OK;
  state->owner.busy = 1;
  code = curl_multi_poll(state->native, NULL, 0u, timeout_ms, ready);
  state->owner.busy = 0;
  if (code != CURLM_OK) {
    status = curl_failure(error, VECTIS_ERR_STATE, curl_multi_strerror(code));
    if (error != NULL)
      error->dependency_code = (int)code;
    return status;
  }
  return VECTIS_OK;
}
vectis_status vectis_curl_multi_completion(vectis_curl_multi *receiver,
                                           size_t offset,
                                           const vectis_curl_result **out,
                                           vectis_error *error) {
  curl_multi_state *state =
      receiver == NULL ? NULL : (curl_multi_state *)receiver->impl;
  vectis_status status =
      curl_check(state == NULL ? NULL : &state->owner, error);
  curl_transfer *transfer;
  if (out != NULL)
    *out = NULL;
  if (status != VECTIS_OK)
    return status;
  if (out == NULL)
    return curl_failure(error, VECTIS_ERR_INVALID,
                        "missing curl completion output");
  transfer = state->head;
  while (transfer != NULL && offset != 0u) {
    transfer = transfer->next;
    --offset;
  }
  if (transfer != NULL)
    *out = &transfer->result;
  return VECTIS_OK;
}
vectis_status vectis_curl_multi_consume(vectis_curl_multi *receiver,
                                        size_t count, vectis_error *error) {
  curl_multi_state *state =
      receiver == NULL ? NULL : (curl_multi_state *)receiver->impl;
  vectis_status status =
      curl_check(state == NULL ? NULL : &state->owner, error);
  curl_transfer *transfer;
  if (status != VECTIS_OK)
    return status;
  if (count > state->queued)
    return curl_failure(error, VECTIS_ERR_INVALID,
                        "curl consume exceeds queued completions");
  while (count-- != 0u) {
    transfer = state->head;
    state->head = transfer->next;
    curl_transfer_clear(transfer, 1);
    free(transfer);
    --state->queued;
    --state->count;
  }
  if (state->head == NULL)
    state->tail = NULL;
  return VECTIS_OK;
}
vectis_status vectis_curl_multi_close(vectis_curl_multi *receiver,
                                      vectis_error *error) {
  curl_multi_state *state =
      receiver == NULL ? NULL : (curl_multi_state *)receiver->impl;
  vectis_status status;
  curl_transfer *transfer, *next;
  if (state == NULL)
    return VECTIS_OK;
  status = curl_check(&state->owner, error);
  if (status != VECTIS_OK)
    return status;
  transfer = state->active;
  while (transfer != NULL) {
    next = transfer->next;
    curl_transfer_quiesce(transfer);
    (void)curl_multi_remove_handle(state->native, transfer->native);
    curl_transfer_clear(transfer, 1);
    free(transfer);
    transfer = next;
  }
  transfer = state->head;
  while (transfer != NULL) {
    next = transfer->next;
    curl_transfer_clear(transfer, 1);
    free(transfer);
    transfer = next;
  }
  (void)curl_multi_cleanup(state->native);
  free(state);
  return VECTIS_OK;
}
