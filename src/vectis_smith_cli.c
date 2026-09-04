#include "vectis_cli.h"
#include "vectis_internal.h"

#include <cai/agent_runtime.h>
#include <cai/auth.h>
#include <cai/tools/terminal.h>
#include <errno.h>
#include <fcntl.h>
#include <lc/lc.h>
#include <libmdf.h>
#include <limits.h>
#include <pthread.h>
#include <softline/softline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vectis/vectis.h>

#define VECTIS_SMITH_CLI_QUEUE_BYTES 65536u
#define VECTIS_SMITH_CLI_DRAIN_BYTES 8192u
#define VECTIS_SMITH_CLI_UI_DRAIN_BYTES 32768u
#define VECTIS_SMITH_CLI_CONTROL_CAPACITY 64u

typedef struct vectis_smith_cli_ring {
  char bytes[VECTIS_SMITH_CLI_QUEUE_BYTES];
  size_t head;
  size_t count;
} vectis_smith_cli_ring;

typedef struct vectis_smith_cli_render {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  vectis_smith_cli_ring markdown;
  vectis_smith_cli_ring rendered;
  pthread_t thread;
  int thread_started;
  int input_closed;
  int renderer_done;
  int renderer_failed;
  int interactive;
  int notify_fd;
  sl_t *editor;
} vectis_smith_cli_render;

typedef struct vectis_smith_cli_chunk {
  const char *data;
  size_t length;
} vectis_smith_cli_chunk;

typedef enum vectis_smith_cli_control_kind {
  VECTIS_SMITH_CLI_CONTROL_NORMAL = 0,
  VECTIS_SMITH_CLI_CONTROL_STEERING = 1
} vectis_smith_cli_control_kind;

typedef struct vectis_smith_cli_control {
  vectis_smith_cli_control_kind kind;
  char *text;
} vectis_smith_cli_control;

typedef struct vectis_smith_cli_agent {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_t thread;
  int thread_started;
  int ready;
  int stopped;
  int failed;
  int stop_requested;
  int notify_fd;
  cai_agent_run_state state;
  char error[256];
  vectis_smith_config config;
  vectis_smith_cli_render *render;
  vectis_smith_cli_control controls[VECTIS_SMITH_CLI_CONTROL_CAPACITY];
  size_t control_head;
  size_t control_count;
} vectis_smith_cli_agent;

static int vectis_smith_cli_chunk_next(sl_t *editor, void *userdata,
                                       const char **out, size_t *out_length) {
  vectis_smith_cli_chunk *chunk;

  (void)editor;
  chunk = (vectis_smith_cli_chunk *)userdata;
  *out = chunk->data;
  *out_length = chunk->length;
  chunk->length = 0u;
  return SL_OK;
}

static size_t vectis_smith_cli_ring_read(vectis_smith_cli_ring *ring, char *out,
                                         size_t capacity) {
  size_t first;
  size_t count;

  count = ring->count < capacity ? ring->count : capacity;
  first = VECTIS_SMITH_CLI_QUEUE_BYTES - ring->head;
  if (first > count) {
    first = count;
  }
  if (first != 0u) {
    memcpy(out, ring->bytes + ring->head, first);
  }
  if (count > first) {
    memcpy(out + first, ring->bytes, count - first);
  }
  ring->head = (ring->head + count) % VECTIS_SMITH_CLI_QUEUE_BYTES;
  ring->count -= count;
  return count;
}

static size_t vectis_smith_cli_ring_write(vectis_smith_cli_ring *ring,
                                          const char *data, size_t length) {
  size_t capacity;
  size_t tail;
  size_t first;

  capacity = VECTIS_SMITH_CLI_QUEUE_BYTES - ring->count;
  if (length > capacity) {
    length = capacity;
  }
  tail = (ring->head + ring->count) % VECTIS_SMITH_CLI_QUEUE_BYTES;
  first = VECTIS_SMITH_CLI_QUEUE_BYTES - tail;
  if (first > length) {
    first = length;
  }
  if (first != 0u) {
    memcpy(ring->bytes + tail, data, first);
  }
  if (length > first) {
    memcpy(ring->bytes, data + first, length - first);
  }
  ring->count += length;
  return length;
}

static int vectis_smith_cli_print_chunk(sl_t *editor, const char *data,
                                        size_t length) {
  vectis_smith_cli_chunk chunk;

  if (length == 0u) {
    return 0;
  }
  if (editor == NULL) {
    return fwrite(data, 1u, length, stdout) == length ? 0 : -1;
  }
  chunk.data = data;
  chunk.length = length;
  return sl_print_above(editor, vectis_smith_cli_chunk_next, &chunk) == SL_OK
             ? 0
             : -1;
}

static void vectis_smith_cli_drain_limited(vectis_smith_cli_render *render,
                                           size_t limit) {
  char buffer[VECTIS_SMITH_CLI_DRAIN_BYTES];
  size_t length;
  size_t capacity;
  size_t drained;

  drained = 0u;
  for (;;) {
    if (drained == limit) {
      break;
    }
    capacity = sizeof(buffer);
    if (limit - drained < capacity) {
      capacity = limit - drained;
    }
    (void)pthread_mutex_lock(&render->mutex);
    length =
        vectis_smith_cli_ring_read(&render->rendered, buffer, capacity);
    (void)pthread_cond_broadcast(&render->changed);
    (void)pthread_mutex_unlock(&render->mutex);
    if (length == 0u) {
      break;
    }
    drained += length;
    if (vectis_smith_cli_print_chunk(
            render->interactive ? render->editor : NULL, buffer, length) != 0) {
      break;
    }
  }
  if (!render->interactive) {
    (void)fflush(stdout);
  }
}

