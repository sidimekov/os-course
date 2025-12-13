#include "vtsh_internal.h"

#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void push_char(char** tok, size_t* tlen, size_t* tcap, char chr) {
  if (*tlen + 1 >= *tcap) {
    size_t new_cap = (*tcap ? *tcap * VTSH_GROWTH_FACTOR : VTSH_TOK_INIT_CAP);
    char* ptr = realloc(*tok, new_cap);
    if (!ptr) {
      perror("realloc");
      // _exit(1);
      _exit(1);
    }
    *tok = ptr;
    *tcap = new_cap;
  }
  (*tok)[(*tlen)++] = chr;
}

static void argv_append(
    char*** argv, size_t* argc, size_t* cap, const char* tok, size_t tlen
) {
  if (tlen == 0) {
    return;
  }
  char* seg2 = malloc(tlen + 1);
  if (!seg2) {
    perror("malloc");
    _exit(1);
  }
  memcpy(seg2, tok, tlen);
  seg2[tlen] = '\0';

  if (*argc + 2 > *cap) {
    size_t new_cap = (*cap ? *cap * VTSH_GROWTH_FACTOR : VTSH_ARGV_INIT_CAP);
    char** vec = realloc(*argv, new_cap * sizeof(char*));
    if (!vec) {
      perror("realloc");
      _exit(1);
    }
    *argv = vec;
    *cap = new_cap;
  }
  (*argv)[(*argc)++] = seg2;
  (*argv)[*argc] = NULL;
}

int vtsh_parse_argv(const char* str, char*** out_argv) {
  char** argv = NULL;
  size_t argc = 0;
  size_t cap = 0;

  char* tok = NULL;
  size_t tlen = 0;
  size_t tcap = 0;

  const char* ptr = str;
  int quotes = 0;

  while (*ptr == ' ' || *ptr == '\t') {
    ++ptr;
  }

  for (; *ptr; ++ptr) {
    int ch_ptr = (int)(unsigned char)*ptr;

    if (*ptr == '\\' && ptr[1]) {
      push_char(&tok, &tlen, &tcap, *++ptr);
      continue;
    }
    if (!quotes && (*ptr == '\'' || *ptr == '\"')) {
      quotes = ch_ptr;
      continue;
    }
    if (quotes && *ptr == quotes) {
      quotes = 0;
      continue;
    }
    if (!quotes && (*ptr == ' ' || *ptr == '\t')) {
      argv_append(&argv, &argc, &cap, tok, tlen);
      tlen = 0;

      while (ptr[1] == ' ' || ptr[1] == '\t') {
        ++ptr;
      }

      continue;
    }

    push_char(&tok, &tlen, &tcap, *ptr);
  }

  argv_append(&argv, &argc, &cap, tok, tlen);

  free(tok);

  *out_argv = argv;
  return (int)argc;
}


// split_by_and helpers

// Проверка, что в командной строке встретились &&
static inline bool vtsh_is_and_and(const char* ptr, int quotes) {
  return quotes == 0 && ptr[0] == '&' && ptr[1] == '&';
}

// обновление флага quotes
static inline void vtsh_update_quotes(int ch_ptr, int* quotes) {
  if (*quotes == 0 && (ch_ptr == '\'' || ch_ptr == '\"')) {
    *quotes = ch_ptr;
  } else if (*quotes && ch_ptr == *quotes) {
    *quotes = 0;
  }
}

static inline void vtsh_check_capacity(
    char*** parts, size_t* cap, size_t need
) {
  if (need >= *cap) {
    size_t new_cap = (*cap ? *cap * VTSH_GROWTH_FACTOR : VTSH_PARTS_INIT_CAP);
    while (need >= new_cap) {
      new_cap *= 2;
    }
    char** tmp = realloc(*parts, new_cap * sizeof(*tmp));
    if (!tmp) {
      perror("realloc");
      _exit(1);
    }
    *parts = tmp;
    *cap = new_cap;
  }
}

static inline void vtsh_append_range(
    char*** parts, size_t* cap, size_t* count, const char* start, size_t len
) {
  char* str = strndup(start, len);
  if (!str) {
    perror("strndup");
    _exit(1);
  }
  vtsh_check_capacity(parts, cap, *count);
  (*parts)[(*count)++] = str;
}

static inline void vtsh_append_cstr(
    char*** parts, size_t* cap, size_t* count, const char* str
) {
  char* dup = strdup(str);
  if (!dup) {
    perror("strdup");
    _exit(1);
  }
  vtsh_check_capacity(parts, cap, *count);
  (*parts)[(*count)++] = dup;
}

char** vtsh_split_by_and(const char* line, size_t* count) {
  size_t cap = VTSH_PARTS_INIT_CAP;
  size_t str_n = 0;
  char** parts = malloc(cap * sizeof(char*));
  if (!parts) {
    perror("malloc");
    _exit(1);
  }

  const char* ptr = line;
  const char* seg_start = line;
  int quotes = 0;
  while (*ptr) {
    if (vtsh_is_and_and(ptr, quotes)) {
      size_t len = (size_t)(ptr - seg_start);
      vtsh_append_range(&parts, &cap, &str_n, seg_start, len);
      ptr += 2;
      seg_start = ptr;
      continue;
    }

    int ch_ptr = (int)(unsigned char)*ptr;
    if (ch_ptr == '\\' && ptr[1]) {
      ++ptr;
    } else {
      vtsh_update_quotes(ch_ptr, &quotes);
    }
    ++ptr;
  }

  vtsh_append_cstr(&parts, &cap, &str_n, seg_start);

  *count = str_n;
  return parts;
}


// split_by_pipe helpers

// Проверка, что в командной строке встретился '|'
static inline bool vtsh_is_pipe(const char* ptr, int quotes) {
  return quotes == 0 && *ptr == '|';
}

// сплит по '|', как '&&'
char** vtsh_split_by_pipe(const char* line, size_t* count) {
  size_t cap = VTSH_PARTS_INIT_CAP;
  size_t str_n = 0;

  char** parts = malloc(cap * sizeof(*parts));
  if (!parts) {
    perror("malloc");
    _exit(1);
  }
  const char* ptr = line;
  const char* seg_start = line;
  int quotes = 0;
  while (*ptr) {
    if (vtsh_is_pipe(ptr, quotes)) {
      size_t len = (size_t)(ptr - seg_start);
      vtsh_append_range(&parts, &cap, &str_n, seg_start, len);
      ++ptr;
      seg_start = ptr;
      continue;
    }

    int ch_ptr = (int)(unsigned char)*ptr;
    if (ch_ptr == '\\' && ptr[1]) {
      ++ptr;
    } else {
      vtsh_update_quotes(ch_ptr, &quotes);
    }
    ++ptr;
  }

  vtsh_append_cstr(&parts, &cap, &str_n, seg_start);

  *count = str_n;
  return parts;
}


inline char* vtsh_lstrip(char* str) {
  while (*str == ' ' || *str == '\t') {
    ++str;
  }
  return str;
}

inline void vtsh_rstrip_inplace(char* str) {
  size_t len = strlen(str);
  while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\t' ||
                     str[len - 1] == '\n')) {
    str[--len] = '\0';
  }
}


void vtsh_free_strv(char** str, size_t n) {
  if (!str) {
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    free(str[i]);
  }
  free(str);
}