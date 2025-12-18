// внутренние структуры / типы

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum { VTPC_MAX_FILES = 64 };
enum { VTPC_PAGE_SIZE = 4096 };
enum { VTPC_CACHE_PAGES = 64 };
enum { VTPC_ALIGNMENT = 4096 };

typedef struct {
  int used;
  int os_fd;
  off_t pos;
  int mode;
  int access;
} VtpcFile;

typedef struct {
  int used;
  int vfd;
  off_t page_index;
  void* data;
  size_t valid_bytes;
  int dirty;
  uint64_t last_use;
} CachePage;

extern VtpcFile g_files[VTPC_MAX_FILES];
extern CachePage g_cache[VTPC_CACHE_PAGES];
extern uint64_t g_use_tick;

int vtpc_fd_alloc(void);
VtpcFile* vtpc_fd_get(int vfd);
void vtpc_fd_free(int vfd);

int vtpc_io_open_direct(const char* path, int mode, int access);
off_t vtpc_io_get_size(int os_fd);  // через fstat
ssize_t vtpc_io_pread_page(int os_fd, void* page_buf, off_t page_index);

CachePage* vtpc_cache_find(int vfd, off_t page_index);
CachePage* vtpc_cache_get_or_load(
    int vfd, int os_fd, off_t page_index
);
void vtpc_cache_forget_file(int vfd);
