#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <unistd.h>

static void stack_check(int result) {
  if (result != 0) {
    fprintf(stderr, "Vectis Debug worker-stack setup failed: %d\n", result);
    abort();
  }
}

/* Linked into Debug executables only, including children invoked by tests.
 * Main-thread limits, explicit attributes and installed libraries are
 * untouched. */
__attribute__((constructor(101))) static void default_worker_stack(void) {
  pthread_attr_t attributes;
  struct rlimit before, after;
  long page;
  size_t guard;
  stack_check(getrlimit(RLIMIT_STACK, &before));
  page = sysconf(_SC_PAGESIZE);
  stack_check(page > 0 ? 0 : -1);
  stack_check(pthread_getattr_default_np(&attributes));
  stack_check(pthread_attr_setstacksize(&attributes, 131072u));
  stack_check(pthread_attr_getguardsize(&attributes, &guard));
  if (guard < (size_t)page)
    stack_check(pthread_attr_setguardsize(&attributes, (size_t)page));
  stack_check(pthread_setattr_default_np(&attributes));
  stack_check(pthread_attr_destroy(&attributes));
  stack_check(getrlimit(RLIMIT_STACK, &after));
  stack_check(before.rlim_cur == after.rlim_cur &&
                      before.rlim_max == after.rlim_max
                  ? 0
                  : -1);
}