static void vectis_smith_cli_drain(vectis_smith_cli_render *render) {
  vectis_smith_cli_drain_limited(render, (size_t)-1);
}

static void vectis_smith_cli_notify(int fd) {
  unsigned char byte;

  if (fd < 0) {
    return;
  }
  byte = 1u;
  if (write(fd, &byte, 1u) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    return;
  }
}

static size_t vectis_smith_cli_mdf_read(void *userdata, char *out,
                                        size_t capacity, int *failed) {
  vectis_smith_cli_render *render;
  size_t length;

  render = (vectis_smith_cli_render *)userdata;
  (void)pthread_mutex_lock(&render->mutex);
  while (render->markdown.count == 0u && !render->input_closed) {
    (void)pthread_cond_wait(&render->changed, &render->mutex);
  }
  length = vectis_smith_cli_ring_read(&render->markdown, out, capacity);
  (void)pthread_cond_broadcast(&render->changed);
  (void)pthread_mutex_unlock(&render->mutex);
  *failed = 0;
  return length;
}

static int vectis_smith_cli_mdf_write(void *userdata, const char *data,
                                      size_t length) {
  vectis_smith_cli_render *render;
  size_t written;

  render = (vectis_smith_cli_render *)userdata;
  while (length != 0u) {
    (void)pthread_mutex_lock(&render->mutex);
    while (render->rendered.count == VECTIS_SMITH_CLI_QUEUE_BYTES) {
      (void)pthread_cond_wait(&render->changed, &render->mutex);
    }
    written = vectis_smith_cli_ring_write(&render->rendered, data, length);
    (void)pthread_cond_broadcast(&render->changed);
    (void)pthread_mutex_unlock(&render->mutex);
    vectis_smith_cli_notify(render->notify_fd);
    data += written;
    length -= written;
  }
  return 0;
}

static void *vectis_smith_cli_render_thread(void *userdata) {
  vectis_smith_cli_render *render;
  mdf_options options;
  mdf_source source;
  mdf_sink sink;
  mdf *renderer;

  render = (vectis_smith_cli_render *)userdata;
  renderer = NULL;
  mdf_options_init(&options);
  memset(&source, 0, sizeof(source));
  memset(&sink, 0, sizeof(sink));
  source.userdata = render;
  source.read = vectis_smith_cli_mdf_read;
  sink.userdata = render;
  sink.write = vectis_smith_cli_mdf_write;
  if (mdf_create(MDF_FORMAT_ANSI, &options, &renderer) != MDF_OK ||
      renderer->render(renderer, &source, &sink) != MDF_OK) {
    (void)pthread_mutex_lock(&render->mutex);
    render->renderer_failed = 1;
    (void)pthread_mutex_unlock(&render->mutex);
  }
  if (renderer != NULL) {
    renderer->destroy(renderer);
  }
  (void)pthread_mutex_lock(&render->mutex);
  render->renderer_done = 1;
  (void)pthread_cond_broadcast(&render->changed);
  (void)pthread_mutex_unlock(&render->mutex);
  return NULL;
}

static int vectis_smith_cli_render_start(vectis_smith_cli_render *render) {
  if (render->thread_started) {
    return 0;
  }
  (void)pthread_mutex_lock(&render->mutex);
  render->input_closed = 0;
  render->renderer_done = 0;
  render->renderer_failed = 0;
  (void)pthread_mutex_unlock(&render->mutex);
  if (pthread_create(&render->thread, NULL, vectis_smith_cli_render_thread,
                     render) != 0) {
    return -1;
  }
  render->thread_started = 1;
  return 0;
}

static int vectis_smith_cli_render_append(vectis_smith_cli_render *render,
                                          const char *data, size_t length) {
  size_t written;

  if (vectis_smith_cli_render_start(render) != 0) {
    return -1;
  }
  while (length != 0u) {
    (void)pthread_mutex_lock(&render->mutex);
    while (render->markdown.count == VECTIS_SMITH_CLI_QUEUE_BYTES) {
      (void)pthread_cond_wait(&render->changed, &render->mutex);
    }
    written = vectis_smith_cli_ring_write(&render->markdown, data, length);
    (void)pthread_cond_broadcast(&render->changed);
    (void)pthread_mutex_unlock(&render->mutex);
    data += written;
    length -= written;
  }
  return 0;
}

static void vectis_smith_cli_render_finish(vectis_smith_cli_render *render) {
  if (!render->thread_started) {
    return;
  }
  (void)pthread_mutex_lock(&render->mutex);
  render->input_closed = 1;
  (void)pthread_cond_broadcast(&render->changed);
  (void)pthread_mutex_unlock(&render->mutex);
  for (;;) {
    (void)pthread_mutex_lock(&render->mutex);
    if (render->renderer_done) {
      (void)pthread_mutex_unlock(&render->mutex);
      break;
    }
    (void)pthread_mutex_unlock(&render->mutex);
    vectis_smith_cli_drain(render);
    usleep(1000u);
  }
  vectis_smith_cli_drain(render);
  (void)pthread_join(render->thread, NULL);
  render->thread_started = 0;
}

static void vectis_smith_cli_render_cleanup(vectis_smith_cli_render *render) {
  vectis_smith_cli_render_finish(render);
  (void)pthread_cond_destroy(&render->changed);
  (void)pthread_mutex_destroy(&render->mutex);
}

static int vectis_smith_cli_render_failed(vectis_smith_cli_render *render) {
  int failed;

  (void)pthread_mutex_lock(&render->mutex);
  failed = render->renderer_failed;
  (void)pthread_mutex_unlock(&render->mutex);
  return failed;
}

