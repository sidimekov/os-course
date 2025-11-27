// src/ema/ema_join_hash.c
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Константы, чтобы не было магических чисел */
#define EMA_DEFAULT_BUCKET_COUNT 65536U
#define EMA_LINE_BUF_SIZE 256U
#define EMA_MAX_WORD_INPUT 32U
#define EMA_WORD_LEN 8U
#define EMA_WORD_BUF_LEN 9U /* 8 + '\0' */
#define EMA_BASE_STR_TO_L 10
#define EMA_EXIT_FAILURE_CODE 1
#define EMA_DEFAULT_REPEAT 1U
#define EMA_HASH_MULT 2654435761u

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
  size_t repeat_count;
} EmaArgs;

static void die_perror(const char* msg) {
  perror(msg);
  _exit(EMA_EXIT_FAILURE_CODE);
}

static void die_msg(const char* msg) {
  if (fputs(msg, stderr) < 0) {
    /* ignore */
  }
  _exit(EMA_EXIT_FAILURE_CODE);
}

static void print_usage_and_exit(const char* prog) {
  if (fprintf(
          stderr,
          "usage: %s --a A.txt --b B.txt --out C.txt "
          "[--bucket_count N] [--repeat N]\n",
          prog ? prog : "ema-join-hash"
      ) < 0) {
    perror("fprintf");
  }
  _exit(EMA_EXIT_FAILURE_CODE);
}

/* порядок: сначала size_t, потом int32_t — чтобы clang-tidy не ругался */
static inline size_t hash_id(size_t bucket_count, int32_t key) {
  const uint32_t HASH_MULT = EMA_HASH_MULT;
  const uint32_t key_u = (uint32_t)key;
  const uint32_t mod = (uint32_t)bucket_count;
  const uint32_t hashed = key_u * HASH_MULT;
  return (size_t)(hashed % mod);
}

static void ht_init(HashTable* table, size_t bucket_count) {
  table->bucket_count = bucket_count;
  table->buckets = (Node**)calloc(bucket_count, sizeof(Node*));
  if (table->buckets == NULL) {
    die_perror("calloc");
  }
}

static void ht_free(HashTable* table) {
  if (table == NULL || table->buckets == NULL) {
    return;
  }
  for (size_t i = 0; i < table->bucket_count; ++i) {
    Node* cur = table->buckets[i];
    while (cur != NULL) {
      Node* next = cur->next;
      free(cur);
      cur = next;
    }
  }
  free(table->buckets);
  table->buckets = NULL;
  table->bucket_count = 0;
}

static void ht_insert(HashTable* table, int32_t key_id, const char* word) {
  const size_t bucket = hash_id(table->bucket_count, key_id);
  Node* node = (Node*)malloc(sizeof(Node));
  if (node == NULL) {
    die_perror("malloc");
  }
  node->id = key_id;
  (void)strncpy(node->word, word, EMA_WORD_LEN);
  node->word[EMA_WORD_LEN] = '\0';
  node->next = table->buckets[bucket];
  table->buckets[bucket] = node;
}

static int64_t read_first_count(FILE* file) {
  char buffer[EMA_LINE_BUF_SIZE];
  const int read_size = (int)sizeof(buffer);
  if (fgets(buffer, read_size, file) == NULL) {
    die_msg("failed to read first line (count)\n");
  }

  errno = 0;
  char* end_ptr = NULL;
  const long long value = strtoll(buffer, &end_ptr, EMA_BASE_STR_TO_L);
  if (errno != 0 || end_ptr == buffer || value < 0) {
    die_msg("invalid count in first line\n");
  }
  return (int64_t)value;
}

/* без VLA: указатель вместо char out_word[EMA_WORD_BUF_LEN] */
static bool read_row(FILE* file, int32_t* out_id, char* out_word) {
  char line[EMA_LINE_BUF_SIZE];
  const int read_size = (int)sizeof(line);
  if (fgets(line, read_size, file) == NULL) {
    return false;
  }

  char* ptr = line;
  while (*ptr == ' ' || *ptr == '\t') {
    ++ptr;
  }
  if (*ptr == '\0' || *ptr == '\n') {
    return false;
  }

  errno = 0;
  char* end_id = NULL;
  const long id_val = strtol(ptr, &end_id, EMA_BASE_STR_TO_L);
  if (errno != 0 || end_id == ptr) {
    die_msg("invalid id in row\n");
  }

  while (*end_id == ' ' || *end_id == '\t') {
    ++end_id;
  }
  if (*end_id == '\0' || *end_id == '\n') {
    die_msg("missing word in row\n");
  }

  char word_buf[EMA_MAX_WORD_INPUT];
  size_t word_len = 0;
  while (*end_id != '\0' && *end_id != '\n' && *end_id != ' ' && *end_id != '\t'
  ) {
    if (word_len + 1 >= sizeof(word_buf)) {
      break;
    }
    word_buf[word_len++] = *end_id;
    ++end_id;
  }
  word_buf[word_len] = '\0';

  *out_id = (int32_t)id_val;
  (void)strncpy(out_word, word_buf, EMA_WORD_LEN);
  out_word[EMA_WORD_LEN] = '\0';
  return true;
}

