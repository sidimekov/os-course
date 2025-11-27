// однопоточная программа-нагрузчик: строит линейную регрессию y = a x + b
// по сгенерированным точкам

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LINREG_DEFAULT_RANDOM_SEED = 42 };
enum { LINREG_STR_TO_NUM_BASE = 10 };
static const double kLinregEps = 1e-12;

// для генерации рандомных чисел
static const uint32_t kLcgA = 1664525U;
static const uint32_t kLcgC = 1013904223U;

// шаг смещения seed между повторами
static const uint32_t kSeedStep = 22695477U;

// структура с параметрами
typedef struct {
  int64_t n;
  int64_t xmin;
  int64_t xmax;
  int64_t ymin;
  int64_t ymax;
  uint32_t seed;
  int repeat;
  bool quiet;
} LinRegConfig;

static void print_usage(const char* prog) {
  int ret = fprintf(
      stderr,
      "usage:\n"
      "  %s --n N --xmin X0 --xmax X1 --ymin Y0 --ymax Y1 "
      "[--seed S] [--repeat R] [--quiet on|off]\n\n"
      "params:\n"
      "  --n        int    number of points (>=2)\n"
      "  --xmin     int    min x\n"
      "  --xmax     int    max x (>= xmin)\n"
      "  --ymin     int    min y\n"
      "  --ymax     int    max y (>= ymin)\n"
      "  --seed     uint   rng seed (default 42)\n"
      "  --repeat   int    repeats (>=1, default 1)\n"
      "  --quiet    on|off less output (default off)\n",
      prog
  );
  if (ret < 0) {
    perror("fprintf");
  }
}

// парсинг on/off
static bool parse_onoff(const char* str, bool* out) {
  if (str == NULL || out == NULL) {
    return false;
  }
  if (strcmp(str, "on") == 0) {
    *out = true;
    return true;
  }
  if (strcmp(str, "off") == 0) {
    *out = false;
    return true;
  }
  return false;
}

// парсинг int64
static bool parse_int64(const char* str, int64_t* out) {
  if (str == NULL || out == NULL) {
    return false;
  }
  errno = 0;
  char* end = NULL;
  long long val = strtoll(str, &end, LINREG_STR_TO_NUM_BASE);
  if (errno != 0 || end == str || *end != '\0') {
    return false;
  }
  *out = (int64_t)val;
  return true;
}

// парсинг uint32
static bool parse_uint32(const char* str, uint32_t* out) {
  if (str == NULL || out == NULL) {
    return false;
  }
  errno = 0;
  char* end = NULL;
  unsigned long val = strtoul(str, &end, LINREG_STR_TO_NUM_BASE);
  if (errno != 0 || end == str || *end != '\0') {
    return false;
  }
  if (val > (unsigned long)UINT32_MAX) {
    return false;
  }
  *out = (uint32_t)val;
  return true;
}

// state = (a * state + c) mod 2^32
static inline uint32_t lcg_next(uint32_t* state) {
  if (state == NULL) {
    return 0U;
  }
  uint32_t lcg_state = *state;
  lcg_state =
      (uint32_t)((uint64_t)kLcgA * (uint64_t)lcg_state + (uint64_t)kLcgC);
  *state = lcg_state;
  return lcg_state;
}

// равномерное целое в [lo, hi] используя lcg_next
static inline int64_t urand_range(uint32_t* state, int64_t low, int64_t high) {
  if (state == NULL) {
    return low;
  }
  if (high < low) {
    int64_t tmp = low;
    low = high;
    high = tmp;
  }
  uint64_t span = (uint64_t)(high - low) + 1ULL;
  uint64_t ret = (uint64_t)lcg_next(state);
  uint64_t off = ret % span;
  return low + (int64_t)off;
}

typedef enum { V_KIND_I64, V_KIND_U32, V_KIND_ONOFF } VtshValKind;

typedef enum {
  FIELD_N,
  FIELD_XMIN,
  FIELD_XMAX,
  FIELD_YMIN,
  FIELD_YMAX,
  FIELD_SEED,
  FIELD_REPEAT,
  FIELD_QUIET
} VtshField;

typedef struct {
  const char* name;
  VtshValKind kind;
  VtshField field;
} VtshOption;

// таблица option
static const VtshOption kOptions[] = {
    {     "--n",   V_KIND_I64,      FIELD_N},
    {  "--xmin",   V_KIND_I64,   FIELD_XMIN},
    {  "--xmax",   V_KIND_I64,   FIELD_XMAX},
    {  "--ymin",   V_KIND_I64,   FIELD_YMIN},
    {  "--ymax",   V_KIND_I64,   FIELD_YMAX},
    {  "--seed",   V_KIND_U32,   FIELD_SEED},
    {"--repeat",   V_KIND_I64, FIELD_REPEAT},
    { "--quiet", V_KIND_ONOFF,  FIELD_QUIET},
};

