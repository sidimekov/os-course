#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct io_backend_ops {
  int (*open_fn)(const char* path, int open_flags, int access_mode);
  int (*close_fn)(int file_descriptor);
  ssize_t (*read_fn)(int file_descriptor, void* buffer, size_t byte_count);
  ssize_t (*write_fn)(
      int file_descriptor, const void* buffer, size_t byte_count
  );
  off_t (*lseek_fn)(int file_descriptor, off_t offset, int whence);
  int (*fsync_fn)(int file_descriptor);
  const char* backend_name;
} io_backend_ops_t;

const io_backend_ops_t* io_backend_get(bool use_vtpc);
