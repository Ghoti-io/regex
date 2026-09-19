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

  // Read from the facts rather than by scanning the program again. Analysis
  // computed this once at compile time, and a second opinion here is a second
  // place to be wrong: an opcode added to the backtracking-only group without
  // a matching case in a scan would route a program to the Pike VM, which
  // would then mis-execute it far from the cause. That is the defect shape
  // documentation/development.md warns about, and reading one field is how it
  // stops being possible.
  //
  // GRX_Facts is conservative before analysis has run: grx_facts_init() sets
  // is_regular to 0, so an unanalysed program goes to the engine that can run
  // anything rather than the one that cannot.
  return !regex->facts.is_regular;
}

GRX_Result grx_exec_pike(const GRX_ExecRequest * request, int * out_matched) {
  if (!request || !out_matched) {
    return GRX_ERR_INVALID;
  }

  *out_matched = 0;
  return GRX_ERR_UNSUPPORTED;
}
