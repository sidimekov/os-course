#pragma once

#include <stdint.h>
#include <sys/types.h>

int vtpc_open(const char* path, int mode, int access);
int vtpc_close(int fd);
ssize_t vtpc_read(int fd, void* buf, size_t count);
ssize_t vtpc_write(int fd, const void* buf, size_t count);
off_t vtpc_lseek(int fd, off_t offset, int whence);
int vtpc_fsync(int fd);

typedef struct {
  uint64_t cache_hit;
  uint64_t cache_miss;
  uint64_t pread_pages;
  uint64_t pwrite_pages;
  uint64_t flush_pages; // сколько грязных страниц сброшено
  uint64_t evict_pages; // сколько раз вытесняли страницу из кэша, когда кэш был полон
  uint64_t skip_load_full_overwrite; // сколько раз при записи не делался pread старой страницы, а перезаписывалась вся страница (оптимизация)
} vtpc_stats;

void vtpc_stats_reset(void);
int vtpc_stats_get(vtpc_stats* out);
