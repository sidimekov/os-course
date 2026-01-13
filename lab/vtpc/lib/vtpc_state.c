#include "vtpc_internal.h"

// таблица виртуальных fd
VtpcFile g_files[VTPC_MAX_FILES];

// страницы кэша
CachePage g_cache[VTPC_CACHE_PAGES];

// счётчик последнего использования для mru
uint64_t g_use_tick = 0;

// счётчики для отладки и замеров
VtpcStats g_stats = {0};
