#include "vectis_cli.h"

#include <cai/agent_runtime.h>
#include <cai/auth.h>
#include <cai/tools/terminal.h>
#include <errno.h>
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
  sl_t *editor;
} vectis_smith_cli_render;

typedef struct vectis_smith_cli_chunk {
  const char *data;
  size_t length;
} vectis_smith_cli_chunk;

typedef struct vectis_smith_cli_idle_context {
  vectis_smith *smith;
  vectis_smith_cli_render *render;
} vectis_smith_cli_idle_context;

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

static void vectis_smith_cli_drain(vectis_smith_cli_render *render) {
  char buffer[VECTIS_SMITH_CLI_DRAIN_BYTES];
  size_t length;

  for (;;) {
    (void)pthread_mutex_lock(&render->mutex);
    length =
        vectis_smith_cli_ring_read(&render->rendered, buffer, sizeof(buffer));
    (void)pthread_cond_broadcast(&render->changed);
    (void)pthread_mutex_unlock(&render->mutex);
    if (length == 0u) {
      break;
    }
    if (vectis_smith_cli_print_chunk(
            render->interactive ? render->editor : NULL, buffer, length) != 0) {
      break;
    }
  }
  if (!render->interactive) {
    (void)fflush(stdout);
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
      (void)pthread_mutex_unlock(&render->mutex);
      vectis_smith_cli_drain(render);
      (void)pthread_mutex_lock(&render->mutex);
      if (render->markdown.count == VECTIS_SMITH_CLI_QUEUE_BYTES) {
        (void)pthread_cond_wait(&render->changed, &render->mutex);
      }
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

static int vectis_smith_cli_event(void *userdata,
                                  const cai_agent_runtime_event *event,
                                  cai_error *error) {
  vectis_smith_cli_render *render;

  (void)error;
  render = (vectis_smith_cli_render *)userdata;
  if (event->type == CAI_AGENT_EVENT_TEXT_DELTA && event->data_length != 0u &&
      vectis_smith_cli_render_append(render, event->data, event->data_length) !=
          0) {
    return CAI_ERR_NOMEM;
  }
  if (event->type == CAI_AGENT_EVENT_RUN_COMPLETED ||
      event->type == CAI_AGENT_EVENT_RUN_FAILED ||
      event->type == CAI_AGENT_EVENT_RESPONSE_COMPLETED ||
      (event->type == CAI_AGENT_EVENT_RUN_STATE_CHANGED &&
       (event->state == CAI_AGENT_COMPLETED ||
        event->state == CAI_AGENT_FAILED ||
        event->state == CAI_AGENT_CANCELLED))) {
    vectis_smith_cli_render_finish(render);
  }
  return CAI_OK;
}

static void vectis_smith_cli_idle(sl_t *editor, void *userdata) {
  vectis_smith_cli_idle_context *context;
  vectis_error error;

  (void)editor;
  context = (vectis_smith_cli_idle_context *)userdata;
  if (vectis_smith_pump(context->smith, 0L, &error) != VECTIS_OK) {
    return;
  }
  vectis_smith_cli_drain(context->render);
}

static int vectis_smith_cli_is_active(cai_agent_run_state state) {
  return state == CAI_AGENT_SAMPLING || state == CAI_AGENT_DISPATCHING_TOOL;
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

static int vectis_smith_cli_open_store(const char *endpoint,
                                       const char *namespace_name,
                                       lc_client **out_client,
                                       vectis_smith_store **out_store) {
  lc_client_config client_config;
  vectis_smith_store_config store_config;
  lc_error lcerr;
  vectis_error error;
  const char *endpoints[1];
  char owner[64];
  char *key_file;
  int rc;

  *out_client = NULL;
  *out_store = NULL;
  key_file = NULL;
  endpoints[0] = endpoint;
  lc_client_config_init(&client_config);
  client_config.endpoints = endpoints;
  client_config.endpoint_count = 1u;
  client_config.default_namespace = namespace_name;
  if (strncmp(endpoint, "pouch://", 8u) == 0) {
    lc_error_init(&lcerr);
    rc = lc_pouch_crypto_default_key_file(&key_file, &lcerr);
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
  lc_error_cleanup(&lcerr);
  lc_pouch_crypto_key_string_free(key_file);
  if (rc != LC_OK) {
    return -1;
  }
  vectis_smith_store_config_init(&store_config);
  store_config.client = *out_client;
  (void)snprintf(owner, sizeof(owner), "vectis-smith-cli-%ld", (long)getpid());
  store_config.owner = owner;
  if (vectis_smith_store_new(&store_config, out_store, &error) != VECTIS_OK) {
    lc_client_close(*out_client);
    *out_client = NULL;
    return -1;
  }
  return 0;
}

static int vectis_smith_cli_submit(vectis_smith *smith, const char *text,
                                   cai_agent_run_state state,
                                   sl_prompt_source_t source,
                                   vectis_error *error) {
  vectis_status status;

  if (vectis_smith_cli_is_active(state)) {
    status = source == SL_PROMPT_SOURCE_QUEUED
                 ? vectis_smith_submit_queued(smith, text, error)
                 : vectis_smith_submit_steering(smith, text, error);
  } else {
    status = source == SL_PROMPT_SOURCE_QUEUED
                 ? vectis_smith_submit_queued(smith, text, error)
                 : vectis_smith_submit(smith, text, error);
  }
  return status == VECTIS_OK ? 0 : -1;
}

static int vectis_smith_cli_interactive(vectis_smith *smith,
                                        vectis_smith_cli_render *render) {
  sl_config_t config;
  sl_prompt_source_t source;
  cai_agent_run_state state;
  vectis_error error;
  sl_t *editor;
  char *input;
  vectis_smith_cli_idle_context idle;

  sl_config_init(&config);
  config.prompt_queue = 1;
  config.prompt_theme = SL_PROMPT_THEME_DEFAULT;
  config.statusline = 1;
  config.status_spinner = 1;
  editor = sl_create_with_config(&config);
  if (editor == NULL) {
    fputs("vectis: failed to create Smith editor\n", stderr);
    return 1;
  }
  render->interactive = 1;
  render->editor = editor;
  idle.smith = smith;
  idle.render = render;
  (void)sl_set_idle_callback(editor, vectis_smith_cli_idle, &idle);
  for (;;) {
    if (vectis_smith_state(smith, &state, &error) != VECTIS_OK) {
      fprintf(stderr, "vectis: %s\n", error.message);
      break;
    }
    input = sl_next_prompt(
        editor, vectis_smith_cli_is_active(state) ? "steer> " : "smith> ",
        &source);
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
      if (vectis_smith_state(smith, &state, &error) != VECTIS_OK ||
          vectis_smith_cli_submit(smith, input, state, source, &error) != 0) {
        fprintf(stderr, "vectis: %s\n", error.message);
      }
    }
    sl_free_string(editor, input);
  }
  sl_destroy(editor);
  render->editor = NULL;
  render->interactive = 0;
  return 0;
}

int vectis_smith_cli_command(int argc, char **argv, int index) {
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
  lc_client *state_client;
  vectis_smith_store *store;
  int rc;

  prompt = NULL;
  session_id = NULL;
  workspace_arg = NULL;
  state_endpoint_arg = NULL;
  state_namespace = "vectis.smith";
  while (index < argc) {
    if (strcmp(argv[index], "-e") == 0 ||
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
  if (vectis_smith_cli_get_workspace(workspace, sizeof(workspace),
                                     workspace_arg) != 0) {
    fputs("vectis: failed to resolve Smith workspace\n", stderr);
    return 1;
  }
  memset(&render, 0, sizeof(render));
  (void)pthread_mutex_init(&render.mutex, NULL);
  (void)pthread_cond_init(&render.changed, NULL);
  vectis_smith_config_init(&config);
  memset(&terminal_config, 0, sizeof(terminal_config));
  terminal_config.root_path = workspace;
  config.runtime.terminal_tool_config = &terminal_config;
  config.client_config.api_key_env = "OPENAI_API_KEY";
  config.runtime.workspace_directory = workspace;
  config.runtime.session_id = session_id;
  config.runtime.resume_latest = session_id == NULL;
  config.runtime.event_callback = vectis_smith_cli_event;
  config.runtime.event_context = &render;
  chatgpt_auth = NULL;
  cai_chatgpt_auth_config_init(&auth_config);
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
      return 1;
    }
    state_endpoint_arg = default_state_endpoint;
  }
  state_client = NULL;
  store = NULL;
  if (vectis_smith_cli_open_store(state_endpoint_arg, state_namespace,
                                  &state_client, &store) != 0) {
    fputs("vectis: failed to open durable Smith LockDC state\n", stderr);
    cai_chatgpt_auth_close(chatgpt_auth);
    vectis_smith_cli_render_cleanup(&render);
    return 1;
  }
  config.store = store;
  smith = NULL;
  if (vectis_smith_open(&config, &smith, &error) != VECTIS_OK) {
    fprintf(stderr, "vectis: %s\n", error.message);
    vectis_smith_store_destroy(store);
    lc_client_close(state_client);
    cai_chatgpt_auth_close(chatgpt_auth);
    vectis_smith_cli_render_cleanup(&render);
    return 1;
  }
  if (prompt == NULL) {
    rc = vectis_smith_cli_interactive(smith, &render);
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
  return rc;
}
