#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vtsh.h"

int main(void) {
  setvbuf(stdin, NULL, _IONBF, 0);

  char* line = NULL;
  size_t cap = 0;

  while (1) {
    vtsh_print_prompt();
    ssize_t n = getline(&line, &cap, stdin);
    if (n < 0)
      break;

    if (vtsh_execute_line(line) < 0)
      break;  // exit
  }
  free(line);
  return 0;
}
