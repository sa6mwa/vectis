#include "vectis_supervisor_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pslog.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SIGNAL_LIMIT 64u
#define FRAME_LIMIT (1024u * 1024u)
#define CAPACITY_LIMIT 4096u
#define INBOX_BYTE_LIMIT (16u * 1024u * 1024u)

typedef struct shared_inbox {
  pthread_mutex_t mutex;
  size_t head, count;
  int closed, poisoned;
  unsigned char pending[SIGNAL_LIMIT];
} shared_inbox;
typedef struct inbox_frame {
  uint32_t kind, correlation_id, flags;
  size_t size;
} inbox_frame;
typedef struct supervisor_timer {
  uint32_t kind;
  long every_ms;
  int enabled;
  struct timespec due;
  struct supervisor_timer *next;
} supervisor_timer;
struct vectis_supervisor_channel {
  shared_inbox *shared;
  size_t mapping_size, capacity, frame_size, stride;
  uint32_t id, signals[SIGNAL_LIMIT];
  size_t signal_count, signal_cursor;
  int prefer_signal;
  int read_fd, write_fd;
  pid_t owner;
  pthread_t thread;
};
typedef struct supervisor_impl {
  vectis_supervisor pub;
  vectis_supervisor_host *host;
  char *name;
  vectis_supervisor_channel channel;
  vectis_supervisor_state state;
  int requested, stop_on_failure, closed, scheduled;
  vectis_supervisor_handler_fn handler;
  void *context;
  supervisor_timer *timers;
  vectis_error last_error;
  pslog_logger *logger;
  struct supervisor_impl *next;
} supervisor_impl;
struct vectis_supervisor_host {
  supervisor_impl *scopes;
  uint32_t next_id;
  int frozen, running, pumping, callback_depth;
  pid_t owner;
  pthread_t thread;
};
static vectis_status fail(vectis_error *error, vectis_status code,
                          const char *message) {
  vectis_error_clear(error);
  if (error != NULL) {
    error->code = code;
    error->source = VECTIS_ERROR_SOURCE_VECTIS;
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
  }
  return code;
}
static void now(struct timespec *value) {
  (void)clock_gettime(CLOCK_MONOTONIC, value);
}
static void after(struct timespec *value, long ms) {
  now(value);
  value->tv_sec += ms / 1000L;
  value->tv_nsec += (ms % 1000L) * 1000000L;
  if (value->tv_nsec >= 1000000000L) {
    ++value->tv_sec;
    value->tv_nsec -= 1000000000L;
  }
}
static long remaining(const struct timespec *due) {
  struct timespec current;
  long seconds, nanoseconds;
  now(&current);
  seconds = (long)(due->tv_sec - current.tv_sec);
  nanoseconds = due->tv_nsec - current.tv_nsec;
  if (nanoseconds < 0L) {
    --seconds;
    nanoseconds += 1000000000L;
  }
  if (seconds < 0L)
    return -1L;
  if (seconds >= LONG_MAX / 1000L)
    return LONG_MAX;
  return seconds * 1000L + (nanoseconds + 999999L) / 1000000L;
}
static int owner(const vectis_supervisor_host *host) {
  return host != NULL && host->owner == getpid() &&
         pthread_equal(host->thread, pthread_self());
}
static int inbox_lock_mode(shared_inbox *shared, int nonblocking) {
  int rc = nonblocking ? pthread_mutex_trylock(&shared->mutex)
                       : pthread_mutex_lock(&shared->mutex);
#ifdef __linux__
  if (rc == EOWNERDEAD) {
    __atomic_store_n(&shared->closed, 1, __ATOMIC_RELEASE);
    shared->poisoned = 1;
    (void)pthread_mutex_consistent(&shared->mutex);
    return 0;
  }
#endif
  return rc;
}
static void channel_dispose(vectis_supervisor_channel *channel) {
  if (channel->read_fd >= 0)
    (void)close(channel->read_fd);
  if (channel->write_fd >= 0)
    (void)close(channel->write_fd);
  channel->read_fd = channel->write_fd = -1;
  if (channel->shared != NULL) {
    (void)munmap(channel->shared, channel->mapping_size);
    channel->shared = NULL;
  }
}
static vectis_status channel_open(vectis_supervisor_channel *channel,
                                  vectis_error *error) {
  int fds[2], rc;
  pthread_mutexattr_t attr;
  channel->mapping_size =
      sizeof(shared_inbox) + channel->capacity * channel->stride;
  channel->shared = mmap(NULL, channel->mapping_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (channel->shared == MAP_FAILED) {
    channel->shared = NULL;
    return fail(error, VECTIS_ERR_NOMEM, "supervisor inbox allocation failed");
  }
  memset(channel->shared, 0, channel->mapping_size);
  rc = pthread_mutexattr_init(&attr);
  if (rc == 0) {
    rc = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
#ifdef __linux__
    if (rc == 0)
      rc = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
#endif
    if (rc == 0)
      rc = pthread_mutex_init(&channel->shared->mutex, &attr);
    (void)pthread_mutexattr_destroy(&attr);
  }
  if (rc != 0 || socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) != 0) {
    channel_dispose(channel);
    return fail(error, VECTIS_ERR_STATE,
                "supervisor inbox initialization failed");
  }
  channel->read_fd = fds[0];
  channel->write_fd = fds[1];
  if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 ||
      fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0 ||
      fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0 ||
      fcntl(fds[1], F_SETFL, O_NONBLOCK) < 0) {
    channel_dispose(channel);
    return fail(error, VECTIS_ERR_STATE,
                "supervisor inbox descriptor setup failed");
  }
  channel->owner = getpid();
  channel->thread = pthread_self();
  return VECTIS_OK;
}
static int pending(const vectis_supervisor_channel *channel) {
  size_t i;
  if (channel->shared->count != 0u)
    return 1;
  for (i = 0; i < channel->signal_count; ++i)
    if (__atomic_load_n(&channel->shared->pending[i], __ATOMIC_ACQUIRE))
      return 1;
  return 0;
}
static int take_signal(vectis_supervisor_channel *channel, uint32_t *kind) {
  size_t i, index;
  for (i = 0; i < channel->signal_count; ++i) {
    index = (channel->signal_cursor + i) % channel->signal_count;
    if (__atomic_exchange_n(&channel->shared->pending[index], 0,
                            __ATOMIC_ACQ_REL)) {
      *kind = channel->signals[index];
      channel->signal_cursor = (index + 1u) % channel->signal_count;
      channel->prefer_signal = 0;
      return 1;
    }
  }
  return 0;
}
/* One datagram is a level-trigger token for all retained frames and signals.
 * Producers and owner drain synchronize through the same process-shared lock.
 */
