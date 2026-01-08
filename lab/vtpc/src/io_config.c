#include "io_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// const for magic numbers
#define RW_ARG_LENGTH 5
#define BLOCK_SIZE_ARG_LENGTH 13
#define BLOCK_COUNT_ARG_LENGTH 14
#define FILE_ARG_LENGTH 7
#define RANGE_ARG_LENGTH 8
#define TYPE_ARG_LENGTH 7
#define CACHE_ARG_LENGTH 8
#define REPEAT_ARG_LENGTH 9
#define VTPC_STATS_ARG_LENGTH 13

// parse range start end
static int parse_range(const char* s, off_t* start, off_t* end) {
  // compact parsing, start end non negative, end can be zero
  const char* dash = strchr(s, '-');
  if (!dash)
    return 0;

  errno = 0;
  char* e1 = NULL;
  long long a = strtoll(s, &e1, 10);
  if (errno != 0 || e1 != dash)
    return 0;

  errno = 0;
  char* e2 = NULL;
  long long b = strtoll(dash + 1, &e2, 10);
  if (errno != 0 || e2 == dash + 1 || *e2 != '\0')
    return 0;

  if (a < 0 || b < 0)
    return 0;
  if (b != 0 && b < a)
    return 0;

  *start = (off_t)a;
  *end = (off_t)b;
  return 1;
}

// parse cli args
bool io_config_parse(int argc, char** argv, config_t* c) {
  memset(c, 0, sizeof(*c));
  c->rw = MODE_READ;
  c->sel = SEL_SEQ;
  c->cache = CACHE_OS;
  c->repeat = 1;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];

    if (strncmp(a, "--rw=", RW_ARG_LENGTH) == 0) {
      c->rw = strcmp(a + RW_ARG_LENGTH, "write") == 0 ? MODE_WRITE : MODE_READ;

    } else if (strncmp(a, "--block_size=", BLOCK_SIZE_ARG_LENGTH) == 0) {
      c->block_size = strtoull(a + BLOCK_SIZE_ARG_LENGTH, NULL, 10);

    } else if (strncmp(a, "--block_count=", BLOCK_COUNT_ARG_LENGTH) == 0) {
      c->block_count = strtoull(a + BLOCK_COUNT_ARG_LENGTH, NULL, 10);

    } else if (strncmp(a, "--file=", FILE_ARG_LENGTH) == 0) {
      c->file = a + FILE_ARG_LENGTH;

    } else if (strncmp(a, "--range=", RANGE_ARG_LENGTH) == 0) {
      const char* v = a + RANGE_ARG_LENGTH;
      if (!parse_range(v, &c->range_start, &c->range_end)) {
        return false;
      }

    } else if (strncmp(a, "--type=", TYPE_ARG_LENGTH) == 0) {
      c->sel = strcmp(a + TYPE_ARG_LENGTH, "random") == 0 ? SEL_RAND : SEL_SEQ;

    } else if (strncmp(a, "--cache=", CACHE_ARG_LENGTH) == 0) {
      if (strcmp(a + CACHE_ARG_LENGTH, "none") == 0) {
        c->cache = CACHE_NONE;
      } else if (strcmp(a + CACHE_ARG_LENGTH, "vtpc") == 0) {
        c->cache = CACHE_VTPC;
      } else {
        c->cache = CACHE_OS;
      }

    } else if (strncmp(a, "--repeat=", REPEAT_ARG_LENGTH) == 0) {
      c->repeat = strtoull(a + REPEAT_ARG_LENGTH, NULL, 10);

      if (c->repeat == 0) {
        return false;
      }

    } else if (strncmp(a, "--vtpc_stats=", VTPC_STATS_ARG_LENGTH) == 0) {
      c->vtpc_stats = strcmp(a + VTPC_STATS_ARG_LENGTH, "on") == 0;
    }
  }

  return c->file && c->block_size && c->block_count;
}

void io_config_usage(const char* p) {
  if (fprintf(
          stderr,
          "usage %s --rw=read|write --block_size=N --block_count=N --file=PATH "
          "[--range=START-END] [--type=sequence|random] [--cache=none|os|vtpc] "
          "[--repeat=N] [--vtpc_stats=on|off]\n",
          p
      ) < 0) {
    perror("fprintf error");
  }
}