static int vectis_smith_cli_render_event(
    void *userdata, const cai_agent_runtime_event *event, cai_error *error) {
  vectis_smith_cli_render *render;

  (void)error;
  render = (vectis_smith_cli_render *)userdata;
  return event->type == CAI_AGENT_EVENT_TEXT_DELTA && event->data_length != 0u &&
                 vectis_smith_cli_render_append(render, event->data,
                                                event->data_length) != 0
             ? CAI_ERR_NOMEM
             : CAI_OK;
}

static int vectis_smith_cli_is_active(cai_agent_run_state state) {
  return state == CAI_AGENT_SAMPLING || state == CAI_AGENT_DISPATCHING_TOOL;
}

static const char *vectis_smith_cli_state_name(cai_agent_run_state state) {
  switch (state) {
  case CAI_AGENT_IDLE:
    return "idle";
  case CAI_AGENT_SAMPLING:
    return "sampling";
  case CAI_AGENT_DISPATCHING_TOOL:
    return "tool";
  case CAI_AGENT_COMPLETED:
    return "completed";
  case CAI_AGENT_FAILED:
    return "failed";
  case CAI_AGENT_CANCELLED:
    return "cancelled";
  default:
    return "unknown";
  }
}

static void vectis_smith_cli_agent_publish(vectis_smith_cli_agent *agent,
                                           cai_agent_run_state state,
                                           const char *message) {
  (void)pthread_mutex_lock(&agent->mutex);
  agent->state = state;
  if (message != NULL) {
    (void)snprintf(agent->error, sizeof(agent->error), "%s", message);
  }
  (void)pthread_cond_broadcast(&agent->changed);
  (void)pthread_mutex_unlock(&agent->mutex);
  vectis_smith_cli_notify(agent->notify_fd);
}

static void vectis_smith_cli_agent_fail(vectis_smith_cli_agent *agent,
                                        const char *message) {
  (void)pthread_mutex_lock(&agent->mutex);
  agent->state = CAI_AGENT_FAILED;
  agent->failed = 1;
  (void)snprintf(agent->error, sizeof(agent->error), "%s", message);
  (void)pthread_cond_broadcast(&agent->changed);
  (void)pthread_mutex_unlock(&agent->mutex);
  vectis_smith_cli_notify(agent->notify_fd);
}

static int vectis_smith_cli_agent_event(
    void *userdata, const cai_agent_runtime_event *event, cai_error *error) {
  vectis_smith_cli_agent *agent;

  (void)error;
  agent = (vectis_smith_cli_agent *)userdata;
  if (event->type == CAI_AGENT_EVENT_TEXT_DELTA && event->data_length != 0u &&
      vectis_smith_cli_render_append(agent->render, event->data,
                                     event->data_length) != 0) {
    return CAI_ERR_NOMEM;
  }
  if (event->type == CAI_AGENT_EVENT_RUN_STATE_CHANGED) {
    vectis_smith_cli_agent_publish(agent, event->state, NULL);
  }
  return CAI_OK;
}

static int vectis_smith_cli_agent_take_control(
    vectis_smith_cli_agent *agent, vectis_smith_cli_control *out) {
  (void)pthread_mutex_lock(&agent->mutex);
  if (agent->control_count == 0u) {
    (void)pthread_mutex_unlock(&agent->mutex);
    return 0;
  }
  *out = agent->controls[agent->control_head];
  agent->controls[agent->control_head].text = NULL;
  agent->control_head =
      (agent->control_head + 1u) % VECTIS_SMITH_CLI_CONTROL_CAPACITY;
  --agent->control_count;
  (void)pthread_cond_broadcast(&agent->changed);
  (void)pthread_mutex_unlock(&agent->mutex);
  return 1;
}

static void *vectis_smith_cli_agent_thread(void *userdata) {
  vectis_smith_cli_agent *agent;
  vectis_smith_cli_control control;
  cai_agent_run_state published_state;
  cai_agent_run_state state;
  vectis_smith *smith;
  vectis_error error;
  vectis_status status;

  agent = (vectis_smith_cli_agent *)userdata;
  smith = NULL;
  status = vectis_smith_open(&agent->config, &smith, &error);
  (void)pthread_mutex_lock(&agent->mutex);
  agent->ready = 1;
  if (status != VECTIS_OK) {
    agent->failed = 1;
    (void)snprintf(agent->error, sizeof(agent->error), "%s", error.message);
  }
  (void)pthread_cond_broadcast(&agent->changed);
  (void)pthread_mutex_unlock(&agent->mutex);
  vectis_smith_cli_notify(agent->notify_fd);
  if (status != VECTIS_OK) {
    (void)pthread_mutex_lock(&agent->mutex);
    agent->stopped = 1;
    (void)pthread_cond_broadcast(&agent->changed);
    (void)pthread_mutex_unlock(&agent->mutex);
    vectis_smith_cli_notify(agent->notify_fd);
    return NULL;
  }
  state = CAI_AGENT_IDLE;
  published_state = state;
  vectis_smith_cli_agent_publish(agent, state, NULL);
  for (;;) {
    (void)pthread_mutex_lock(&agent->mutex);
    if (agent->stop_requested) {
      (void)pthread_mutex_unlock(&agent->mutex);
      break;
    }
    (void)pthread_mutex_unlock(&agent->mutex);
    while (vectis_smith_cli_agent_take_control(agent, &control)) {
      if (control.kind == VECTIS_SMITH_CLI_CONTROL_STEERING &&
          vectis_smith_cli_is_active(state)) {
        status = vectis_smith_submit_steering(smith, control.text, &error);
      } else {
        status = vectis_smith_submit_queued(smith, control.text, &error);
      }
      free(control.text);
      if (status != VECTIS_OK) {
        vectis_smith_cli_agent_publish(agent, state, error.message);
      }
    }
    status = vectis_smith_pump(smith, 50L, &error);
    if (status != VECTIS_OK) {
      vectis_smith_cli_agent_fail(agent, error.message);
      break;
    }
    if (vectis_smith_state(smith, &state, &error) != VECTIS_OK) {
      vectis_smith_cli_agent_fail(agent, error.message);
      break;
    }
    if (state != published_state) {
      published_state = state;
      vectis_smith_cli_agent_publish(agent, state, NULL);
    }
  }
  vectis_smith_close(smith);
  (void)pthread_mutex_lock(&agent->mutex);
  agent->stopped = 1;
  (void)pthread_cond_broadcast(&agent->changed);
  (void)pthread_mutex_unlock(&agent->mutex);
  vectis_smith_cli_notify(agent->notify_fd);
  return NULL;
}

