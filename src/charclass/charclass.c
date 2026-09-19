/**
 * @file
 *
 * Character-class sets: sorted disjoint code-point ranges.
 *
 * Status: stub. The membership test and the free path are written, because
 * both are independent of how ranges are added; grx_charclass_add_range()
 * itself is not, since normalising a new range against the existing ones is
 * where the design decisions are.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>

#include "charclass_internal.h"

GRX_Result grx_charclass_add_range(GRX_CharClass * cls, uint32_t low,
    uint32_t high, const GRX_Limits * limits) {
  (void)limits;

  if (!cls || low > high) {
    return GRX_ERR_INVALID;
  }

  // TODO: insert in sorted order, merging with any range it touches, and
  // fail with GRX_ERR_LIMIT past limits->max_class_ranges.
  return GRX_ERR_UNSUPPORTED;
}

int grx_charclass_contains(const GRX_CharClass * cls, uint32_t codepoint) {
  if (!cls) {
    return 0;
  }

  // Binary search over the sorted ranges. The class is disjoint, so the first
  // range whose high is at or above the code point is the only candidate.
  size_t low = 0;
  size_t high = cls->count;
  int found = 0;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    if (codepoint < cls->ranges[mid].low) {
      high = mid;
    }
    else if (codepoint > cls->ranges[mid].high) {
      low = mid + 1;
    }
    else {
      found = 1;
      break;
    }
  }

  return cls->negated ? !found : found;
}

void grx_charclass_clear(GRX_CharClass * cls) {
  if (!cls) {
    return;
  }

  if (cls->ranges && cls->allocator) {
    gcu_allocator_free(cls->allocator, cls->ranges);
  }
  cls->ranges = NULL;
  cls->count = 0;
  cls->capacity = 0;
  cls->negated = 0;
}
