#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "vtpc_internal.h"

static void cache_touch(CachePage* p) {
  p->last_use = ++g_use_tick;
}

// ищет страницу в кэше по ключу
CachePage* vtpc_cache_find(int vfd, off_t page_index) {
  for (int i = 0; i < VTPC_CACHE_PAGES; ++i) {
    if (g_cache[i].used && g_cache[i].vfd == vfd &&
        g_cache[i].page_index == page_index) {
      return &g_cache[i];
    }
  }
  return NULL;
}

// пишет dirty страницу на диск
int vtpc_cache_flush_page(CachePage* p) {
  if (!p || !p->used) {
    return 0;
  }
  if (!p->dirty) {
    return 0;
  }

  VtpcFile* f = vtpc_fd_get(p->vfd);
  if (!f) {
    return -1;
  }

  g_stats.flush_pages += 1;

  ssize_t w = vtpc_io_pwrite_page(f->os_fd, p->data, p->page_index);
  if (w < 0) {
    return -1;
  }
  if (w != (ssize_t)VTPC_PAGE_SIZE) {
    errno = EIO;
    return -1;
  }

  p->dirty = 0;
  return 0;
}

// выбирает слот mru, при вытеснении делает flush
static CachePage* cache_pick_slot_mru(void) {
  for (int i = 0; i < VTPC_CACHE_PAGES; ++i) {
    if (!g_cache[i].used) {
      return &g_cache[i];
    }
  }

  int victim = 0;
  uint64_t best = g_cache[0].last_use;
  for (int i = 1; i < VTPC_CACHE_PAGES; ++i) {
    if (g_cache[i].last_use > best) {
      best = g_cache[i].last_use;
      victim = i;
    }
  }

  CachePage* v = &g_cache[victim];

  g_stats.evict_pages += 1;

  if (vtpc_cache_flush_page(v) != 0) {
    return NULL;
  }

  v->used = 0;
  v->vfd = -1;
  v->page_index = 0;
  v->dirty = 0;
  v->last_use = 0;

  return v;
}

// берёт страницу для чтения, при промахе грузит с диска
CachePage* vtpc_cache_get_or_load(
    int vfd, int os_fd, off_t page_index, off_t file_size
) {
  CachePage* p = vtpc_cache_find(vfd, page_index);
  if (p) {
    g_stats.cache_hit += 1;
    cache_touch(p);
    return p;
  }

  g_stats.cache_miss += 1;

  p = cache_pick_slot_mru();
  if (!p) {
    return NULL;
  }

  if (!p->data) {
    void* mem = NULL;
    int rc = posix_memalign(&mem, VTPC_ALIGNMENT, (size_t)VTPC_PAGE_SIZE);
    if (rc != 0) {
      errno = rc;
      return NULL;
    }
    p->data = mem;
  }

  off_t page_start = page_index * (off_t)VTPC_PAGE_SIZE;
  if (page_start < file_size) {
    ssize_t r = vtpc_io_pread_page(os_fd, p->data, page_index);
    if (r < 0) {
      return NULL;
    }
    if (r < (ssize_t)VTPC_PAGE_SIZE) {
      memset(
          (unsigned char*)p->data + r, 0, (size_t)VTPC_PAGE_SIZE - (size_t)r
      );
    }
  } else {
    memset(p->data, 0, (size_t)VTPC_PAGE_SIZE);
  }

  p->used = 1;
  p->vfd = vfd;
  p->page_index = page_index;
  p->dirty = 0;
  cache_touch(p);

  return p;
}

// берёт страницу для записи, init управляет подгрузкой старых данных
CachePage* vtpc_cache_get_for_write_ex(
    int vfd, int os_fd, off_t page_index, off_t file_size, VtpcPageInit init
) {
  CachePage* p = vtpc_cache_find(vfd, page_index);
  if (p) {
    g_stats.cache_hit += 1;
    cache_touch(p);
    return p;
  }

  g_stats.cache_miss += 1;

  p = cache_pick_slot_mru();
  if (!p) {
    return NULL;
  }

  if (!p->data) {
    void* mem = NULL;
    int rc = posix_memalign(&mem, VTPC_ALIGNMENT, (size_t)VTPC_PAGE_SIZE);
    if (rc != 0) {
      errno = rc;
      return NULL;
    }
    p->data = mem;
  }

  off_t page_start = page_index * (off_t)VTPC_PAGE_SIZE;
  if (init == VTPC_PAGE_INIT_ZERO) {
    memset(p->data, 0, (size_t)VTPC_PAGE_SIZE);
  }
  if (init == VTPC_PAGE_INIT_LOAD) {
    if (page_start < file_size) {
      ssize_t r = vtpc_io_pread_page(os_fd, p->data, page_index);
      if (r < 0) {
        return NULL;
      }
      if (r < (ssize_t)VTPC_PAGE_SIZE) {
        memset(
            (unsigned char*)p->data + r, 0, (size_t)VTPC_PAGE_SIZE - (size_t)r
        );
      }
    } else {
      memset(p->data, 0, (size_t)VTPC_PAGE_SIZE);
    }
  }

  p->used = 1;
  p->vfd = vfd;
  p->page_index = page_index;
  p->dirty = 0;
  cache_touch(p);

  return p;
}

// берёт страницу для записи, по умолчанию грузит старые данные
CachePage* vtpc_cache_get_for_write(
    int vfd, int os_fd, off_t page_index, off_t file_size
) {
  return vtpc_cache_get_for_write_ex(
      vfd, os_fd, page_index, file_size, VTPC_PAGE_INIT_LOAD
  );
}

// сбрасывает все страницы файла
int vtpc_cache_flush_file(int vfd) {
  for (int i = 0; i < VTPC_CACHE_PAGES; ++i) {
    if (g_cache[i].used && g_cache[i].vfd == vfd) {
      if (vtpc_cache_flush_page(&g_cache[i]) != 0) {
        return -1;
      }
    }
  }
  return 0;
}

// забывает страницы файла, используется при close
void vtpc_cache_forget_file(int vfd) {
  for (int i = 0; i < VTPC_CACHE_PAGES; ++i) {
    if (g_cache[i].used && g_cache[i].vfd == vfd) {
      g_cache[i].used = 0;
      g_cache[i].vfd = -1;
      g_cache[i].page_index = 0;
      g_cache[i].dirty = 0;
      g_cache[i].last_use = 0;
    }
  }
}
