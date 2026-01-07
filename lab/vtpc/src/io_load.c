#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "io_backend.h"

enum { IO_ALIGN = 4096, IO_DIRECT_MIN_BLOCK = 512 };

static const int IO_NUM_BASE = 10;
static const mode_t IO_DEFAULT_MODE = 0666;
static const unsigned char IO_WRITE_PATTERN = 0xAB;

static const uint32_t IO_LCG_MULTIPLIER = 1664525U;
static const uint32_t IO_LCG_INCREMENT = 1013904223U;

static const size_t IO_ARG_RW_PREFIX_LEN = 5U;
static const size_t IO_ARG_BLOCK_SIZE_PREFIX_LEN = 13U;
static const size_t IO_ARG_BLOCK_COUNT_PREFIX_LEN = 14U;
static const size_t IO_ARG_FILE_PREFIX_LEN = 7U;
static const size_t IO_ARG_RANGE_PREFIX_LEN = 8U;
static const size_t IO_ARG_DIRECT_PREFIX_LEN = 9U;
static const size_t IO_ARG_TYPE_PREFIX_LEN = 7U;
static const size_t IO_ARG_BACKEND_PREFIX_LEN = 10U;
static const size_t IO_ARG_CACHE_PREFIX_LEN = 8U;

typedef enum { MODE_READ = 0, MODE_WRITE = 1 } rw_mode_t;
typedef enum { SEL_SEQ = 0, SEL_RAND = 1 } sel_mode_t;
typedef enum {
  CACHE_UNSPEC = 0,
  CACHE_NONE,
  CACHE_OS,
  CACHE_VTPC
} cache_mode_t;

typedef struct {
  rw_mode_t operation_mode;
  size_t block_size;
  size_t block_count;
  const char* file_path;
  off_t range_start;
  off_t range_end; /* 0 означает нет верхней границы */
  bool use_direct;
  sel_mode_t selection_mode;
  bool use_vtpc;
  cache_mode_t cache_mode;
  bool backend_set;
  bool direct_set;
} config_t;

// печать сообщений об ошибках без падения
static void safe_fprintf(FILE* stream, const char* format_string, ...) {
  va_list args;
  va_start(args, format_string);
  const int ret_code = vfprintf(stream, format_string, args);
  va_end(args);
  if (ret_code < 0) {
    perror("vfprintf");
  }
}

// простой генератор псевдослучайных чисел
static uint32_t lcg_next(uint32_t* random_state) {
  *random_state = IO_LCG_MULTIPLIER * (*random_state) + IO_LCG_INCREMENT;
  return *random_state;
}

// вывод справки по параметрам
static void print_usage(const char* program_name) {
  safe_fprintf(
      stderr,
      "Usage: %s "
      "--rw=read|write "
      "--block_size=N "
      "--block_count=N "
      "--file=PATH "
      "[--range=START-END] "
      "[--type=sequence|random] "
      "[--cache=none|os|vtpc] "
      "[--direct=on|off] "
      "[--backend=libc|vtpc]\n",
      program_name
  );
}

// разбор диапазона start end
static bool parse_range(
    const char* string, off_t* range_start, off_t* range_end
) {
  if (string == NULL || range_start == NULL || range_end == NULL) {
    return false;
  }

  const char* dash_position = strchr(string, '-');
  if (dash_position == NULL) {
    return false;
  }

  char* local_end_pointer = NULL;
  errno = 0;
  const long long start_value =
      strtoll(string, &local_end_pointer, IO_NUM_BASE);
  if (errno != 0 || local_end_pointer != dash_position) {
    return false;
  }

  errno = 0;
  const long long end_value =
      strtoll(dash_position + 1, &local_end_pointer, IO_NUM_BASE);
  if (errno != 0 || local_end_pointer == (dash_position + 1) ||
      *local_end_pointer != '\0') {
    return false;
  }

  if (start_value < 0 || end_value < 0) {
    return false;
  }

  if (end_value != 0 && end_value < start_value) {
    return false;
  }

  *range_start = (off_t)start_value;
  *range_end = (off_t)end_value;
  return true;
}