// поиск опции по имени
static int vtsh_find_option(const char* key) {
  const size_t count = sizeof(kOptions) / sizeof(kOptions[0]);
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(key, kOptions[i].name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

// применить опцию к конфигу
static bool vtsh_apply_option(
    const VtshOption* opt, const char* val, LinRegConfig* cfg
) {
  if (opt == NULL || val == NULL || cfg == NULL) {
    return false;
  }
  switch (opt->kind) {
    case V_KIND_I64: {
      int64_t parsed = 0;
      if (!parse_int64(val, &parsed)) {
        return false;
      }
      switch (opt->field) {
        case FIELD_N:
          cfg->n = parsed;
          break;
        case FIELD_XMIN:
          cfg->xmin = parsed;
          break;
        case FIELD_XMAX:
          cfg->xmax = parsed;
          break;
        case FIELD_YMIN:
          cfg->ymin = parsed;
          break;
        case FIELD_YMAX:
          cfg->ymax = parsed;
          break;
        case FIELD_REPEAT:
          if (parsed < 1 || parsed > INT32_MAX) {
            return false;
          }
          cfg->repeat = (int)parsed;
          break;
        default:
          return false;
      }
      return true;
    }
    case V_KIND_U32: {
      uint32_t parsed = 0;
      if (!parse_uint32(val, &parsed)) {
        return false;
      }
      if (opt->field == FIELD_SEED) {
        cfg->seed = parsed;
        return true;
      }
      return false;
    }
    case V_KIND_ONOFF: {
      bool parsed = false;
      if (!parse_onoff(val, &parsed)) {
        return false;
      }
      if (opt->field == FIELD_QUIET) {
        cfg->quiet = parsed;
        return true;
      }
      return false;
    }
  }
  return false;
}

// парсинг аргументов --key value
static bool parse_args(int argc, char** argv, LinRegConfig* cfg) {
  if (cfg == NULL) {
    return false;
  }
  *cfg = (LinRegConfig){
      .n = -1,
      .xmin = 0,
      .xmax = 0,
      .ymin = 0,
      .ymax = 0,
      .seed = LINREG_DEFAULT_RANDOM_SEED,
      .repeat = 1,
      .quiet = false,
  };

  for (int i = 1; i < argc;) {
    const char* key = argv[i];
    const char* val = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (val == NULL) {
      return false;
    }
    int idx = vtsh_find_option(key);
    if (idx < 0) {
      return false;
    }
    if (!vtsh_apply_option(&kOptions[idx], val, cfg)) {
      return false;
    }
    i += 2;
  }

  if (cfg->n < 2) {
    return false;
  }
  if (cfg->xmax < cfg->xmin) {
    return false;
  }
  if (cfg->ymax < cfg->ymin) {
    return false;
  }
  return true;
}

// основной расчёт
static int run_linreg(const LinRegConfig* cfg) {
  if (cfg == NULL) {
    return 2;
  }

  for (int rep = 0; rep < cfg->repeat; ++rep) {
    uint32_t rng_state = cfg->seed + (uint32_t)rep * kSeedStep;

    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;

    for (int64_t i = 0; i < cfg->n; ++i) {
      const double x_val =
          (double)urand_range(&rng_state, cfg->xmin, cfg->xmax);
      const double y_val =
          (double)urand_range(&rng_state, cfg->ymin, cfg->ymax);
      sum_x += x_val;
      sum_y += y_val;
      sum_xx += x_val * x_val;
      sum_xy += x_val * y_val;
    }

    const double n_d = (double)cfg->n;
    const double denominator = n_d * sum_xx - sum_x * sum_x;

    double coef_a = 0.0;
    double coef_b = 0.0;

    if (fabs(denominator) < kLinregEps) {
      coef_a = 0.0;
      coef_b = sum_y / n_d;
    } else {
      coef_a = (n_d * sum_xy - sum_x * sum_y) / denominator;
      coef_b = (sum_y - coef_a * sum_x) / n_d;
    }

    if (!cfg->quiet) {
      printf(
          "iter=%d n=%lld coef_a=%.10f coef_b=%.10f\n",
          rep,
          (long long)cfg->n,
          coef_a,
          coef_b
      );
    }
  }

  return 0;
}

int main(int argc, char** argv) {
  LinRegConfig cfg;
  if (!parse_args(argc, argv, &cfg)) {
    print_usage(argv[0]);
    return 2;
  }
  const int ret_code = run_linreg(&cfg);
  return ret_code;
}
