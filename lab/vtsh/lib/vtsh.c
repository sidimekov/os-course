#define _GNU_SOURCE
#include "vtsh.h"

#include <errno.h>
#include <fcntl.h>
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
  VTSH_SIGNAL_EXIT_BASE = 128,
  VTSH_REDIRS_CHMOD_OPEN = 0666
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

  const char* dir = (argv[1] != NULL) ? argv[1] : getenv("HOME");
  int ret_code = (dir != NULL) ? chdir(dir) : -1;
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

  const size_t stack_size = 1U << 20U;  // 1mb
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

// Проверка, что в командной строке встретились &&
static inline bool vtsh_is_and_and(const char* ptr, int quotes) {
  return quotes == 0 && ptr[0] == '&' && ptr[1] == '&';
}

// обновление флага quotes
static inline void vtsh_update_quotes(int ch_ptr, int* quotes) {
  if (*quotes == 0 && (ch_ptr == '\'' || ch_ptr == '\"')) {
    *quotes = ch_ptr;
  } else if (*quotes && ch_ptr == *quotes) {
    *quotes = 0;
  }
}

static inline void vtsh_check_capacity(
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
  vtsh_check_capacity(parts, cap, *count);
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
  vtsh_check_capacity(parts, cap, *count);
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

// split_by_pipe helpers

// Проверка, что в командной строке встретился '|'
static inline bool vtsh_is_pipe(const char* ptr, int quotes) {
  return quotes == 0 && *ptr == '|';
}

// сплит по '|', как '&&'
static char** split_by_pipe(const char* line, size_t* count) {
  size_t cap = VTSH_PARTS_INIT_CAP;
  size_t str_n = 0;

  char** parts = malloc(cap * sizeof(*parts));
  if (!parts) {
    perror("malloc");
    _exit(1);
  }
  const char* ptr = line;
  const char* seg_start = line;
  int quotes = 0;
  while (*ptr) {
    if (vtsh_is_pipe(ptr, quotes)) {
      size_t len = (size_t)(ptr - seg_start);
      vtsh_append_range(&parts, &cap, &str_n, seg_start, len);
      ++ptr;
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

// команда с перенаправлениями
typedef struct {
  char** argv;
  char* in_path;
  char* out_path;
  bool append;
} VtshCmd;

// аргументы для clone в одной структуре
typedef struct {
  VtshCmd* cmd;
  int (*pipes)[2];
  size_t pipe_n;
  size_t idx;
} VtshCloneArgs;

static VtshCmd vtsh_parse_cmd_with_redirs(const char* seg) {
  VtshCmd cmd = {0};
  char** argv = NULL;
  int argc = parse_argv(seg, &argv);

  // clean args without redirs
  char** clean = malloc(((size_t)argc + 1U) * sizeof(char*));
  if (!clean) {
    perror("malloc");
    _exit(1);
  }
  int clean_i = 0;

  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], ">") == 0 || strcmp(argv[i], ">>") == 0) {
      bool app = (argv[i][1] == '>');
      if (i + 1 >= argc) {
        if (fprintf(stderr, "redirect: missing filename\n") < 0) {
          perror("fprintf");
        }
        _exit(1);
      }
      cmd.out_path = strdup(argv[i + 1]);
      cmd.append = app;
      i++;
      continue;
    }

    if (strcmp(argv[i], "<") == 0) {
      if (i + 1 >= argc) {
        if (fprintf(stderr, "redirect: missing filename\n") < 0) {
          perror("fprintf");
        }
        _exit(1);
      }
      cmd.in_path = strdup(argv[i + 1]);
      i++;
      continue;
    }
    clean[clean_i++] = argv[i];
  }
  clean[clean_i] = NULL;

  free(argv);
  cmd.argv = clean;
  return cmd;
}

static void vtsh_apply_redirs(const VtshCmd* cmd) {
  if (cmd->in_path) {
    int fdes = open(cmd->in_path, O_RDONLY);
    if (fdes < 0) {
      perror("open <");
      _exit(1);
    }
    if (dup2(fdes, STDIN_FILENO) < 0) {
      perror("dup2 <");
      _exit(1);
    }
    close(fdes);
  }
  if (cmd->out_path) {
    uint flags = O_WRONLY | O_CREAT;
    if (cmd->append) {
      flags |= O_APPEND;
    } else {
      flags |= O_TRUNC;
    }
    int fdes = open(cmd->out_path, (int)flags, VTSH_REDIRS_CHMOD_OPEN);
    if (fdes < 0) {
      perror("open >");
      _exit(1);
    }
    if (dup2(fdes, STDOUT_FILENO) < 0) {
      perror("dup2 >");
      _exit(1);
    }
    close(fdes);
  }
}

// vtsh run pipeline helpers

