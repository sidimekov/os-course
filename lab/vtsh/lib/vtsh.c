#define _GNU_SOURCE
#include "vtsh.h"

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
  VTSH_TOK_INIT_CAP = 16,
  VTSH_ARGV_INIT_CAP = 8,
  VTSH_PARTS_INIT_CAP = 4,
  VTSH_GROWTH_FACTOR = 2,
  VTSH_EXEC_ERROR = 127,
  VTSH_SIGNAL_EXIT_BASE = 128
};
static const double VTSH_NSEC_PER_SEC = 1e9;
#define VTSH_EXIT_CODE 0xEE00

static double timespec_diff_sec(struct timespec time0, struct timespec time1) {
  time_t diff_sec = time1.tv_sec - time0.tv_sec;
  long diff_nsec = time1.tv_nsec - time0.tv_nsec;
  return (double)diff_sec + (double)diff_nsec / VTSH_NSEC_PER_SEC;
}

void vtsh_print_prompt(void) {
  if (fprintf(stdout, "vtsh> ") < 0) {
    perror("fprintf");
  }
  if (fflush(stdout) != 0) {
    perror("fflush");
  }
}

static void push_char(char** tok, size_t* tlen, size_t* tcap, char chr) {
  if (*tlen + 1 >= *tcap) {
    size_t new_cap = (*tcap ? *tcap * VTSH_GROWTH_FACTOR : VTSH_TOK_INIT_CAP);
    char* ptr = realloc(*tok, new_cap);
    if (!ptr) {
      perror("realloc");
      // _exit(1);
      _exit(1);
    }
    *tok = ptr;
    *tcap = new_cap;
  }
  (*tok)[(*tlen)++] = chr;
}

static void argv_append(
    char*** argv, size_t* argc, size_t* cap, const char* tok, size_t tlen
) {
  if (tlen == 0) {
    return;
  }
  char* seg2 = malloc(tlen + 1);
  if (!seg2) {
    perror("malloc");
    _exit(1);
  }
  memcpy(seg2, tok, tlen);
  seg2[tlen] = '\0';

  if (*argc + 2 > *cap) {
    size_t new_cap = (*cap ? *cap * VTSH_GROWTH_FACTOR : VTSH_ARGV_INIT_CAP);
    char** vec = realloc(*argv, new_cap * sizeof(char*));
    if (!vec) {
      perror("realloc");
      _exit(1);
    }
    *argv = vec;
    *cap = new_cap;
  }
  (*argv)[(*argc)++] = seg2;
  (*argv)[*argc] = NULL;
}

static int parse_argv(const char* str, char*** out_argv) {
  char** argv = NULL;
  size_t argc = 0;
  size_t cap = 0;

  char* tok = NULL;
  size_t tlen = 0;
  size_t tcap = 0;

  const char* ptr = str;
  int quotes = 0;

  while (*ptr == ' ' || *ptr == '\t') {
    ++ptr;
  }

  for (; *ptr; ++ptr) {
    int ch_ptr = (int)(unsigned char)*ptr;

    if (*ptr == '\\' && ptr[1]) {
      push_char(&tok, &tlen, &tcap, *++ptr);
      continue;
    }
    if (!quotes && (*ptr == '\'' || *ptr == '\"')) {
      quotes = ch_ptr;
      continue;
    }
    if (quotes && *ptr == quotes) {
      quotes = 0;
      continue;
    }
    if (!quotes && (*ptr == ' ' || *ptr == '\t')) {
      argv_append(&argv, &argc, &cap, tok, tlen);
      tlen = 0;

      while (ptr[1] == ' ' || ptr[1] == '\t') {
        ++ptr;
      }

      continue;
    }

    push_char(&tok, &tlen, &tcap, *ptr);
  }

  argv_append(&argv, &argc, &cap, tok, tlen);

  free(tok);

  *out_argv = argv;
  return (int)argc;
}

// run_one helpers

