/**
 * @file
 *
 * The compiler: a parsed pattern to a program, plus the public entry points
 * that own a compiled regex.
 *
 * grx_compile_program() is the four steps of documentation/design.md
 * section 3 in order: lower the syntax tree to the IR, analyse the IR for
 * the facts, generate instructions, and copy the capture names forward. The
 * IR is freed on the way out - a compiled regex is the program and the facts,
 * and keeping the tree that produced it would be keeping a second answer to
 * every question the facts already answer.
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
#include "../ir/lower_internal.h"
#include "compile_internal.h"

/**
 * Copy the capture names out of the IR into the array the public API reads.
 *
 * One `char *` per group, NULL where the group is unnamed, so that
 * grx_regex_capture_name() is an index rather than a search.
 */
static GRX_Result copy_capture_names(GRX_Regex * regex, const GRX_IR * ir) {
  if (!regex->capture_count) {
    return GRX_OK;
  }

  regex->capture_names = gcu_allocator_calloc(
      regex->allocator, regex->capture_count, sizeof(char *));
  if (!regex->capture_names) {
    return GRX_ERR_OOM;
  }

  for (size_t i = 0; i < ir->nodes.count; i++) {
    const GRX_IRNode * node = grx_ir_node(ir, (uint32_t)i);
    if (!node || node->kind != GRX_IR_CAPTURE || node->b == GRX_INDEX_NONE) {
      continue;
    }
    if (!node->a || node->a > regex->capture_count) {
      continue;
    }
    const char * name = grx_ir_name(ir, node->b);
    if (!name || regex->capture_names[node->a - 1]) {
      continue;
    }

    size_t length = strlen(name);
    char * copy = gcu_allocator_malloc(regex->allocator, length + 1);
    if (!copy) {
      return GRX_ERR_OOM;
    }
    memcpy(copy, name, length + 1);
    regex->capture_names[node->a - 1] = copy;
  }

  return GRX_OK;
}

/**
 * Copy the mark names out of the IR, so a match can be asked which it passed.
 *
 * One `char *` per distinct `(*MARK:NAME)`, in the order lowering numbered
 * them, because what an instruction carries is that number.
 */
static GRX_Result copy_mark_names(GRX_Regex * regex, const GRX_IR * ir) {
  regex->mark_count = grx_ir_mark_count(ir);
  if (!regex->mark_count) {
    return GRX_OK;
  }

  regex->mark_names = gcu_allocator_calloc(
      regex->allocator, regex->mark_count, sizeof(char *));
  if (!regex->mark_names) {
    regex->mark_count = 0;
    return GRX_ERR_OOM;
  }

  for (size_t i = 0; i < regex->mark_count; i++) {
    const char * name = grx_ir_mark_name(ir, (uint32_t)i);
    if (!name) {
      continue;
    }
    size_t length = strlen(name);
    char * copy = gcu_allocator_malloc(regex->allocator, length + 1);
    if (!copy) {
      return GRX_ERR_OOM;
    }
    memcpy(copy, name, length + 1);
    regex->mark_names[i] = copy;
  }

  return GRX_OK;
}

GRX_Result grx_compile_program(const GRX_Pattern * pattern,
    const GRX_Limits * limits, const GRX_Allocator * allocator,
    GRX_Error * out_error, GRX_Regex ** out_regex) {
  if (!pattern || !limits || !allocator || !out_regex) {
    return GRX_ERR_INVALID;
  }
  *out_regex = NULL;

  GRX_IR * ir = NULL;
  GRX_Result result
      = grx_lower_pattern(pattern, limits, allocator, out_error, &ir);
  if (result != GRX_OK) {
    return result;
  }

  GRX_Regex * regex = gcu_allocator_calloc(allocator, 1, sizeof(GRX_Regex));
  if (!regex) {
    grx_ir_free(ir);
    return grx_error_set(
        out_error, GRX_ERR_OOM, GRX_DIAG_OUT_OF_MEMORY, GRX_NPOS, 0);
  }

  regex->allocator = allocator;
  regex->syntax = pattern->syntax;
  regex->options = pattern->options;
  regex->capture_count = pattern->capture_count;
  regex->capture_names = NULL;
  regex->mark_count = 0;
  regex->mark_names = NULL;
  grx_program_init(&regex->program, allocator, limits);
  grx_facts_init(&regex->facts);

  result = grx_analyze_ir(ir, &regex->facts);
  if (result == GRX_OK && limits->max_lookbehind_length
      && regex->facts.max_lookbehind > limits->max_lookbehind_length) {
    // Checked here rather than in the parser because it is a property of the
    // lowered tree: `(?<=a{3,7})` is seven bytes and `(?<=a|bcd)` is three,
    // and neither is visible from the syntax alone.
    //
    // An unbounded body is GRX_NPOS and so exceeds every finite cap, which
    // is the whole point of setting one: a caller who bounds a lookbehind is
    // defending against the body that may read the entire subject, and
    // `(?<=a+)` is exactly that body. A dialect whose lookbehind is
    // unbounded (ECMAScript, documentation/dialects.md section 5.4) accepts
    // the pattern as *syntax*; this limit is the caller's own policy on top
    // of that, which is why the default does not apply it.
    result = grx_error_set(out_error, GRX_ERR_LIMIT,
        GRX_DIAG_LIMIT_LOOKBEHIND_LENGTH, GRX_NPOS, 0);
  }
  if (result == GRX_OK && ir->max_variable_lookbehind != GRX_NPOS
      && regex->facts.max_variable_lookbehind > ir->max_variable_lookbehind) {
    // The dialect's own bound, not the caller's: a pattern that exceeds it
    // is not valid PCRE2, so this is a syntax error and not a limit.
    result = grx_error_set(out_error, GRX_ERR_SYNTAX,
        GRX_DIAG_VARIABLE_LOOKBEHIND, GRX_NPOS, 0);
  }
  if (result == GRX_OK) {
    result = grx_codegen_program(ir, limits, out_error, &regex->program);
  }
  if (result == GRX_OK) {
    result = copy_capture_names(regex, ir);
    if (result == GRX_OK) {
      result = copy_mark_names(regex, ir);
    }
    if (result != GRX_OK) {
      result = grx_error_set(
          out_error, result, GRX_DIAG_OUT_OF_MEMORY, GRX_NPOS, 0);
    }
  }
  if (result == GRX_OK) {
    // Filled in after codegen, because they are properties of the program
    // rather than of the tree it came from.
    regex->facts.program_size = regex->program.insts.count;
    regex->facts.capture_count = regex->capture_count;
  }

  grx_ir_free(ir);
  if (result != GRX_OK) {
    grx_regex_free(regex);
    return result;
  }

  *out_regex = regex;
  return GRX_OK;
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
  if (regex->mark_names) {
    for (size_t i = 0; i < regex->mark_count; i++) {
      gcu_allocator_free(allocator, regex->mark_names[i]);
    }
    gcu_allocator_free(allocator, regex->mark_names);
  }
  grx_program_clear(&regex->program);
  gcu_allocator_free(allocator, regex);
}