static int vectis_smith_cli_agent_start(vectis_smith_cli_agent *agent,
                                         const vectis_smith_config *config,
                                         vectis_smith_cli_render *render,
                                         int notify_fd) {
  memset(agent, 0, sizeof(*agent));
  if (pthread_mutex_init(&agent->mutex, NULL) != 0) {
    return -1;
  }
  if (pthread_cond_init(&agent->changed, NULL) != 0) {
    (void)pthread_mutex_destroy(&agent->mutex);
    return -1;
  }
  agent->config = *config;
  agent->config.runtime.event_callback = vectis_smith_cli_agent_event;
  agent->config.runtime.event_context = agent;
  agent->render = render;
  agent->notify_fd = notify_fd;
  agent->state = CAI_AGENT_IDLE;
  if (pthread_create(&agent->thread, NULL, vectis_smith_cli_agent_thread,
                     agent) != 0) {
    (void)pthread_cond_destroy(&agent->changed);
    (void)pthread_mutex_destroy(&agent->mutex);
    return -1;
  }
  agent->thread_started = 1;
  (void)pthread_mutex_lock(&agent->mutex);
  while (!agent->ready) {
    (void)pthread_cond_wait(&agent->changed, &agent->mutex);
  }
  (void)pthread_mutex_unlock(&agent->mutex);
  return agent->failed ? -1 : 0;
}

static void vectis_smith_cli_agent_stop(vectis_smith_cli_agent *agent,
                                        vectis_smith_cli_render *render) {
  size_t i;

  if (!agent->thread_started) {
    return;
  }
  (void)pthread_mutex_lock(&agent->mutex);
  agent->stop_requested = 1;
  (void)pthread_cond_broadcast(&agent->changed);
  (void)pthread_mutex_unlock(&agent->mutex);
  for (;;) {
    (void)pthread_mutex_lock(&agent->mutex);
    if (agent->stopped) {
      (void)pthread_mutex_unlock(&agent->mutex);
      break;
    }
    (void)pthread_mutex_unlock(&agent->mutex);
    vectis_smith_cli_drain(render);
    usleep(1000u);
  }
  (void)pthread_join(agent->thread, NULL);
  agent->thread_started = 0;
  for (i = 0u; i < agent->control_count; ++i) {
    size_t index;

    index = (agent->control_head + i) % VECTIS_SMITH_CLI_CONTROL_CAPACITY;
    free(agent->controls[index].text);
  }
  (void)pthread_cond_destroy(&agent->changed);
  (void)pthread_mutex_destroy(&agent->mutex);
}

static int vectis_smith_cli_agent_submit(
    vectis_smith_cli_agent *agent, vectis_smith_cli_control_kind kind,
    const char *text) {
  size_t tail;
  char *copy;

  if (text == NULL || text[0] == '\0') {
    return -1;
  }
  copy = strdup(text);
  if (copy == NULL) {
    return -1;
  }
  (void)pthread_mutex_lock(&agent->mutex);
  if (agent->failed || agent->stop_requested ||
      agent->control_count == VECTIS_SMITH_CLI_CONTROL_CAPACITY) {
    (void)pthread_mutex_unlock(&agent->mutex);
    free(copy);
    return -1;
  }
  tail = (agent->control_head + agent->control_count) %
         VECTIS_SMITH_CLI_CONTROL_CAPACITY;
  agent->controls[tail].kind = kind;
  agent->controls[tail].text = copy;
  ++agent->control_count;
  (void)pthread_cond_broadcast(&agent->changed);
  (void)pthread_mutex_unlock(&agent->mutex);
  vectis_smith_cli_notify(agent->notify_fd);
  return 0;
}

static void vectis_smith_cli_agent_snapshot(vectis_smith_cli_agent *agent,
                                             cai_agent_run_state *state,
                                             int *failed, char *message,
                                             size_t message_capacity) {
  (void)pthread_mutex_lock(&agent->mutex);
  *state = agent->state;
  *failed = agent->failed;
  if (message_capacity != 0u) {
    (void)snprintf(message, message_capacity, "%s", agent->error);
  }
  (void)pthread_mutex_unlock(&agent->mutex);
}

static int vectis_smith_cli_get_workspace(char *out, size_t capacity,
                                          const char *requested) {
  if (requested != NULL) {
    if (strlen(requested) >= capacity) {
      return -1;
    }
    memcpy(out, requested, strlen(requested) + 1u);
    return 0;
  }
  return getcwd(out, capacity) == NULL ? -1 : 0;
}

