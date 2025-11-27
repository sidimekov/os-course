#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

enum { IO_ALIGN = 4096, IO_DIRECT_MIN_BLOCK = 512 };

static const int IO_NUM_BASE = 10;
static const mode_t IO_DEFAULT_MODE = 0666;
static const unsigned char IO_WRITE_PATTERN = 0xAB;

static const size_t IO_ARG_RW_PREFIX_LEN = 5U;
static const size_t IO_ARG_BLOCK_SIZE_PREFIX_LEN = 13U;
static const size_t IO_ARG_BLOCK_COUNT_PREFIX_LEN = 14U;
static const size_t IO_ARG_FILE_PREFIX_LEN = 7U;
static const size_t IO_ARG_RANGE_PREFIX_LEN = 8U;
static const size_t IO_ARG_DIRECT_PREFIX_LEN = 9U;
static const size_t IO_ARG_TYPE_PREFIX_LEN = 7U;

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
} config_t;

/* простой детерминированный LCG */
static uint32_t lcg_next(uint32_t* random_state) {
  const uint32_t multiplier = 1664525U;
  const uint32_t increment = 1013904223U;
  *random_state = multiplier * (*random_state) + increment;
  return *random_state;
}

static void print_usage(const char* program_name) {
  if (fprintf(
          stderr,
          "Usage: %s "
          "--rw=read|write "
          "--block_size=N "
          "--block_count=N "
          "--file=PATH "
          "[--range=START-END] "
          "[--direct=on|off] "
          "[--type=sequence|random]\n",
          program_name
      ) < 0) {
    perror("fprintf");
  }
}

static bool parse_range(const char* string, off_t* range_start, off_t* range_end) {
  if (string == NULL || range_start == NULL || range_end == NULL) {
    return false;
  }

  const char* dash_position = strchr(string, '-');
  if (dash_position == NULL) {
    return false;
  }

  char* local_end_pointer = NULL;
  errno = 0;
  long long start_value = strtoll(string, &local_end_pointer, IO_NUM_BASE);
  if (errno != 0 || local_end_pointer != dash_position) {
    return false;
  }

  errno = 0;
  long long end_value = strtoll(dash_position + 1, &local_end_pointer, IO_NUM_BASE);
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
  unsigned long long parsed_value = strtoull(string, &end_pointer, IO_NUM_BASE);
  if (errno != 0 || end_pointer == string || *end_pointer != '\0') {
    return false;
  }
  *output_value = (size_t)parsed_value;
  return true;
}

static bool parse_args(int argument_count, char** argument_values, config_t* config) {
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

  for (int argument_index = 1; argument_index < argument_count; ++argument_index) {
    const char* argument = argument_values[argument_index];
    if (strncmp(argument, "--rw=", IO_ARG_RW_PREFIX_LEN) == 0) {
      const char* value = argument + IO_ARG_RW_PREFIX_LEN;
      if (strcmp(value, "read") == 0) {
        config->operation_mode = MODE_READ;
      } else if (strcmp(value, "write") == 0) {
        config->operation_mode = MODE_WRITE;
      } else {
        if (fprintf(stderr, "unknown rw mode: %s\n", value) < 0) {
          perror("fprintf");
        }
        return false;
      }
    } else if (strncmp(argument, "--block_size=", IO_ARG_BLOCK_SIZE_PREFIX_LEN) == 0) {
      const char* value = argument + IO_ARG_BLOCK_SIZE_PREFIX_LEN;
      if (!parse_size_t(value, &config->block_size)) {
        if (fprintf(stderr, "bad block_size: %s\n", value) < 0) {
          perror("fprintf");
        }
        return false;
      }
    } else if (strncmp(argument, "--block_count=", IO_ARG_BLOCK_COUNT_PREFIX_LEN) == 0) {
      const char* value = argument + IO_ARG_BLOCK_COUNT_PREFIX_LEN;
      if (!parse_size_t(value, &config->block_count)) {
        if (fprintf(stderr, "bad block_count: %s\n", value) < 0) {
          perror("fprintf");
        }
        return false;
      }
    } else if (strncmp(argument, "--file=", IO_ARG_FILE_PREFIX_LEN) == 0) {
      config->file_path = argument + IO_ARG_FILE_PREFIX_LEN;
    } else if (strncmp(argument, "--range=", IO_ARG_RANGE_PREFIX_LEN) == 0) {
      const char* value = argument + IO_ARG_RANGE_PREFIX_LEN;
      if (!parse_range(value, &config->range_start, &config->range_end)) {
        if (fprintf(stderr, "bad range: %s\n", value) < 0) {
          perror("fprintf");
        }
        return false;
      }
    } else if (strncmp(argument, "--direct=", IO_ARG_DIRECT_PREFIX_LEN) == 0) {
      const char* value = argument + IO_ARG_DIRECT_PREFIX_LEN;
      if (strcmp(value, "on") == 0) {
        config->use_direct = true;
      } else if (strcmp(value, "off") == 0) {
        config->use_direct = false;
      } else {
        if (fprintf(stderr, "bad direct value: %s\n", value) < 0) {
          perror("fprintf");
        }
        return false;
      }
    } else if (strncmp(argument, "--type=", IO_ARG_TYPE_PREFIX_LEN) == 0) {
      const char* value = argument + IO_ARG_TYPE_PREFIX_LEN;
      if (strcmp(value, "sequence") == 0) {
        config->selection_mode = SEL_SEQ;
      } else if (strcmp(value, "random") == 0) {
        config->selection_mode = SEL_RAND;
      } else {
        if (fprintf(stderr, "bad type: %s\n", value) < 0) {
          perror("fprintf");
        }
        return false;
      }
    } else {
      if (fprintf(stderr, "unknown argument: %s\n", argument) < 0) {
        perror("fprintf");
      }
      return false;
    }
  }

  if (config->file_path == NULL || config->block_size == 0U ||
      config->block_count == 0U) {
    return false;
  }

  return true;
}