static EmaArgs parse_args(int argc, char** argv) {
  EmaArgs args;
  args.path_a = NULL;
  args.path_b = NULL;
  args.path_out = NULL;
  args.bucket_count = EMA_DEFAULT_BUCKET_COUNT;
  args.repeat_count = EMA_DEFAULT_REPEAT;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--a") == 0 && i + 1 < argc) {
      args.path_a = argv[++i];
    } else if (strcmp(argv[i], "--b") == 0 && i + 1 < argc) {
      args.path_b = argv[++i];
    } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      args.path_out = argv[++i];
    } else if (strcmp(argv[i], "--bucket_count") == 0 && i + 1 < argc) {
      args.bucket_count = (size_t)strtoull(argv[++i], NULL, EMA_BASE_STR_TO_L);
    } else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
      args.repeat_count = (size_t)strtoull(argv[++i], NULL, EMA_BASE_STR_TO_L);
    } else {
      print_usage_and_exit((argc > 0) ? argv[0] : "ema-join-hash");
    }
  }

  if (args.path_a == NULL || args.path_b == NULL || args.path_out == NULL) {
    print_usage_and_exit((argc > 0) ? argv[0] : "ema-join-hash");
  }
  if (args.bucket_count == 0U) {
    die_msg("bucket_count must be > 0\n");
  }
  if (args.repeat_count == 0U) {
    die_msg("repeat must be > 0\n");
  }
  return args;
}

int main(int argc, char** argv) {
  const EmaArgs args = parse_args(argc, argv);

  FILE* file_a = fopen(args.path_a, "r");
  if (file_a == NULL) {
    die_perror("fopen A");
  }

  const int64_t count_a = read_first_count(file_a);

  HashTable table;
  ht_init(&table, args.bucket_count);

  for (int64_t i = 0; i < count_a; ++i) {
    int32_t id_a = 0;
    char word_a[EMA_WORD_BUF_LEN];
    if (!read_row(file_a, &id_a, word_a)) {
      if (fprintf(stderr, "unexpected EOF in A at row %" PRId64 "\n", i) < 0) {
        perror("fprintf");
      }
      (void)fclose(file_a);
      ht_free(&table);
      _exit(EMA_EXIT_FAILURE_CODE);
    }
    ht_insert(&table, id_a, word_a);
  }
  if (fclose(file_a) != 0) {
    ht_free(&table);
    die_perror("fclose A");
  }

  /* Повторяем по B repeat_count раз, но out_count запоминаем
   * только на последней итерации - даёт дополнительную CPU-нагрузку */
  size_t out_count = 0;
  int64_t count_b_final = 0;

  for (size_t rep = 0; rep < args.repeat_count; ++rep) {
    FILE* file_b = fopen(args.path_b, "r");
    if (file_b == NULL) {
      ht_free(&table);
      die_perror("fopen B");
    }
    const int64_t count_b = read_first_count(file_b);

    size_t local_out_count = 0;
    for (int64_t i = 0; i < count_b; ++i) {
      int32_t id_b = 0;
      char word_b[EMA_WORD_BUF_LEN];
      if (!read_row(file_b, &id_b, word_b)) {
        if (fprintf(
                stderr,
                "unexpected EOF in B at row %" PRId64 " (rep=%zu)\n",
                i,
                rep
            ) < 0) {
          perror("fprintf");
        }
        (void)fclose(file_b);
        ht_free(&table);
        _exit(EMA_EXIT_FAILURE_CODE);
      }
      const size_t bucket = hash_id(table.bucket_count, id_b);
      for (Node* node = table.buckets[bucket]; node != NULL;
           node = node->next) {
        if (node->id == id_b) {
          ++local_out_count;
        }
      }
    }

    if (fclose(file_b) != 0) {
      ht_free(&table);
      die_perror("fclose B");
    }

    if (rep == args.repeat_count - 1U) {
      out_count = local_out_count;
      count_b_final = count_b;
    }
  }

  /* Второй проход (уже без повторов) — записываем результат в файл. */
  FILE* file_b2 = fopen(args.path_b, "r");
  if (file_b2 == NULL) {
    ht_free(&table);
    die_perror("fopen B second pass");
  }
  (void)read_first_count(file_b2);

  FILE* file_out = fopen(args.path_out, "w");
  if (file_out == NULL) {
    (void)fclose(file_b2);
    ht_free(&table);
    die_perror("fopen out");
  }

  if (fprintf(file_out, "%zu\n", out_count) < 0) {
    (void)fclose(file_b2);
    (void)fclose(file_out);
    ht_free(&table);
    die_perror("fprintf out_count");
  }

  for (int64_t i = 0; i < count_b_final; ++i) {
    int32_t id_b = 0;
    char word_b[EMA_WORD_BUF_LEN];
    if (!read_row(file_b2, &id_b, word_b)) {
      if (fprintf(
              stderr,
              "unexpected EOF in B (second pass) at row %" PRId64 "\n",
              i
          ) < 0) {
        perror("fprintf");
      }
      (void)fclose(file_b2);
      (void)fclose(file_out);
      ht_free(&table);
      _exit(EMA_EXIT_FAILURE_CODE);
    }
    const size_t bucket = hash_id(table.bucket_count, id_b);
    for (Node* node = table.buckets[bucket]; node != NULL; node = node->next) {
      if (node->id == id_b) {
        if (fprintf(file_out, "%d %s %s\n", id_b, node->word, word_b) < 0) {
          (void)fclose(file_b2);
          (void)fclose(file_out);
          ht_free(&table);
          die_perror("fprintf join row");
        }
      }
    }
  }

  if (fclose(file_b2) != 0) {
    (void)fclose(file_out);
    ht_free(&table);
    die_perror("fclose B second pass");
  }
  if (fclose(file_out) != 0) {
    ht_free(&table);
    die_perror("fclose out");
  }

  ht_free(&table);
  return 0;
}
