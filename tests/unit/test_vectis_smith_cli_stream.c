#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* The CLI bridge is intentionally source-included so this test exercises its
 * private bounded queues and real libmdf streaming renderer without opening a
 * provider client or terminal UI. */
#define vectis_smith_open vectis_smith_test_open
#define vectis_smith_close vectis_smith_test_close
#define vectis_smith_submit_steering vectis_smith_test_submit_steering
#define vectis_smith_submit_queued vectis_smith_test_submit_queued
#define vectis_smith_pump vectis_smith_test_pump
#define vectis_smith_state vectis_smith_test_state
#include "../../src/vectis_smith_cli.c"
#undef vectis_smith_open
#undef vectis_smith_close
#undef vectis_smith_submit_steering
#undef vectis_smith_submit_queued
#undef vectis_smith_pump
#undef vectis_smith_state

static cai_agent_run_state test_runtime_state;
static cai_agent_runtime_event_fn test_event_callback;
static void *test_event_context;
static int test_emit_text;
static int test_emit_failure;
static int test_log_fd = -1;

#define TEST_VT_ROWS 8
#define TEST_VT_COLS 80

typedef struct test_vt {
  char cells[TEST_VT_ROWS][TEST_VT_COLS + 1];
  int row;
  int column;
  int scroll_top;
  int scroll_bottom;
} test_vt;

static void test_vt_clear(test_vt *vt) {
  int row;

  for (row = 0; row < TEST_VT_ROWS; ++row) {
    memset(vt->cells[row], ' ', TEST_VT_COLS);
    vt->cells[row][TEST_VT_COLS] = '\0';
  }
  vt->row = 0;
  vt->column = 0;
  vt->scroll_top = 0;
  vt->scroll_bottom = TEST_VT_ROWS - 1;
}

static void test_vt_scroll(test_vt *vt) {
  int row;

  for (row = vt->scroll_top + 1; row <= vt->scroll_bottom; ++row) {
    memcpy(vt->cells[row - 1], vt->cells[row], TEST_VT_COLS + 1u);
  }
  memset(vt->cells[vt->scroll_bottom], ' ', TEST_VT_COLS);
  vt->cells[vt->scroll_bottom][TEST_VT_COLS] = '\0';
  vt->row = vt->scroll_bottom;
}

static void test_vt_lf(test_vt *vt) {
  if (vt->row == vt->scroll_bottom) {
    test_vt_scroll(vt);
  } else {
    ++vt->row;
  }
}

static void test_vt_put(test_vt *vt, char character) {
  if (vt->column == TEST_VT_COLS) {
    vt->column = 0;
    test_vt_lf(vt);
  }
  vt->cells[vt->row][vt->column++] = character;
}

static size_t test_vt_number(const char *data, size_t length, size_t index,
                             int *out) {
  int value;

  value = 0;
  while (index < length && data[index] >= '0' && data[index] <= '9') {
    value = value * 10 + (data[index] - '0');
    ++index;
  }
  *out = value;
  return index;
}

static size_t test_vt_csi(test_vt *vt, const char *data, size_t length,
                          size_t index) {
  int first;
  int second;
  int has_first;
  char command;

  first = 0;
  second = 0;
  has_first = 0;
  if (index < length && data[index] == '?') {
    while (index < length && (data[index] < '@' || data[index] > '~'))
      ++index;
    return index < length ? index + 1u : index;
  }
  if (index < length && data[index] >= '0' && data[index] <= '9') {
    has_first = 1;
    index = test_vt_number(data, length, index, &first);
  }
  if (index < length && data[index] == ';') {
    ++index;
    index = test_vt_number(data, length, index, &second);
  }
  if (index == length)
    return index;
  command = data[index++];
  switch (command) {
  case 'A':
    vt->row -= has_first && first > 0 ? first : 1;
    break;
  case 'B':
    vt->row += has_first && first > 0 ? first : 1;
    break;
  case 'C':
    vt->column += has_first && first > 0 ? first : 1;
    break;
  case 'D':
    vt->column -= has_first && first > 0 ? first : 1;
    break;
  case 'H':
  case 'f':
    vt->row = has_first && first > 0 ? first - 1 : 0;
    vt->column = second > 0 ? second - 1 : 0;
    break;
  case 'J':
    if (first == 2)
      test_vt_clear(vt);
    break;
  case 'K':
    memset(vt->cells[vt->row] + vt->column, ' ',
           (size_t)(TEST_VT_COLS - vt->column));
    break;
  case 'r':
    if (!has_first) {
      vt->scroll_top = 0;
      vt->scroll_bottom = TEST_VT_ROWS - 1;
    } else {
      vt->scroll_top = first > 0 ? first - 1 : 0;
      vt->scroll_bottom = second > 0 ? second - 1 : TEST_VT_ROWS - 1;
    }
    break;
  default:
    break;
  }
  if (vt->row < 0)
    vt->row = 0;
  if (vt->row >= TEST_VT_ROWS)
    vt->row = TEST_VT_ROWS - 1;
  if (vt->column < 0)
    vt->column = 0;
  if (vt->column >= TEST_VT_COLS)
    vt->column = TEST_VT_COLS - 1;
  return index;
}

