#define _GNU_SOURCE
#include "vtsh.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double timespec_diff_sec(struct timespec a, struct timespec b) {
  time_t ds = b.tv_sec - a.tv_sec;
  long dns = b.tv_nsec - a.tv_nsec;
  return (double)ds + (double)dns / 1e9;
}

void vtsh_print_prompt(void) {
  fprintf(stdout, "vtsh> ");
  fflush(stdout);
}

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void push_char(char** tok, size_t* tlen, size_t* tcap, char c) {
  if (*tlen + 1 >= *tcap) {
    size_t new_cap = (*tcap ? *tcap * 2 : 16);
    char* p = realloc(*tok, new_cap);
    if (!p) {
      perror("realloc");
      exit(1);
    }
    *tok = p;
    *tcap = new_cap;
  }
  (*tok)[(*tlen)++] = c;
}

static void argv_append(
    char*** argv, size_t* argc, size_t* cap, const char* tok, size_t tlen
) {
  if (tlen == 0)
    return;
  char* s2 = malloc(tlen + 1);
  if (!s2) {
    perror("malloc");
    exit(1);
  }
  memcpy(s2, tok, tlen);
  s2[tlen] = '\0';

  if (*argc + 2 > *cap) {
    size_t new_cap = (*cap ? *cap * 2 : 8);
    char** v = realloc(*argv, new_cap * sizeof(char*));
    if (!v) {
      perror("realloc");
      exit(1);
    }
    *argv = v;
    *cap = new_cap;
  }
  (*argv)[(*argc)++] = s2;
  (*argv)[*argc] = NULL;
}

static int parse_argv(const char* s, char*** out_argv) {
  char** argv = NULL;
  size_t argc = 0, cap = 0;

  char* tok = NULL;
  size_t tlen = 0, tcap = 0;

  const char* p = s;
  int quotes = 0;

  while (*p == ' ' || *p == '\t')
    ++p;

  for (; *p; ++p) {
    if (*p == '\\' && p[1]) {
      push_char(&tok, &tlen, &tcap, *++p);
      continue;
    }
    if (!quotes && (*p == '\'' || *p == '\"')) {
      quotes = *p;
      continue;
    }
    if (quotes && *p == quotes) {
      quotes = 0;
      continue;
    }
    if (!quotes && (*p == ' ' || *p == '\t')) {
      argv_append(&argv, &argc, &cap, tok, tlen);
      tlen = 0;

      while (p[1] == ' ' || p[1] == '\t')
        ++p;

      continue;
    }

    push_char(&tok, &tlen, &tcap, *p);
  }

  argv_append(&argv, &argc, &cap, tok, tlen);

  free(tok);

  *out_argv = argv;
  return (int)argc;
}

static int run_one(char** argv, int argc, double* elapsed_sec, bool* is_time) {
  if (!argv || !argv[0]) {
    if (is_time)
      *is_time = false;
    return 0;
  }

  bool t_flag = false;
  if (argc > 0) {
    const char* last = argv[argc - 1];
    if (last && (strcmp(argv[argc - 1], "-t") == 0 ||
                 strcmp(argv[argc - 1], "--time") == 0)) {
      t_flag = true;
      free(argv[argc - 1]);
      argv[--argc] = NULL;
    }
  }
  if (is_time)
    *is_time = t_flag;

  if (argc == 0) {
    *elapsed_sec = 0.0;
    return 0;
  }

  if (strcmp(argv[0], "exit") == 0) {
    return 0xEE00;
  }
  if (strcmp(argv[0], "cd") == 0) {
    struct timespec t0 = {0}, t1 = {0};
    if (t_flag)
      clock_gettime(CLOCK_MONOTONIC, &t0);

    const char* dir = argv[1] ? argv[1] : getenv("HOME");
    int rc = dir ? chdir(dir) : -1;
    if (rc != 0)
      perror("cd");
    if (t_flag) {
      clock_gettime(CLOCK_MONOTONIC, &t1);
      *elapsed_sec = timespec_diff_sec(t0, t1);
    } else {
      *elapsed_sec = 0.0;
    }
    return (rc == 0) ? 0 : 1;
  }

  struct timespec t0 = {0}, t1 = {0};

  if (t_flag)
    clock_gettime(CLOCK_MONOTONIC, &t0);

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 127;
  }
  if (pid == 0) {
    execvp(argv[0], argv);
    if (errno == ENOENT) {
      dprintf(STDOUT_FILENO, "Command not found\n");
    }
    // perror("execvp");
    _exit(127);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid");
    return 127;
  }

  if (t_flag) {
    clock_gettime(CLOCK_MONOTONIC, &t1);
    *elapsed_sec = timespec_diff_sec(t0, t1);
  }

  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return 127;
}

static char** split_by_and(const char* line, size_t* count) {
  size_t cap = 4, n = 0;
  char** parts = malloc(cap * sizeof(char*));
  if (!parts) {
    perror("malloc");
    exit(1);
  }

  const char *p = line, *seg_start = line;
  int quotes = 0;
  while (*p) {
    if (quotes == 0 && p[0] == '&' && p[1] == '&') {
      size_t len = (size_t)(p - seg_start);
      char* s = strndup(seg_start, len);
      if (!s) {
        perror("strndup");
        exit(1);
      }
      if (n >= cap) {
        cap *= 2;
        parts = realloc(parts, cap * sizeof(char*));
        if (!parts) {
          perror("realloc");
          exit(1);
        }
      }
      parts[n++] = s;
      p += 2;
      seg_start = p;
      continue;
    }
    if (quotes == 0 && (*p == '\'' || *p == '\"'))
      quotes = *p;
    else if (quotes && *p == quotes)
      quotes = 0;
    else if (*p == '\\' && p[1])
      ++p;
    ++p;
  }
  if (seg_start) {
    char* s = strdup(seg_start);
    if (!s) {
      perror("strdup");
      exit(1);
    }
    if (n >= cap) {
      cap *= 2;
      parts = realloc(parts, cap * sizeof(char*));
      if (!parts) {
        perror("realloc");
        exit(1);
      }
    }
    parts[n++] = s;
  }
  *count = n;
  return parts;
}

int vtsh_execute_line(const char* line) {
  if (!line)
    return 0;

  size_t parts_n = 0;
  char** parts = split_by_and(line, &parts_n);

  int last_status = 0;
  for (size_t i = 0; i < parts_n; ++i) {
    char* seg = parts[i];

    while (*seg == ' ' || *seg == '\t')
      ++seg;
    size_t L = strlen(seg);
    while (L > 0 &&
           (seg[L - 1] == ' ' || seg[L - 1] == '\t' || seg[L - 1] == '\n'))
      seg[--L] = '\0';
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
    int rc = run_one(argv, argc, &elapsed, &is_time);
    if (rc == 0xEE00) {  // exit
      for (char** arg = argv; arg && *arg; ++arg)
        free(*arg);
      free(argv);
      for (size_t k = i; k < parts_n; ++k)
        free(parts[k]);
      free(parts);
      return -1;
    }

    if (is_time) {
      printf("[time] %.6f s\n", elapsed);
      fflush(stdout);
    }

    last_status = rc;

    for (char** arg = argv; arg && *arg; ++arg)
      free(*arg);
    free(argv);
    free(parts[i]);
  }
  free(parts);
  return 0;
}
