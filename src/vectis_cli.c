#include "vectis_cli.h"

#include <stdio.h>
#include <string.h>

static void vectis_cli_usage(FILE *stream) {
  fputs("usage: vectis [--version] [--help] [script.lua]\n", stream);
}

int vectis_cli_main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--help") == 0) {
    vectis_cli_usage(stdout);
    return 0;
  }
  if (argc > 1 && strcmp(argv[1], "--version") == 0) {
    puts("vectis 0.0.0");
    return 0;
  }

  (void)argv;
  fputs("vectis: Lua runner is not implemented yet\n", stderr);
  return 64;
}
