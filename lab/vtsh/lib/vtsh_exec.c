// Функции запуска одной команды и изменение времени

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <linux/sched.h>  // struct clone_args
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>  // SYS_clone3
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "vtsh.h"
#include "vtsh_internal.h"

double vtsh_timespec_diff_sec(struct timespec time0, struct timespec time1) {
  time_t diff_sec = time1.tv_sec - time0.tv_sec;
  long diff_nsec = time1.tv_nsec - time0.tv_nsec;
  return (double)diff_sec + (double)diff_nsec / VTSH_NSEC_PER_SEC;
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
    *elapsed_sec = t_flag ? vtsh_timespec_diff_sec(time0, time1) : 0.0;
  }
  return (ret_code == 0) ? 0 : 1;
}

static void vtsh_expand_env_vars(char** argv, int argc) {
  if (!argv || argc <= 0) {
    return;
  }

  for (int i = 0; i < argc; ++i) {
    char* str = argv[i];
    if (!str || str[0] != '$') {
      continue;
    }

    const char* name = str + 1;
    if (*name == '\0') {
      continue;
    }

    // valid env var name
    bool valid = true;
    for (const unsigned char* ptr = (const unsigned char*)name; *ptr; ++ptr) {
      if (!isalnum(*ptr) && *ptr != '_') {
        valid = false;
        break;
      }
    }
    if (!valid) {
      continue;
    }

    const char* val = getenv(name);

    free(str);

    if (!val) {
      argv[i] = strdup("");
    } else {
      argv[i] = strdup(val);
    }

    if (!argv[i]) {
      perror("strdup");
      _exit(1);
    }
  }
}

static int vtsh_child_main(void* arg) {
  char** argv = (char**)arg;

  execvp(argv[0], argv);
  if (errno == ENOENT) {
    dprintf(STDOUT_FILENO, "Command not found\n");
  }
  _exit(VTSH_EXEC_ERROR);
}

// simple wrapper to run fn(arg) in child created by clone3
pid_t vtsh_spawn_fn(int (*func)(void*), void* arg) {
  pid_t pid = -1;

  struct clone_args args;
  memset(&args, 0, sizeof(args));
  args.exit_signal = SIGCHLD;

  pid = (pid_t)syscall(SYS_clone3, &args, sizeof(args));

  if (pid == -1 && (errno == ENOSYS || errno == EPERM)) {
    pid = fork();
  }

  if (pid < 0) {
    return -1;
  }

  if (pid == 0) {
    int ret_code = func(arg);
    _exit(ret_code);
  }

  return pid;
}

static int run_external(char** argv, bool t_flag, double* elapsed_sec) {
  struct timespec time0 = {0};
  struct timespec time1 = {0};
  if (t_flag && clock_gettime(CLOCK_MONOTONIC, &time0) != 0) {
    perror("clock_gettime");
  }

  pid_t pid = vtsh_spawn_fn(vtsh_child_main, argv);
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
  return VTSH_SIGNAL_EXIT_BASE;
}

int vtsh_run_one(char** argv, int argc, double* elapsed_sec, bool* is_time) {
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

  vtsh_expand_env_vars(argv, argc);

  if (strcmp(argv[0], "exit") == 0) {
    return VTSH_EXIT_CODE;
  }
  if (strcmp(argv[0], "cd") == 0) {
    return builtin_cd(argv, t_flag, elapsed_sec);
  }

  return run_external(argv, t_flag, elapsed_sec);
}

inline void vtsh_print_time(double elapsed) {
  if (printf("[time] %.6f s\n", elapsed) < 0) {
    perror("printf");
  }
  if (fflush(stdout) != 0) {
    perror("fflush");
  }
}