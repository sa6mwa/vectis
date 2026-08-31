#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The CLI bridge is intentionally source-included so this test exercises its
 * private bounded queues and real libmdf streaming renderer without opening a
 * provider client or terminal UI. */
#define vectis_smith_submit vectis_smith_test_submit
#define vectis_smith_submit_steering vectis_smith_test_submit_steering
#define vectis_smith_submit_queued vectis_smith_test_submit_queued
#include "../../src/vectis_smith_cli.c"
#undef vectis_smith_submit
#undef vectis_smith_submit_steering
#undef vectis_smith_submit_queued

static int smith_operation;

vectis_status vectis_smith_test_submit(vectis_smith *smith, const char *text,
                                       vectis_error *error) {
  (void)smith;
  (void)text;
  (void)error;
  smith_operation = 1;
  return VECTIS_OK;
}

vectis_status vectis_smith_test_submit_steering(vectis_smith *smith,
                                                const char *text,
                                                vectis_error *error) {
  (void)smith;
  (void)text;
  (void)error;
  smith_operation = 2;
  return VECTIS_OK;
}

vectis_status vectis_smith_test_submit_queued(vectis_smith *smith,
                                              const char *text,
                                              vectis_error *error) {
  (void)smith;
  (void)text;
  (void)error;
  smith_operation = 3;
  return VECTIS_OK;
}

static void test_render_multiple_responses(void) {
  vectis_smith_cli_render render;
  int pipe_fds[2];
  int saved_stdout;
  char output[4096];
  ssize_t nread;

  memset(&render, 0, sizeof(render));
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

static void test_submit_routes_steering_and_queue(void) {
  vectis_error error;

  vectis_error_clear(&error);
  smith_operation = 0;
  assert(vectis_smith_cli_submit((vectis_smith *)1, "steer", CAI_AGENT_SAMPLING,
                                 SL_PROMPT_SOURCE_DIRECT, &error) == 0);
  assert(smith_operation == 2);
  smith_operation = 0;
  assert(vectis_smith_cli_submit((vectis_smith *)1, "later", CAI_AGENT_SAMPLING,
                                 SL_PROMPT_SOURCE_QUEUED, &error) == 0);
  assert(smith_operation == 3);
  smith_operation = 0;
  assert(vectis_smith_cli_submit((vectis_smith *)1, "next", CAI_AGENT_COMPLETED,
                                 SL_PROMPT_SOURCE_DIRECT, &error) == 0);
  assert(smith_operation == 1);
  smith_operation = 0;
  assert(vectis_smith_cli_submit((vectis_smith *)1, "queued next",
                                 CAI_AGENT_COMPLETED, SL_PROMPT_SOURCE_QUEUED,
                                 &error) == 0);
  assert(smith_operation == 3);
}

int main(void) {
  test_render_multiple_responses();
  test_submit_routes_steering_and_queue();
  return 0;
}
