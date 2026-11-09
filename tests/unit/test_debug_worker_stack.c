#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct observation {
  size_t expected;
  int nested, guard;
} observation;
static void guard_fault(int number) { _exit(number == SIGSEGV ? 77 : 78); }
static void *observe(void *context) {
  observation *test = (observation *)context, nested;
  pthread_attr_t attributes;
  pthread_t worker;
  size_t size, guard;
  void *base;
  pid_t child;
  int status;
  assert(pthread_getattr_np(pthread_self(), &attributes) == 0);
  assert(pthread_attr_getstack(&attributes, &base, &size) == 0);
  assert(pthread_attr_getguardsize(&attributes, &guard) == 0 && guard > 0u);
  assert(pthread_attr_destroy(&attributes) == 0 && size == test->expected);
  if (test->nested) {
    nested.expected = 131072u;
    nested.nested = nested.guard = 0;
    assert(pthread_create(&worker, NULL, observe, &nested) == 0);
    assert(pthread_join(worker, NULL) == 0);
  }
  if (test->guard) {
    child = fork();
    assert(child >= 0);
    if (child == 0) {
      volatile unsigned char value;
      assert(signal(SIGSEGV, guard_fault) != SIG_ERR);
      value = *((volatile unsigned char *)base - 1);
      (void)value;
      _exit(79);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 77);
  }
  return NULL;
}
int main(int argc, char **argv) {
  pthread_attr_t attributes;
  pthread_t worker;
  observation test;
  pid_t child;
  int i, status;
  test.guard = argc == 2 && strcmp(argv[1], "guard") == 0;
  for (i = 0; i < 3; ++i) {
    test.expected = i == 0 ? 262144u : 131072u;
    test.nested = i == 1;
    assert(pthread_attr_init(&attributes) == 0);
    if (i == 0)
      assert(pthread_attr_setstacksize(&attributes, test.expected) == 0);
    assert(pthread_create(&worker, i == 0 ? &attributes : NULL, observe,
                          &test) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(pthread_attr_destroy(&attributes) == 0);
  }
  if (argc == 1) {
    child = fork();
    assert(child >= 0);
    if (child == 0) {
      execl(argv[0], argv[0], "child", (char *)NULL);
      _exit(1);
    }
    assert(waitpid(child, &status, 0) == child && status == 0);
  }
  puts("128 KiB default/nested/exec workers, explicit sizes and guards passed");
  return 0;
}