static void vtsh_cmd_free(VtshCmd* cmd) {
  if (!cmd) {
    return;
  }
  if (cmd->argv) {
    for (char** ptr = cmd->argv; *ptr; ptr++) {
      free(*ptr);
    }
    free(cmd->argv);
  }
  free(cmd->in_path);
  free(cmd->out_path);
}
static void vtsh_cmds_free(VtshCmd* cmds, size_t n) {
  if (!cmds) {
    return;
  }
  for (size_t i = 0; i < n; i++) {
    vtsh_cmd_free(&cmds[i]);
  }
  free(cmds);
}

// Собрать массив команд из частей pipe
static VtshCmd* vtsh_build_cmds_from_pipe_parts(
    char** pipe_parts, size_t pipe_n
) {
  VtshCmd* cmds = calloc(pipe_n, sizeof(*cmds));
  if (!cmds) {
    perror("calloc");
    return NULL;
  }
  for (size_t i = 0; i < pipe_n; ++i) {
    cmds[i] = vtsh_parse_cmd_with_redirs(pipe_parts[i]);
  }
  return cmds;
}

// создать N - 1 pipes для N команд
static int (*vtsh_create_pipes(size_t pipe_n))[2] {
  if (pipe_n <= 1) {
    return NULL;
  }
  int(*pipes)[2] = malloc((pipe_n - 1) * sizeof(int[2]));
  if (!pipes) {
    perror("malloc");
    return NULL;
  }
  for (size_t i = 0; i + 1 < pipe_n; ++i) {
    if (pipe(pipes[i]) < 0) {
      perror("pipe");

      // закрыть уже открытые pipes
      for (size_t k = 0; k < i; ++k) {
        close(pipes[k][0]);
        close(pipes[k][1]);
      }
      free(pipes);
      return NULL;
    }
  }
  return pipes;
}

static void vtsh_close_all_pipes(int (*pipes)[2], size_t pipe_n) {
  if (!pipes) {
    return;
  }
  for (size_t k = 0; k + 1 < pipe_n; ++k) {
    close(pipes[k][0]);
    close(pipes[k][1]);
  }
}

// запустить один процесс в пайплайне из дочернего
static void vtsh_exec_pipeline_child(
    VtshCmd* cmd, int (*pipes)[2], size_t pipe_n, size_t idx
) {
  // connect pipe input/output if needed
  if (pipe_n > 1) {
    if (idx > 0) {
      if (dup2(pipes[idx - 1][0], STDIN_FILENO) < 0) {
        perror("dup2 pipe in");
        _exit(1);
      }
    }
    if (idx + 1 < pipe_n) {
      if (dup2(pipes[idx][1], STDOUT_FILENO) < 0) {
        perror("dup2 pipe out");
        _exit(1);
      }
    }
    // close all pipe file descriptors after dup2
    for (size_t k = 0; k + 1 < pipe_n; ++k) {
      close(pipes[k][0]);
      close(pipes[k][1]);
    }
  }

  vtsh_apply_redirs(cmd);

  if (!cmd->argv || !cmd->argv[0]) {
    _exit(0);
  }
  execvp(cmd->argv[0], cmd->argv);
  if (errno == ENOENT) {
    dprintf(STDOUT_FILENO, "Command not found\n");
  }
  _exit(VTSH_EXEC_ERROR);
}

// ожидание всех дочерних процессов пайплайна, вернуть статус последнего
static int vtsh_wait_pipeline(pid_t* pids, size_t pipe_n) {
  int last_status = 0;
  for (size_t i = 0; i < pipe_n; ++i) {
    if (pids[i] <= 0) {
      continue;
    }
    int status = 0;
    if (waitpid(pids[i], &status, 0) >= 0) {
      if (i == pipe_n - 1) {
        if (WIFEXITED(status)) {
          last_status = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
          last_status = VTSH_SIGNAL_EXIT_BASE + WTERMSIG(status);
        } else {
          last_status = VTSH_EXEC_ERROR;
        }
      }
    }
  }
  return last_status;
}

//
static int vtsh_pipeline_clone_entry(void* arg) {
  VtshCloneArgs* pipeline = (VtshCloneArgs*)arg;
  vtsh_exec_pipeline_child(
      pipeline->cmd, pipeline->pipes, pipeline->pipe_n, pipeline->idx
  );
  _exit(VTSH_EXEC_ERROR);
}