// разбор числа в size t
static bool parse_size_t(const char* string, size_t* output_value) {
  if (string == NULL || output_value == NULL) {
    return false;
  }

  char* end_pointer = NULL;
  errno = 0;
  const unsigned long long parsed_value =
      strtoull(string, &end_pointer, IO_NUM_BASE);
  if (errno != 0 || end_pointer == string || *end_pointer != '\0') {
    return false;
  }
  *output_value = (size_t)parsed_value;
  return true;
}

// разбор выбора бэкенда
static bool parse_backend(const char* value, bool* use_vtpc) {
  if (value == NULL || use_vtpc == NULL) {
    return false;
  }
  if (strcmp(value, "libc") == 0) {
    *use_vtpc = false;
    return true;
  }
  if (strcmp(value, "vtpc") == 0) {
    *use_vtpc = true;
    return true;
  }
  return false;
}

// разбор режима кэша и установка флагов
static bool parse_cache(const char* value, config_t* config) {
  if (value == NULL || config == NULL) {
    return false;
  }
  if (strcmp(value, "none") == 0) {
    config->cache_mode = CACHE_NONE;
    return true;
  }
  if (strcmp(value, "os") == 0) {
    config->cache_mode = CACHE_OS;
    return true;
  }
  if (strcmp(value, "vtpc") == 0) {
    config->cache_mode = CACHE_VTPC;
    return true;
  }
  return false;
}

// обработка режима read write
static bool handle_rw_argument(const char* value, config_t* config) {
  if (strcmp(value, "read") == 0) {
    config->operation_mode = MODE_READ;
    return true;
  }
  if (strcmp(value, "write") == 0) {
    config->operation_mode = MODE_WRITE;
    return true;
  }
  safe_fprintf(stderr, "unknown rw mode: %s\n", value);
  return false;
}

// обработка флага direct
static bool handle_direct_argument(const char* value, config_t* config) {
  if (strcmp(value, "on") == 0) {
    config->use_direct = true;
    config->direct_set = true;
    return true;
  }
  if (strcmp(value, "off") == 0) {
    config->use_direct = false;
    config->direct_set = true;
    return true;
  }
  safe_fprintf(stderr, "bad direct value: %s\n", value);
  return false;
}

// обработка типа доступа sequence random
static bool handle_type_argument(const char* value, config_t* config) {
  if (strcmp(value, "sequence") == 0) {
    config->selection_mode = SEL_SEQ;
    return true;
  }
  if (strcmp(value, "random") == 0) {
    config->selection_mode = SEL_RAND;
    return true;
  }
  safe_fprintf(stderr, "bad type: %s\n", value);
  return false;
}

