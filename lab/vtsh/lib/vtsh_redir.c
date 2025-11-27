// Функции связанные с VtshCmd, редиректами и пайпами
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "vtsh_internal.h"

// для тестов вывод IO Error
static void vtsh_io_error_and_exit(void) {
  if (printf("I/O error\n") < 0) {
    perror("printf");
  }
  if (fflush(stdout) != 0) {
    perror("fflush");
  }
  _exit(1);
}

// Для теста syntax error
static VtshCmd vtsh_syntax_error_finish(
    VtshCmd cmd, char **argv, char **clean, int clean_i
) {
  if (printf("Syntax error\n") < 0) {
    perror("printf");
  }
  if (fflush(stdout) != 0) {
    perror("fflush");
  }

  // освободить argv из vtsh_parse_argv
  if (argv) {
    for (char **ptr = argv; *ptr; ++ptr) {
      free(*ptr);
    }
    free(argv);
  }

  // освободить уже собранный clean-argv
  if (clean) {
    for (int i = 0; i < clean_i; ++i) {
      free(clean[i]);
    }
    free(clean);
  }

  free(cmd.in_path);
  free(cmd.out_path);
  cmd.argv = NULL;
  cmd.in_path = NULL;
  cmd.out_path = NULL;
  cmd.append = false;
  return cmd;
}

// добавить один символ в буфер plain (часть разделённая < >)
static void vtsh_plain_push(char** plain, size_t* len, size_t* cap, char chr) {
  if (*len + 1 >= *cap) {
    size_t new_cap = *cap ? (*cap * 2) : VTSH_TOK_INIT_CAP;
    char* tmp = realloc(*plain, new_cap);
    if (!tmp) {
      perror("realloc");
      _exit(1);
    }
    *plain = tmp;
    *cap = new_cap;
  }
  (*plain)[(*len)++] = chr;
}

// если буфер plain не пустой -> закинуть его в clean argv
static void vtsh_plain_flush_to_clean(
    char** plain, size_t* len, char*** clean, int* clean_i
) {
  if (*len == 0) {
    return;
  }
  char* str = malloc(*len + 1);
  if (!str) {
    perror("malloc");
    _exit(1);
  }
  memcpy(str, *plain, *len);
  str[*len] = '\0';
  (*clean)[(*clean_i)++] = str;
  *len = 0;
}

// get filename from current token tail or from next argv
static char* vtsh_take_fname(
    const char* tok,
    size_t tok_len,
    size_t* ptr,
    char** argv,
    int* idx,
    int argc
) {
  size_t start = *ptr;
  while (*ptr < tok_len && tok[*ptr] != '<' && tok[*ptr] != '>') {
    ++(*ptr);
  }
  size_t len = *ptr - start;
  if (len > 0) {
    char* name = strndup(tok + start, len);
    if (!name) {
      perror("strndup");
      _exit(1);
    }
    return name;
  }

  // имени в текущем токене нет поэтому берём следующий аргумент
  if (*idx + 1 >= argc) {
    return NULL;
  }

  const char *next = argv[*idx + 1];
  if (next[0] == '<' || next[0] == '>') {
    // < >hello не пропускать как в одном тесте
    return NULL;
  }

  (*idx)++;
  char* name = strdup(argv[*idx]);
  if (!name) {
    perror("strdup");
    _exit(1);
  }
  return name;
}

VtshCmd vtsh_parse_cmd_with_redirs(const char* seg) {
  VtshCmd cmd = {0};
  char** argv = NULL;

  // парсинг по пробелам и др.
  int argc = vtsh_parse_argv(seg, &argv);

  // clean args without redirs - args for execvp
  char** clean = malloc(((size_t)argc + 1U) * sizeof(char*));
  if (!clean) {
    perror("malloc");
    _exit(1);
  }
  int clean_i = 0;

  for (int i = 0; i < argc; i++) {
    const char* tok = argv[i];
    size_t tok_len = strlen(tok);

    // буфер для части токена, разделённой < > |
    char* plain = NULL;
    size_t plain_len = 0;
    size_t plain_cap = 0;

    size_t ptr = 0;
    while (ptr < tok_len) {
      char chr = tok[ptr];
      if (chr == '>' || chr == '<') {
        // plain буфер в clean argv
        vtsh_plain_flush_to_clean(&plain, &plain_len, &clean, &clean_i);

        bool is_out = (chr == '>');
        bool append = false;
        ++ptr;
        if (is_out && ptr < tok_len && tok[ptr] == '>') {
          append = true;
          ++ptr;
        }

        // собрать имя файла из этого же токена до следующего < или >
        char* fname = vtsh_take_fname(tok, tok_len, &ptr, argv, &i, argc);

        // нет имени файла => синтаксическая ошибка редиректа
        // это для теста
        if (fname == NULL) {
          cmd = vtsh_syntax_error_finish(cmd, argv, clean, clean_i);
          return cmd;
        }


        // запись редирект
        if (is_out) {
          if (cmd.out_path != NULL) {
            free(fname);
            cmd = vtsh_syntax_error_finish(cmd, argv, clean, clean_i);
            return cmd;
          }
          cmd.out_path = fname;
          cmd.append = append;
        } else {
          if (cmd.in_path != NULL) {
            free(fname);
            cmd = vtsh_syntax_error_finish(cmd, argv, clean, clean_i);
            return cmd;
          }
          cmd.in_path = fname;
        }
      } else {
        vtsh_plain_push(&plain, &plain_len, &plain_cap, chr);
        ++ptr;
      }
    }
    if (plain_len > 0) {
      char* str = malloc(plain_len + 1);
      if (!str) {
        perror("malloc");
        _exit(1);
      }
      memcpy(str, plain, plain_len);
      str[plain_len] = '\0';
      clean[clean_i++] = str;
    }
    free(plain);
  }
  clean[clean_i] = NULL;

  // освобождается только массив argv, строки в нём уже не нужны
  for (char **ptr = argv; *ptr; ++ptr) {
    free(*ptr);
  }
  free(argv);

  cmd.argv = clean;
  return cmd;
}

static void vtsh_apply_redirs(const VtshCmd* cmd) {
  if (cmd->in_path) {
    int fdes = open(cmd->in_path, O_RDONLY);
    if (fdes < 0) {
      vtsh_io_error_and_exit();
    }
    if (dup2(fdes, STDIN_FILENO) < 0) {
      close(fdes);
      vtsh_io_error_and_exit();
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
      vtsh_io_error_and_exit();
    }
    if (dup2(fdes, STDOUT_FILENO) < 0) {
      close(fdes);
      vtsh_io_error_and_exit();
    }
    close(fdes);
  }
}

// vtsh run pipeline helpers

void vtsh_cmd_free(VtshCmd* cmd) {
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
void vtsh_cmds_free(VtshCmd* cmds, size_t n) {
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
int vtsh_run_pipeline(char** pipe_parts, size_t pipe_n) {
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

// child entry for single command with redirs
static int vtsh_single_redir_child(void* arg) {
  VtshCmd* cmd = (VtshCmd*)arg;
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
int vtsh_run_single_with_redirs(
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
    *elapsed_sec = t_flag ? vtsh_timespec_diff_sec(time0, time1) : 0.0;
  }

  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return VTSH_SIGNAL_EXIT_BASE + WTERMSIG(status);
  }
  return VTSH_EXEC_ERROR;
}