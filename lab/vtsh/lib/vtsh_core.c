#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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

// background jobs (&)

typedef struct {
  pid_t pid;
  int job_id;
  char* cmdline;
} VtshJob;

static VtshJob* g_jobs = NULL;
static size_t g_jobs_len = 0;
static size_t g_jobs_cap = 0;
static int g_next_job_id = 1;

static int vtsh_bg_child_main(void* arg) {
  char* line = (char*)arg;
  (void)vtsh_execute_line(line);
  free(line);
  return 0;
}

static int vtsh_add_job(pid_t pid, char* cmdline) {
  if (g_jobs_len == g_jobs_cap) {
    size_t new_cap = (g_jobs_cap == 0) ? 4U : (g_jobs_cap * 2U);
    VtshJob* tmp = realloc(g_jobs, new_cap * sizeof(*tmp));
    if (!tmp) {
      perror("realloc");
      _exit(1);
    }
    g_jobs = tmp;
    g_jobs_cap = new_cap;
  }
  int job_id = g_next_job_id++;
  g_jobs[g_jobs_len].pid = pid;
  g_jobs[g_jobs_len].job_id = job_id;
  g_jobs[g_jobs_len].cmdline = cmdline;
  g_jobs_len++;
  return job_id;
}

static void vtsh_start_background_job(char* cmdline) {
  pid_t pid = vtsh_spawn_fn(vtsh_bg_child_main, cmdline);
  if (pid < 0) {
    perror("vtsh_spawn_fn");
    free(cmdline);
    return;
  }

  int job_id = vtsh_add_job(pid, cmdline);

  if (printf("[%d] %d\n", job_id, (int)pid) < 0) {
    perror("printf");
  }
  if (fflush(stdout) != 0) {
    perror("fflush");
  }
}

static void vtsh_check_background_jobs(void) {
  size_t iter = 0;
  while (iter < g_jobs_len) {
    int status = 0;
    pid_t pid = waitpid(g_jobs[iter].pid, &status, WNOHANG);
    if (pid == 0) {
      ++iter;
      continue;
    }
    if (pid < 0) {
      free(g_jobs[iter].cmdline);
      g_jobs[iter] = g_jobs[g_jobs_len - 1];
      g_jobs_len--;
      continue;
    }

    VtshJob job = g_jobs[iter];
    free(job.cmdline);
    g_jobs[iter] = g_jobs[g_jobs_len - 1];
    g_jobs_len--;

    const char* status_word = "Done";
    if (WIFSIGNALED(status)) {
      status_word = "Terminated";
    }

    if (printf("[%d] %s\t(pid=%d)\n", job.job_id, status_word, (int)pid) < 0) {
      perror("printf");
    }
    if (fflush(stdout) != 0) {
      perror("fflush");
    }
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

static int vtsh_run_segment_foreground(
    char* seg, size_t part_idx, size_t parts_n, char** parts, int* last_status
) {
  if (vtsh_handle_pipeline_segment(seg, last_status)) {
    return 0;
  }

  VtshCmd cmd = vtsh_parse_cmd_with_redirs(seg);
  bool has_redir =
      (cmd.in_path   != NULL) ||
      (cmd.out_path  != NULL) ||
      (cmd.err_path  != NULL) ||
      cmd.err_to_out ||
      cmd.out_to_err;

  if (!has_redir) {
    int ret_code =
        vtsh_handle_simple_cmd(&cmd, part_idx, parts_n, parts, last_status);
    if (ret_code < 0) {
      return -1;
    }
    return 0;
  }

  vtsh_handle_redir_cmd(&cmd, last_status);
  return 0;
}

// отличать & от 2>&1 например
static bool vtsh_is_redir_ampersand(const char* seg, const char* amp_pos) {
  if (!seg || !amp_pos) {
    return false;
  }
  if (amp_pos == seg) {
    return false;
  }

  const char* prev = amp_pos - 1;
  if (*prev != '>') {
    return false;
  }

  const char* next = amp_pos + 1;
  if (*next < '0' || *next > '9') {
    return false;
  }

  return true;
}

int vtsh_execute_line(const char* line) {
  if (!line) {
    return 0;
  }

  vtsh_check_background_jobs();

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

    const char* ptr = seg;
    const char* job_start = seg;
    int quotes = 0;
    bool has_bg_amp = false;

    // ищем & вне кавычек и обрабатываем cmd1 & cmd2 & cmd3
    while (*ptr) {
      if (*ptr == '\\' && ptr[1] != '\0') {
        ptr += 2;
        continue;
      }
      if (quotes == 0 && (*ptr == '\'' || *ptr == '"')) {
        quotes = (int)(unsigned char)*ptr;
        ++ptr;
        continue;
      }
      if (quotes != 0 && *ptr == (char)quotes) {
        quotes = 0;
        ++ptr;
        continue;
      }

      if (quotes == 0 && *ptr == '&') {
        if (vtsh_is_redir_ampersand(seg, ptr)) {
          ++ptr;
          continue;
        }

        has_bg_amp = true;

        const char* job_str = job_start;
        size_t len = (size_t)(ptr - job_start);

        // trim слева и справа
        while (len > 0 && (*job_str == ' ' || *job_str == '\t')) {
          ++job_str;
          --len;
        }
        while (len > 0 && (job_str[len - 1] == ' ' || job_str[len - 1] == '\t')
        ) {
          --len;
        }

        if (len > 0) {
          char* job_line = strndup(job_str, len);
          if (!job_line) {
            perror("strndup");
            _exit(1);
          }
          vtsh_start_background_job(job_line);
        }

        ++ptr;
        job_start = ptr;
        continue;
      }

      ++ptr;
    }

    if (has_bg_amp) {
      // tail after &
      const char* tail = job_start;
      while (*tail == ' ' || *tail == '\t') {
        ++tail;
      }

      if (*tail != '\0') {
        char* tail_copy = strdup(tail);
        if (!tail_copy) {
          perror("strdup");
          _exit(1);
        }
        int ret_code = vtsh_run_segment_foreground(
            tail_copy, i, parts_n, parts, &last_status
        );
        free(tail_copy);
        if (ret_code < 0) {
          free(seg0);
          free(parts);
          return -1;
        }
      }

      free(seg0);
      continue;
    }

    // no &
    int ret_code =
        vtsh_run_segment_foreground(seg, i, parts_n, parts, &last_status);
    free(seg0);
    if (ret_code < 0) {
      free(parts);
      return -1;
    }
  }
  free(parts);

  vtsh_check_background_jobs();

  return 0;
}