static int vectis_smith_cli_mkdir_p(const char *path) {
  char current[PATH_MAX];
  size_t i;

  if (path == NULL || path[0] != '/') {
    return -1;
  }
  if (strlen(path) >= sizeof(current)) {
    return -1;
  }
  memcpy(current, path, strlen(path) + 1u);
  for (i = 1u; current[i] != '\0'; ++i) {
    if (current[i] != '/') {
      continue;
    }
    current[i] = '\0';
    if (mkdir(current, 0700) != 0 && errno != EEXIST) {
      return -1;
    }
    current[i] = '/';
  }
  if (mkdir(current, 0700) != 0 && errno != EEXIST) {
    return -1;
  }
  return 0;
}

static int vectis_smith_cli_default_state_endpoint(char *out,
                                                   size_t out_capacity) {
  const char *state_home;
  const char *home;
  char storage[PATH_MAX];
  int written;

  state_home = getenv("XDG_STATE_HOME");
  if (state_home == NULL || state_home[0] == '\0') {
    home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
      return -1;
    }
    written = snprintf(storage, sizeof(storage), "%s/.local/state/vectis/smith",
                       home);
  } else {
    written = snprintf(storage, sizeof(storage), "%s/vectis/smith", state_home);
  }
  if (written < 0 || (size_t)written >= sizeof(storage) ||
      vectis_smith_cli_mkdir_p(storage) != 0) {
    return -1;
  }
  written =
      snprintf(out, out_capacity, "pouch://%s?single_writer=false", storage);
  return written >= 0 && (size_t)written < out_capacity ? 0 : -1;
}

static void vectis_smith_cli_redact_endpoint(const char *endpoint, char *out,
                                             size_t out_capacity) {
  const char *end;
  const char *authority;
  const char *at;
  const char *slash;
  size_t length;

  if (out == NULL || out_capacity == 0u) {
    return;
  }
  out[0] = '\0';
  if (endpoint == NULL) {
    return;
  }
  end = strchr(endpoint, '?');
  length = end == NULL ? strlen(endpoint) : (size_t)(end - endpoint);
  authority = strstr(endpoint, "://");
  if (authority != NULL) {
    authority += 3u;
    slash = memchr(authority, '/', length - (size_t)(authority - endpoint));
    at = memchr(authority, '@', slash == NULL
                                     ? length - (size_t)(authority - endpoint)
                                     : (size_t)(slash - authority));
    if (at != NULL) {
      size_t prefix = (size_t)(authority - endpoint);
      size_t suffix = length - (size_t)(at + 1 - endpoint);
      if (prefix + suffix >= out_capacity) {
        return;
      }
      memcpy(out, endpoint, prefix);
      memcpy(out + prefix, at + 1, suffix);
      out[prefix + suffix] = '\0';
      return;
    }
  }
  if (length >= out_capacity) {
    length = out_capacity - 1u;
  }
  memcpy(out, endpoint, length);
  out[length] = '\0';
}

static int vectis_smith_cli_open_store(const char *endpoint,
                                       const char *namespace_name,
                                       pslog_logger *logger, char *failure,
                                       size_t failure_capacity,
                                       lc_client **out_client,
                                       vectis_smith_store **out_store) {
  lc_client_config client_config;
  vectis_smith_store_config store_config;
  lc_error lcerr;
  vectis_error error;
  const char *endpoints[1];
  char owner[64];
  char diagnostic_endpoint[PATH_MAX + 64u];
  char dependency_message[256];
  char *key_file;
  int rc;

  *out_client = NULL;
  *out_store = NULL;
  if (failure != NULL && failure_capacity != 0u) {
    failure[0] = '\0';
  }
  key_file = NULL;
  endpoints[0] = endpoint;
  lc_client_config_init(&client_config);
  client_config.endpoints = endpoints;
  client_config.endpoint_count = 1u;
  client_config.default_namespace = namespace_name;
  client_config.logger = logger;
  if (strncmp(endpoint, "pouch://", 8u) == 0) {
    lc_error_init(&lcerr);
    rc = lc_pouch_crypto_default_key_file(&key_file, &lcerr);
    if (rc != LC_OK && failure != NULL && failure_capacity != 0u) {
      vectis_smith_lockdc_diagnostic_message(lcerr.message,
                                             dependency_message,
                                             sizeof(dependency_message));
      (void)snprintf(failure, failure_capacity, "%s",
                     dependency_message);
    }
    lc_error_cleanup(&lcerr);
    if (rc != LC_OK) {
      return -1;
    }
    client_config.pouch_crypto_key_file = key_file;
    client_config.pouch_crypto_generate_key_file = 1;
    client_config.pouch_crypto_generate_key_file_set = 1;
  }
  lc_error_init(&lcerr);
  rc = lc_client_open(&client_config, out_client, &lcerr);
  if (rc != LC_OK && failure != NULL && failure_capacity != 0u) {
    vectis_smith_lockdc_diagnostic_message(lcerr.message, dependency_message,
                                           sizeof(dependency_message));
    (void)snprintf(failure, failure_capacity, "%s", dependency_message);
  }
  lc_error_cleanup(&lcerr);
  lc_pouch_crypto_key_string_free(key_file);
  if (rc != LC_OK) {
    return -1;
  }
  vectis_smith_store_config_init(&store_config);
  store_config.client = *out_client;
  (void)snprintf(owner, sizeof(owner), "vectis-smith-cli-%ld", (long)getpid());
  store_config.owner = owner;
  vectis_smith_cli_redact_endpoint(endpoint, diagnostic_endpoint,
                                   sizeof(diagnostic_endpoint));
  if (vectis_smith_store_new(&store_config, out_store, &error) != VECTIS_OK) {
    if (failure != NULL && failure_capacity != 0u) {
      (void)snprintf(failure, failure_capacity, "%s", error.message);
    }
    lc_client_close(*out_client);
    *out_client = NULL;
    return -1;
  }
  if (vectis_smith_store_set_diagnostic_context(*out_store,
                                                 diagnostic_endpoint,
                                                 namespace_name) != 0) {
    if (failure != NULL && failure_capacity != 0u) {
      (void)snprintf(failure, failure_capacity,
                     "failed to retain lockdc diagnostic context");
    }
    vectis_smith_store_destroy(*out_store);
    *out_store = NULL;
    lc_client_close(*out_client);
    *out_client = NULL;
    return -1;
  }
  return 0;
}

