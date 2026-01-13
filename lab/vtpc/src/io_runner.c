#define _GNU_SOURCE
#include "io_runner.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "io_backend.h"
#include "io_utils.h"
#include "vtpc.h"

static const size_t ALIGNMENT = 4096;
static const unsigned char WRITE_PATTERN = 0xAB;
static const size_t O_DIRECT_ALIGN = 512;

// один проход нагрузки
static int run_pass(
    const config_t* c, const io_backend_ops_t* ops, int fd, void* buf
) {
  uint32_t rng = 1;
  const off_t start = c->range_start;
  const off_t end = c->range_end;
  off_t off = start;

  // сколько байт доступно для random sel mode
  const off_t span =
      (end != 0) ? (end - start) : (off_t)c->block_count * (off_t)c->block_size;
  off_t blocks = (span > 0) ? (span / (off_t)c->block_size) : 1;
  if (blocks <= 0) {
    blocks = 1;
  }

  for (size_t i = 0; i < c->block_count; ++i) {
    if (c->sel == SEL_RAND) {
      off_t idx = (off_t)(lcg_next(&rng) % (uint32_t)blocks);
      off = start + idx * (off_t)c->block_size;
    }

    if (ops->lseek_fn(fd, off, SEEK_SET) < 0) {
      return -1;
    }

    ssize_t rc = (c->rw == MODE_READ) ? ops->read_fn(fd, buf, c->block_size)
                                      : ops->write_fn(fd, buf, c->block_size);

    if (rc < 0) {
      return -1;
    }

    // short io is treated as error for stable benchmarking
    if ((size_t)rc != c->block_size) {
      return -1;
    }

    if (c->sel == SEL_SEQ) {
      off += c->block_size;

      if (end != 0 && off + (off_t)c->block_size > end) {
        off = start;
      }
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

#ifdef O_DIRECT
  if (use_direct) {
    // o direct expects aligned block size
    if ((c->block_size % O_DIRECT_ALIGN) != 0) {
      return -1;
    }
    flags |= O_DIRECT;
  }
#else
  (void)use_direct;
#endif

  int fd = ops->open_fn(c->file, flags, 0666);
  if (fd < 0) {
    return -1;
  }

  void* buf = NULL;
  int mem_rc = posix_memalign(&buf, ALIGNMENT, c->block_size);
  if (mem_rc != 0 || buf == NULL) {
    // posix memalign returns error code
    errno = mem_rc;
    ops->close_fn(fd);
    return -1;
  }
  if (c->rw == MODE_WRITE) {
    memset(buf, (int)WRITE_PATTERN, c->block_size);
  }

  for (size_t pass = 1; pass <= c->repeat; ++pass) {
    if (use_vtpc && c->vtpc_stats) {
      vtpc_stats_reset();
    }

    if (run_pass(c, ops, fd, buf) != 0) {
      ops->close_fn(fd);
      free(buf);
      return -1;
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
