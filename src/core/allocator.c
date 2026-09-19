/**
 * @file
 *
 * The library's default allocator, which is cutil's.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/regex/allocator.h>

const GRX_Allocator * grx_allocator_default(void) {
  return gcu_allocator_default();
}
