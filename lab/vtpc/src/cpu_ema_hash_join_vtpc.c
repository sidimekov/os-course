#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "vtpc.h"

#define EMA_DEFAULT_BUCKET_COUNT 65536U
#define EMA_CHUNK_BUF_SIZE       4096U
#define EMA_LINE_BUF_SIZE        512U
#define EMA_MAX_WORD_INPUT       64U
#define EMA_WORD_LEN             8U
#define EMA_WORD_BUF_LEN         9U
#define EMA_BASE_10              10
#define EMA_EXIT_FAILURE_CODE    1

typedef struct Node {
  int32_t id;
  char word[EMA_WORD_BUF_LEN];
  struct Node* next;
} Node;

typedef struct {
  Node** buckets;
  size_t bucket_count;
} HashTable;

typedef struct {
  const char* path_a;
  const char* path_b;
  const char* path_out;
  size_t bucket_count;
  int repeats;
} Args;

typedef struct {
  int fd;
  unsigned char buf[EMA_CHUNK_BUF_SIZE];
  size_t pos;
  size_t len;
  bool eof;
} Reader;

static void die_perror(const char* msg) {
  perror(msg);
  _exit(EMA_EXIT_FAILURE_CODE);
}

static void die_msg(const char* msg) {
  (void)fputs(msg, stderr);
  _exit(EMA_EXIT_FAILURE_CODE);
}

static void print_usage_and_exit(const char* prog) {
  (void)fprintf(stderr,
                "usage: %s --a A.txt --b B.txt --out C.txt [--bucket_count N] [--repeat R]\n",
                prog ? prog : "cpu-ema-hash-join-vtpc");
  _exit(EMA_EXIT_FAILURE_CODE);
}

static inline size_t hash_id(size_t bucket_count, int32_t key) {
  const uint32_t hash_mult = 2654435761u;
  const uint32_t key_u = (uint32_t)key;
  const uint32_t mod = (uint32_t)bucket_count;
  const uint32_t hashed = key_u * hash_mult;
  return (size_t)(hashed % mod);
}

static void ht_init(HashTable* ht, size_t bucket_count) {
  ht->bucket_count = bucket_count;
  ht->buckets = (Node**)calloc(bucket_count, sizeof(Node*));
  if (!ht->buckets) {
    die_perror("calloc");
  }
}

static void ht_free(HashTable* ht) {
  if (!ht || !ht->buckets) return;
  for (size_t i = 0; i < ht->bucket_count; ++i) {
    Node* cur = ht->buckets[i];
    while (cur) {
      Node* nxt = cur->next;
      free(cur);
      cur = nxt;
    }
  }
  free(ht->buckets);
  ht->buckets = NULL;
  ht->bucket_count = 0U;
}

static void ht_insert(HashTable* ht, int32_t id, const char* word) {
  const size_t b = hash_id(ht->bucket_count, id);
  Node* node = (Node*)malloc(sizeof(Node));
  if (!node) die_perror("malloc");
  node->id = id;
  (void)strncpy(node->word, word, EMA_WORD_LEN);
  node->word[EMA_WORD_LEN] = '\0';
  node->next = ht->buckets[b];
  ht->buckets[b] = node;
}

static void reader_init(Reader* r, int fd) {
  r->fd = fd;
  r->pos = 0U;
  r->len = 0U;
  r->eof = false;
}

static void reader_reset(Reader* r) {
  r->pos = 0U;
  r->len = 0U;
  r->eof = false;
}

static ssize_t reader_fill(Reader* r) {
  if (r->eof) return 0;
  const ssize_t n = vtpc_read(r->fd, r->buf, sizeof(r->buf));
  if (n < 0) return -1;
  if (n == 0) {
    r->eof = true;
    r->pos = 0U;
    r->len = 0U;
    return 0;
  }
  r->pos = 0U;
  r->len = (size_t)n;
  return n;
}

static bool reader_readline(Reader* r, char* out, size_t out_cap) {
  size_t out_len = 0U;
  for (;;) {
    if (r->pos >= r->len) {
      const ssize_t filled = reader_fill(r);
      if (filled < 0) die_perror("vtpc_read");
      if (filled == 0) {
        if (out_len == 0U) return false;
        out[out_len] = '\0';
        return true;
      }
    }

    const unsigned char ch = r->buf[r->pos++];
    if (ch == (unsigned char)'\n') {
      out[out_len] = '\0';
      return true;
    }
    if (out_len + 1U >= out_cap) {
      die_msg("line too long\n");
    }
    out[out_len++] = (char)ch;
  }
}

