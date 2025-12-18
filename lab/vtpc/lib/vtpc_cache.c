#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "vtpc_internal.h"

static void cache_touch(CachePage* p) {
  p->last_use = ++g_use_tick;
}

CachePage* vtpc_cache_find(int vfd, off_t page_index) {
  for (int i = 0; i < VTPC_CACHE_PAGES; ++i) {
    if (g_cache[i].used && g_cache[i].vfd == vfd &&
        g_cache[i].page_index == page_index) {
      return &g_cache[i];
    }
  }
  return NULL;
}

static CachePage* cache_pick_slot_mru(void) {
  for (int i = 0; i < VTPC_CACHE_PAGES; ++i) {
    if (!g_cache[i].used)
      return &g_cache[i];
  }

  int victim = 0;
  uint64_t best = g_cache[0].last_use;
  for (int i = 1; i < VTPC_CACHE_PAGES; ++i) {
    if (g_cache[i].last_use > best) {
      best = g_cache[i].last_use;
      victim = i;
    }
  }

  g_cache[victim].used = 0;
  g_cache[victim].vfd = -1;
  g_cache[victim].page_index = 0;
  g_cache[victim].valid_bytes = 0;
  g_cache[victim].dirty = 0;
  g_cache[victim].last_use = 0;

  return &g_cache[victim];
}

CachePage* vtpc_cache_get_or_load(int vfd, int os_fd, off_t page_index) {
  CachePage* p = vtpc_cache_find(vfd, page_index);
  if (p) {
    cache_touch(p);
    return p;
  }

  p = cache_pick_slot_mru();

  if (!p->data) {
    void* mem = NULL;
    int rc = posix_memalign(&mem, VTPC_ALIGNMENT, (size_t)VTPC_PAGE_SIZE);
    if (rc != 0) {
      errno = rc;
      return NULL;
    }
    p->data = mem;
  }

  ssize_t r = vtpc_io_pread_page(os_fd, p->data, page_index);
  if (r < 0) {
    return NULL;
  }

  p->used = 1;
  p->vfd = vfd;
  p->page_index = page_index;
  p->valid_bytes = (size_t)r;
  p->dirty = 0;
  cache_touch(p);

  return p;
}

void vtpc_cache_forget_file(int vfd) {
  for (int i = 0; i < VTPC_CACHE_PAGES; ++i) {
    if (g_cache[i].used && g_cache[i].vfd == vfd) {
      g_cache[i].used = 0;
      g_cache[i].vfd = -1;
      g_cache[i].page_index = 0;
      g_cache[i].valid_bytes = 0;
      g_cache[i].dirty = 0;
      g_cache[i].last_use = 0;
    }
  }
}
