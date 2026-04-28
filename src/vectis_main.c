#include "vectis_cli.h"

#ifdef __linux__
#define PID0_IMPLEMENTATION 1
#include <libpid0-0.3.0.h>
#endif

int main(int argc, char **argv) {
#ifdef __linux__
  return pid0_run(vectis_cli_main, argc, argv);
#else
  return vectis_cli_main(argc, argv);
#endif
}
