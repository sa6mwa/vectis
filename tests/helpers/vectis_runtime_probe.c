#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  (void)argv;
  if (argc == 1) {
    execl("/proc/self/exe", "runtime-probe", "child", (char *)0);
    return 1;
  }
  puts("self-exec OK");
  return 0;
}