static int64_t parse_nonneg_i64(const char* s, const char* err) {
  errno = 0;
  char* endp = NULL;
  const long long v = strtoll(s, &endp, EMA_BASE_10);
  if (errno != 0 || endp == s || v < 0) die_msg(err);
  return (int64_t)v;
}

static int32_t parse_i32(const char* s, const char* err) {
  errno = 0;
  char* endp = NULL;
  const long v = strtol(s, &endp, EMA_BASE_10);
  if (errno != 0 || endp == s) die_msg(err);
  return (int32_t)v;
}

static bool parse_row(const char* line, int32_t* out_id, char out_word[EMA_WORD_BUF_LEN]) {
  const char* p = line;
  while (*p == ' ' || *p == '\t') ++p;
  if (*p == '\0') return false;

  const int32_t id = parse_i32(p, "bad id\n");
  char* end_id = NULL;
  (void)strtol(p, &end_id, EMA_BASE_10);
  if (!end_id) die_msg("bad id\n");

  p = end_id;
  while (*p == ' ' || *p == '\t') ++p;
  if (*p == '\0') die_msg("missing word\n");

  char wbuf[EMA_MAX_WORD_INPUT];
  size_t wlen = 0U;
  while (*p != '\0' && *p != ' ' && *p != '\t') {
    if (wlen + 1U >= sizeof(wbuf)) break;
    wbuf[wlen++] = *p++;
  }
  wbuf[wlen] = '\0';

  *out_id = id;
  (void)strncpy(out_word, wbuf, EMA_WORD_LEN);
  out_word[EMA_WORD_LEN] = '\0';
  return true;
}

static Args parse_args(int argc, char** argv) {
  Args a;
  a.path_a = NULL;
  a.path_b = NULL;
  a.path_out = NULL;
  a.bucket_count = EMA_DEFAULT_BUCKET_COUNT;
  a.repeats = 1;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--a") == 0 && i + 1 < argc) {
      a.path_a = argv[++i];
    } else if (strcmp(argv[i], "--b") == 0 && i + 1 < argc) {
      a.path_b = argv[++i];
    } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      a.path_out = argv[++i];
    } else if (strcmp(argv[i], "--bucket_count") == 0 && i + 1 < argc) {
      a.bucket_count = (size_t)strtoull(argv[++i], NULL, EMA_BASE_10);
    } else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
      errno = 0;
      char* endp = NULL;
      const long r = strtol(argv[++i], &endp, EMA_BASE_10);
      if (errno != 0 || endp == argv[i] || r <= 0) die_msg("bad --repeat\n");
      a.repeats = (int)r;
    } else {
      print_usage_and_exit(argv[0]);
    }
  }

  if (!a.path_a || !a.path_b || !a.path_out) print_usage_and_exit(argv[0]);
  if (a.bucket_count == 0U) die_msg("bucket_count must be > 0\n");
  return a;
}

static int open_read_vtpc(const char* path) {
  const int fd = vtpc_open(path, O_RDONLY, 0);
  if (fd < 0) die_perror("vtpc_open(O_RDONLY)");
  return fd;
}

static int open_write_vtpc(const char* path) {
  const int fd = vtpc_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) die_perror("vtpc_open(O_WRONLY|O_CREAT|O_TRUNC)");
  return fd;
}

static void rewind_vtpc(int fd) {
  const off_t off = vtpc_lseek(fd, 0, SEEK_SET);
  if (off < 0) die_perror("vtpc_lseek(SEEK_SET)");
}

static int64_t read_count_line(Reader* r) {
  char line[EMA_LINE_BUF_SIZE];
  if (!reader_readline(r, line, sizeof(line))) die_msg("empty file\n");
  return parse_nonneg_i64(line, "bad count\n");
}

static void build_hash_from_a(const Args* args, HashTable* ht) {
  const int fd_a = open_read_vtpc(args->path_a);
  Reader r;
  reader_init(&r, fd_a);

  const int64_t count_a = read_count_line(&r);

  char line[EMA_LINE_BUF_SIZE];
  for (int64_t i = 0; i < count_a; ++i) {
    if (!reader_readline(&r, line, sizeof(line))) die_msg("EOF in A\n");
    int32_t id_a = 0;
    char word_a[EMA_WORD_BUF_LEN];
    (void)parse_row(line, &id_a, word_a);
    ht_insert(ht, id_a, word_a);
  }

  if (vtpc_close(fd_a) != 0) die_perror("vtpc_close A");
}