// разбор одного аргумента командной строки
static bool process_single_argument(const char* argument, config_t* config) {
  if (strncmp(argument, "--rw=", IO_ARG_RW_PREFIX_LEN) == 0) {
    const char* value = argument + IO_ARG_RW_PREFIX_LEN;
    return handle_rw_argument(value, config);
  }

  if (strncmp(argument, "--block_size=", IO_ARG_BLOCK_SIZE_PREFIX_LEN) == 0) {
    const char* value = argument + IO_ARG_BLOCK_SIZE_PREFIX_LEN;
    if (!parse_size_t(value, &config->block_size)) {
      safe_fprintf(stderr, "bad block_size: %s\n", value);
      return false;
    }
    return true;
  }

  if (strncmp(argument, "--block_count=", IO_ARG_BLOCK_COUNT_PREFIX_LEN) == 0) {
    const char* value = argument + IO_ARG_BLOCK_COUNT_PREFIX_LEN;
    if (!parse_size_t(value, &config->block_count)) {
      safe_fprintf(stderr, "bad block_count: %s\n", value);
      return false;
    }
    return true;
  }

  if (strncmp(argument, "--file=", IO_ARG_FILE_PREFIX_LEN) == 0) {
    config->file_path = argument + IO_ARG_FILE_PREFIX_LEN;
    return true;
  }

  if (strncmp(argument, "--range=", IO_ARG_RANGE_PREFIX_LEN) == 0) {
    const char* value = argument + IO_ARG_RANGE_PREFIX_LEN;
    if (!parse_range(value, &config->range_start, &config->range_end)) {
      safe_fprintf(stderr, "bad range: %s\n", value);
      return false;
    }
    return true;
  }

  if (strncmp(argument, "--direct=", IO_ARG_DIRECT_PREFIX_LEN) == 0) {
    const char* value = argument + IO_ARG_DIRECT_PREFIX_LEN;
    return handle_direct_argument(value, config);
  }

  if (strncmp(argument, "--type=", IO_ARG_TYPE_PREFIX_LEN) == 0) {
    const char* value = argument + IO_ARG_TYPE_PREFIX_LEN;
    return handle_type_argument(value, config);
  }

  if (strncmp(argument, "--backend=", IO_ARG_BACKEND_PREFIX_LEN) == 0) {
    const char* value = argument + IO_ARG_BACKEND_PREFIX_LEN;
    if (!parse_backend(value, &config->use_vtpc)) {
      safe_fprintf(stderr, "bad backend: %s (use libc|vtpc)\n", value);
      return false;
    }
    config->backend_set = true;
    return true;
  }

  if (strncmp(argument, "--cache=", IO_ARG_CACHE_PREFIX_LEN) == 0) {
    const char* value = argument + IO_ARG_CACHE_PREFIX_LEN;
    if (!parse_cache(value, config)) {
      safe_fprintf(stderr, "bad cache: %s (use none|os|vtpc)\n", value);
      return false;
    }
    return true;
  }

  safe_fprintf(stderr, "unknown argument: %s\n", argument);
  return false;
}

// разбор всех аргументов и заполнение конфига
static bool parse_args(
    int argument_count, char** argument_values, config_t* config
) {
  if (config == NULL) {
    return false;
  }

  config->operation_mode = MODE_READ;
  config->block_size = 0U;
  config->block_count = 0U;
  config->file_path = NULL;
  config->range_start = 0;
  config->range_end = 0;
  config->use_direct = false;
  config->selection_mode = SEL_SEQ;
  config->use_vtpc = false;
  config->cache_mode = CACHE_UNSPEC;
  config->backend_set = false;
  config->direct_set = false;

  for (int argument_index = 1; argument_index < argument_count;
       ++argument_index) {
    if (!process_single_argument(argument_values[argument_index], config)) {
      return false;
    }
  }

  if (config->file_path == NULL || config->block_size == 0U ||
      config->block_count == 0U) {
    return false;
  }

  if (config->cache_mode != CACHE_UNSPEC) {
    if (config->backend_set || config->direct_set) {
      safe_fprintf(stderr, "warning: cache overrides backend and direct\n");
    }

    if (config->cache_mode == CACHE_NONE) {
      config->use_vtpc = false;
      config->use_direct = true;
    } else if (config->cache_mode == CACHE_OS) {
      config->use_vtpc = false;
      config->use_direct = false;
    } else {
      config->use_vtpc = true;
      config->use_direct = true;
    }
  }

  return true;
}

// получение размера файла через stat
static bool get_path_size(const char* path_string, off_t* output_size) {
  if (path_string == NULL || output_size == NULL) {
    return false;
  }
  struct stat file_stat;
  if (stat(path_string, &file_stat) != 0) {
    return false;
  }
  *output_size = file_stat.st_size;
  return true;
}