static void test_vt_apply(test_vt *vt, const char *data, size_t length) {
  size_t index;

  for (index = 0u; index < length;) {
    if (data[index] == '\033' && index + 1u < length &&
        data[index + 1u] == '[') {
      index = test_vt_csi(vt, data, length, index + 2u);
    } else if (data[index] == '\r') {
      vt->column = 0;
      ++index;
    } else if (data[index] == '\n') {
      test_vt_lf(vt);
      ++index;
    } else {
      if ((unsigned char)data[index] >= 32u)
        test_vt_put(vt, data[index]);
      ++index;
    }
  }
}

static int test_vt_contains(const test_vt *vt, const char *needle) {
  int row;

  for (row = 0; row < TEST_VT_ROWS; ++row) {
    if (strstr(vt->cells[row], needle) != NULL)
      return 1;
  }
  return 0;
}

static void test_log(const char *prefix, const char *text) {
  char line[256];
  int length;

  length = snprintf(line, sizeof(line), "%s%s\n", prefix, text);
  assert(length > 0 && (size_t)length < sizeof(line));
  assert(write(test_log_fd, line, (size_t)length) == length);
}

vectis_status vectis_smith_test_open(const vectis_smith_config *config,
                                     vectis_smith **out, vectis_error *error) {
  test_runtime_state = CAI_AGENT_IDLE;
  test_event_callback = config->runtime.event_callback;
  test_event_context = config->runtime.event_context;
  test_emit_text = 0;
  *out = (vectis_smith *)1;
  vectis_error_clear(error);
  return VECTIS_OK;
}

void vectis_smith_test_close(vectis_smith *smith) { (void)smith; }