static int vectis_smith_cli_verbosity_argument(const char *argument) {
  size_t index;

  if (argument == NULL) {
    return 0;
  }
  if (strcmp(argument, "--verbose") == 0) {
    return 1;
  }
  if (argument[0] != '-' || argument[1] != 'v') {
    return 0;
  }
  for (index = 1u; argument[index] == 'v'; ++index) {
  }
  return argument[index] == '\0' ? (int)(index - 1u) : 0;
}

static int vectis_smith_cli_logging_requested(void) {
  static const char *const names[] = {
      "LOG_MODE",          "LOG_LEVEL",      "LOG_DISABLE_TIMESTAMP",
      "LOG_VERBOSE_FIELDS", "LOG_NO_COLOR",   "LOG_FORCE_COLOR",
      "LOG_PALETTE",       "LOG_OUTPUT",     "LOG_OUTPUT_FILE_MODE",
      "LOG_TIME_FORMAT",   "LOG_UTC"};
  size_t index;

  for (index = 0u; index < sizeof(names) / sizeof(names[0]); ++index) {
    if (getenv(names[index]) != NULL) {
      return 1;
    }
  }
  return 0;
}

static pslog_logger *vectis_smith_cli_logger_new(int verbosity) {
  pslog_config config;
  pslog_logger *root;
  pslog_logger *logger;

  if (verbosity == 0 && !vectis_smith_cli_logging_requested()) {
    return NULL;
  }
  pslog_default_config(&config);
  config.output = pslog_output_from_fp(stderr, 0);
  root = pslog_new_from_env("LOG_", &config);
  if (root == NULL) {
    return NULL;
  }
  if (verbosity == 0) {
    return root;
  }
  logger = root->with_level(root, verbosity == 1 ? PSLOG_LEVEL_DEBUG
                                                  : PSLOG_LEVEL_TRACE);
  root->destroy(root);
  return logger;
}

typedef struct vectis_smith_cli_ui {
  vectis_smith_cli_agent *agent;
  vectis_smith_cli_render *render;
  int wake_fd;
} vectis_smith_cli_ui;

static int vectis_smith_cli_nonblocking(int fd) {
  int flags;

  flags = fcntl(fd, F_GETFL, 0);
  return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}

static int vectis_smith_cli_ui_apply(sl_t *editor,
                                     vectis_smith_cli_ui *ui) {
  cai_agent_run_state state;
  const char *elements[3];
  char state_element[64];
  char error_element[256];
  int failed;

  vectis_smith_cli_agent_snapshot(ui->agent, &state, &failed, error_element,
                                   sizeof(error_element));
  (void)snprintf(state_element, sizeof(state_element), "agent: %s",
                 vectis_smith_cli_state_name(state));
  elements[0] = "Smith";
  elements[1] = state_element;
  elements[2] = failed || error_element[0] != '\0' ? error_element : NULL;
  if (sl_set_status_elements(editor, elements,
                             elements[2] == NULL ? 2u : 3u) != SL_OK ||
      sl_set_status_busy(editor, vectis_smith_cli_is_active(state)) != SL_OK) {
    return SL_ERROR;
  }
  return SL_OK;
}

static int vectis_smith_cli_ui_watch(sl_t *editor,
                                     const sl_watch_event_t *event,
                                     void *userdata) {
  vectis_smith_cli_ui *ui;
  unsigned char bytes[4096];

  (void)event;
  ui = (vectis_smith_cli_ui *)userdata;
  while (read(ui->wake_fd, bytes, sizeof(bytes)) > 0) {
  }
  vectis_smith_cli_drain_limited(ui->render,
                                 VECTIS_SMITH_CLI_UI_DRAIN_BYTES);
  return vectis_smith_cli_ui_apply(editor, ui);
}

static int vectis_smith_cli_command_enter(sl_t *editor, sl_key_t key,
                                           void *userdata,
                                           sl_key_action_t *action) {
  const char *buffer;

  (void)key;
  (void)userdata;
  buffer = sl_buffer(editor);
  *action = buffer != NULL && buffer[0] == ':' ? SL_KEY_ACTION_SUBMIT
                                                : SL_KEY_ACTION_PASS;
  return SL_OK;
}

