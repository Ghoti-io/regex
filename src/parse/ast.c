/**
 * @file
 *
 * The public face of a parsed pattern: the wrappers that resolve defaults,
 * the accessors, the dump, and the free.
 *
 * Everything here works on whatever grx_parse_pattern() produced, so it is
 * written once and does not change as the parser grows.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/pattern.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "parse_internal.h"

GRX_Result grx_pattern_parse(const char * pattern, GRX_Syntax syntax,
    uint32_t options, GRX_Pattern ** out_pattern) {
  if (!pattern) {
    return GRX_ERR_INVALID;
  }

  return grx_pattern_parse_with_allocator(pattern, strlen(pattern), syntax,
      options, NULL, NULL, NULL, out_pattern);
}

GRX_Result grx_pattern_parse_with_allocator(const char * pattern,
    size_t length, GRX_Syntax syntax, uint32_t options,
    const GRX_Limits * limits, const GRX_Allocator * allocator,
    GRX_Error * out_error, GRX_Pattern ** out_pattern) {
  if (!out_pattern) {
    return GRX_ERR_INVALID;
  }
  *out_pattern = NULL;
  grx_error_clear(out_error);

  GRX_Limits defaults;
  if (!limits) {
    grx_limits_default(&defaults);
    limits = &defaults;
  }
  if (!allocator) {
    allocator = grx_allocator_default();
  }

  return grx_parse_pattern(pattern, length, syntax, options, limits,
      allocator, out_error, out_pattern);
}

GRX_Syntax grx_pattern_syntax(const GRX_Pattern * pattern) {
  return pattern ? pattern->syntax : GRX_SYNTAX_COUNT;
}

size_t grx_pattern_capture_count(const GRX_Pattern * pattern) {
  return pattern ? pattern->capture_count : 0;
}

size_t grx_pattern_node_count(const GRX_Pattern * pattern) {
  return pattern ? pattern->node_count : 0;
}

GRX_Result grx_pattern_dump(const GRX_Pattern * pattern, FILE * out) {
  if (!pattern || !out) {
    return GRX_ERR_INVALID;
  }

  fprintf(out, "pattern: syntax=%s options=0x%08x nodes=%zu captures=%zu\n",
      grx_syntax_name(pattern->syntax), pattern->options, pattern->node_count,
      pattern->capture_count);

  // TODO: walk the tree once the parser builds one.
  return GRX_OK;
}

void grx_pattern_free(GRX_Pattern * pattern) {
  if (!pattern) {
    return;
  }

  const GRX_Allocator * allocator = pattern->allocator;
  if (!allocator) {
    allocator = grx_allocator_default();
  }
  if (pattern->nodes) {
    gcu_allocator_free(allocator, pattern->nodes);
  }
  gcu_allocator_free(allocator, pattern);
}