vectis_status vectis_smith_test_submit_steering(vectis_smith *smith,
                                                const char *text,
                                                vectis_error *error) {
  (void)smith;
  test_log("S:", text);
  test_runtime_state = CAI_AGENT_IDLE;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_smith_test_submit_queued(vectis_smith *smith,
                                              const char *text,
                                              vectis_error *error) {
  (void)smith;
  test_log("N:", text);
  test_runtime_state = CAI_AGENT_SAMPLING;
  test_emit_text = 1;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_smith_test_pump(vectis_smith *smith, long timeout_ms,
                                     vectis_error *error) {
  cai_agent_runtime_event event;

  (void)smith;
  (void)timeout_ms;
  if (test_emit_failure) {
    memset(&event, 0, sizeof(event));
    event.type = CAI_AGENT_EVENT_RUN_FAILED;
    event.data = "offline provider failureTRAILER";
    event.data_length = strlen("offline provider failure");
    assert(test_event_callback(test_event_context, &event, NULL) == CAI_OK);
    test_runtime_state = CAI_AGENT_FAILED;
    test_emit_failure = 0;
  }
  if (test_emit_text) {
    memset(&event, 0, sizeof(event));
    event.type = CAI_AGENT_EVENT_TEXT_DELTA;
    /* A live model delta is not necessarily newline-terminated. */
    event.data = "streamed output";
    event.data_length = strlen(event.data);
    assert(test_event_callback(test_event_context, &event, NULL) == CAI_OK);
    event.type = CAI_AGENT_EVENT_RESPONSE_COMPLETED;
    event.data = NULL;
    event.data_length = 0u;
    assert(test_event_callback(test_event_context, &event, NULL) == CAI_OK);
    test_emit_text = 0;
  }
  usleep(1000u);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_smith_test_state(vectis_smith *smith,
                                      cai_agent_run_state *out,
                                      vectis_error *error) {
  (void)smith;
  *out = test_runtime_state;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static void test_render_multiple_responses(void) {
  vectis_smith_cli_render render;
  int pipe_fds[2];
  int saved_stdout;
  char output[4096];
  ssize_t nread;

  memset(&render, 0, sizeof(render));
  render.notify_fd = -1;
  assert(pthread_mutex_init(&render.mutex, NULL) == 0);
  assert(pthread_cond_init(&render.changed, NULL) == 0);
  assert(pipe(pipe_fds) == 0);
  assert(fflush(stdout) == 0);
  saved_stdout = dup(STDOUT_FILENO);
  assert(saved_stdout >= 0);
  assert(dup2(pipe_fds[1], STDOUT_FILENO) == STDOUT_FILENO);
  assert(close(pipe_fds[1]) == 0);

  assert(vectis_smith_cli_render_append(&render, "# first response\n", 17u) ==
         0);
  usleep(100000u);
  /* Exec output is delivered while the response is still open. */
  {
    struct pollfd ready = {pipe_fds[0], POLLIN, 0};
    assert(poll(&ready, 1u, 1000) == 1);
  }
  vectis_smith_cli_render_finish(&render);
  assert(!vectis_smith_cli_render_failed(&render));
  assert(vectis_smith_cli_render_append(&render, "second response\n", 16u) ==
         0);
  vectis_smith_cli_render_finish(&render);
  assert(!vectis_smith_cli_render_failed(&render));
  assert(fflush(stdout) == 0);
  assert(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO);
  assert(close(saved_stdout) == 0);

  nread = read(pipe_fds[0], output, sizeof(output) - 1u);
  assert(nread > 0);
  output[nread] = '\0';
  assert(close(pipe_fds[0]) == 0);
  assert(strstr(output, "first response") != NULL);
  assert(strstr(output, "second response") != NULL);
  vectis_smith_cli_render_cleanup(&render);
}

static void test_plain_delta_reaches_renderer_before_boundary(void) {
  vectis_smith_cli_render render;
  char output[128];
  size_t length;
  int attempt;

  memset(&render, 0, sizeof(render));
  render.interactive = 1;
  render.notify_fd = -1;
  assert(pthread_mutex_init(&render.mutex, NULL) == 0);
  assert(pthread_cond_init(&render.changed, NULL) == 0);
  assert(vectis_smith_cli_render_append(&render, "streamed output",
                                        strlen("streamed output")) == 0);
  for (attempt = 0; attempt < 100; ++attempt) {
    (void)pthread_mutex_lock(&render.mutex);
    length = vectis_smith_cli_ring_read(&render.rendered, output,
                                        sizeof(output) - 1u);
    (void)pthread_cond_broadcast(&render.changed);
    (void)pthread_mutex_unlock(&render.mutex);
    if (length != 0u) {
      output[length] = '\0';
      break;
    }
    usleep(10000u);
  }
  assert(length != 0u);
  assert(strstr(output, "streamed") != NULL);
  vectis_smith_cli_render_cleanup(&render);
}

static void test_agent_control_bridge_is_fifo(void) {
  vectis_smith_cli_agent agent;
  vectis_smith_cli_control control;

  memset(&agent, 0, sizeof(agent));
  agent.notify_fd = -1;
  assert(pthread_mutex_init(&agent.mutex, NULL) == 0);
  assert(pthread_cond_init(&agent.changed, NULL) == 0);
  assert(vectis_smith_cli_agent_submit(&agent, VECTIS_SMITH_CLI_CONTROL_NORMAL,
                                       "next") == 0);
  assert(vectis_smith_cli_agent_submit(
             &agent, VECTIS_SMITH_CLI_CONTROL_STEERING, "steer") == 0);
  assert(vectis_smith_cli_agent_take_control(&agent, &control));
  assert(control.kind == VECTIS_SMITH_CLI_CONTROL_NORMAL);
  assert(strcmp(control.text, "next") == 0);
  free(control.text);
  assert(vectis_smith_cli_agent_take_control(&agent, &control));
  assert(control.kind == VECTIS_SMITH_CLI_CONTROL_STEERING);
  assert(strcmp(control.text, "steer") == 0);
  free(control.text);
  assert(!vectis_smith_cli_agent_take_control(&agent, &control));
  (void)pthread_cond_destroy(&agent.changed);
  (void)pthread_mutex_destroy(&agent.mutex);
}

static void test_softline_queued_turns_profile(void) {
  sl_prompt_queue_keys_t keys;
  sl_prompt_queue_profile_t profile;
  sl_t *editor;

  editor = sl_create();
  assert(editor != NULL);
  assert(sl_set_prompt_queue(editor, 1, 4, 2) == SL_OK);
  assert(sl_set_prompt_queue_profile(
             editor, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) == SL_OK);
  assert(sl_get_prompt_queue_profile(editor, &profile) == SL_OK);
  assert(profile == SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS);
  assert(sl_get_prompt_queue_keys(editor, &keys) == SL_OK);
  assert(keys.submit_or_promote_newest == SL_KEY_ALT_ENTER);
  sl_destroy(editor);
}

static void test_cli_diagnostic_endpoint_redaction(void) {
  char endpoint[256];
  char message[256];

  vectis_smith_cli_redact_endpoint(
      "https://operator:secret@lockd.example/v1?access_token=secret", endpoint,
      sizeof(endpoint));
  assert(strcmp(endpoint, "https://lockd.example/v1") == 0);
  vectis_smith_cli_redact_endpoint(
      "pouch:///var/lib/vectis?crypto_key=secret&single_writer=false", endpoint,
      sizeof(endpoint));
  assert(strcmp(endpoint, "pouch:///var/lib/vectis") == 0);
  vectis_smith_lockdc_diagnostic_message(
      "request to https://operator:secret@lockd.example/v1 failed", message,
      sizeof(message));
  assert(strcmp(message, "lockdc returned a redacted dependency error") == 0);
  vectis_smith_lockdc_diagnostic_message(
      "pouch binary control record magic mismatch", message, sizeof(message));
  assert(strcmp(message, "pouch binary control record magic mismatch") == 0);
}

static void test_cli_verbosity_arguments(void) {
  assert(vectis_smith_cli_verbosity_argument("-v") == 1);
  assert(vectis_smith_cli_verbosity_argument("-vv") == 2);
  assert(vectis_smith_cli_verbosity_argument("--verbose") == 1);
  assert(vectis_smith_cli_verbosity_argument("--version") == 0);
}

static void test_interactive_queue_and_promote(void) {
  char transcript[4096];
  char log[256];
  cai_terminal_tool_config terminal;
  vectis_smith_cli_render render;
  vectis_smith_config config;
  int log_fds[2];
  int master;
  int status;
  int flags;
  int i;
  pid_t child;
  ssize_t length;
  size_t log_length;
  size_t transcript_length;

  assert(pipe(log_fds) == 0);
  child = forkpty(&master, NULL, NULL, NULL);
  assert(child >= 0);
  if (child == 0) {
    assert(close(log_fds[0]) == 0);
    test_log_fd = log_fds[1];
    memset(&render, 0, sizeof(render));
    render.notify_fd = -1;
    assert(pthread_mutex_init(&render.mutex, NULL) == 0);
    assert(pthread_cond_init(&render.changed, NULL) == 0);
    vectis_smith_config_init(&config);
    memset(&terminal, 0, sizeof(terminal));
    config.runtime.terminal_tool_config = &terminal;
    assert(vectis_smith_cli_interactive(&config, &render) == 0);
    vectis_smith_cli_render_cleanup(&render);
    assert(close(log_fds[1]) == 0);
    _exit(0);
  }
  assert(close(log_fds[1]) == 0);
  log[0] = '\0';
  transcript[0] = '\0';
  usleep(100000u);
  assert(write(master, "first\r", 6u) == 6);
  usleep(100000u);
  assert(write(master, "later\r", 6u) == 6);
  usleep(50000u);
  assert(write(master, "\033\r", 2u) == 2);
  flags = fcntl(master, F_GETFL, 0);
  assert(flags >= 0 && fcntl(master, F_SETFL, flags | O_NONBLOCK) == 0);
  flags = fcntl(log_fds[0], F_GETFL, 0);
  assert(flags >= 0 && fcntl(log_fds[0], F_SETFL, flags | O_NONBLOCK) == 0);
  log_length = 0u;
  transcript_length = 0u;
  for (i = 0; i < 100; ++i) {
    struct pollfd fds[2];

    memset(fds, 0, sizeof(fds));
    fds[0].fd = log_fds[0];
    fds[0].events = POLLIN;
    fds[1].fd = master;
    fds[1].events = POLLIN;
    assert(poll(fds, 2u, 20) >= 0);
    if ((fds[0].revents & POLLIN) != 0) {
      length =
          read(log_fds[0], log + log_length, sizeof(log) - log_length - 1u);
      if (length > 0) {
        log_length += (size_t)length;
        log[log_length] = '\0';
      }
    }
    if ((fds[1].revents & POLLIN) != 0) {
      length = read(master, transcript + transcript_length,
                    sizeof(transcript) - transcript_length - 1u);
      if (length > 0) {
        transcript_length += (size_t)length;
        transcript[transcript_length] = '\0';
      }
    }
    if (strcmp(log, "N:first\nS:later\n") == 0) {
      break;
    }
  }
  assert(strcmp(log, "N:first\nS:later\n") == 0);
  /* The completed prefix is visible during the active response, and the
   * response-complete event flushes its final unterminated word. Softline
   * redraw control bytes may separate the two terminal fragments. */
  assert(strstr(transcript, "streamed") != NULL);
  assert(strstr(transcript, "output") != NULL);
  usleep(100000u);
  assert(write(master, ":quit\r", 6u) == 6);
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(transcript_length != 0u);
  assert(close(master) == 0);
  assert(close(log_fds[0]) == 0);
}

static void test_interactive_live_output_stays_visible(void) {
  char transcript[8192];
  cai_terminal_tool_config terminal;
  vectis_smith_cli_render render;
  vectis_smith_config config;
  int log_fds[2];
  int master;
  int status;
  int flags;
  int probe_replied;
  int visible;
  int i;
  pid_t child;
  ssize_t length;
  size_t transcript_length;
  struct winsize size;
  test_vt terminal_screen;

  memset(&size, 0, sizeof(size));
  size.ws_row = TEST_VT_ROWS;
  size.ws_col = TEST_VT_COLS;
  assert(pipe(log_fds) == 0);
  child = forkpty(&master, NULL, NULL, &size);
  assert(child >= 0);
  if (child == 0) {
    assert(close(log_fds[0]) == 0);
    test_log_fd = log_fds[1];
    memset(&render, 0, sizeof(render));
    render.notify_fd = -1;
    assert(pthread_mutex_init(&render.mutex, NULL) == 0);
    assert(pthread_cond_init(&render.changed, NULL) == 0);
    vectis_smith_config_init(&config);
    memset(&terminal, 0, sizeof(terminal));
    config.runtime.terminal_tool_config = &terminal;
    assert(vectis_smith_cli_interactive(&config, &render) == 0);
    vectis_smith_cli_render_cleanup(&render);
    assert(close(log_fds[1]) == 0);
    _exit(0);
  }
  assert(close(log_fds[1]) == 0);
  flags = fcntl(master, F_GETFL, 0);
  assert(flags >= 0 && fcntl(master, F_SETFL, flags | O_NONBLOCK) == 0);
  transcript_length = 0u;
  probe_replied = 0;
  visible = 0;
  test_vt_clear(&terminal_screen);
  for (i = 0; i < 100; ++i) {
    struct pollfd ready;

    ready.fd = master;
    ready.events = POLLIN;
    ready.revents = 0;
    assert(poll(&ready, 1u, 20) >= 0);
    if ((ready.revents & POLLIN) == 0)
      continue;
    length = read(master, transcript + transcript_length,
                  sizeof(transcript) - transcript_length);
    if (length > 0) {
      test_vt_apply(&terminal_screen, transcript + transcript_length,
                    (size_t)length);
      transcript_length += (size_t)length;
    }
    if (test_vt_contains(&terminal_screen, "> "))
      break;
  }
  assert(test_vt_contains(&terminal_screen, "> "));
  assert(write(master, "first\r", 6u) == 6);
  for (i = 0; i < 200; ++i) {
    struct pollfd ready;

    ready.fd = master;
    ready.events = POLLIN;
    ready.revents = 0;
    assert(poll(&ready, 1u, 20) >= 0);
    if ((ready.revents & POLLIN) == 0)
      continue;
    length = read(master, transcript + transcript_length,
                  sizeof(transcript) - transcript_length);
    if (length <= 0)
      continue;
    test_vt_apply(&terminal_screen, transcript + transcript_length,
                  (size_t)length);
    transcript_length += (size_t)length;
    if (!probe_replied && transcript_length >= 4u &&
        memmem(transcript, transcript_length, "\033[6n", 4u) != NULL) {
      assert(write(master, "\033[8;1R", 6u) == 6);
      probe_replied = 1;
    }
    if (test_vt_contains(&terminal_screen, "streamed") &&
        test_vt_contains(&terminal_screen, "output")) {
      visible = 1;
      break;
    }
  }
  assert(probe_replied);
  assert(visible);
  assert(write(master, ":quit\r", 6u) == 6);
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(close(master) == 0);
  assert(close(log_fds[0]) == 0);
}

static void test_large_exec_burst(void) {
  vectis_smith_cli_render render;
  char *burst;
  size_t length = 1024u * 1024u;
  size_t i;
  int saved_stdout;
  FILE *output;

  memset(&render, 0, sizeof(render));
  render.notify_fd = -1;
  assert(pthread_mutex_init(&render.mutex, NULL) == 0);
  assert(pthread_cond_init(&render.changed, NULL) == 0);
  burst = malloc(length);
  assert(burst != NULL);
  for (i = 0u; i < length; ++i)
    burst[i] = i % 64u == 63u ? '\n' : 'x';
  output = fopen("smith-burst-output.tmp", "w+");
  assert(output != NULL);
  assert(fflush(stdout) == 0);
  saved_stdout = dup(STDOUT_FILENO);
  assert(saved_stdout >= 0);
  assert(dup2(fileno(output), STDOUT_FILENO) == STDOUT_FILENO);
  alarm(10u);
  assert(vectis_smith_cli_render_append(&render, burst, length) == 0);
  vectis_smith_cli_render_finish(&render);
  alarm(0u);
  assert(!vectis_smith_cli_render_failed(&render));
  assert(fflush(stdout) == 0);
  assert(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO);
  assert(close(saved_stdout) == 0);
  rewind(output);
  i = 0u;
  while (fgetc(output) != EOF)
    ++i;
  assert(i >= length);
  assert(fclose(output) == 0);
  assert(unlink("smith-burst-output.tmp") == 0);
  free(burst);
  vectis_smith_cli_render_cleanup(&render);
}

static void test_ui_rearms_pending_output(void) {
  vectis_smith_cli_render render;
  vectis_smith_cli_agent agent;
  vectis_smith_cli_ui ui;
  sl_t *editor;
  int fds[2];
  int saved_stdout;
  int sink;
  struct pollfd ready;

  memset(&render, 0, sizeof(render));
  memset(&agent, 0, sizeof(agent));
  assert(pthread_mutex_init(&render.mutex, NULL) == 0);
  assert(pthread_cond_init(&render.changed, NULL) == 0);
  assert(pthread_mutex_init(&agent.mutex, NULL) == 0);
  assert(pipe(fds) == 0);
  assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
  assert(fcntl(fds[1], F_SETFL, O_NONBLOCK) == 0);
  render.notify_fd = fds[1];
  memset(render.rendered.bytes, 'x', sizeof(render.rendered.bytes));
  render.rendered.count = sizeof(render.rendered.bytes);
  ui.agent = &agent;
  ui.render = &render;
  ui.wake_fd = fds[0];
  editor = sl_create();
  assert(editor != NULL);
  assert(fflush(stdout) == 0);
  saved_stdout = dup(STDOUT_FILENO);
  sink = open("/dev/null", O_WRONLY);
  assert(saved_stdout >= 0 && sink >= 0);
  assert(dup2(sink, STDOUT_FILENO) == STDOUT_FILENO);
  vectis_smith_cli_notify(fds[1]);
  (void)vectis_smith_cli_ui_watch(editor, NULL, &ui);
  assert(render.rendered.count ==
         VECTIS_SMITH_CLI_QUEUE_BYTES - VECTIS_SMITH_CLI_UI_DRAIN_BYTES);
  ready.fd = fds[0];
  ready.events = POLLIN;
  ready.revents = 0;
  assert(poll(&ready, 1u, 1000) == 1);
  (void)vectis_smith_cli_ui_watch(editor, NULL, &ui);
  assert(render.rendered.count == 0u);
  assert(poll(&ready, 1u, 0) == 0);
  assert(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO);
  close(saved_stdout);
  close(sink);
  close(fds[0]);
  close(fds[1]);
  sl_destroy(editor);
  pthread_mutex_destroy(&agent.mutex);
  vectis_smith_cli_render_cleanup(&render);
}

static void test_pending_turn_stays_busy(void) {
  vectis_smith_cli_agent agent;
  vectis_smith_cli_control control;
  vectis_smith_cli_ui ui;
  sl_prompt_queue_delivery_t delivery;
  sl_t *editor = sl_create();
  memset(&agent, 0, sizeof(agent));
  memset(&ui, 0, sizeof(ui));
  agent.notify_fd = -1;
  pthread_mutex_init(&agent.mutex, NULL);
  pthread_cond_init(&agent.changed, NULL);
  ui.agent = &agent;
  assert(editor != NULL);
  assert(sl_set_prompt_queue(editor, 1, 4, 2) == SL_OK);
  assert(sl_set_prompt_queue_profile(
             editor, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) == SL_OK);
  assert(vectis_smith_cli_agent_submit(&agent, VECTIS_SMITH_CLI_CONTROL_NORMAL,
                                       "first") == 0);
  assert(vectis_smith_cli_ui_apply(editor, &ui) == SL_OK);
  assert(sl_get_prompt_queue_delivery(editor, &delivery) == SL_OK);
  assert(delivery == SL_PROMPT_QUEUE_DELIVERY_MANUAL);
  assert(vectis_smith_cli_agent_take_control(&agent, &control));
  free(control.text);
  /* Popping the bridge queue is not worker acknowledgement. */
  assert(vectis_smith_cli_ui_apply(editor, &ui) == SL_OK);
  assert(sl_get_prompt_queue_delivery(editor, &delivery) == SL_OK);
  assert(delivery == SL_PROMPT_QUEUE_DELIVERY_MANUAL);
  agent.state = CAI_AGENT_SAMPLING;
  agent.pending_normal = 0u;
  assert(vectis_smith_cli_ui_apply(editor, &ui) == SL_OK);
  assert(sl_get_prompt_queue_delivery(editor, &delivery) == SL_OK);
  assert(delivery == SL_PROMPT_QUEUE_DELIVERY_MANUAL);
  agent.state = CAI_AGENT_COMPLETED;
  assert(vectis_smith_cli_ui_apply(editor, &ui) == SL_OK);
  assert(sl_get_prompt_queue_delivery(editor, &delivery) == SL_OK);
  assert(delivery == SL_PROMPT_QUEUE_DELIVERY_AUTO);
  assert(vectis_smith_cli_agent_submit(
             &agent, VECTIS_SMITH_CLI_CONTROL_STEERING, "idle promotion") == 0);
  assert(vectis_smith_cli_ui_apply(editor, &ui) == SL_OK);
  assert(sl_get_prompt_queue_delivery(editor, &delivery) == SL_OK);
  assert(delivery == SL_PROMPT_QUEUE_DELIVERY_MANUAL);
  assert(vectis_smith_cli_agent_take_control(&agent, &control));
  assert(control.kind == VECTIS_SMITH_CLI_CONTROL_NORMAL);
  free(control.text);
  sl_destroy(editor);
  pthread_cond_destroy(&agent.changed);
  pthread_mutex_destroy(&agent.mutex);
}

static void test_interactive_response_boundaries(void) {
  vectis_smith_cli_render render;
  vectis_smith_cli_agent agent;
  cai_agent_runtime_event event;
  char output[4096];
  const char *answers[] = {"Hello world.", "Second answer."};
  size_t length;
  int i;
  memset(&render, 0, sizeof(render));
  memset(&agent, 0, sizeof(agent));
  render.interactive = 1;
  render.notify_fd = agent.notify_fd = -1;
  pthread_mutex_init(&render.mutex, NULL);
  pthread_cond_init(&render.changed, NULL);
  pthread_mutex_init(&agent.mutex, NULL);
  pthread_cond_init(&agent.changed, NULL);
  agent.render = &render;
  alarm(10u);
  for (i = 0; i < 2; ++i) {
    memset(&event, 0, sizeof(event));
    event.type = CAI_AGENT_EVENT_TEXT_DELTA;
    event.data = answers[i];
    event.data_length = strlen(answers[i]);
    assert(vectis_smith_cli_agent_event(&agent, &event, NULL) == CAI_OK);
    /* A steering delivery can end one response while the agent keeps its
     * overall turn active. This response boundary must flush libmdf now,
     * rather than waiting for a terminal run-state transition. */
    event.type = CAI_AGENT_EVENT_RESPONSE_COMPLETED;
    assert(vectis_smith_cli_agent_event(&agent, &event, NULL) == CAI_OK);
    pthread_mutex_lock(&render.mutex);
    while (render.document_closed)
      pthread_cond_wait(&render.changed, &render.mutex);
    length = vectis_smith_cli_ring_read(&render.rendered, output,
                                        sizeof(output) - 1u);
    pthread_cond_broadcast(&render.changed);
    pthread_mutex_unlock(&render.mutex);
    output[length] = '\0';
    assert(strstr(output, answers[i]) != NULL);
    assert(strstr(output, "  ") != NULL);
    assert(strchr(output, '\n') != NULL);
  }
  alarm(0u);
  vectis_smith_cli_render_cleanup(&render);
  pthread_mutex_destroy(&agent.mutex);
  pthread_cond_destroy(&agent.changed);
}

static void test_exec_terminal_status(void) {
  vectis_smith_cli_render render;
  int fds[2];
  int saved_stderr;
  char diagnostic[1024];
  ssize_t length;
  memset(&render, 0, sizeof(render));
  test_event_callback = vectis_smith_cli_render_event;
  test_event_context = &render;
  test_emit_text = 0;
  test_emit_failure = 1;
  assert(pipe(fds) == 0);
  saved_stderr = dup(STDERR_FILENO);
  assert(saved_stderr >= 0);
  assert(dup2(fds[1], STDERR_FILENO) == STDERR_FILENO);
  close(fds[1]);
  /* The fake provider reports failure as an event; pump itself succeeds. */
  assert(vectis_smith_cli_exec((vectis_smith *)1, &render) != 0);
  memset(&render, 0, sizeof(render));
  assert(vectis_smith_cli_exec((vectis_smith *)1, &render) != 0);
  test_runtime_state = CAI_AGENT_CANCELLED;
  assert(vectis_smith_cli_exec((vectis_smith *)1, &render) != 0);
  assert(fflush(stderr) == 0);
  assert(dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO);
  close(saved_stderr);
  length = read(fds[0], diagnostic, sizeof(diagnostic) - 1u);
  assert(length > 0);
  diagnostic[length] = '\0';
  close(fds[0]);
  assert(strstr(diagnostic, "offline provider failure") != NULL);
  assert(strstr(diagnostic, "TRAILER") == NULL);
  assert(strstr(diagnostic, "without a diagnostic") != NULL);
  assert(strstr(diagnostic, "Smith turn cancelled") != NULL);
  test_runtime_state = CAI_AGENT_COMPLETED;
  assert(vectis_smith_cli_exec((vectis_smith *)1, &render) == 0);
}

int main(void) {
  test_exec_terminal_status();
  test_pending_turn_stays_busy();
  test_interactive_response_boundaries();
  test_large_exec_burst();
  test_ui_rearms_pending_output();
  test_render_multiple_responses();
  test_plain_delta_reaches_renderer_before_boundary();
  test_agent_control_bridge_is_fifo();
  test_softline_queued_turns_profile();
  test_cli_diagnostic_endpoint_redaction();
  test_cli_verbosity_arguments();
  test_interactive_queue_and_promote();
  test_interactive_live_output_stays_visible();
  return 0;
}
