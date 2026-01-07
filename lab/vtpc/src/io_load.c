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

typedef enum { MODE_READ = 0, MODE_WRITE = 1 } rw_mode_t;
typedef enum { SEL_SEQ = 0, SEL_RAND = 1 } sel_mode_t;

typedef struct {
  rw_mode_t operation_mode;
  size_t block_size;
  size_t block_count;
  const char* file_path;
  off_t range_start;
  off_t range_end; /* 0 -> no upper limit */
  bool use_direct;
  sel_mode_t selection_mode;
  bool use_vtpc;
} config_t;

static void safe_fprintf(FILE* stream, const char* format_string, ...) {
  va_list args;
  va_start(args, format_string);
  const int ret_code = vfprintf(stream, format_string, args);
  va_end(args);
  if (ret_code < 0) {
    perror("vfprintf");
  }
}

static uint32_t lcg_next(uint32_t* random_state) {
  *random_state = IO_LCG_MULTIPLIER * (*random_state) + IO_LCG_INCREMENT;
  return *random_state;
}

static void print_usage(const char* program_name) {
  safe_fprintf(
      stderr,
      "Usage: %s "
      "--rw=read|write "
      "--block_size=N "
      "--block_count=N "
      "--file=PATH "
      "[--range=START-END] "
      "[--direct=on|off] "
      "[--type=sequence|random] "
      "[--backend=libc|vtpc]\n",
      program_name
  );
}

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
  if (errno != 0 || end_value < 0) {
    return false;
  }

  if (start_value < 0) {
    return false;
  }

  *range_start = (off_t)start_value;
  *range_end = (off_t)end_value;
  return true;
}

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

static bool handle_direct_argument(const char* value, config_t* config) {
  if (strcmp(value, "on") == 0) {
    config->use_direct = true;
    return true;
  }
  if (strcmp(value, "off") == 0) {
    config->use_direct = false;
    return true;
  }
  safe_fprintf(stderr, "bad direct value: %s\n", value);
  return false;
}

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
    return true;
  }

  safe_fprintf(stderr, "unknown argument: %s\n", argument);
  return false;
}

static bool parse_args(
    int argument_count, char** argument_values, config_t* config
) {
  if (config == NULL) {
    return false;
  }

  /* defaults */
  config->operation_mode = MODE_READ;
  config->block_size = 0U;
  config->block_count = 0U;
  config->file_path = NULL;
  config->range_start = 0;
  config->range_end = 0;
  config->use_direct = false;
  config->selection_mode = SEL_SEQ;
  config->use_vtpc = false;

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
  return true;
}

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

static off_t calculate_offset(
    const config_t* config,
    off_t* current_offset,
    uint32_t* random_state
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

static bool perform_single_io_operation(
    const config_t* config,
    const io_backend_ops_t* backend,
    int file_descriptor,
    void* buffer,
    off_t offset_value
) {
  if (!seek_to_offset(backend, file_descriptor, offset_value)) {
    perror("lseek/vtpc_lseek");
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
    perror(
        (config->operation_mode == MODE_READ) ? "read/vtpc_read"
                                              : "write/vtpc_write"
    );
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

static bool allocate_buffer(
    const config_t* config,
    const io_backend_ops_t* backend,
    int file_descriptor,
    void** output_buffer
) {
  const bool need_aligned_buffer = (config->use_direct && !config->use_vtpc);
  if (need_aligned_buffer) {
    if ((config->block_size % IO_DIRECT_MIN_BLOCK) != 0U) {
      safe_fprintf(
          stderr,
          "block_size must be multiple of %d for O_DIRECT\n",
          IO_DIRECT_MIN_BLOCK
      );
      (void)backend->close_fn(file_descriptor);
      return false;
    }

    void* aligned_buffer = NULL;
    const int align_result =
        posix_memalign(&aligned_buffer, IO_ALIGN, config->block_size);
    if (align_result != 0 || aligned_buffer == NULL) {
      safe_fprintf(stderr, "posix_memalign failed, rc=%d\n", align_result);
      (void)backend->close_fn(file_descriptor);
      return false;
    }
    *output_buffer = aligned_buffer;
  } else {
    void* buffer = malloc(config->block_size);
    if (buffer == NULL) {
      perror("malloc");
      (void)backend->close_fn(file_descriptor);
      return false;
    }
    *output_buffer = buffer;
  }
  return true;
}

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

int main(int argument_count, char** argument_values) {
  config_t config;
  if (!parse_args(argument_count, argument_values, &config)) {
    print_usage(argument_values[0]);
    return EXIT_FAILURE;
  }

  const io_backend_ops_t* backend = io_backend_get(config.use_vtpc);
  if (backend == NULL) {
    safe_fprintf(stderr, "failed to select backend\n");
    return EXIT_FAILURE;
  }

  unsigned int open_flags =
      (config.operation_mode == MODE_READ) ? O_RDONLY : (O_WRONLY | O_CREAT);

  if (config.use_direct && config.use_vtpc) {
    safe_fprintf(stderr, "warning: --direct is ignored for vtpc backend\n");
  }

#ifdef O_DIRECT
  if (config.use_direct && !config.use_vtpc) {
    open_flags |= O_DIRECT;
  }
#else
  if (config.use_direct && !config.use_vtpc) {
    safe_fprintf(stderr, "O_DIRECT is not supported on this platform\n");
    return EXIT_FAILURE;
  }
#endif

  if (config.operation_mode == MODE_READ && config.range_end == 0) {
    off_t file_size = 0;
    if (!get_path_size(config.file_path, &file_size)) {
      perror("stat");
      return EXIT_FAILURE;
    }
    config.range_end = file_size;
  }

  const int file_descriptor =
      backend->open_fn(config.file_path, (int)open_flags, (int)IO_DEFAULT_MODE);
  if (file_descriptor < 0) {
    perror("open/vtpc_open");
    return EXIT_FAILURE;
  }

  void* buffer = NULL;
  if (!allocate_buffer(&config, backend, file_descriptor, &buffer)) {
    return EXIT_FAILURE;
  }

  if (config.operation_mode == MODE_WRITE) {
    memset(buffer, IO_WRITE_PATTERN, config.block_size);
  }

  perform_io_operations(&config, backend, file_descriptor, buffer);

  if (config.operation_mode == MODE_WRITE) {
    if (backend->fsync_fn(file_descriptor) != 0) {
      perror("fsync/vtpc_fsync");
    }
  }

  free(buffer);
  if (backend->close_fn(file_descriptor) != 0) {
    perror("close/vtpc_close");
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
