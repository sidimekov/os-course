#include "vtpc.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "vtpc_internal.h"

int vtpc_open(const char* path, int mode, int access) {
  if (!path) {
    errno = EINVAL;
    return -1;
  }

  int vfd = vtpc_fd_alloc();
  if (vfd < 0) {
    return -1;
  }

  int os_fd = vtpc_io_open_direct(path, mode, access);
  if (os_fd < 0) {
    vtpc_fd_free(vfd);
    return -1;
  }

  g_files[vfd].os_fd = os_fd;
  g_files[vfd].pos = 0;
  g_files[vfd].mode = mode;
  g_files[vfd].access = access;

  off_t sz = vtpc_io_get_size(os_fd);
  if (sz < 0) {
    close(os_fd);
    vtpc_fd_free(vfd);
    return -1;
  }
  g_files[vfd].size = sz;

  return vfd;
}

int vtpc_close(int fd) {
  VtpcFile* f = vtpc_fd_get(fd);
  if (!f) {
    errno = EBADF;
    return -1;
  }

  const int os_fd = f->os_fd;

  if (((unsigned int)f->mode & O_ACCMODE) != O_RDONLY) {
    if (vtpc_fsync(fd) != 0) {
      return -1;
    }
  }

  vtpc_cache_forget_file(fd);

  if (close(os_fd) != 0) {
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
  unsigned int acc = (unsigned int)f->mode & O_ACCMODE;
  if (acc == O_WRONLY) {
    errno = EBADF;
    return -1;
  }

  if (!buf && count != 0) {
    errno = EINVAL;
    return -1;
  }
  if (count == 0) {
    return 0;
  }

  off_t disk_size = vtpc_io_get_size(f->os_fd);
  if (disk_size < 0) {
    return -1;
  }

  if (disk_size > f->size) {
    f->size = disk_size;
  }

  if (f->pos >= f->size) {
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

    off_t page_start = page_index * (off_t)VTPC_PAGE_SIZE;
    size_t page_limit = (size_t)VTPC_PAGE_SIZE;
    if (page_start + (off_t)page_limit > f->size) {
      page_limit = (size_t)(f->size - page_start);
    }

    if (page_off >= page_limit) {
      break;
    }

    size_t avail = page_limit - page_off;
    size_t need = count - total;
    size_t chunk = (avail < need) ? avail : need;

    memcpy(out + total, (unsigned char*)p->data + page_off, chunk);

    total += chunk;
    f->pos += (off_t)chunk;

    if (f->pos >= f->size) {
      break;
    }
  }

  return (ssize_t)total;
}

ssize_t vtpc_write(int fd, const void* buf, size_t count) {
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

  const unsigned char* in = (const unsigned char*)buf;
  size_t total = 0;

  while (total < count) {
    off_t cur = f->pos;
    off_t page_index = cur / (off_t)VTPC_PAGE_SIZE;
    size_t page_off = (size_t)(cur % (off_t)VTPC_PAGE_SIZE);

    CachePage* p = vtpc_cache_get_for_write(fd, f->os_fd, page_index, f->size);
    if (!p) {
      if (total > 0) {
        break;
      }
      return -1;
    }

    size_t available = (size_t)VTPC_PAGE_SIZE - page_off;
    size_t need = count - total;
    size_t chunk = (available < need) ? available : need;

    memcpy((unsigned char*)p->data + page_off, in + total, chunk);
    p->dirty = 1;
    p->last_use = ++g_use_tick;

    total += chunk;
    f->pos += (off_t)chunk;

    if (f->pos > f->size) {
      f->size = f->pos;
    }
  }

  return (ssize_t)total;
}

int vtpc_fsync(int fd) {
  VtpcFile* f = vtpc_fd_get(fd);
  if (!f) {
    return -1;
  }

  if (vtpc_cache_flush_file(fd) != 0) {
    return -1;
  }

  if (vtpc_io_truncate(f->os_fd, f->size) != 0) {
    return -1;
  }

  if (vtpc_io_fsync(f->os_fd) != 0) {
    return -1;
  }

  return 0;
}
