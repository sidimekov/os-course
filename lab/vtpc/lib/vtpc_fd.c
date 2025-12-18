#include <errno.h>
#include <string.h>

#include "vtpc_internal.h"

// fd helpers

int vtpc_fd_alloc(void) {
  for (int i = 0; i < VTPC_MAX_FILES; ++i) {
    if (!g_files[i].used) {
      g_files[i].used = 1;
      return i;
    }
  }
  errno = EMFILE;
  return -1;
}

VtpcFile* vtpc_fd_get(int vfd) {
  if (vfd < 0 || vfd >= VTPC_MAX_FILES || !g_files[vfd].used) {
    errno = EBADF;
    return NULL;
  }
  return &g_files[vfd];
}

void vtpc_fd_free(int vfd) {
  memset(&g_files[vfd], 0, sizeof(g_files[vfd]));
}