// выбор случайного смещения по блокам
static off_t choose_random_offset(
    off_t start_offset,
    off_t end_offset,
    size_t block_size,
    uint32_t* random_state
) {
  const off_t length = end_offset - start_offset;
  const off_t block_total = length / (off_t)block_size;
  if (block_total <= 0) {
    return start_offset;
  }

  const uint32_t random_value = lcg_next(random_state);
  const off_t index_value = (off_t)(random_value % (uint32_t)block_total);
  return start_offset + (off_t)block_size * index_value;
}

// переход на нужное смещение
static bool seek_to_offset(
    const io_backend_ops_t* backend, int file_descriptor, off_t offset_value
) {
  if (backend == NULL) {
    return false;
  }
  const off_t new_position =
      backend->lseek_fn(file_descriptor, offset_value, SEEK_SET);
  if (new_position < 0) {
    return false;
  }
  return true;
}

// расчет следующего смещения по режиму sequence random
static off_t calculate_offset(
    const config_t* config, off_t* current_offset, uint32_t* random_state
) {
  if (config->selection_mode == SEL_SEQ) {
    off_t offset_value = *current_offset;
    *current_offset += (off_t)config->block_size;
    if (config->range_end != 0 &&
        (*current_offset + (off_t)config->block_size > config->range_end)) {
      *current_offset = config->range_start;
    }
    return offset_value;
  }

  if (config->range_end == 0) {
    return config->range_start;
  }

  return choose_random_offset(
      config->range_start, config->range_end, config->block_size, random_state
  );
}

// выполнение одного read или write
static bool perform_single_io_operation(
    const config_t* config,
    const io_backend_ops_t* backend,
    int file_descriptor,
    void* buffer,
    off_t offset_value
) {
  if (!seek_to_offset(backend, file_descriptor, offset_value)) {
    perror("lseek");
    return false;
  }

  ssize_t bytes_processed = 0;
  if (config->operation_mode == MODE_READ) {
    bytes_processed =
        backend->read_fn(file_descriptor, buffer, config->block_size);
  } else {
    bytes_processed =
        backend->write_fn(file_descriptor, buffer, config->block_size);
  }

  if (bytes_processed < 0) {
    perror((config->operation_mode == MODE_READ) ? "read" : "write");
    return false;
  }

  if ((size_t)bytes_processed != config->block_size) {
    safe_fprintf(
        stderr,
        "short %s at offset %jd: got %zd, expected %zu\n",
        (config->operation_mode == MODE_READ) ? "read" : "write",
        (intmax_t)offset_value,
        bytes_processed,
        config->block_size
    );
    return false;
  }

  return true;
}

// проверка ограничений для o direct
static bool validate_direct_requirements(const config_t* config) {
  if (!config->use_direct) {
    return true;
  }

#ifndef O_DIRECT
  safe_fprintf(stderr, "O_DIRECT is not supported on this platform\n");
  return false;
#else
  if ((config->block_size % IO_DIRECT_MIN_BLOCK) != 0U) {
    safe_fprintf(
        stderr,
        "block_size must be multiple of %d for O_DIRECT\n",
        IO_DIRECT_MIN_BLOCK
    );
    return false;
  }
  if ((config->range_start % (off_t)IO_DIRECT_MIN_BLOCK) != 0) {
    safe_fprintf(stderr, "range start must be aligned for O_DIRECT\n");
    return false;
  }
  return true;
#endif
}

// выделение буфера с учетом выравнивания
static bool allocate_buffer(const config_t* config, void** output_buffer) {
  if (config == NULL || output_buffer == NULL) {
    return false;
  }

  if (config->use_direct) {
    void* aligned_buffer = NULL;
    const int align_result =
        posix_memalign(&aligned_buffer, IO_ALIGN, config->block_size);
    if (align_result != 0 || aligned_buffer == NULL) {
      safe_fprintf(stderr, "posix_memalign failed, rc=%d\n", align_result);
      return false;
    }
    *output_buffer = aligned_buffer;
    return true;
  }

  void* buffer = malloc(config->block_size);
  if (buffer == NULL) {
    perror("malloc");
    return false;
  }
  *output_buffer = buffer;
  return true;
}