// запуск пайплайна cmd1 | cmd2 | ... | cmdN
static int vtsh_run_pipeline(char** pipe_parts, size_t pipe_n) {
  // парсинг в VtshCmd
  VtshCmd* cmds = vtsh_build_cmds_from_pipe_parts(pipe_parts, pipe_n);
  if (!cmds) {
    return VTSH_EXEC_ERROR;
  }

  // создать пары
  int(*pipes)[2] = vtsh_create_pipes(pipe_n);
  if (pipe_n > 1 && !pipes) {
    vtsh_cmds_free(cmds, pipe_n);
    return VTSH_EXEC_ERROR;
  }

  // аллокация pids
  pid_t* pids = malloc(pipe_n * sizeof(pid_t));
  if (!pids) {
    perror("malloc");
    vtsh_close_all_pipes(pipes, pipe_n);
    free(pipes);
    vtsh_cmds_free(cmds, pipe_n);
    return VTSH_EXEC_ERROR;
  }

  const size_t stack_size = 1U << 20U;  // 1 mb
  void** stacks = calloc(pipe_n, sizeof(void*));
  if (!stacks) {
    perror("calloc");
    vtsh_close_all_pipes(pipes, pipe_n);
    free(pipes);
    vtsh_cmds_free(cmds, pipe_n);
    free(pids);
    return VTSH_EXEC_ERROR;
  }

  // clone для создания дочерних процессов
  for (size_t i = 0; i < pipe_n; ++i) {
    void* stack = malloc(stack_size);
    if (!stack) {
      perror("malloc stack");
      pids[i] = -1;
      continue;
    }
    stacks[i] = stack;
    void* stack_top = (char*)stack + stack_size;

    VtshCloneArgs* pipeline = malloc(sizeof(*pipeline));
    if (!pipeline) {
      perror("malloc clone args");
      pids[i] = -1;
      continue;
    }
    pipeline->cmd = &cmds[i];
    pipeline->pipes = pipes;
    pipeline->pipe_n = pipe_n;
    pipeline->idx = i;

    // lowercase comment: create child with SIGCHLD so waitpid works
    pid_t pid = clone(vtsh_pipeline_clone_entry, stack_top, SIGCHLD, pipeline);
    if (pid < 0) {
      perror("clone");
      pids[i] = -1;
      free(pipeline);
      continue;
    }
    pids[i] = pid;
  }

  vtsh_close_all_pipes(pipes, pipe_n);
  free(pipes);

  int ret_code = vtsh_wait_pipeline(pids, pipe_n);
  free(pids);

  vtsh_cmds_free(cmds, pipe_n);

  return ret_code;
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

// child entry for single command with redirs
static int vtsh_single_redir_child(void *arg) {
  VtshCmd *cmd = (VtshCmd*)arg;
  vtsh_apply_redirs(cmd);
  if (!cmd->argv || !cmd->argv[0]) {
    _exit(0);
  }
  execvp(cmd->argv[0], cmd->argv);
  if (errno == ENOENT) {
    dprintf(STDOUT_FILENO, "Command not found\n");
  } else {
    perror("execvp");
  }
  _exit(VTSH_EXEC_ERROR);
}

// run one external cmd that has redirs
static int vtsh_run_single_with_redirs(
    VtshCmd* cmd, bool t_flag, double* elapsed_sec
) {
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

  pid_t pid = clone(vtsh_single_redir_child, stack_top, SIGCHLD, cmd);
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
  return VTSH_EXEC_ERROR;
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

    size_t pipe_n = 0;
    char** pipe_parts = split_by_pipe(seg, &pipe_n);
    if (pipe_n > 1) {
      int ret_code = vtsh_run_pipeline(pipe_parts, pipe_n);
      for (size_t i = 0; i < pipe_n; i++) {
        free(pipe_parts[i]);
      }
      free(pipe_parts);
      last_status = ret_code;
      free(seg0);
      continue;
    }

    for (size_t i = 0; i < pipe_n; i++) {
      free(pipe_parts[i]);
    }
    free(pipe_parts);

    VtshCmd cmd = vtsh_parse_cmd_with_redirs(seg);
    bool has_redir = (cmd.in_path != NULL) || (cmd.out_path != NULL);

    if (!has_redir) {
      // run_one

      // count argc
      int argc = 0;
      if (cmd.argv) {
        for (char** ptr = cmd.argv; *ptr; ++ptr) {
          argc++;
        }
      }

      double elapsed = 0.0;
      bool is_time = false;
      int ret_code = run_one(cmd.argv, argc, &elapsed, &is_time);

      if (ret_code == VTSH_EXIT_CODE) {
        vtsh_cmd_free(&cmd);
        vtsh_free_parts_span(
            parts, (VtshSpan){.start_index = i, .total_count = parts_n}
        );
        return -1;
      }

      if (is_time) {
        vtsh_print_time(elapsed);
      }

      last_status = ret_code;
      vtsh_cmd_free(&cmd);
      free(seg0);
      continue;
    }

    bool t_flag = false;
    // проверка -t/--time на конце cmd.argv
    int argc = 0;
    if (cmd.argv) {
      for (char** ptr = cmd.argv; *ptr; ++ptr) {
        argc++;
      }
    }
    if (argc > 0) {
      const char* last = cmd.argv[argc - 1];
      if (last && (strcmp(last, "-t") == 0 || strcmp(last, "--time") == 0)) {
        free(cmd.argv[argc - 1]);
        cmd.argv[argc - 1] = NULL;
        t_flag = true;
        argc--;
      }
    }

    double elapsed = 0.0;
    int ret_code = vtsh_run_single_with_redirs(&cmd, t_flag, &elapsed);
    if (t_flag) {
      vtsh_print_time(elapsed);
    }
    last_status = ret_code;
    vtsh_cmd_free(&cmd);
    free(seg0);
  }
  free(parts);
  return 0;
}