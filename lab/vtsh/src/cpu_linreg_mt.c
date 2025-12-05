// Многопоточный нагрузчик: вычисление линейной регрессии y = a x + b
// Каждый поток обрабатывает свою часть точек
// Использование:
//   ./cpu-linreg-mt --threads=12 --n=1000000 --repeat=250

#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { LINREG_DEFAULT_RANDOM_SEED = 42, LINREG_STR_TO_NUM_BASE = 10 };

static const double kLinregEps = 1e-12;
static const double kInv2Pow32 =
    1.0 / 4294967296.0;
static const long long kIntArgMax = 1000000LL; 

// для генерации псевдослучайных чисел
static const uint32_t kLcgA = 1664525U;
static const uint32_t kLcgC = 1013904223U;

// шаг смещения seed между потоками
static const uint32_t kSeedStep = 22695477U;

typedef struct {
  long long n;       // число точек на поток
  long long repeat;  // число повторов
  double xmin, xmax;
  double ymin, ymax;
  uint32_t seed;  // начальный seed для LCG
} ThreadParams;

static uint32_t lcg_next(uint32_t* state) {
  // unsigned overflow по стандарту определён // cert-int30-c
  *state = kLcgA * (*state) + kLcgC;
  return *state;
}

// генерируем x в [xmin, xmax], y в [ymin, ymax] (равномерно)
static void generate_point(
    const ThreadParams* prms, uint32_t* state, double* x_out, double* y_out
) {
  const uint32_t rx = lcg_next(state);
  const uint32_t ry = lcg_next(state);
  const double ux = rx * kInv2Pow32;
  const double uy = ry * kInv2Pow32;
  *x_out = prms->xmin + (prms->xmax - prms->xmin) * ux;
  *y_out = prms->ymin + (prms->ymax - prms->ymin) * uy;
}

// функция, выполняемая каждым потоком
static void* thread_main(void* arg) {
  ThreadParams* prms = (ThreadParams*)arg;
  uint32_t state = prms->seed;

  for (long long r = 0; r < prms->repeat; ++r) {
    // аккумуляторы для сумм
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;

    for (long long i = 0; i < prms->n; ++i) {
      double x = 0.0;
      double y = 0.0;
      generate_point(prms, &state, &x, &y);
      sum_x += x;
      sum_y += y;
      sum_xx += x * x;
      sum_xy += x * y;
    }

    // коэффициенты линейной регрессии
    const double n_d = (double)prms->n;
    const double denom = n_d * sum_xx - sum_x * sum_x;
    if (fabs(denom) < kLinregEps) {
      continue;
    }
    const double a = (n_d * sum_xy - sum_x * sum_y) / denom;
    const double b = (sum_y - a * sum_x) / n_d;

    volatile double sink = a + b;
    (void)sink;
  }

  return NULL;
}

// простейший парсер long long из строк вида "--n=1000000"
static int parse_ll_arg(const char* arg, const char* prefix, long long* out) {
  const size_t len = strlen(prefix);
  if (strncmp(arg, prefix, len) != 0) {
    return 0;
  }
  const char* val = arg + len;
  char* endptr = NULL;
  errno = 0;
  long long v = strtoll(val, &endptr, LINREG_STR_TO_NUM_BASE);
  if (errno != 0 || endptr == val || *endptr != '\0' || v <= 0) {
    // cert-err33-c: проверяем код возврата fprintf
    if (fprintf(stderr, "bad value for %s: %s\n", prefix, val) < 0) {
      perror("fprintf");
    }
    return 0;
  }
  *out = v;
  return 1;
}

static int parse_int_arg(const char* arg, const char* prefix, int* out) {
  long long tmp = 0;
  if (!parse_ll_arg(arg, prefix, &tmp)) {
    return 0;
  }
  if (tmp <= 0 || tmp > kIntArgMax) {  // modernize-avoid-magic-numbers
    if (fprintf(stderr, "bad value for %s: %lld\n", prefix, tmp) < 0) {
      perror("fprintf");  // cert-err33-c
    }
    return 0;
  }

  *out = (int)tmp;
  return 1;
}

