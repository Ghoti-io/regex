/**
 * @file
 *
 * The pattern parser: dialect text to a node tree.
 *
 * Status: stub. grx_parse_pattern() validates its arguments and then reports
 * GRX_ERR_UNSUPPORTED. It is deliberately not a partial parser: half a parser
 * accepts patterns it should reject, and a test suite written against it
 * records the half rather than the specification. The dialect specification
 * is documentation/dialects.md.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/pattern.h>
#include <stddef.h>
#include <string.h>

#include "../core/core_internal.h"
#include "parse_internal.h"

GRX_Result grx_parse_pattern(const char * pattern, size_t length,
    GRX_Syntax syntax, uint32_t options, const GRX_Limits * limits,
    const GRX_Allocator * allocator, GRX_Error * out_error,
    GRX_Pattern ** out_pattern) {
  (void)options;
  (void)allocator;

  if (!out_pattern || (!pattern && length) || !limits) {
    return GRX_ERR_INVALID;
  }
  if ((unsigned)syntax >= (unsigned)GRX_SYNTAX_COUNT) {
    return GRX_ERR_INVALID;
  }
  if (limits->max_pattern_length && length > limits->max_pattern_length) {
    return grx_error_set(out_error, GRX_ERR_LIMIT,
        GRX_DIAG_LIMIT_PATTERN_LENGTH, limits->max_pattern_length, 0);
  }

  return grx_error_set(out_error, GRX_ERR_UNSUPPORTED,
      GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, GRX_NPOS, 0);
}