static bool get_file_size(int file_descriptor, off_t* output_size) {
  if (output_size == NULL) {
    return false;
  }
  struct stat file_stat;
  if (fstat(file_descriptor, &file_stat) != 0) {
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
  /* end_offset > start_offset is expected */
  const off_t length = end_offset - start_offset;
  const off_t block_total = length / (off_t)block_size;
  if (block_total <= 0) {
    return start_offset;
  }

  const uint32_t random_value = lcg_next(random_state);
  const off_t index = (off_t)(random_value % (uint32_t)block_total);
  return start_offset + (off_t)block_size * index;
}

int main(int argument_count, char** argument_values) {
  config_t config;
  if (!parse_args(argument_count, argument_values, &config)) {
    print_usage(argument_values[0]);
    return EXIT_FAILURE;
  }

  int open_flags =
      (config.operation_mode == MODE_READ) ? O_RDONLY : (O_WRONLY | O_CREAT);
#ifdef O_DIRECT
  if (config.use_direct) {
    open_flags |= O_DIRECT;
  }
#else
  if (config.use_direct) {
    if (fprintf(stderr, "O_DIRECT is not supported on this platform\n") < 0) {
      perror("fprintf");
    }
    return EXIT_FAILURE;
  }
#endif

  const int file_descriptor = open(config.file_path, open_flags, IO_DEFAULT_MODE);
  if (file_descriptor < 0) {
    perror("open");
    return EXIT_FAILURE;
  }

  if (config.operation_mode == MODE_READ && config.range_end == 0) {
    off_t file_size = 0;
    if (!get_file_size(file_descriptor, &file_size)) {
      perror("fstat");
      (void)close(file_descriptor);
      return EXIT_FAILURE;
    }
    config.range_end = file_size;
  }

  void* buffer = NULL;
  if (config.use_direct) {
    if ((config.block_size % IO_DIRECT_MIN_BLOCK) != 0U) {
      if (fprintf(
              stderr,
              "block_size must be multiple of %d for O_DIRECT\n",
              IO_DIRECT_MIN_BLOCK
          ) < 0) {
        perror("fprintf");
      }
      (void)close(file_descriptor);
      return EXIT_FAILURE;
    }
    void* local_buffer = NULL;
    const int posix_return_code = posix_memalign(&local_buffer, IO_ALIGN, config.block_size);
    if (posix_return_code != 0 || local_buffer == NULL) {
      if (fprintf(
              stderr,
              "posix_memalign failed, rc=%d\n",
              posix_return_code
          ) < 0) {
        perror("fprintf");
      }
      (void)close(file_descriptor);
      return EXIT_FAILURE;
    }
    buffer = local_buffer;
  } else {
    buffer = malloc(config.block_size);
    if (buffer == NULL) {
      perror("malloc");
      (void)close(file_descriptor);
      return EXIT_FAILURE;
    }
  }

  if (config.operation_mode == MODE_WRITE) {
    memset(buffer, IO_WRITE_PATTERN, config.block_size);
  }

  /* инициализация LCG state */
  uint32_t random_state = (uint32_t)time(NULL);

  off_t current_offset = config.range_start;

  for (size_t block_index = 0; block_index < config.block_count; ++block_index) {
    off_t offset = 0;
    if (config.selection_mode == SEL_SEQ) {
      offset = current_offset;
      current_offset += (off_t)config.block_size;
      if (config.range_end != 0 &&
          (current_offset + (off_t)config.block_size > config.range_end)) {
        current_offset = config.range_start;
      }
    } else {
      if (config.range_end == 0) {
        /* если верха нет - остаёмся в начале диапазона */
        offset = config.range_start;
      } else {
        offset = choose_random_offset(
            config.range_start, config.range_end, config.block_size, &random_state
        );
      }
    }

    ssize_t bytes_processed = 0;
    if (config.operation_mode == MODE_READ) {
      bytes_processed = pread(file_descriptor, buffer, config.block_size, offset);
    } else {
      bytes_processed = pwrite(file_descriptor, buffer, config.block_size, offset);
    }

    if (bytes_processed < 0) {
      perror((config.operation_mode == MODE_READ) ? "pread" : "pwrite");
      break;
    }

    if ((size_t)bytes_processed != config.block_size) {
      if (fprintf(
              stderr,
              "short %s at offset %jd: got %zd, expected %zu\n",
              (config.operation_mode == MODE_READ) ? "read" : "write",
              (intmax_t)offset,
              bytes_processed,
              config.block_size
          ) < 0) {
        perror("fprintf");
      }
      break;
    }
  }

  free(buffer);
  (void)close(file_descriptor);
  return EXIT_SUCCESS;
}
