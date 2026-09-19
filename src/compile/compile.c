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

#include "compile_internal.h"

/** The mnemonic for one opcode, for grx_regex_dump(). */
static const char * opcode_name(GRX_Opcode op) {
  switch (op) {
    case GRX_OP_MATCH:
      return "match";
    case GRX_OP_CHAR:
      return "char";
    case GRX_OP_CLASS:
      return "class";
    case GRX_OP_ANY:
      return "any";
    case GRX_OP_SPLIT:
      return "split";
    case GRX_OP_JMP:
      return "jmp";
    case GRX_OP_SAVE:
      return "save";
    case GRX_OP_ASSERT:
      return "assert";
    case GRX_OP_BACKREF:
      return "backref";
    case GRX_OP_LOOKAROUND:
      return "lookaround";
    case GRX_OP_ATOMIC:
      return "atomic";
    case GRX_OP_RECURSE:
      return "recurse";
    case GRX_OP_COUNT:
    default:
      return "?";
  }
}

GRX_Result grx_compile_program(const GRX_Pattern * pattern,
    const GRX_Limits * limits, const GRX_Allocator * allocator,
    GRX_Error * out_error, GRX_Regex ** out_regex) {
  (void)pattern;
  (void)limits;
  (void)allocator;

  if (!out_regex) {
    return GRX_ERR_INVALID;
  }
  if (out_error) {
    out_error->code = GRX_ERR_UNSUPPORTED;
    out_error->offset = GRX_NPOS;
    const char * message = "the compiler is not implemented yet";
    size_t length = strlen(message);
    memcpy(out_error->message, message, length + 1);
  }

  return GRX_ERR_UNSUPPORTED;
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
  return regex ? regex->program.count : 0;
}

GRX_Result grx_regex_dump(const GRX_Regex * regex, FILE * out) {
  if (!regex || !out) {
    return GRX_ERR_INVALID;
  }

  fprintf(out, "regex: syntax=%s options=0x%08x insts=%zu captures=%zu\n",
      grx_syntax_name(regex->syntax), regex->options, regex->program.count,
      regex->capture_count);

  for (size_t i = 0; i < regex->program.count; i++) {
    const GRX_Inst * inst = &regex->program.insts[i];
    fprintf(out, "  %4zu  %-10s %10u %10u\n", i, opcode_name(inst->op),
        inst->x, inst->y);
  }

  return GRX_OK;
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
  gcu_allocator_free(allocator, regex->program.insts);
  gcu_allocator_free(allocator, regex);
}