static vectis_status wake(vectis_supervisor_channel *channel,
                          vectis_error *error) {
  char byte = 1;
  ssize_t rc;
  do {
#ifdef MSG_NOSIGNAL
    rc = send(channel->write_fd, &byte, 1u, MSG_NOSIGNAL);
#else
    rc = send(channel->write_fd, &byte, 1u, 0);
#endif
  } while (rc < 0 && errno == EINTR);
  if (rc == 1 || (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
    return VECTIS_OK;
  __atomic_store_n(&channel->shared->closed, 1, __ATOMIC_RELEASE);
  return fail(error, VECTIS_ERR_STATE, "closed");
}
vectis_status vectis_supervisor_channel_send_frame(
    vectis_supervisor_channel *channel, uint32_t kind, uint32_t correlation,
    uint32_t flags, const void *payload, size_t size, vectis_error *error) {
  shared_inbox *shared;
  inbox_frame *frame;
  int lock_rc;
  vectis_status status = VECTIS_OK;
  if (channel == NULL || (payload == NULL && size != 0u) ||
      size > channel->frame_size)
    return fail(error, VECTIS_ERR_INVALID,
                "supervisor frame exceeds declared bound");
  if (channel->shared == NULL || channel->write_fd < 0)
    return fail(error, VECTIS_ERR_STATE, "closed");
  shared = channel->shared;
  if (__atomic_load_n(&shared->closed, __ATOMIC_ACQUIRE))
    return fail(error, VECTIS_ERR_STATE, "closed");
  lock_rc = inbox_lock_mode(shared, 1);
  if (lock_rc == EBUSY)
    return fail(error, VECTIS_ERR_CONFLICT, "full");
  if (lock_rc != 0)
    return fail(error, VECTIS_ERR_STATE, "supervisor inbox lock failed");
  if (__atomic_load_n(&shared->closed, __ATOMIC_ACQUIRE))
    status = fail(error, VECTIS_ERR_STATE, "closed");
  else if (shared->count == channel->capacity)
    status = fail(error, VECTIS_ERR_CONFLICT, "full");
  else {
    frame =
        (inbox_frame *)((unsigned char *)(shared + 1) +
                        ((shared->head + shared->count) % channel->capacity) *
                            channel->stride);
    frame->kind = kind;
    frame->correlation_id = correlation;
    frame->flags = flags;
    frame->size = size;
    if (size != 0u)
      memcpy(frame + 1, payload, size);
    ++shared->count;
    status = wake(channel, error);
  }
  (void)pthread_mutex_unlock(&shared->mutex);
  return status;
}
vectis_status vectis_supervisor_channel_send(vectis_supervisor_channel *channel,
                                             uint32_t kind, const void *payload,
                                             size_t size, vectis_error *error) {
  return vectis_supervisor_channel_send_frame(channel, kind, 0u, 0u, payload,
                                              size, error);
}
vectis_status
vectis_supervisor_channel_signal(vectis_supervisor_channel *channel,
                                 uint32_t kind, vectis_error *error) {
  size_t i;
  if (channel == NULL)
    return fail(error, VECTIS_ERR_INVALID, "channel is required");
  for (i = 0; i < channel->signal_count; ++i)
    if (channel->signals[i] == kind)
      break;
  if (i == channel->signal_count)
    return fail(error, VECTIS_ERR_INVALID, "undeclared signal kind");
  if (channel->shared == NULL || channel->write_fd < 0 ||
      __atomic_load_n(&channel->shared->closed, __ATOMIC_ACQUIRE))
    return fail(error, VECTIS_ERR_STATE, "closed");
  /* Every accepted producer must attempt its own wake. Another producer can
   * be suspended between publishing the coalesced bit and sending a token. */
  (void)__atomic_exchange_n(&channel->shared->pending[i], 1, __ATOMIC_ACQ_REL);
  return wake(channel, error);
}
vectis_status
vectis_supervisor_channel_receive(vectis_supervisor_channel *channel,
                                  vectis_supervisor_event *event, void *buffer,
                                  size_t capacity, vectis_error *error) {
  inbox_frame *frame;
  char byte;
  size_t drained;
  int lock_rc;
  vectis_status status = VECTIS_OK;
  if (channel == NULL || event == NULL || (buffer == NULL && capacity != 0u))
    return fail(error, VECTIS_ERR_INVALID, "receive arguments are required");
  if (channel->owner != getpid() ||
      !pthread_equal(channel->thread, pthread_self()) || channel->read_fd < 0 ||
      channel->shared == NULL)
    return fail(error, VECTIS_ERR_STATE,
                "channel receive belongs to supervisor owner");
  if (__atomic_load_n(&channel->shared->closed, __ATOMIC_ACQUIRE))
    return fail(error, VECTIS_ERR_STATE, "closed");
  lock_rc = inbox_lock_mode(channel->shared, 1);
  if (lock_rc == EBUSY)
    return VECTIS_ERR_TIMEOUT; /* leave the level token armed */
  if (lock_rc != 0)
    return fail(error, VECTIS_ERR_STATE, "supervisor inbox lock failed");
  memset(event, 0, sizeof(*event));
  event->channel_id = channel->id;
  for (drained = 0u; drained < channel->capacity + SIGNAL_LIMIT + 1u; ++drained)
    if (recv(channel->read_fd, &byte, 1u, 0) <= 0)
      break;
  if (__atomic_load_n(&channel->shared->closed, __ATOMIC_ACQUIRE))
    status = fail(error, VECTIS_ERR_STATE, "closed");
  else if (channel->prefer_signal && take_signal(channel, &event->kind))
    event->type = VECTIS_SUPERVISOR_SIGNAL;
  else if (channel->shared->count != 0u) {
    frame = (inbox_frame *)((unsigned char *)(channel->shared + 1) +
                            channel->shared->head * channel->stride);
    if (capacity < frame->size)
      status = fail(error, VECTIS_ERR_INVALID, "receive buffer too small");
    else {
      event->type = VECTIS_SUPERVISOR_MESSAGE;
      event->kind = frame->kind;
      event->correlation_id = frame->correlation_id;
      event->flags = frame->flags;
      event->payload_size = frame->size;
      event->payload = buffer;
      if (frame->size != 0u)
        memcpy(buffer, frame + 1, frame->size);
      channel->shared->head = (channel->shared->head + 1u) % channel->capacity;
      --channel->shared->count;
      channel->prefer_signal = 1;
    }
  } else {
    if (!take_signal(channel, &event->kind))
      status = VECTIS_ERR_TIMEOUT;
    else
      event->type = VECTIS_SUPERVISOR_SIGNAL;
  }
  if (pending(channel))
    (void)wake(channel, NULL);
  (void)pthread_mutex_unlock(&channel->shared->mutex);
  return status;
}
int vectis_supervisor_channel_fd(const vectis_supervisor_channel *channel) {
  return channel != NULL && channel->owner == getpid() ? channel->read_fd : -1;
}
void vectis_supervisor_config_init(vectis_supervisor_config *config) {
  if (config == NULL)
    return;
  memset(config, 0, sizeof(*config));
  config->capacity = 64u;
  config->max_frame_size = 4096u;
  config->start_with_app = 1;
}
vectis_supervisor_host *vectis_supervisor_host_create(void) {
  vectis_supervisor_host *host = calloc(1u, sizeof(*host));
  if (host != NULL) {
    host->owner = getpid();
    host->thread = pthread_self();
    host->next_id = 1u;
  }
  return host;
}
int vectis_supervisor_host_declaring(const vectis_supervisor_host *host) {
  return owner(host) && !host->frozen;
}
vectis_status vectis_supervisor_new(vectis_app *app,
                                    const vectis_supervisor_config *config,
                                    vectis_supervisor **out,
                                    vectis_error *error) {
  vectis_supervisor_host *host;
  supervisor_impl *scope, **tail;
  size_t i, j, alignment;
  vectis_status status;
  if (out != NULL)
    *out = NULL;
  if (config == NULL || out == NULL || config->name == NULL ||
      config->name[0] == '\0' || config->capacity == 0u ||
      config->capacity > CAPACITY_LIMIT || config->max_frame_size == 0u ||
      config->max_frame_size > FRAME_LIMIT ||
      config->signal_kind_count > SIGNAL_LIMIT ||
      (config->signal_kind_count != 0u && config->signal_kinds == NULL))
    return fail(error, VECTIS_ERR_INVALID,
                "invalid supervisor declaration bounds or name");
  status = vectis_app_supervisor_host(app, &host, error);
  if (status != VECTIS_OK)
    return status;
  if (!vectis_supervisor_host_declaring(host))
    return fail(error, VECTIS_ERR_STATE,
                "supervisors must be declared before app start");
  tail = &host->scopes;
  while (*tail != NULL) {
    if (strcmp((*tail)->name, config->name) == 0)
      return fail(error, VECTIS_ERR_CONFLICT, "duplicate supervisor name");
    tail = &(*tail)->next;
  }
  for (i = 0; i < config->signal_kind_count; ++i)
    for (j = 0; j < i; ++j)
      if (config->signal_kinds[i] == config->signal_kinds[j])
        return fail(error, VECTIS_ERR_INVALID, "duplicate signal kind");
  scope = calloc(1u, sizeof(*scope));
  if (scope == NULL)
    return fail(error, VECTIS_ERR_NOMEM, "supervisor allocation failed");
  scope->name = strdup(config->name);
  if (scope->name == NULL) {
    free(scope);
    return fail(error, VECTIS_ERR_NOMEM, "supervisor name allocation failed");
  }
  scope->host = host;
  scope->state = VECTIS_SUPERVISOR_DECLARED;
  scope->requested = config->start_with_app;
  scope->stop_on_failure = config->stop_on_failure;
  scope->handler = config->handler;
  scope->context = config->context;
  scope->logger = vectis_logger(app);
  scope->channel.capacity = config->capacity;
  scope->channel.frame_size = config->max_frame_size;
  alignment = sizeof(inbox_frame);
  scope->channel.stride =
      ((sizeof(inbox_frame) + config->max_frame_size + alignment - 1u) /
       alignment) *
      alignment;
  if (config->capacity >
      (INBOX_BYTE_LIMIT - sizeof(shared_inbox)) / scope->channel.stride) {
    free(scope->name);
    free(scope);
    return fail(error, VECTIS_ERR_INVALID,
                "supervisor inbox exceeds 16 MiB total bound");
  }
  scope->channel.id = host->next_id++;
  scope->channel.signal_count = config->signal_kind_count;
  for (i = 0; i < config->signal_kind_count; ++i)
    scope->channel.signals[i] = config->signal_kinds[i];
  scope->channel.read_fd = scope->channel.write_fd = -1;
  status = channel_open(&scope->channel, error);
  if (status != VECTIS_OK) {
    free(scope->name);
    free(scope);
    return status;
  }
  scope->pub.impl = scope;
  scope->pub.start = vectis_supervisor_start;
  scope->pub.stop = vectis_supervisor_stop;
  scope->pub.wait = vectis_supervisor_wait;
  scope->pub.state = vectis_supervisor_get_state;
  scope->pub.channel = vectis_supervisor_channel_get;
  scope->pub.timer = vectis_supervisor_timer;
  scope->pub.close = vectis_supervisor_close;
  *tail = scope;
  *out = &scope->pub;
  return VECTIS_OK;
}
static vectis_status invoke(supervisor_impl *scope,
                            vectis_supervisor_event *event,
                            vectis_error *error) {
  vectis_status status = VECTIS_OK;
  vectis_error_clear(error);
  if (scope->handler != NULL) {
    ++scope->host->callback_depth;
    status = scope->handler(scope->context, event, error);
    --scope->host->callback_depth;
  }
  if (status != VECTIS_OK) {
    vectis_error saved;
    vectis_error_clear(&saved);
    if (error != NULL && error->code != VECTIS_OK)
      saved = *error;
    else
      (void)fail(&saved, status, "supervisor callback failed");
    if (event->type != VECTIS_SUPERVISOR_STOP)
      (void)vectis_supervisor_stop(&scope->pub, 0L, NULL);
    scope->last_error = saved;
    scope->state = VECTIS_SUPERVISOR_FAILED;
    if (scope->logger != NULL) {
      pslog_field fields[3];
      fields[0] = pslog_str("sub", "service");
      fields[1] = pslog_str("scope", scope->name);
      fields[2] = pslog_i64("status", status);
      scope->logger->error(scope->logger, saved.message, fields, 3u);
    }
    if (error != NULL)
      *error = saved;
    if (scope->stop_on_failure && event->type != VECTIS_SUPERVISOR_STOP)
      return VECTIS_OK;
  }
  return status;
}
vectis_status vectis_supervisor_start(vectis_supervisor *self,
                                      vectis_error *error) {
  supervisor_impl *scope;
  supervisor_timer *timer;
  vectis_supervisor_event event;
  vectis_status status;
  if (self == NULL || self->impl == NULL)
    return fail(error, VECTIS_ERR_INVALID, "supervisor is required");
  scope = self->impl;
  if (!owner(scope->host) || scope->closed)
    return fail(error, VECTIS_ERR_STATE, "supervisor start belongs to owner");
  if (scope->host->callback_depth)
    return fail(error, VECTIS_ERR_STATE,
                "scope start is not allowed inside a supervisor callback");
  if (scope->state == VECTIS_SUPERVISOR_MATERIALIZING ||
      scope->state == VECTIS_SUPERVISOR_STOPPING)
    return fail(error, VECTIS_ERR_STATE,
                "supervisor lifecycle transition is active");
  scope->requested = 1;
  if (!scope->host->running || scope->state == VECTIS_SUPERVISOR_RUNNING)
    return VECTIS_OK;
  scope->state = VECTIS_SUPERVISOR_MATERIALIZING;
  if (scope->channel.shared == NULL) {
    status = channel_open(&scope->channel, error);
    if (status != VECTIS_OK) {
      scope->state = VECTIS_SUPERVISOR_FAILED;
      (void)fail(&scope->last_error, status,
                 "supervisor ingress allocation failed");
      if (error != NULL)
        scope->last_error = *error;
      return status;
    }
  }
  /* Restart requires producers to have quiesced; never wait on their lock. */
  if (inbox_lock_mode(scope->channel.shared, 1) != 0) {
    scope->state = VECTIS_SUPERVISOR_FAILED;
    (void)fail(&scope->last_error, VECTIS_ERR_CONFLICT,
               "supervisor inbox is busy at start");
    if (error != NULL)
      *error = scope->last_error;
    return VECTIS_ERR_CONFLICT;
  }
  if (scope->channel.shared->poisoned) {
    (void)pthread_mutex_unlock(&scope->channel.shared->mutex);
    scope->state = VECTIS_SUPERVISOR_FAILED;
    (void)fail(
        &scope->last_error, VECTIS_ERR_STATE,
        "supervisor inbox owner died; restart app to prepare fresh ingress");
    if (error != NULL)
      *error = scope->last_error;
    return VECTIS_ERR_STATE;
  }
  __atomic_store_n(&scope->channel.shared->closed, 0, __ATOMIC_RELEASE);
  (void)pthread_mutex_unlock(&scope->channel.shared->mutex);
  for (timer = scope->timers; timer != NULL; timer = timer->next)
    after(&timer->due, timer->every_ms);
  memset(&event, 0, sizeof(event));
  event.type = VECTIS_SUPERVISOR_START;
  status = invoke(scope, &event, error);
  if (status != VECTIS_OK)
    return status;
  if (scope->state != VECTIS_SUPERVISOR_FAILED) {
    scope->state = VECTIS_SUPERVISOR_RUNNING;
    vectis_error_clear(&scope->last_error);
  }
  return VECTIS_OK;
}
vectis_status vectis_supervisor_stop(vectis_supervisor *self, long deadline_ms,
                                     vectis_error *error) {
  supervisor_impl *scope;
  vectis_supervisor_event event;
  vectis_status status;
  if (self == NULL || self->impl == NULL)
    return fail(error, VECTIS_ERR_INVALID, "supervisor is required");
  scope = self->impl;
  if (!owner(scope->host))
    return fail(error, VECTIS_ERR_STATE, "supervisor stop belongs to owner");
  if (scope->host->callback_depth)
    return fail(error, VECTIS_ERR_STATE,
                "scope stop is not allowed inside a supervisor callback");
  scope->requested = 0;
  if (scope->state == VECTIS_SUPERVISOR_STOPPED ||
      scope->state == VECTIS_SUPERVISOR_STOPPING)
    return VECTIS_OK;
  if (scope->channel.shared != NULL)
    __atomic_store_n(&scope->channel.shared->closed, 1, __ATOMIC_RELEASE);
  memset(&event, 0, sizeof(event));
  event.type = VECTIS_SUPERVISOR_STOP;
  event.deadline_ms = deadline_ms;
  if (scope->state == VECTIS_SUPERVISOR_DECLARED ||
      scope->state == VECTIS_SUPERVISOR_FAILED) {
    scope->state = VECTIS_SUPERVISOR_STOPPED;
    return VECTIS_OK;
  }
  scope->state = VECTIS_SUPERVISOR_STOPPING;
  status = invoke(scope, &event, error);
  scope->state = status == VECTIS_OK ? VECTIS_SUPERVISOR_STOPPED
                                     : VECTIS_SUPERVISOR_FAILED;
  return status;
}
vectis_status vectis_supervisor_wait(vectis_supervisor *self, long deadline_ms,
                                     vectis_error *error) {
  (void)deadline_ms;
  if (self == NULL || self->impl == NULL)
    return fail(error, VECTIS_ERR_INVALID, "supervisor is required");
  if (vectis_supervisor_get_state(self) != VECTIS_SUPERVISOR_STOPPED)
    return fail(error, VECTIS_ERR_STATE, "stop serial supervisor before wait");
  return VECTIS_OK;
}
vectis_supervisor_state
vectis_supervisor_get_state(const vectis_supervisor *self) {
  return self != NULL && self->impl != NULL
             ? ((supervisor_impl *)self->impl)->state
             : VECTIS_SUPERVISOR_STOPPED;
}
vectis_status vectis_supervisor_channel_get(vectis_supervisor *self,
                                            vectis_supervisor_channel **out,
                                            vectis_error *error) {
  if (self == NULL || self->impl == NULL || out == NULL)
    return fail(error, VECTIS_ERR_INVALID,
                "supervisor and output are required");
  *out = &((supervisor_impl *)self->impl)->channel;
  return VECTIS_OK;
}
vectis_status vectis_supervisor_timer(vectis_supervisor *self, uint32_t kind,
                                      long every_ms, vectis_error *error) {
  supervisor_impl *scope;
  supervisor_timer *timer;
  if (self == NULL || self->impl == NULL || every_ms <= 0L)
    return fail(error, VECTIS_ERR_INVALID,
                "positive timer interval is required");
  scope = self->impl;
  if (!vectis_supervisor_host_declaring(scope->host))
    return fail(error, VECTIS_ERR_STATE,
                "timers must be declared before app start");
  for (timer = scope->timers; timer != NULL; timer = timer->next)
    if (timer->kind == kind)
      return fail(error, VECTIS_ERR_CONFLICT, "duplicate timer kind");
  timer = calloc(1u, sizeof(*timer));
  if (timer == NULL)
    return fail(error, VECTIS_ERR_NOMEM, "timer allocation failed");
  timer->kind = kind;
  timer->every_ms = every_ms;
  timer->enabled = 1;
  timer->next = scope->timers;
  scope->timers = timer;
  return VECTIS_OK;
}
int vectis_supervisor_host_requested(const vectis_supervisor_host *host) {
  supervisor_impl *scope;
  if (host == NULL)
    return 0;
  for (scope = host->scopes; scope != NULL; scope = scope->next)
    if (scope->requested && !scope->closed)
      return 1;
  return 0;
}
vectis_status vectis_supervisor_host_prepare(vectis_supervisor_host *host,
                                             vectis_error *error) {
  supervisor_impl *scope;
  vectis_status status;
  if (host == NULL)
    return VECTIS_OK;
  if (!owner(host))
    return fail(error, VECTIS_ERR_STATE,
                "supervisor preparation belongs to owner");
  host->frozen = 1;
  for (scope = host->scopes; scope != NULL; scope = scope->next) {
    if (scope->closed)
      continue;
    if (scope->channel.shared != NULL &&
        scope->state != VECTIS_SUPERVISOR_DECLARED)
      channel_dispose(&scope->channel);
    if (scope->channel.shared == NULL) {
      status = channel_open(&scope->channel, error);
      if (status != VECTIS_OK)
        return status;
    }
  }
  return VECTIS_OK;
}
void vectis_supervisor_host_producer(vectis_supervisor_host *host) {
  supervisor_impl *scope;
  if (host == NULL)
    return;
  for (scope = host->scopes; scope != NULL; scope = scope->next) {
    if (scope->channel.read_fd >= 0)
      (void)close(scope->channel.read_fd);
    scope->channel.read_fd = -1;
  }
}
vectis_status vectis_supervisor_host_start(vectis_supervisor_host *host,
                                           vectis_error *error) {
  supervisor_impl *scope;
  vectis_status status;
  if (host == NULL)
    return VECTIS_OK;
  if (!owner(host))
    return fail(error, VECTIS_ERR_STATE, "supervisor startup belongs to owner");
  host->running = 1;
  for (scope = host->scopes; scope != NULL; scope = scope->next) {
    if (!scope->requested || scope->closed)
      continue;
    status = vectis_supervisor_start(&scope->pub, error);
    if (status != VECTIS_OK) {
      (void)vectis_supervisor_host_stop(host, 0L, NULL);
      return status;
    }
  }
  return VECTIS_OK;
}
vectis_status vectis_supervisor_host_stop(vectis_supervisor_host *host, long ms,
                                          vectis_error *error) {
  supervisor_impl *scope;
  struct timespec deadline;
  vectis_status status, first = VECTIS_OK;
  int requested;
  if (host == NULL)
    return VECTIS_OK;
  if (!owner(host))
    return fail(error, VECTIS_ERR_STATE,
                "supervisor shutdown belongs to owner");
  after(&deadline, ms > 0L ? ms : 0L);
  host->running = 0;
  for (scope = host->scopes; scope != NULL; scope = scope->next) {
    requested = scope->requested;
    ms = remaining(&deadline);
    status = vectis_supervisor_stop(&scope->pub, ms > 0L ? ms : 0L,
                                    first == VECTIS_OK ? error : NULL);
    scope->requested = requested;
    if (first == VECTIS_OK && status != VECTIS_OK)
      first = status;
    /* Keep closed ingress mapped until producers have joined. prepare/destroy
     * dispose it only after the application's quiescence boundary. */
  }
  return first;
}
vectis_status vectis_supervisor_host_poll(vectis_supervisor_host *host, int fd,
                                          long timeout_ms, int *ready,
                                          vectis_error *error) {
  supervisor_impl *scope;
  supervisor_timer *timer;
  struct pollfd *fds;
  supervisor_impl **polled;
  size_t count = 1u, index;
  long wait_ms = timeout_ms, due_ms;
  int rc;
  vectis_status status = VECTIS_OK;
  if (ready != NULL)
    *ready = 0;
  if (timeout_ms < -1L)
    return fail(error, VECTIS_ERR_INVALID,
                "supervisor timeout must be -1 or nonnegative");
  if (host != NULL && (!owner(host) || host->pumping || !host->running))
    return fail(error, VECTIS_ERR_STATE,
                "supervisor pump requires running owner and cannot recurse");
  if (host != NULL) {
    host->pumping = 1;
    for (scope = host->scopes; scope != NULL; scope = scope->next) {
      if (scope->state != VECTIS_SUPERVISOR_RUNNING)
        continue;
      ++count;
      if (scope->scheduled)
        wait_ms = 0L;
      for (timer = scope->timers; timer != NULL; timer = timer->next) {
        if (!timer->enabled)
          continue;
        due_ms = remaining(&timer->due);
        if (due_ms < 0L)
          due_ms = 0L;
        if (wait_ms < 0L || due_ms < wait_ms)
          wait_ms = due_ms;
      }
    }
  }
  fds = calloc(count, sizeof(*fds));
  polled = calloc(count, sizeof(*polled));
  if (fds == NULL || polled == NULL) {
    free(fds);
    free(polled);
    if (host != NULL)
      host->pumping = 0;
    return fail(error, VECTIS_ERR_NOMEM, "supervisor poll allocation failed");
  }
  fds[0].fd = fd;
  fds[0].events = POLLIN;
  index = 1u;
  if (host != NULL)
    for (scope = host->scopes; scope != NULL; scope = scope->next) {
      if (scope->state != VECTIS_SUPERVISOR_RUNNING)
        continue;
      polled[index] = scope;
      fds[index].fd = scope->channel.read_fd;
      fds[index++].events = POLLIN;
    }
  rc = poll(fds, (nfds_t)count, wait_ms > INT_MAX ? INT_MAX : (int)wait_ms);
  if (rc < 0 && errno != EINTR)
    status = fail(error, VECTIS_ERR_STATE, "supervisor poll failed");
  if (rc > 0 && fds[0].revents != 0 && ready != NULL)
    *ready = 1;
  index = 1u;
  if (host != NULL && status == VECTIS_OK)
    for (index = 1u; index < count; ++index) {
      vectis_supervisor_event event;
      void *buffer;
      int probe;
      scope = polled[index];
      probe = scope->scheduled || timeout_ms == 0L;
      scope->scheduled = 0;
      if (scope->state != VECTIS_SUPERVISOR_RUNNING)
        continue;
      if (fds[index].revents & POLLIN) {
        buffer = malloc(scope->channel.frame_size);
        if (buffer == NULL) {
          status = fail(error, VECTIS_ERR_NOMEM,
                        "supervisor frame allocation failed");
          break;
        }
        status = vectis_supervisor_channel_receive(
            &scope->channel, &event, buffer, scope->channel.frame_size, error);
        if (status == VECTIS_OK)
          status = invoke(scope, &event, error);
        else if (status == VECTIS_ERR_TIMEOUT)
          status = VECTIS_OK;
        free(buffer);
        if (status != VECTIS_OK)
          break;
      }
      if (scope->state != VECTIS_SUPERVISOR_RUNNING)
        continue;
      for (timer = scope->timers; timer != NULL; timer = timer->next) {
        if (!timer->enabled || remaining(&timer->due) > 0L)
          continue;
        after(&timer->due, timer->every_ms);
        memset(&event, 0, sizeof(event));
        event.type = VECTIS_SUPERVISOR_TIMER;
        event.kind = timer->kind;
        status = invoke(scope, &event, error);
        if (status != VECTIS_OK || scope->state != VECTIS_SUPERVISOR_RUNNING)
          break;
      }
      if (status != VECTIS_OK)
        break;
      if (probe && scope->state == VECTIS_SUPERVISOR_RUNNING) {
        memset(&event, 0, sizeof(event));
        event.type = VECTIS_SUPERVISOR_PROBE;
        status = invoke(scope, &event, error);
        if (status != VECTIS_OK)
          break;
      }
    }
  free(fds);
  free(polled);
  if (host != NULL)
    host->pumping = 0;
  return status;
}
vectis_status vectis_supervisor_schedule(vectis_supervisor *self,
                                         vectis_error *error) {
  supervisor_impl *scope;
  if (self == NULL || self->impl == NULL)
    return fail(error, VECTIS_ERR_INVALID, "supervisor is required");
  scope = self->impl;
  if (!owner(scope->host) || scope->state != VECTIS_SUPERVISOR_RUNNING)
    return fail(error, VECTIS_ERR_STATE,
                "source scheduling requires the running owner");
  scope->scheduled = 1;
  return VECTIS_OK;
}
vectis_status vectis_supervisor_host_pump(vectis_supervisor_host *host, long ms,
                                          vectis_error *error) {
  return vectis_supervisor_host_poll(host, -1, ms, NULL, error);
}
int vectis_supervisor_host_dispatching(const vectis_supervisor_host *host) {
  return host != NULL && (host->pumping || host->callback_depth);
}
void vectis_supervisor_host_after_fork_producer(vectis_supervisor_host *host) {
  vectis_supervisor_host_producer(host);
}
void vectis_supervisor_close(vectis_supervisor *self) {
  supervisor_impl *scope;
  if (self == NULL || self->impl == NULL)
    return;
  scope = self->impl;
  if (!owner(scope->host) || scope->host->callback_depth)
    return;
  (void)vectis_supervisor_stop(self, 0L, NULL);
  scope->closed = 1;
}
void vectis_supervisor_host_destroy(vectis_supervisor_host *host) {
  supervisor_impl *scope, *next;
  supervisor_timer *timer, *timer_next;
  if (host == NULL)
    return;
  if (owner(host))
    (void)vectis_supervisor_host_stop(host, 0L, NULL);
  for (scope = host->scopes; scope != NULL; scope = next) {
    next = scope->next;
    channel_dispose(&scope->channel);
    for (timer = scope->timers; timer != NULL; timer = timer_next) {
      timer_next = timer->next;
      free(timer);
    }
    free(scope->name);
    free(scope);
  }
  free(host);
}

vectis_status vectis_supervisor_last_error(const vectis_supervisor *self,
                                           vectis_error *out) {
  supervisor_impl *scope;
  if (self == NULL || self->impl == NULL || out == NULL)
    return VECTIS_ERR_INVALID;
  scope = self->impl;
  if (!owner(scope->host) || scope->closed)
    return VECTIS_ERR_STATE;
  *out = scope->last_error;
  return VECTIS_OK;
}
vectis_status vectis_supervisor_timer_enable(vectis_supervisor *self,
                                             uint32_t kind, int enabled,
                                             vectis_error *error) {
  supervisor_impl *scope;
  supervisor_timer *timer;
  if (self == NULL || self->impl == NULL)
    return fail(error, VECTIS_ERR_INVALID, "supervisor is required");
  scope = self->impl;
  if (!owner(scope->host) || scope->closed)
    return fail(error, VECTIS_ERR_STATE, "timer belongs to owner");
  for (timer = scope->timers; timer != NULL; timer = timer->next) {
    if (timer->kind != kind)
      continue;
    if (enabled && !timer->enabled)
      after(&timer->due, timer->every_ms);
    timer->enabled = enabled != 0;
    return VECTIS_OK;
  }
  return fail(error, VECTIS_ERR_INVALID, "unknown timer kind");
}
