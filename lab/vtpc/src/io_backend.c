#define _GNU_SOURCE
#include "io_backend.h"

#include <fcntl.h>
#include <unistd.h>

#include "vtpc.h"

static int libc_open_wrap(const char* path, int open_flags, int access_mode) {
  return open(path, open_flags, access_mode);
}

static int libc_close_wrap(int file_descriptor) {
  return close(file_descriptor);
}

static ssize_t libc_read_wrap(
    int file_descriptor, void* buffer, size_t byte_count
) {
  return read(file_descriptor, buffer, byte_count);
}

static ssize_t libc_write_wrap(
    int file_descriptor, const void* buffer, size_t byte_count
) {
  return write(file_descriptor, buffer, byte_count);
}

static off_t libc_lseek_wrap(int file_descriptor, off_t offset, int whence) {
  return lseek(file_descriptor, offset, whence);
}

static int libc_fsync_wrap(int file_descriptor) {
  return fsync(file_descriptor);
}

static const io_backend_ops_t LIBC_BACKEND = {
    .open_fn = libc_open_wrap,
    .close_fn = libc_close_wrap,
    .read_fn = libc_read_wrap,
    .write_fn = libc_write_wrap,
    .lseek_fn = libc_lseek_wrap,
    .fsync_fn = libc_fsync_wrap,
    .backend_name = "libc",
};

static int vtpc_open_wrap(const char* path, int open_flags, int access_mode) {
  return vtpc_open(path, open_flags, access_mode);
}

static int vtpc_close_wrap(int file_descriptor) {
  return vtpc_close(file_descriptor);
}

static ssize_t vtpc_read_wrap(
    int file_descriptor, void* buffer, size_t byte_count
) {
  return vtpc_read(file_descriptor, buffer, byte_count);
}

static ssize_t vtpc_write_wrap(
    int file_descriptor, const void* buffer, size_t byte_count
) {
  return vtpc_write(file_descriptor, buffer, byte_count);
}

static off_t vtpc_lseek_wrap(int file_descriptor, off_t offset, int whence) {
  return vtpc_lseek(file_descriptor, offset, whence);
}

static int vtpc_fsync_wrap(int file_descriptor) {
  return vtpc_fsync(file_descriptor);
}

static const io_backend_ops_t VTPC_BACKEND = {
    .open_fn = vtpc_open_wrap,
    .close_fn = vtpc_close_wrap,
    .read_fn = vtpc_read_wrap,
    .write_fn = vtpc_write_wrap,
    .lseek_fn = vtpc_lseek_wrap,
    .fsync_fn = vtpc_fsync_wrap,
    .backend_name = "vtpc",
};

const io_backend_ops_t* io_backend_get(bool use_vtpc) {
  if (use_vtpc) {
    return &VTPC_BACKEND;
  }
  return &LIBC_BACKEND;
}
