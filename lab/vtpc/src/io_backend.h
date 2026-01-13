#pragma once

#include <stddef.h>
#include <sys/types.h>

typedef struct io_backend_ops {
  int     (*open_fn)(const char*, int, int);
  int     (*close_fn)(int);
  ssize_t (*read_fn)(int, void*, size_t);
  ssize_t (*write_fn)(int, const void*, size_t);
  off_t   (*lseek_fn)(int, off_t, int);
  int     (*fsync_fn)(int);
} io_backend_ops_t;

// libc или vtpc
const io_backend_ops_t* io_backend_get(int use_vtpc);
