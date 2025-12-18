#include "vtpc.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "vtpc_internal.h"

int vtpc_open(const char* path, int mode, int access) {
  if (!path) {
    errno = EINVAL;
    return -1;
  }

  int vfd = vtpc_fd_alloc();
  if (vfd < 0)
    return -1;

  int os_fd = vtpc_io_open_direct(path, mode, access);
  if (os_fd < 0) {
    vtpc_fd_free(vfd);
    return -1;
  }

  g_files[vfd].os_fd = os_fd;
  g_files[vfd].pos = 0;
  g_files[vfd].mode = mode;
  g_files[vfd].access = access;

  return vfd;
}

int vtpc_close(int fd) {
  VtpcFile* f = vtpc_fd_get(fd);
  if (!f) {
    return -1;
  }

  vtpc_cache_forget_file(fd);

  if (close(f->os_fd) != 0) {
    return -1;
  }

  vtpc_fd_free(fd);
  return 0;
}

off_t vtpc_lseek(int fd, off_t offset, int whence) {
  VtpcFile* f = vtpc_fd_get(fd);
  if (!f) {
    return (off_t)-1;
  }

  if (whence != SEEK_SET || offset < 0) {
    errno = EINVAL;
    return (off_t)-1;
  }

  f->pos = offset;
  return offset;
}

ssize_t vtpc_read(int fd, void* buf, size_t count) {
  VtpcFile* f = vtpc_fd_get(fd);
  if (!f) {
    return -1;
  }

  if (!buf && count != 0) {
    errno = EINVAL;
    return -1;
  }
  if (count == 0) {
    return 0;
  }

  off_t file_size = vtpc_io_get_size(f->os_fd);
  if (file_size < 0) {
    return -1;
  }

  if (f->pos >= file_size) {
    return 0;
  }

  size_t total = 0;
  unsigned char* out = (unsigned char*)buf;

  while (total < count) {
    off_t cur = f->pos;

    off_t page_index = cur / (off_t)VTPC_PAGE_SIZE;
    size_t page_off = (size_t)(cur % (off_t)VTPC_PAGE_SIZE);

    CachePage* p = vtpc_cache_get_or_load(fd, f->os_fd, page_index);
    if (!p) {
      if (total > 0) {
        break;
      }
      return -1;
    }

    if (p->valid_bytes == 0) {
      break;
    }

    if (page_off >= p->valid_bytes) {
      break;
    }

    size_t avail = p->valid_bytes - page_off;
    size_t need = count - total;
    size_t chunk = (avail < need) ? avail : need;

    memcpy(out + total, (unsigned char*)p->data + page_off, chunk);

    total += chunk;
    f->pos += (off_t)chunk;

    if (f->pos >= file_size)
      break;
  }

  return (ssize_t)total;
}

ssize_t vtpc_write(int fd, const void* buf, size_t count) {
  return write(fd, buf, count);
}

int vtpc_fsync(int fd) {
  return fsync(fd);
}
