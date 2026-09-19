/**
 * @file
 *
 * Membership in a sorted, disjoint range array.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <stddef.h>
#include <stdint.h>

#include "range_internal.h"

int grx_range_contains(
    const GRX_CharRange * ranges, size_t count, uint32_t codepoint) {
  if (!ranges || !count) {
    return 0;
  }

  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    if (codepoint < ranges[mid].low) {
      high = mid;
    }
    else if (codepoint > ranges[mid].high) {
      low = mid + 1;
    }
    else {
      return 1;
    }
  }

  return 0;
}
