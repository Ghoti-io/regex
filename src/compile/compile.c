/**
 * @file
 *
 * The compiler: a parsed pattern to a program, plus the public entry points
 * that own a compiled regex.
 *
 * Status: stub. grx_compile_program() reports GRX_ERR_UNSUPPORTED; the
 * accessors, the dump and the free path are written against the struct in
 * compile_internal.h and work as soon as it is populated.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/compile.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/pattern.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../core/core_internal.h"
#include "compile_internal.h"

GRX_Result grx_compile_program(const GRX_Pattern * pattern,
    const GRX_Limits * limits, const GRX_Allocator * allocator,
    GRX_Error * out_error, GRX_Regex ** out_regex) {
  (void)pattern;
  (void)limits;
  (void)allocator;

  if (!out_regex) {
    return GRX_ERR_INVALID;
  }

  return grx_error_set(out_error, GRX_ERR_UNSUPPORTED,
      GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, GRX_NPOS, 0);
}

void grx_facts_init(GRX_Facts * facts) {
  if (!facts) {
    return;
  }

  // Every field is the value that claims nothing. A caller must be able to
  // read facts from a regex that analysis has not touched and be misled by
  // none of them, so "regular" is false, the length bounds are as wide as
  // they go, and no prefilter is offered.
  *facts = (GRX_Facts) {
    .is_regular = 0,
    .anchored_start = 0,
    .anchored_end = 0,
    .can_match_empty = 1,
    .has_backreference = 0,
    .has_lookaround = 0,
    .has_recursion = 0,
    .has_duplicate_names = 0,
    .min_length = 0,
    .max_length = GRX_NPOS,
    .max_lookbehind = 0,
    .capture_count = 0,
    .program_size = 0,
    .literal_prefix = NULL,
    .literal_prefix_length = 0,
    .required_literal = NULL,
    .required_literal_length = 0,
    .first_bytes_known = 0,
    .first_bytes = {0},
  };
}

GRX_Result grx_regex_facts(const GRX_Regex * regex, GRX_Facts * out_facts) {
  if (!regex || !out_facts) {
    return GRX_ERR_INVALID;
  }

  *out_facts = regex->facts;
  return GRX_OK;
}

GRX_Result grx_regex_compile(const char * pattern, GRX_Syntax syntax,
    uint32_t options, GRX_Regex ** out_regex) {
  if (!pattern) {
    return GRX_ERR_INVALID;
  }

  return grx_regex_compile_with_allocator(pattern, strlen(pattern), syntax,
      options, NULL, NULL, NULL, out_regex);
}

GRX_Result grx_regex_compile_with_allocator(const char * pattern,
    size_t length, GRX_Syntax syntax, uint32_t options,
    const GRX_Limits * limits, const GRX_Allocator * allocator,
    GRX_Error * out_error, GRX_Regex ** out_regex) {
  if (!out_regex) {
    return GRX_ERR_INVALID;
  }
  *out_regex = NULL;
  grx_error_clear(out_error);

  GRX_Limits defaults;
  if (!limits) {
    grx_limits_default(&defaults);
    limits = &defaults;
  }
  if (!allocator) {
    allocator = grx_allocator_default();
  }

  GRX_Pattern * parsed = NULL;
  GRX_Result result = grx_pattern_parse_with_allocator(pattern, length, syntax,
      options, limits, allocator, out_error, &parsed);
  if (result != GRX_OK) {
    return result;
  }

  result = grx_compile_program(parsed, limits, allocator, out_error, out_regex);
  grx_pattern_free(parsed);
  return result;
}

GRX_Result grx_regex_compile_pattern(const GRX_Pattern * pattern,
    const GRX_Limits * limits, const GRX_Allocator * allocator,
    GRX_Error * out_error, GRX_Regex ** out_regex) {
  if (!pattern || !out_regex) {
    return GRX_ERR_INVALID;
  }
  *out_regex = NULL;
  grx_error_clear(out_error);

  GRX_Limits defaults;
  if (!limits) {
    grx_limits_default(&defaults);
    limits = &defaults;
  }
  if (!allocator) {
    allocator = grx_allocator_default();
  }

  return grx_compile_program(pattern, limits, allocator, out_error, out_regex);
}

size_t grx_regex_capture_count(const GRX_Regex * regex) {
  return regex ? regex->capture_count : 0;
}

const char * grx_regex_capture_name(const GRX_Regex * regex, size_t index) {
  if (!regex || !regex->capture_names || index == 0
      || index > regex->capture_count) {
    return NULL;
  }

  return regex->capture_names[index - 1];
}

GRX_Result grx_regex_capture_index(
    const GRX_Regex * regex, const char * name, size_t * out_index) {
  if (!regex || !name || !out_index) {
    return GRX_ERR_INVALID;
  }
  if (!regex->capture_names) {
    return GRX_ERR_INVALID;
  }

  for (size_t i = 0; i < regex->capture_count; i++) {
    const char * candidate = regex->capture_names[i];
    if (candidate && strcmp(candidate, name) == 0) {
      *out_index = i + 1;
      return GRX_OK;
    }
  }

  return GRX_ERR_INVALID;
}

GRX_Syntax grx_regex_syntax(const GRX_Regex * regex) {
  return regex ? regex->syntax : GRX_SYNTAX_COUNT;
}

size_t grx_regex_program_size(const GRX_Regex * regex) {
  return regex ? regex->program.insts.count : 0;
}

GRX_Result grx_regex_dump(const GRX_Regex * regex, FILE * out) {
  if (!regex || !out) {
    return GRX_ERR_INVALID;
  }

  fprintf(out, "regex: syntax=%s options=0x%08x captures=%zu regular=%s\n",
      grx_syntax_name(regex->syntax), regex->options, regex->capture_count,
      regex->facts.is_regular ? "yes" : "no");

  return grx_program_dump(&regex->program, out);
}

void grx_regex_free(GRX_Regex * regex) {
  if (!regex) {
    return;
  }

  const GRX_Allocator * allocator = regex->allocator;
  if (!allocator) {
    allocator = grx_allocator_default();
  }
  if (regex->capture_names) {
    for (size_t i = 0; i < regex->capture_count; i++) {
      gcu_allocator_free(allocator, regex->capture_names[i]);
    }
    gcu_allocator_free(allocator, regex->capture_names);
  }
  grx_program_clear(&regex->program);
  gcu_allocator_free(allocator, regex);
}
