// Функции связанные с VtshCmd, редиректами и пайпами
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
  if (fprintf(stderr, "I/O error\n") < 0) {
    perror("fprintf");
  }
  if (fflush(stderr) != 0) {
    perror("fflush");
  }
  _exit(1);
}

// Для теста syntax error
static VtshCmd vtsh_syntax_error_finish(
    VtshCmd cmd, char** argv, char** clean, int clean_i
) {
  if (printf("Syntax error\n") < 0) {
    perror("printf");
  }
  if (fflush(stdout) != 0) {
    perror("fflush");
  }

  // освободить argv из vtsh_parse_argv
  if (argv) {
    for (char** ptr = argv; *ptr; ++ptr) {
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
  free(cmd.err_path);

  cmd.argv = NULL;
  cmd.in_path = NULL;
  cmd.out_path = NULL;
  cmd.err_path = NULL;
  cmd.append = false;
  cmd.err_append = false;
  cmd.err_to_out = false;
  cmd.out_to_err = false;
  cmd.err_before_out = false;
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

  const char* next = argv[*idx + 1];
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

// plain как номер дескриптора
// если успешно - return true и значение в *out_fd, plain_len обнуляется
static bool vtsh_plain_take_fd(char* plain, size_t* plain_len, int* out_fd) {
  if (plain == NULL || plain_len == NULL || out_fd == NULL) {
    return false;
  }
  if (*plain_len == 0) {
    return false;
  }

  for (size_t i = 0; i < *plain_len; ++i) {
    if (!isdigit((unsigned char)plain[i])) {
      return false;
    }
  }

  char buf[32];
  if (*plain_len >= sizeof(buf)) {
    return false;
  }
  memcpy(buf, plain, *plain_len);
  buf[*plain_len] = '\0';

  errno = 0;
  char* endptr = NULL;
  long val = strtol(buf, &endptr, VTSH_STRTO_BASE);
  if (errno != 0 || endptr == buf || *endptr != '\0' || val < 0 ||
      val > INT_MAX) {
    return false;
  }

  *out_fd = (int)val;
  *plain_len = 0;
  return true;
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
        // попытка вытащить номер дескриптора перед оператором (например "2>")
        int fd_hint = -1;
        if (!vtsh_plain_take_fd(plain, &plain_len, &fd_hint)) {
          // не номер fd
          vtsh_plain_flush_to_clean(&plain, &plain_len, &clean, &clean_i);
        }

        bool is_out = (chr == '>');
        bool append = false;
        ++ptr;
        if (is_out && ptr < tok_len && tok[ptr] == '>') {
          append = true;
          ++ptr;
        }

        // проверка на 2>&1 и 1>&2 без пробелов
        if (ptr < tok_len && tok[ptr] == '&') {
          ++ptr;

          // номер целевого дескриптора
          size_t fd_start = ptr;
          while (ptr < tok_len && tok[ptr] >= '0' && tok[ptr] <= '9') {
            ++ptr;
          }
          size_t fd_len = ptr - fd_start;
          if (!is_out || fd_len == 0 || append) {
            cmd = vtsh_syntax_error_finish(cmd, argv, clean, clean_i);
            return cmd;
          }

          char buf[32];
          if (fd_len >= sizeof(buf)) {
            cmd = vtsh_syntax_error_finish(cmd, argv, clean, clean_i);
            return cmd;
          }
          memcpy(buf, tok + fd_start, fd_len);
          buf[fd_len] = '\0';

          errno = 0;
          char* endptr = NULL;
          long dest_fd = strtol(buf, &endptr, VTSH_STRTO_BASE);
          if (errno != 0 || endptr == buf || *endptr != '\0' || dest_fd < 0 ||
              dest_fd > INT_MAX) {
            cmd = vtsh_syntax_error_finish(cmd, argv, clean, clean_i);
            return cmd;
          }

          int src_fd = -1;
          if (fd_hint >= 0) {
            src_fd = fd_hint;
          } else {
            src_fd = is_out ? STDOUT_FILENO : STDIN_FILENO;
          }

          // 2>&1 и 1>&2
          if (src_fd == STDERR_FILENO && dest_fd == STDOUT_FILENO) {
            if (cmd.err_path != NULL || cmd.err_to_out || cmd.err_append) {
              cmd = vtsh_syntax_error_finish(cmd, argv, clean, clean_i);
              return cmd;
            }
            cmd.err_to_out = true;
          } else if (src_fd == STDOUT_FILENO && dest_fd == STDERR_FILENO) {
            if (cmd.out_path != NULL || cmd.out_to_err || cmd.append) {
              cmd = vtsh_syntax_error_finish(cmd, argv, clean, clean_i);
              return cmd;
            }
            cmd.out_to_err = true;
          } else {
            cmd = vtsh_syntax_error_finish(cmd, argv, clean, clean_i);
            return cmd;
          }

          continue;  // след символ токена
        }

        // redir в файл
        char* fname = vtsh_take_fname(tok, tok_len, &ptr, argv, &i, argc);
        if (fname == NULL) {
          cmd = vtsh_syntax_error_finish(cmd, argv, clean, clean_i);
          return cmd;
        }

        if (is_out) {
          int target_fd = (fd_hint >= 0) ? fd_hint : STDOUT_FILENO;

          if (target_fd == STDOUT_FILENO) {
            if (cmd.out_path != NULL) {
              free(fname);
              cmd = vtsh_syntax_error_finish(cmd, argv, clean, clean_i);
              return cmd;
            }
            cmd.out_path = fname;
            cmd.append = append;
          } else if (target_fd == STDERR_FILENO) {
            if (cmd.err_path != NULL) {
              free(fname);
              cmd = vtsh_syntax_error_finish(cmd, argv, clean, clean_i);
              return cmd;
            }
            cmd.err_path = fname;
            cmd.err_append = append;
          } else {
            free(fname);
            cmd = vtsh_syntax_error_finish(cmd, argv, clean, clean_i);
            return cmd;
          }
        } else {
          // input redir: <file or 0<file
          int target_fd = (fd_hint >= 0) ? fd_hint : STDIN_FILENO;
          if (target_fd != STDIN_FILENO) {
            free(fname);
            cmd = vtsh_syntax_error_finish(cmd, argv, clean, clean_i);
            return cmd;
          }
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

  for (char** ptr = argv; *ptr; ++ptr) {
    free(*ptr);
  }
  free(argv);

  cmd.argv = clean;

  // stdout раньше по умолчанию
  cmd.err_before_out = false;

  // есть и stderr-файл и stdout-файл - смотрим какой редирект был первым
  if (cmd.err_path && cmd.out_path) {
    const char* ptr = seg;
    while (*ptr != '\0') {
      if (ptr[0] == '2' && ptr[1] == '>') {
        cmd.err_before_out = true;
        break;
      }
      if (ptr[0] == '>') {
        cmd.err_before_out = false;
        break;
      }
      ++ptr;
    }
  }

  return cmd;
}

static void vtsh_apply_one_file_redir(
    const char* path, bool append, int dest_fd
) {
  uint flags = O_WRONLY | O_CREAT;
  if (append) {
    flags |= O_APPEND;
  } else {
    flags |= O_TRUNC;
  }
  int fdes = open(path, (int)flags, VTSH_REDIRS_CHMOD_OPEN);
  if (fdes < 0) {
    vtsh_io_error_and_exit();
  }
  if (dup2(fdes, dest_fd) < 0) {
    close(fdes);
    vtsh_io_error_and_exit();
  }
  close(fdes);
}

static void vtsh_apply_redirs(const VtshCmd* cmd) {
  if (cmd == NULL) {
    return;
  }

  // 2>&1 / 1>&2
  if (cmd->err_to_out) {
    if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
      vtsh_io_error_and_exit();
    }
  }
  if (cmd->out_to_err) {
    if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
      vtsh_io_error_and_exit();
    }
  }

  if (cmd->err_before_out) {
    // 2>.. потом >..
    vtsh_apply_one_file_redir(cmd->err_path, cmd->err_append, STDERR_FILENO);
    vtsh_apply_one_file_redir(cmd->out_path, cmd->append, STDOUT_FILENO);
  } else {
    if (cmd->out_path) {
      vtsh_apply_one_file_redir(cmd->out_path, cmd->append, STDOUT_FILENO);
    }
    if (cmd->err_path) {
      vtsh_apply_one_file_redir(cmd->err_path, cmd->err_append, STDERR_FILENO);
    }
  }

  // stdin
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
  free(cmd->err_path);
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
// что такое pipe()
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

  // const size_t stack_size = 1U << 20U;  // 1 mb
  // void** stacks = calloc(pipe_n, sizeof(void*));
  // if (!stacks) {
  //   perror("calloc");
  //   vtsh_close_all_pipes(pipes, pipe_n);
  //   free(pipes);
  //   vtsh_cmds_free(cmds, pipe_n);
  //   free(pids);
  //   return VTSH_EXEC_ERROR;
  // }

  // clone3 для создания дочерних процессов
  for (size_t i = 0; i < pipe_n; ++i) {
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

    pid_t pid = vtsh_spawn_fn(vtsh_pipeline_clone_entry, pipeline);
    if (pid < 0) {
      perror("clone3");
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

  pid_t pid = vtsh_spawn_fn(vtsh_single_redir_child, cmd);
  if (pid < 0) {
    perror("clone3");
    return VTSH_EXEC_ERROR;
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid");
    return VTSH_EXEC_ERROR;
  }

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