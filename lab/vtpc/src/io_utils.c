#include "io_utils.h"

#include <stdarg.h>

static const uint32_t LCG_MULT = 1664525U;
static const uint32_t LCG_INC = 1013904223U;

uint32_t lcg_next(uint32_t* state) {
  *state = (*state) * LCG_MULT + LCG_INC;
  return *state;
}
