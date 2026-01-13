#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef enum { MODE_READ, MODE_WRITE } rw_mode_t;
typedef enum { SEL_SEQ, SEL_RAND } sel_mode_t;
typedef enum { CACHE_NONE, CACHE_OS, CACHE_VTPC } cache_mode_t;

typedef struct {
  rw_mode_t rw;
  size_t block_size;
  size_t block_count;
  const char* file;
  off_t range_start;
  off_t range_end;
  sel_mode_t sel;
  cache_mode_t cache;
  size_t repeat;
  bool vtpc_stats;
} config_t;

bool io_config_parse(int argc, char** argv, config_t* cfg);
void io_config_usage(const char* prog);
