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
static int test_log_fd = -1;

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
  if (test_emit_text) {
    memset(&event, 0, sizeof(event));
    event.type = CAI_AGENT_EVENT_TEXT_DELTA;
    event.data = "streamed output\n\n";
    event.data_length = strlen(event.data);
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
  usleep(100000u);
  assert(write(master, ":quit\r", 6u) == 6);
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(transcript_length != 0u);
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

int main(void) {
  test_large_exec_burst();
  test_ui_rearms_pending_output();
  test_render_multiple_responses();
  test_agent_control_bridge_is_fifo();
  test_softline_queued_turns_profile();
  test_cli_diagnostic_endpoint_redaction();
  test_cli_verbosity_arguments();
  test_interactive_queue_and_promote();
  return 0;
}
