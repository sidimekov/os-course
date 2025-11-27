#ifndef VTSH_H
#define VTSH_H

#ifdef __cplusplus
extern "C" {
#endif

/* "vtsh>" print */
void vtsh_print_prompt(void);

/* Execute one line
 * Returns -1 if user input is exit
 * else 0
 */
int vtsh_execute_line(const char *line);

#ifdef __cplusplus
}
#endif

#endif // VTSH_H
