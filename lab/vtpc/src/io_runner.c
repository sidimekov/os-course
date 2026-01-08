#define _GNU_SOURCE
#include "io_runner.h"
#include <inttypes.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "io_backend.h"
#include "io_utils.h"
#include "vtpc.h"

#define ALIGNMENT 4096
#define PATTERN_VALUE 0xab

// один проход нагрузки
static int run_pass(
    const config_t* c, const io_backend_ops_t* ops, int fd, void* buf
) {
  uint32_t rng = 1;
  off_t off = 0;

  for (size_t i = 0; i < c->block_count; ++i) {
    if (c->sel == SEL_RAND) {
      off = (off_t)(lcg_next(&rng) % c->block_count) * (off_t)c->block_size;
    }

    ops->lseek_fn(fd, off, SEEK_SET);

    ssize_t rc = (c->rw == MODE_READ) ? ops->read_fn(fd, buf, c->block_size)
                                      : ops->write_fn(fd, buf, c->block_size);

    if (rc <= 0) {
      return -1;
    }

    if (c->sel == SEL_SEQ) {
      off += c->block_size;
    }
  }
  return 0;
}

// запуск нагрузки
int io_runner_run(const config_t* c) {
  const int use_vtpc = (c->cache == CACHE_VTPC);
  const int use_direct = (c->cache == CACHE_NONE || c->cache == CACHE_VTPC);

  const io_backend_ops_t* ops = io_backend_get(use_vtpc);

  int flags = (c->rw == MODE_READ) ? O_RDONLY : (O_CREAT | O_WRONLY);
  if (use_direct) {
    flags |= O_DIRECT;
  }

  int fd = ops->open_fn(c->file, flags, 0666);
  if (fd < 0) {
    return -1;
  }

  void* buf = NULL;
  if (posix_memalign(&buf, ALIGNMENT, c->block_size) != 0) {
    return -1;
  }
  if (c->rw == MODE_WRITE) {
    memset(buf, PATTERN_VALUE, c->block_size);
  }

  for (size_t pass = 1; pass <= c->repeat; ++pass) {
    if (use_vtpc && c->vtpc_stats) {
      vtpc_stats_reset();
    }

    if (run_pass(c, ops, fd, buf) != 0) {
      break;
    }

    if (use_vtpc && c->vtpc_stats) {
      vtpc_stats s;
      if (vtpc_stats_get(&s) == 0) {
        printf(
            "vtpc pass %zu hit %" PRIu64 " miss %" PRIu64 "\n",
            pass,
            s.cache_hit,
            s.cache_miss
        );
      }
    }
  }

  if (c->rw == MODE_WRITE) {
    ops->fsync_fn(fd);
  }
  ops->close_fn(fd);
  free(buf);
  return 0;
}
