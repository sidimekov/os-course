#define _GNU_SOURCE

#include "vtpc.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>

enum { VTPC_MAX_FILES = 64 };

typedef struct {
  int used;
  int os_fd;
  off_t pos;
  int mode;
  int access;
} VtpcFile;

static VtpcFile vtpc_files[VTPC_MAX_FILES];

// helpers

static int vtpc_alloc_slot(void) {
  for (int i = 0; i < VTPC_MAX_FILES; i++) {
    if (!vtpc_files[i].used) {
      vtpc_files[i].used = 1;
      return i;
    }
  }
  errno = EMFILE;
  return -1;
}

static VtpcFile* vtpc_get_file(int fd) {
  if (fd < 0 || fd > VTPC_MAX_FILES || !vtpc_files[fd].used) {
    errno = EBADF;
    return NULL;
  }
  return &vtpc_files[fd];
}

static void vtpc_free_slot(int vfd) {
  memset(&vtpc_files[vfd], 0, sizeof(vtpc_files[vfd]));
}

// API

int vtpc_open(const char* path, int mode, int access) {
  if (!path) {
    errno = EINVAL;
    return -1;
  }

  int vfd = vtpc_alloc_slot();
  if (vfd < 0) {
    return -1;
  }

  int os_fd;
  if (mode & O_CREAT) {
    os_fd = open(path, mode, access);
  } else {
    os_fd = open(path, mode);
  }

  if (os_fd < 0) {
    vtpc_free_slot(vfd);
    return -1;
  }

  vtpc_files[vfd].os_fd = os_fd;
  vtpc_files[vfd].pos = 0;
  vtpc_files[vfd].mode = mode;
  vtpc_files[vfd].access = access;

  return vfd;
}

int vtpc_close(int fd) {
  VtpcFile* f = vtpc_get_file(fd);
  if (!f) {
    return -1;
  }

  if (close(f->os_fd) != 0) {
    return -1;
  }

  vtpc_free_slot(fd);
  return 0;
}

off_t vtpc_lseek(int fd, off_t offset, int whence) {
  VtpcFile* f = vtpc_get_file(fd);
  if (!f) {
    return (off_t)-1;
  }

  // по заданию только абсолютное позиционирование
  if (whence != SEEK_SET) {
    errno = EINVAL;
    return (off_t)-1;
  }
  if (offset < 0) {
    errno = EINVAL;
    return (off_t)-1;
  }

  f->pos = offset;
  return offset;
}

ssize_t vtpc_read(int fd, void* buf, size_t count) {
  return read(fd, buf, count);
}

ssize_t vtpc_write(int fd, const void* buf, size_t count) {
  return write(fd, buf, count);
}

int vtpc_fsync(int fd) {
  return fsync(fd);
}
