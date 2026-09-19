/**
 * @file
 *
 * Result strings, the error structure, and the default limits.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <string.h>

const char * grx_result_string(GRX_Result result) {
  switch (result) {
    case GRX_OK:
      return "No error";
    case GRX_ERR_IO:
      return "I/O error";
    case GRX_ERR_FORMAT:
      return "Invalid format";
    case GRX_ERR_UNSUPPORTED:
      return "Unsupported feature";
    case GRX_ERR_LIMIT:
      return "Limit exceeded";
    case GRX_ERR_CORRUPT:
      return "Corrupt data";
    case GRX_ERR_OOM:
      return "Out of memory";
    case GRX_ERR_INVALID:
      return "Invalid argument";
    case GRX_ERR_INTERNAL:
      return "Internal error";
    case GRX_ERR_SYNTAX:
      return "Invalid pattern syntax";
    case GRX_RESULT_COUNT:
    default:
      return "Unknown error";
  }
}

void grx_error_clear(GRX_Error * error) {
  if (!error) {
    return;
  }

  error->code = GRX_OK;
  error->diag = GRX_DIAG_NONE;
  error->offset = GRX_NPOS;
  error->length = 0;
  error->message[0] = '\0';
}

void grx_limits_default(GRX_Limits * limits) {
  if (!limits) {
    return;
  }

  // Every field has a value here, which is the opposite of the choice model
  // made for its record counts. The reason is that a regular expression is
  // not bounded by the size of its input the way a mesh is: a pattern of a
  // dozen bytes can ask for 2^32 repetitions, and a pattern of twenty can
  // take exponential time on a subject of thirty. The caller can still lift
  // any of these by setting it to 0, but the default must be a number.
  //
  // The values are measured, not guessed. documentation/dialects.md section
  // 7 has the report `tools/limits/measure.py` produces and the reasoning
  // for each number; the short version is that 251 real patterns were asked
  // how much of each resource they need, the tightest default leaves six
  // times what the costliest of them uses, and the 17 pairs in the ReDoS
  // corpus are refused in 105 to 173 milliseconds on an idle machine and in
  // 276 to 414 on a busy one. Two tests keep the halves of that true:
  // tests/unit/test_limits.cpp and tests/conformance/test_redos.cpp.
  //
  // max_subject_length is the one deliberate zero: a subject is a buffer the
  // caller already holds, so its size is already bounded by something the
  // caller decided, and a default cap here would reject a large document for
  // no reason this library can justify.
  *limits = (GRX_Limits) {
    .max_pattern_length = 65536,
    .max_nesting_depth = 128,
    .max_nodes = 100000,
    .max_program_size = 200000,
    .max_captures = 1000,
    .max_repeat_count = 65536,
    .max_class_ranges = 10000,
    .max_lookbehind_length = 255,
    .max_recursion_depth = 256,
    .max_steps = 10000000,
    .max_backtrack = 100000,
    .max_match_memory = 8 * 1024 * 1024,
    .max_subject_length = 0,
  };
}