static bool strip_time_flag(char** argv, int* argc) {
  if (!argv || !*argc) {
    return false;
  }

  const char* last = argv[*argc - 1];
  if (last && (strcmp(last, "-t") == 0 || strcmp(last, "--time") == 0)) {
    free(argv[*argc - 1]);
    argv[--(*argc)] = NULL;
    return true;
  }
  return false;
}

static int builtin_cd(char** argv, bool t_flag, double* elapsed_sec) {
  struct timespec time0 = {0};
  struct timespec time1 = {0};
  if (t_flag && clock_gettime(CLOCK_MONOTONIC, &time0) != 0) {
    perror("clock_gettime");
  }

  const char* dir = argv[1] ? argv[1] : secure_getenv("HOME");
  int ret_code = dir ? chdir(dir) : -1;
  if (ret_code != 0) {
    perror("cd");
  }

  if (t_flag && clock_gettime(CLOCK_MONOTONIC, &time1) != 0) {
    perror("clock_gettime");
  }
  if (elapsed_sec) {
    *elapsed_sec = t_flag ? timespec_diff_sec(time0, time1) : 0.0;
  }
  return (ret_code == 0) ? 0 : 1;
}

static int vtsh_child_main(void* arg) {
  char** argv = (char**)arg;
  execvp(argv[0], argv);
  if (errno == ENOENT) {
    dprintf(STDOUT_FILENO, "Command not found\n");
  }
  _exit(VTSH_EXEC_ERROR);
}

static int run_external(char** argv, bool t_flag, double* elapsed_sec) {
  struct timespec time0 = {0};
  struct timespec time1 = {0};
  if (t_flag && clock_gettime(CLOCK_MONOTONIC, &time0) != 0) {
    perror("clock_gettime");
  }

  const size_t stack_size = 1U << 20U;
  void* stack = malloc(stack_size);
  if (!stack) {
    perror("malloc");
    return VTSH_EXEC_ERROR;
  }
  void* stack_top = (char*)stack + stack_size;

  pid_t pid = clone(vtsh_child_main, stack_top, SIGCHLD, argv);
  if (pid < 0) {
    perror("clone");
    free(stack);
    return VTSH_EXEC_ERROR;
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid");
    free(stack);
    return VTSH_EXEC_ERROR;
  }
  free(stack);

  if (t_flag && clock_gettime(CLOCK_MONOTONIC, &time1) != 0) {
    perror("clock_gettime");
  }
  if (elapsed_sec) {
    *elapsed_sec = t_flag ? timespec_diff_sec(time0, time1) : 0.0;
  }

  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return VTSH_SIGNAL_EXIT_BASE + WTERMSIG(status);
  }
  return VTSH_SIGNAL_EXIT_BASE;
}

static int run_one(char** argv, int argc, double* elapsed_sec, bool* is_time) {
  if (!argv || !argv[0]) {
    if (is_time) {
      *is_time = false;
    }
    return 0;
  }

  bool t_flag = strip_time_flag(argv, &argc);
  if (is_time) {
    *is_time = t_flag;
  }

  if (argc == 0) {
    if (elapsed_sec) {
      *elapsed_sec = 0.0;
    }
    return 0;
  }

  if (strcmp(argv[0], "exit") == 0) {
    return VTSH_EXIT_CODE;
  }
  if (strcmp(argv[0], "cd") == 0) {
    return builtin_cd(argv, t_flag, elapsed_sec);
  }

  return run_external(argv, t_flag, elapsed_sec);
}

// split_by_and helpers

static inline bool vtsh_is_and_and(const char* ptr, int quotes) {
  return quotes == 0 && ptr[0] == '&' && ptr[1] == '&';
}

static inline void vtsh_update_quotes(int ch_ptr, int* quotes) {
  if (*quotes == 0 && (ch_ptr == '\'' || ch_ptr == '\"')) {
    *quotes = ch_ptr;
  } else if (*quotes && ch_ptr == *quotes) {
    *quotes = 0;
  }
}

