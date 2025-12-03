#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vtsh.h"
#include "vtsh_internal.h"

void vtsh_print_prompt(void) {
  if (fprintf(stdout, "vtsh> ") < 0) {
    perror("fprintf");
  }
  if (fflush(stdout) != 0) {
    perror("fflush");
  }
}

// vtsh_execute_line helpers

static inline void vtsh_free_argv(char** argv) {
  if (!argv) {
    return;
  }
  for (char** arg = argv; *arg; ++arg) {
    free(*arg);
  }
  free(argv);
}

static inline void vtsh_free_parts_span(char** parts, VtshSpan span) {
  for (size_t k = span.start_index; k < span.total_count; ++k) {
    free(parts[k]);
  }
  free(parts);
}

static int vtsh_handle_pipeline_segment(char* seg, int* out_status) {
  size_t pipe_n = 0;
  char** pipe_parts = vtsh_split_by_pipe(seg, &pipe_n);
  if (pipe_n > 1) {
    int ret_code = vtsh_run_pipeline(pipe_parts, pipe_n);
    vtsh_free_strv(pipe_parts, pipe_n);
    *out_status = ret_code;
    return 1;  // success
  }
  // no pipes
  vtsh_free_strv(pipe_parts, pipe_n);
  return 0;
}

static int vtsh_handle_simple_cmd(
    VtshCmd* cmd, size_t part_idx, size_t parts_n, char** parts, int* out_status
) {
  // count argc
  int argc = 0;
  if (cmd->argv) {
    for (char** ptr = cmd->argv; *ptr; ++ptr) {
      argc++;
    }
  }

  double elapsed = 0.0;
  bool is_time = false;
  int ret_code = vtsh_run_one(cmd->argv, argc, &elapsed, &is_time);

  if (ret_code == VTSH_EXIT_CODE) {
    vtsh_cmd_free(cmd);
    vtsh_free_parts_span(
        parts, (VtshSpan){.start_index = part_idx, .total_count = parts_n}
    );
    return -1;
  }

  if (is_time) {
    vtsh_print_time(elapsed);
  }

  *out_status = ret_code;
  vtsh_cmd_free(cmd);
  return 0;
}

static int vtsh_handle_redir_cmd(VtshCmd* cmd, int* out_status) {
  // Если парсер вернул пустую команду (syntax error),
  // то сообщение уже напечатано, статус vtsh должен быть 0
  if (cmd->argv == NULL) {
    if (out_status) {
      *out_status = 0;
    }
    vtsh_cmd_free(cmd);
    return 0;
  }

  bool t_flag = false;
  // проверка -t/--time на конце cmd.argv
  int argc = 0;
  if (cmd->argv) {
    for (char** ptr = cmd->argv; *ptr; ++ptr) {
      argc++;
    }
  }

  if (argc > 0) {
    const char* last = cmd->argv[argc - 1];
    if (last && (strcmp(last, "-t") == 0 || strcmp(last, "--time") == 0)) {
      free(cmd->argv[argc - 1]);
      cmd->argv[argc - 1] = NULL;
      t_flag = true;
      argc--;
    }
  }

  double elapsed = 0.0;
  int ret_code = vtsh_run_single_with_redirs(cmd, t_flag, &elapsed);
  if (t_flag) {
    vtsh_print_time(elapsed);
  }
  *out_status = ret_code;
  vtsh_cmd_free(cmd);
  return 0;
}

// execute on bg without ret code
static int vtsh_bg_child_main(void* arg) {
  char* line = arg;
  (void)vtsh_execute_line(line);
  free(line);
  return 0;
}

int vtsh_execute_line(const char* line) {
  if (!line) {
    return 0;
  }

  // & handle
  const char* end = line + strlen(line);

  while (end > line && isspace((unsigned char)end[-1])) {
    --end;
  }

  bool background = false;
  if (end > line && end[-1] == '&') {
    background = true;
    --end;  // remove &

    while (end > line && isspace((unsigned char)end[-1])) {
      --end;
    }
  }

  if (background) {
    size_t clean_len = (size_t)(end - line);
    char* clean_line = malloc(clean_len + 1);
    if (!clean_line) {
      perror("malloc");
      return 0;
    }

    memcpy(clean_line, line, clean_len);
    clean_line[clean_len] = '\0';

    pid_t pid = vtsh_spawn_fn(vtsh_bg_child_main, clean_line);
    if (pid < 0) {
      perror("vtsh_spawn_fn");
      free(clean_line);
      return 0;
    }

    printf("[bg] %d\n", pid);

    return 0;
  }

  size_t parts_n = 0;
  char** parts = vtsh_split_by_and(line, &parts_n);

  int last_status = 0;
  for (size_t i = 0; i < parts_n; ++i) {
    char* seg0 = parts[i];
    char* seg = vtsh_lstrip(seg0);
    vtsh_rstrip_inplace(seg);

    if (*seg == '\0') {
      free(parts[i]);
      continue;
    }

    if (i > 0 && last_status != 0) {
      free(parts[i]);
      continue;
    }

    if (vtsh_handle_pipeline_segment(seg, &last_status)) {
      free(seg0);
      continue;
    }

    VtshCmd cmd = vtsh_parse_cmd_with_redirs(seg);
    bool has_redir = (cmd.in_path != NULL) || (cmd.out_path != NULL);

    if (!has_redir) {
      // run_one
      int ret_code =
          vtsh_handle_simple_cmd(&cmd, i, parts_n, parts, &last_status);
      free(seg0);
      if (ret_code < 0) {
        return -1;
      }
      continue;
    }

    vtsh_handle_redir_cmd(&cmd, &last_status);
    free(seg0);
  }
  free(parts);
  return 0;
}