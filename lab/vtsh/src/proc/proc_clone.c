#define _GNU_SOURCE
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum { PROC_EXEC_ERROR = 127, PROC_SIGNAL_EXIT_BASE = 128 };

static int child_main(void* arg) {
  char** child_argv = (char**)arg;
  execvp(child_argv[0], child_argv);

  perror("execvp");
  _exit(PROC_EXEC_ERROR);
}

static void usage(const char* prog) {
  (void)fprintf(
      stderr,
      "Usage:\n"
      "  %s -- <prog> [args...]\n\n"
      "Examples:\n"
      "  %s -- /bin/echo hello world\n"
      "  %s -- ls -la\n",
      prog,
      prog,
      prog
  );
}

int main(int argc, char** argv) {
  int iter = 1;
  while (iter < argc && strcmp(argv[iter], "--") != 0) {
    ++iter;
  }
  if (iter >= argc - 1) {
    usage(argv[0]);
    return 2;
  }
  char** child_argv = &argv[iter + 1];

  const size_t stack_size = 1U << 20U;

  void* stack = malloc(stack_size);
  if (!stack) {
    perror("malloc");
    return 1;
  }

  // bottom of stack for clone
  void* stack_top = (char*)stack + stack_size;

  pid_t pid = clone(child_main, stack_top, SIGCHLD, child_argv);
  if (pid < 0) {
    perror("clone");
    free(stack);
    return 1;
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid");
    free(stack);
    return 1;
  }

  free(stack);

  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return PROC_SIGNAL_EXIT_BASE + WTERMSIG(status);
  }
  return PROC_EXEC_ERROR;
}
