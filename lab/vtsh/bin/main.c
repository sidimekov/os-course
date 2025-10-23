#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "vtsh.h"

int main(void) {
  if (setvbuf(stdin, NULL, _IONBF, 0) != 0) {
    perror("setvbuf(stdin)");
    return EXIT_FAILURE;
  }

  char* line = NULL;
  size_t cap = 0;

  for (;;) {
    vtsh_print_prompt();
    ssize_t n_line = getline(&line, &cap, stdin);
    if (n_line < 0) {
      break;
    }

    if (vtsh_execute_line(line) < 0) {
      break;  // exit
    }
  }
  free(line);
  return EXIT_SUCCESS;
}
