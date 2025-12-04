#ifndef VTSH_INTERNAL_H
#define VTSH_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

enum {
  VTSH_TOK_INIT_CAP = 16,
  VTSH_ARGV_INIT_CAP = 8,
  VTSH_PARTS_INIT_CAP = 4,
  VTSH_GROWTH_FACTOR = 2,
  VTSH_EXEC_ERROR = 127,
  VTSH_SIGNAL_EXIT_BASE = 128,
  VTSH_REDIRS_CHMOD_OPEN = 0666,
  VTSH_STRTO_BASE = 10
};

static const double VTSH_NSEC_PER_SEC = 1e9;

#define VTSH_EXIT_CODE 0xEE00

typedef struct {
  char** argv;
  char* in_path;
  char* out_path;
  bool append;

  char* err_path;
  bool err_append;      // 2>>file
  bool err_to_out;      // 2>&1
  bool out_to_err;      // 1>&2
  bool err_before_out;  // stderr redit встретился до stdout redir
} VtshCmd;

typedef struct {
  size_t start_index;
  size_t total_count;
} VtshSpan;

typedef struct {
  VtshCmd* cmd;
  int (*pipes)[2];
  size_t pipe_n;
  size_t idx;
} VtshCloneArgs;

// parse
int vtsh_parse_argv(const char* str, char*** out_argv);
char** vtsh_split_by_and(const char* line, size_t* count);
char** vtsh_split_by_pipe(const char* line, size_t* count);
char* vtsh_lstrip(char* str);
void vtsh_rstrip_inplace(char* str);
void vtsh_free_strv(char** strv, size_t n);

// exec
double vtsh_timespec_diff_sec(struct timespec time0, struct timespec time1);
int vtsh_run_one(char** argv, int argc, double* elapsed_sec, bool* is_time);
void vtsh_print_time(double elapsed);

// redir/pipeline
VtshCmd vtsh_parse_cmd_with_redirs(const char* seg);
void vtsh_cmd_free(VtshCmd* cmd);
void vtsh_cmds_free(VtshCmd* cmds, size_t n);
int vtsh_run_pipeline(char** pipe_parts, size_t pipe_n);
int vtsh_run_single_with_redirs(VtshCmd* cmd, bool t_flag, double* elapsed_sec);

pid_t vtsh_spawn_fn(int (*func)(void*), void* arg);

#endif  // VTSH_INTERNAL_H
