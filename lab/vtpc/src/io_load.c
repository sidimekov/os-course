#define _GNU_SOURCE

#include <stdlib.h>

#include "io_config.h"
#include "io_runner.h"

int main(int argc, char** argv) {
  config_t cfg;
  if (!io_config_parse(argc, argv, &cfg)) {
    io_config_usage(argv[0]);
    return EXIT_FAILURE;
  }

  return io_runner_run(&cfg);
}