static inline void vtsh_ensure_capacity(
    char*** parts, size_t* cap, size_t need
) {
  if (need >= *cap) {
    size_t new_cap = (*cap ? *cap * VTSH_GROWTH_FACTOR : VTSH_PARTS_INIT_CAP);
    while (need >= new_cap) {
      new_cap *= 2;
    }
    char** tmp = realloc(*parts, new_cap * sizeof(*tmp));
    if (!tmp) {
      perror("realloc");
      _exit(1);
    }
    *parts = tmp;
    *cap = new_cap;
  }
}

static inline void vtsh_append_range(
    char*** parts, size_t* cap, size_t* count, const char* start, size_t len
) {
  char* str = strndup(start, len);
  if (!str) {
    perror("strndup");
    _exit(1);
  }
  vtsh_ensure_capacity(parts, cap, *count);
  (*parts)[(*count)++] = str;
}

static inline void vtsh_append_cstr(
    char*** parts, size_t* cap, size_t* count, const char* str
) {
  char* dup = strdup(str);
  if (!dup) {
    perror("strdup");
    _exit(1);
  }
  vtsh_ensure_capacity(parts, cap, *count);
  (*parts)[(*count)++] = dup;
}

static char** split_by_and(const char* line, size_t* count) {
  size_t cap = VTSH_PARTS_INIT_CAP;
  size_t str_n = 0;
  char** parts = malloc(cap * sizeof(char*));
  if (!parts) {
    perror("malloc");
    _exit(1);
  }

  const char* ptr = line;
  const char* seg_start = line;
  int quotes = 0;
  while (*ptr) {
    if (vtsh_is_and_and(ptr, quotes)) {
      size_t len = (size_t)(ptr - seg_start);
      vtsh_append_range(&parts, &cap, &str_n, seg_start, len);
      ptr += 2;
      seg_start = ptr;
      continue;
    }

    int ch_ptr = (int)(unsigned char)*ptr;
    if (ch_ptr == '\\' && ptr[1]) {
      ++ptr;
    } else {
      vtsh_update_quotes(ch_ptr, &quotes);
    }
    ++ptr;
  }

  vtsh_append_cstr(&parts, &cap, &str_n, seg_start);

  *count = str_n;
  return parts;
}

// vtsh_execute_line helpers

static inline char* vtsh_lstrip(char* str) {
  while (*str == ' ' || *str == '\t') {
    ++str;
  }
  return str;
}

static inline void vtsh_rstrip_inplace(char* str) {
  size_t len = strlen(str);
  while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\t' ||
                     str[len - 1] == '\n')) {
    str[--len] = '\0';
  }
}

static inline void vtsh_free_argv(char** argv) {
  if (!argv) {
    return;
  }
  for (char** arg = argv; *arg; ++arg) {
    free(*arg);
  }
  free(argv);
}

typedef struct {
  size_t start_index;
  size_t total_count;
} VtshSpan;

static inline void vtsh_free_parts_span(char** parts, VtshSpan span) {
  for (size_t k = span.start_index; k < span.total_count; ++k) {
    free(parts[k]);
  }
  free(parts);
}

static inline void vtsh_print_time(double elapsed) {
  if (printf("[time] %.6f s\n", elapsed) < 0) {
    perror("printf");
  }
  if (fflush(stdout) != 0) {
    perror("fflush");
  }
}

int vtsh_execute_line(const char* line) {
  if (!line) {
    return 0;
  }

  size_t parts_n = 0;
  char** parts = split_by_and(line, &parts_n);

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

    char** argv = NULL;
    int argc = parse_argv(seg, &argv);

    double elapsed = 0.0;
    bool is_time = false;
    int ret_code = run_one(argv, argc, &elapsed, &is_time);

    if (ret_code == VTSH_EXIT_CODE) {
      vtsh_free_argv(argv);
      vtsh_free_parts_span(
          parts, (VtshSpan){.start_index = i, .total_count = parts_n}
      );
      return -1;
    }

    if (is_time) {
      vtsh_print_time(elapsed);
    }

    last_status = ret_code;

    vtsh_free_argv(argv);

    free(seg0);
  }
  free(parts);
  return 0;
}