static void scan_b_no_output(Reader* r, const HashTable* ht, int64_t count_b) {
  char line[EMA_LINE_BUF_SIZE];
  for (int64_t i = 0; i < count_b; ++i) {
    if (!reader_readline(r, line, sizeof(line))) die_msg("EOF in B\n");
    int32_t id_b = 0;
    char word_b[EMA_WORD_BUF_LEN];
    (void)parse_row(line, &id_b, word_b);

    const size_t bucket = hash_id(ht->bucket_count, id_b);
    for (Node* n = ht->buckets[bucket]; n; n = n->next) {
      if (n->id == id_b) {
        /* пусто */
      }
    }
  }
}

static size_t count_matches_last_pass(Reader* r, const HashTable* ht, int64_t count_b) {
  size_t out_count = 0U;
  char line[EMA_LINE_BUF_SIZE];
  for (int64_t i = 0; i < count_b; ++i) {
    if (!reader_readline(r, line, sizeof(line))) die_msg("EOF in B\n");
    int32_t id_b = 0;
    char word_b[EMA_WORD_BUF_LEN];
    (void)parse_row(line, &id_b, word_b);

    const size_t bucket = hash_id(ht->bucket_count, id_b);
    for (Node* n = ht->buckets[bucket]; n; n = n->next) {
      if (n->id == id_b) ++out_count;
    }
  }
  return out_count;
}

static void write_join_rows(Reader* r, const HashTable* ht, int64_t count_b, int fd_out) {
  char line[EMA_LINE_BUF_SIZE];
  for (int64_t i = 0; i < count_b; ++i) {
    if (!reader_readline(r, line, sizeof(line))) die_msg("EOF in B\n");
    int32_t id_b = 0;
    char word_b[EMA_WORD_BUF_LEN];
    (void)parse_row(line, &id_b, word_b);

    const size_t bucket = hash_id(ht->bucket_count, id_b);
    for (Node* n = ht->buckets[bucket]; n; n = n->next) {
      if (n->id == id_b) {
        char out_line[128];
        const int nbytes = snprintf(out_line, sizeof(out_line), "%d %s %s\n",
                                    id_b, n->word, word_b);
        if (nbytes <= 0) die_msg("snprintf\n");
        if (vtpc_write(fd_out, out_line, (size_t)nbytes) < 0) die_perror("vtpc_write out");
      }
    }
  }
}

int main(int argc, char** argv) {
  const Args args = parse_args(argc, argv);

  HashTable ht;
  ht_init(&ht, args.bucket_count);
  build_hash_from_a(&args, &ht);

  /* B открываем один раз и просто перематываем */
  const int fd_b = open_read_vtpc(args.path_b);
  Reader rb;
  reader_init(&rb, fd_b);

  /* повторы 1..repeats-1: нагрузка без вывода */
  for (int rep = 1; rep < args.repeats; ++rep) {
    rewind_vtpc(fd_b);
    reader_reset(&rb);
    const int64_t count_b = read_count_line(&rb);
    scan_b_no_output(&rb, &ht, count_b);
  }

  /* последний проход: считаем out_count */
  rewind_vtpc(fd_b);
  reader_reset(&rb);
  const int64_t count_b = read_count_line(&rb);
  const size_t out_count = count_matches_last_pass(&rb, &ht, count_b);

  /* последний проход: пишем out */
  const int fd_out = open_write_vtpc(args.path_out);

  char header[64];
  const int hdr_n = snprintf(header, sizeof(header), "%zu\n", out_count);
  if (hdr_n <= 0) die_msg("snprintf\n");
  if (vtpc_write(fd_out, header, (size_t)hdr_n) < 0) die_perror("vtpc_write header");

  rewind_vtpc(fd_b);
  reader_reset(&rb);
  (void)read_count_line(&rb);
  write_join_rows(&rb, &ht, count_b, fd_out);

  if (vtpc_fsync(fd_out) != 0) die_perror("vtpc_fsync out");
  if (vtpc_close(fd_out) != 0) die_perror("vtpc_close out");
  if (vtpc_close(fd_b) != 0) die_perror("vtpc_close B");

  ht_free(&ht);
  return 0;
}