static int vectis_smith_cli_interactive(const vectis_smith_config *smith_config,
                                        vectis_smith_cli_render *render) {
  sl_config_t config;
  sl_prompt_source_t source;
  cai_agent_run_state state;
  vectis_smith_cli_agent agent;
  vectis_smith_cli_control_kind kind;
  vectis_smith_cli_ui ui;
  sl_t *editor;
  char *input;
  int wake_fds[2];
  sl_watch_id_t watch;
  int failed;
  int rc;

  wake_fds[0] = -1;
  wake_fds[1] = -1;
  if (pipe(wake_fds) != 0 || vectis_smith_cli_nonblocking(wake_fds[0]) != 0 ||
      vectis_smith_cli_nonblocking(wake_fds[1]) != 0) {
    if (wake_fds[0] >= 0) {
      (void)close(wake_fds[0]);
    }
    if (wake_fds[1] >= 0) {
      (void)close(wake_fds[1]);
    }
    fputs("vectis: failed to create Smith UI wake channel\n", stderr);
    return 1;
  }
  render->notify_fd = wake_fds[1];
  if (vectis_smith_cli_agent_start(&agent, smith_config, render,
                                   wake_fds[1]) != 0) {
    char message[256];

    vectis_smith_cli_agent_snapshot(&agent, &state, &failed, message,
                                     sizeof(message));
    (void)state;
    (void)failed;
    fprintf(stderr, "vectis: %s\n",
            message[0] == '\0' ? "failed to open Smith runtime" : message);
    vectis_smith_cli_agent_stop(&agent, render);
    render->notify_fd = -1;
    (void)close(wake_fds[0]);
    (void)close(wake_fds[1]);
    return 1;
  }
  sl_config_init(&config);
  config.prompt_queue = 1;
  config.prompt_theme = SL_PROMPT_THEME_DEFAULT;
  config.statusline = 1;
  config.status_spinner = 1;
  editor = sl_create_with_config(&config);
  if (editor == NULL) {
    fputs("vectis: failed to create Smith editor\n", stderr);
    vectis_smith_cli_agent_stop(&agent, render);
    render->notify_fd = -1;
    (void)close(wake_fds[0]);
    (void)close(wake_fds[1]);
    return 1;
  }
  render->interactive = 1;
  render->editor = editor;
  ui.agent = &agent;
  ui.render = render;
  ui.wake_fd = wake_fds[0];
  rc = 0;
  if (sl_set_prompt_queue_profile(editor,
                                  SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) != SL_OK ||
      sl_bind_key(editor, SL_KEY_ENTER, vectis_smith_cli_command_enter, NULL) !=
          SL_OK ||
      sl_watch_add(editor, wake_fds[0], SL_WATCH_READ | SL_WATCH_HANGUP,
                   vectis_smith_cli_ui_watch, &ui, &watch) != SL_OK ||
      vectis_smith_cli_ui_apply(editor, &ui) != SL_OK) {
    fputs("vectis: failed to configure Smith interactive UI\n", stderr);
    rc = 1;
  }
  for (;;) {
    if (rc != 0) {
      break;
    }
    input = sl_next_prompt(editor, "smith> ", &source);
    if (input == NULL) {
      if (sl_last_readline_status(editor) == SL_READLINE_EOF ||
          sl_last_readline_status(editor) == SL_READLINE_INTERRUPTED) {
        break;
      }
      continue;
    }
    if (strcmp(input, ":quit") == 0 || strcmp(input, ":exit") == 0) {
      sl_free_string(editor, input);
      break;
    }
    if (input[0] != '\0') {
      vectis_smith_cli_agent_snapshot(&agent, &state, &failed, NULL, 0u);
      kind = (source == SL_PROMPT_SOURCE_PROMOTED ||
              (source == SL_PROMPT_SOURCE_DIRECT &&
               vectis_smith_cli_is_active(state)))
                 ? VECTIS_SMITH_CLI_CONTROL_STEERING
                 : VECTIS_SMITH_CLI_CONTROL_NORMAL;
      if (vectis_smith_cli_agent_submit(&agent, kind, input) != 0) {
        fputs("vectis: Smith input queue is unavailable\n", stderr);
        rc = 1;
      } else if (kind == VECTIS_SMITH_CLI_CONTROL_NORMAL) {
        /* Make Enter queue immediately, before the worker observes the turn. */
        (void)sl_set_status_busy(editor, 1);
      }
    }
    sl_free_string(editor, input);
  }
  sl_destroy(editor);
  render->editor = NULL;
  render->interactive = 0;
  vectis_smith_cli_agent_stop(&agent, render);
  vectis_smith_cli_render_finish(render);
  render->notify_fd = -1;
  (void)close(wake_fds[0]);
  (void)close(wake_fds[1]);
  return rc;
}