// нормализация диапазона для режима random
static void normalize_range(config_t* config) {
  if (config->operation_mode == MODE_READ) {
    off_t file_size = 0;
    if (get_path_size(config->file_path, &file_size)) {
      if (config->range_end == 0 || config->range_end > file_size) {
        config->range_end = file_size;
      }
    }
  }

  if (config->selection_mode != SEL_RAND) {
    return;
  }

  if (config->range_end != 0) {
    return;
  }

  if (config->operation_mode != MODE_WRITE) {
    return;
  }

  off_t file_size = 0;
  if (get_path_size(config->file_path, &file_size) && file_size > 0) {
    config->range_end = file_size;
    return;
  }

  config->range_end = config->range_start +
                      (off_t)config->block_size * (off_t)config->block_count;
}

// проверка диапазона для чтения
static bool validate_read_range(const config_t* config) {
  if (config->operation_mode != MODE_READ) {
    return true;
  }

  off_t file_size = 0;
  if (!get_path_size(config->file_path, &file_size)) {
    return true;
  }

  if (config->range_start >= file_size) {
    safe_fprintf(stderr, "range start is beyond file size\n");
    return false;
  }

  if (config->range_end != 0 &&
      config->range_start + (off_t)config->block_size > config->range_end) {
    safe_fprintf(stderr, "range is smaller than one block\n");
    return false;
  }

  return true;
}

// основной цикл io операций
static void perform_io_operations(
    const config_t* config,
    const io_backend_ops_t* backend,
    int file_descriptor,
    void* buffer
) {
  uint32_t random_state = (uint32_t)time(NULL);
  off_t current_offset = config->range_start;

  for (size_t block_index = 0; block_index < config->block_count;
       ++block_index) {
    off_t offset_value =
        calculate_offset(config, &current_offset, &random_state);

    if (!perform_single_io_operation(
            config, backend, file_descriptor, buffer, offset_value
        )) {
      break;
    }
  }
}

// точка входа и запуск сценария нагрузки
int main(int argument_count, char** argument_values) {
  config_t config;
  if (!parse_args(argument_count, argument_values, &config)) {
    print_usage(argument_values[0]);
    return EXIT_FAILURE;
  }

  normalize_range(&config);

  if (!validate_read_range(&config)) {
    return EXIT_FAILURE;
  }

  if (!validate_direct_requirements(&config)) {
    return EXIT_FAILURE;
  }

  const io_backend_ops_t* backend = io_backend_get(config.use_vtpc);
  if (backend == NULL) {
    safe_fprintf(stderr, "failed to select backend\n");
    return EXIT_FAILURE;
  }

  unsigned int open_flags =
      (config.operation_mode == MODE_READ) ? O_RDONLY : (O_WRONLY | O_CREAT);

#ifdef O_DIRECT
  if (config.use_direct) {
    open_flags |= O_DIRECT;
  }
#endif

  const int file_descriptor =
      backend->open_fn(config.file_path, (int)open_flags, (int)IO_DEFAULT_MODE);
  if (file_descriptor < 0) {
    perror("open");
    return EXIT_FAILURE;
  }

  void* buffer = NULL;
  if (!allocate_buffer(&config, &buffer)) {
    (void)backend->close_fn(file_descriptor);
    return EXIT_FAILURE;
  }

  if (config.operation_mode == MODE_WRITE) {
    memset(buffer, IO_WRITE_PATTERN, config.block_size);
  }

  perform_io_operations(&config, backend, file_descriptor, buffer);

  if (config.operation_mode == MODE_WRITE) {
    if (backend->fsync_fn(file_descriptor) != 0) {
      perror("fsync");
    }
  }

  free(buffer);

  if (backend->close_fn(file_descriptor) != 0) {
    perror("close");
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
