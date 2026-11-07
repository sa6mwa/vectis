#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <libmdf.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static void *page_worker(void *context) {
  mdf_options options;
  mdf_pager_options pager;
  pthread_attr_t attributes;
  size_t stack;
  mdf_status status;
  assert(pthread_getattr_np(pthread_self(), &attributes) == 0);
  assert(pthread_attr_getstacksize(&attributes, &stack) == 0);
  assert(pthread_attr_destroy(&attributes) == 0);
  printf("pager worker stack: %lu bytes; pthread_create attributes: NULL\n",
         (unsigned long)stack);
  fflush(stdout);
  mdf_options_init(&options);
  memset(&pager, 0, sizeof(pager));
  pager.cache_budget = 65536u;
  status = mdf_pager_file_with_options((const char *)context, &options,
                                       MDF_PAGER_FORMAT_MARKDOWN, NULL, &pager);
  assert(status == MDF_OK);
  return NULL;
}

int main(int argc, char **argv) {
  struct winsize size;
  struct pollfd ready;
  struct termios restored;
  char transcript[8192], chunk[2048];
  size_t used, keep;
  ssize_t length;
  pid_t child;
  pthread_t worker;
  int master, status, sent, finished, i;
  FILE *file;

  assert(argc >= 2 && argc <= 5);
  if (argc >= 4) {
    file = fopen(argv[3], "wb");
    assert(file != NULL);
    for (i = 0; i < 60000; ++i)
      assert(fputs("bounded pager input with searchable passage\n", file) >= 0);
    assert(fclose(file) == 0);
  }
  memset(&size, 0, sizeof(size));
  size.ws_row = 24;
  size.ws_col = 80;
  child = forkpty(&master, NULL, NULL, &size);
  assert(child >= 0);
  if (child == 0) {
    assert(setenv("TERM", "xterm-256color", 1) == 0);
    if (argc == 3)
      assert(signal(SIGINT, SIG_IGN) != SIG_ERR);
    if (argc == 5) {
      assert(pthread_create(&worker, NULL, page_worker, argv[3]) == 0);
      assert(pthread_join(worker, NULL) == 0);
      _exit(0);
    }
    if (argc < 4)
      execl(argv[1], argv[1], "--action", "docs", "--pager", (char *)NULL);
    else
      execl(argv[1], argv[1], argv[2], argv[3], (char *)NULL);
    _exit(127);
  }
  used = 0u;
  sent = finished = 0;
  memset(transcript, 0, sizeof(transcript));
  ready.fd = master;
  ready.events = POLLIN;
  /* Readiness, not a delay, controls each interaction. CTest owns cancellation.
   */
  for (;;) {
    ready.revents = 0;
    assert(poll(&ready, 1u, 30000) > 0);
    length = read(master, chunk, sizeof(chunk));
    if (length <= 0) {
      assert(length == 0 || errno == EIO);
      break;
    }
    keep = used;
    if (keep + (size_t)length >= sizeof(transcript)) {
      keep = sizeof(transcript) / 2u;
      memmove(transcript, transcript + used - keep, keep);
    }
    memcpy(transcript + keep, chunk, (size_t)length);
    used = keep + (size_t)length;
    transcript[used] = '\0';
    if (strstr(transcript, "\033[?1049l") != NULL)
      finished = 1;
    if (!sent && strstr(transcript, argc < 4 ? "Vectis documentation"
                                             : "bounded pager input") != NULL) {
      assert(write(master, argc == 3 ? "\003" : "q", 1u) == 1);
      sent = 1;
    }
  }
  assert(waitpid(child, &status, 0) == child);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != (argc == 3 ? 130 : 0))
    fprintf(stderr, "pager child status %d: %s\n", status, transcript);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == (argc == 3 ? 130 : 0) &&
         sent && finished);
  assert(tcgetattr(master, &restored) == 0);
  assert((restored.c_lflag & (ICANON | ECHO)) == (ICANON | ECHO));
  if (argc == 4) {
    assert(strstr(transcript, "MDF pager cancellation, callback cleanup and "
                              "bounded cache passed") != NULL);
  }
  if (argc >= 4)
    assert(unlink(argv[3]) == 0);
  assert(close(master) == 0);
  puts("interactive pager terminal cleanup passed");
  return 0;
}