int vectis_smith_cli_command(int argc, char **argv, int index,
                             int initial_verbosity) {
  vectis_smith_cli_render render;
  vectis_smith_config config;
  vectis_error error;
  cai_chatgpt_auth_config auth_config;
  cai_chatgpt_auth *chatgpt_auth;
  cai_error caierr;
  cai_terminal_tool_config terminal_config;
  cai_agent_run_state state;
  vectis_smith *smith;
  const char *prompt;
  const char *session_id;
  const char *workspace_arg;
  const char *state_endpoint_arg;
  const char *state_namespace;
  char workspace[4096];
  char default_state_endpoint[PATH_MAX + 64u];
  char state_diagnostic_endpoint[PATH_MAX + 64u];
  char state_failure[512];
  lc_client *state_client;
  pslog_logger *logger;
  vectis_smith_store *store;
  int verbosity;
  int rc;

  prompt = NULL;
  session_id = NULL;
  workspace_arg = NULL;
  state_endpoint_arg = NULL;
  state_namespace = "vectis.smith";
  verbosity = initial_verbosity;
  while (index < argc) {
    if (vectis_smith_cli_verbosity_argument(argv[index]) != 0) {
      verbosity += vectis_smith_cli_verbosity_argument(argv[index]);
    } else if (strcmp(argv[index], "-e") == 0 ||
        strcmp(argv[index], "--execute") == 0) {
      if (index + 1 >= argc) {
        fprintf(stderr, "vectis: %s requires a prompt\n", argv[index]);
        return 64;
      }
      prompt = argv[++index];
    } else if ((strcmp(argv[index], "-s") == 0 ||
                strcmp(argv[index], "--session") == 0)) {
      if (index + 1 >= argc) {
        fprintf(stderr, "vectis: %s requires a session id\n", argv[index]);
        return 64;
      }
      session_id = argv[++index];
    } else if ((strcmp(argv[index], "-w") == 0 ||
                strcmp(argv[index], "--workspace") == 0)) {
      if (index + 1 >= argc) {
        fprintf(stderr, "vectis: %s requires a workspace directory\n",
                argv[index]);
        return 64;
      }
      workspace_arg = argv[++index];
    } else if (strcmp(argv[index], "--state-endpoint") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --state-endpoint requires an endpoint\n", stderr);
        return 64;
      }
      state_endpoint_arg = argv[++index];
    } else if (strcmp(argv[index], "--state-namespace") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --state-namespace requires a namespace\n", stderr);
        return 64;
      }
      state_namespace = argv[++index];
    } else {
      fprintf(stderr, "vectis: unknown smith argument: %s\n", argv[index]);
      return 64;
    }
    ++index;
  }
  logger = vectis_smith_cli_logger_new(verbosity);
  if ((verbosity != 0 || vectis_smith_cli_logging_requested()) &&
      logger == NULL) {
    fputs("vectis: failed to configure pslog diagnostics\n", stderr);
    return 1;
  }
  if (vectis_smith_cli_get_workspace(workspace, sizeof(workspace),
                                     workspace_arg) != 0) {
    fputs("vectis: failed to resolve Smith workspace\n", stderr);
    if (logger != NULL) {
      logger->destroy(logger);
    }
    return 1;
  }
  memset(&render, 0, sizeof(render));
  render.notify_fd = -1;
  (void)pthread_mutex_init(&render.mutex, NULL);
  (void)pthread_cond_init(&render.changed, NULL);
  vectis_smith_config_init(&config);
  memset(&terminal_config, 0, sizeof(terminal_config));
  terminal_config.root_path = workspace;
  config.runtime.terminal_tool_config = &terminal_config;
  config.client_config.api_key_env = "OPENAI_API_KEY";
  config.client_config.logger = logger;
  config.runtime.logger = logger;
  config.runtime.workspace_directory = workspace;
  config.runtime.session_id = session_id;
  config.runtime.resume_latest = session_id == NULL;
  config.runtime.event_callback = vectis_smith_cli_render_event;
  config.runtime.event_context = &render;
  chatgpt_auth = NULL;
  cai_chatgpt_auth_config_init(&auth_config);
  auth_config.logger = logger;
  cai_error_init(&caierr);
  if (cai_chatgpt_auth_open(&auth_config, &chatgpt_auth, &caierr) == CAI_OK) {
    config.client_config.chatgpt_auth = chatgpt_auth;
  }
  cai_error_cleanup(&caierr);
  if (state_endpoint_arg == NULL) {
    if (vectis_smith_cli_default_state_endpoint(
            default_state_endpoint, sizeof(default_state_endpoint)) != 0) {
      fputs("vectis: failed to create default Smith state directory\n", stderr);
      cai_chatgpt_auth_close(chatgpt_auth);
      vectis_smith_cli_render_cleanup(&render);
      if (logger != NULL) {
        logger->destroy(logger);
      }
      return 1;
    }
    state_endpoint_arg = default_state_endpoint;
  }
  state_client = NULL;
  store = NULL;
  vectis_smith_cli_redact_endpoint(state_endpoint_arg,
                                   state_diagnostic_endpoint,
                                   sizeof(state_diagnostic_endpoint));
  if (vectis_smith_cli_open_store(state_endpoint_arg, state_namespace, logger,
                                  state_failure, sizeof(state_failure),
                                  &state_client, &store) != 0) {
    fprintf(stderr,
            "vectis: lockdc error: unable to open durable Smith state "
            "(endpoint=%s namespace=%s): %s\n",
            state_diagnostic_endpoint, state_namespace,
            state_failure[0] != '\0' ? state_failure : "unknown lockdc error");
    cai_chatgpt_auth_close(chatgpt_auth);
    vectis_smith_cli_render_cleanup(&render);
    if (logger != NULL) {
      logger->destroy(logger);
    }
    return 1;
  }
  config.store = store;
  smith = NULL;
  if (prompt == NULL) {
    rc = vectis_smith_cli_interactive(&config, &render);
  } else if (vectis_smith_open(&config, &smith, &error) != VECTIS_OK) {
    fprintf(stderr, "vectis: %s\n", error.message);
    rc = 1;
  } else if (vectis_smith_submit(smith, prompt, &error) != VECTIS_OK) {
    fprintf(stderr, "vectis: %s\n", error.message);
    rc = 1;
  } else {
    rc = 0;
    do {
      if (vectis_smith_pump(smith, 100L, &error) != VECTIS_OK) {
        fprintf(stderr, "vectis: %s\n", error.message);
        rc = 1;
        break;
      }
      vectis_smith_cli_drain(&render);
      if (vectis_smith_state(smith, &state, &error) != VECTIS_OK) {
        rc = 1;
        break;
      }
    } while (vectis_smith_cli_is_active(state));
  }
  vectis_smith_cli_render_finish(&render);
  if (vectis_smith_cli_render_failed(&render)) {
    fputs("vectis: failed to render streamed Smith Markdown\n", stderr);
    rc = 1;
  }
  vectis_smith_close(smith);
  vectis_smith_store_destroy(store);
  lc_client_close(state_client);
  cai_chatgpt_auth_close(chatgpt_auth);
  vectis_smith_cli_render_cleanup(&render);
  if (logger != NULL) {
    logger->destroy(logger);
  }
  return rc;
}