static int parse_double_arg(const char* arg, const char* prefix, double* out) {
  const size_t len = strlen(prefix);
  if (strncmp(arg, prefix, len) != 0) {
    return 0;
  }
  const char* val = arg + len;
  char* endptr = NULL;
  errno = 0;
  double v = strtod(val, &endptr);
  if (errno != 0 || endptr == val || *endptr != '\0') {
    if (fprintf(stderr, "bad value for %s: %s\n", prefix, val) < 0) {
      perror("fprintf");
    }
    return 0;
  }
  *out = v;
  return 1;
}

static void print_usage(const char* prog) {
  const int r = fprintf(
      stderr,
      "Usage: %s --threads=N --n=POINTS_PER_THREAD --repeat=R\n"
      "Optional: --xmin=VAL --xmax=VAL --ymin=VAL --ymax=VAL\n",
      prog
  );
  if (r < 0) {
    perror("fprintf");
  }
}

int main(int argc, char** argv) {
  int threads = 0;
  long long n_per_thread = 0;
  long long repeat = 1;
  double xmin = 0.0, xmax = 100.0;
  double ymin = 0.0, ymax = 100.0;

  for (int i = 1; i < argc; ++i) {
    if (parse_int_arg(argv[i], "--threads=", &threads)) {
      continue;
    }
    if (parse_ll_arg(argv[i], "--n=", &n_per_thread)) {
      continue;
    }
    if (parse_ll_arg(argv[i], "--repeat=", &repeat)) {
      continue;
    }
    if (parse_double_arg(argv[i], "--xmin=", &xmin)) {
      continue;
    }
    if (parse_double_arg(argv[i], "--xmax=", &xmax)) {
      continue;
    }
    if (parse_double_arg(argv[i], "--ymin=", &ymin)) {
      continue;
    }
    if (parse_double_arg(argv[i], "--ymax=", &ymax)) {
      continue;
    }

    if (fprintf(stderr, "unknown argument: %s\n", argv[i]) < 0) {
      perror("fprintf");
    }
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  if (threads <= 0) {
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc <= 0) {
      nproc = 1;
    }
    // узкое преобразование безопасно: nproc > 0 и разумные значения
    threads = (int)nproc;
  }

  if (n_per_thread <= 0 || repeat <= 0) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  pthread_t* tids = (pthread_t*)calloc((size_t)threads, sizeof(*tids));
  ThreadParams* params =
      (ThreadParams*)calloc((size_t)threads, sizeof(*params));
  if (tids == NULL || params == NULL) {
    perror("calloc");
    free(tids);
    free(params);
    return EXIT_FAILURE;
  }

  const uint32_t base_seed = LINREG_DEFAULT_RANDOM_SEED;

  int created = 0;
  for (int i = 0; i < threads; ++i) {
    params[i].n = n_per_thread;
    params[i].repeat = repeat;
    params[i].xmin = xmin;
    params[i].xmax = xmax;
    params[i].ymin = ymin;
    params[i].ymax = ymax;
    params[i].seed = base_seed + (uint32_t)i * kSeedStep;

    const int ret_code =
        pthread_create(&tids[i], NULL, thread_main, &params[i]);
    if (ret_code != 0) {
      if (fprintf(stderr, "pthread_create failed, ret_code=%d\n", ret_code) <
          0) {  // cert-err33-c
        perror("fprintf");
      }
      created = i;  // сколько реально создали
                    // количество
      break;
    }
    created = i + 1;
  }

  for (int i = 0; i < created; ++i) {
    const int jrc = pthread_join(tids[i], NULL);
    if (jrc != 0) {  // cert-err33-c: логируем ошибку join
      if (fprintf(stderr, "pthread_join failed, ret_code=%d\n", jrc) < 0) {
        perror("fprintf");
      }
    }
  }

  free(tids);
  free(params);
  return EXIT_SUCCESS;
}
