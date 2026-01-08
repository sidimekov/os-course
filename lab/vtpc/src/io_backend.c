#define _GNU_SOURCE
#include "io_backend.h"

#include <fcntl.h>
#include <unistd.h>

#include "vtpc.h"

// libc wrappers (фикс сигнатур)
static int libc_open(const char* p, int f, int m) {
  return open(p, f, m);
}
static int libc_close(int fd) {
  return close(fd);
}
static ssize_t libc_read(int fd, void* b, size_t c) {
  return read(fd, b, c);
}
static ssize_t libc_write(int fd, const void* b, size_t c) {
  return write(fd, b, c);
}
static off_t libc_lseek(int fd, off_t o, int w) {
  return lseek(fd, o, w);
}
static int libc_fsync(int fd) {
  return fsync(fd);
}

// vtpc wrappers
static int vtpc_open_w(const char* p, int f, int m) {
  return vtpc_open(p, f, m);
}
static int vtpc_close_w(int fd) {
  return vtpc_close(fd);
}
static ssize_t vtpc_read_w(int fd, void* b, size_t c) {
  return vtpc_read(fd, b, c);
}
static ssize_t vtpc_write_w(int fd, const void* b, size_t c) {
  return vtpc_write(fd, b, c);
}
static off_t vtpc_lseek_w(int fd, off_t o, int w) {
  return vtpc_lseek(fd, o, w);
}
static int vtpc_fsync_w(int fd) {
  return vtpc_fsync(fd);
}

static const io_backend_ops_t LIBC_OPS = {
    libc_open, libc_close, libc_read, libc_write, libc_lseek, libc_fsync
};

static const io_backend_ops_t VTPC_OPS = {
    vtpc_open_w,
    vtpc_close_w,
    vtpc_read_w,
    vtpc_write_w,
    vtpc_lseek_w,
    vtpc_fsync_w
};

const io_backend_ops_t* io_backend_get(int use_vtpc) {
  return use_vtpc ? &VTPC_OPS : &LIBC_OPS;
}
