/**
 * @file
 *
 * The Pike VM: a lockstep simulation of the compiled program.
 *
 * Every thread of the NFA advances one input position at a time, so the cost
 * is O(subject x program) however the pattern is written. That bound is the
 * reason this engine exists: it is what makes a pattern supplied by an
 * untrusted party safe to run.
 *
 * Status: stub. Reports GRX_ERR_UNSUPPORTED until the compiler produces a
 * program to run.
 *
 * Reference: Russ Cox, "Regular Expression Matching: the Virtual Machine
 * Approach" (swtch.com/~rsc/regexp/regexp2.html).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>

#include "exec_internal.h"

int grx_exec_program_needs_backtracking(const GRX_Regex * regex) {
  if (!regex) {
    return 0;
  }

  for (size_t i = 0; i < regex->program.count; i++) {
    switch (regex->program.insts[i].op) {
      case GRX_OP_BACKREF:
      case GRX_OP_LOOKAROUND:
      case GRX_OP_ATOMIC:
      case GRX_OP_RECURSE:
        return 1;
      default:
        break;
    }
  }

  return 0;
}

GRX_Result grx_exec_pike(const GRX_ExecRequest * request, int * out_matched) {
  if (!request || !out_matched) {
    return GRX_ERR_INVALID;
  }

  *out_matched = 0;
  return GRX_ERR_UNSUPPORTED;
}
