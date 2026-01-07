#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vtpc_internal.h"

// открывает файл с direct io, для o_wronly подменяет на o_rdwr
int vtpc_io_open_direct(const char* path, int mode, int access) {
  unsigned int open_mode = (unsigned int)mode;

  unsigned int acc = open_mode & (unsigned int)O_ACCMODE;
  if (acc == O_WRONLY) {
    open_mode = (open_mode & ~(unsigned int)O_ACCMODE) | O_RDWR;
  }

  open_mode |= O_DIRECT;

  int os_fd = 0;
  if (open_mode & O_CREAT) {
    os_fd = open(path, (int)open_mode, access);
  } else {
    os_fd = open(path, (int)open_mode);
  }

  return os_fd;
}

// читает размер файла через fstat, используется только при open
off_t vtpc_io_get_size(int os_fd) {
  struct stat st;
  if (fstat(os_fd, &st) != 0) {
    return (off_t)-1;
  }
  return st.st_size;
}

// читает страницу, считает обращение к диску
ssize_t vtpc_io_pread_page(int os_fd, void* page_buf, off_t page_index) {
  g_stats.pread_pages += 1;
  off_t off = page_index * (off_t)VTPC_PAGE_SIZE;
  return pread(os_fd, page_buf, (size_t)VTPC_PAGE_SIZE, off);
}

// пишет страницу, считает обращение к диску
ssize_t vtpc_io_pwrite_page(int os_fd, const void* page_buf, off_t page_index) {
  g_stats.pwrite_pages += 1;
  off_t off = page_index * (off_t)VTPC_PAGE_SIZE;
  return pwrite(os_fd, page_buf, (size_t)VTPC_PAGE_SIZE, off);
}

int vtpc_io_fsync(int os_fd) {
  return fsync(os_fd);
}

int vtpc_io_truncate(int os_fd, off_t size) {
  return ftruncate(os_fd, size);
}
