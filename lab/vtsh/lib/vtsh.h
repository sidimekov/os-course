#pragma once
#include <stdbool.h>

/* Execute one line
 * Returns -1 if user input is exit
 * else 0
 */
int vtsh_execute_line(const char* line);

/* "vtsh>" print */
void vtsh_print_prompt(void);
