/**
 * @file
 *
 * The backtracking engine: every construct, bounded by the limits.
 *
 * This is the engine that can run a backreference, a lookaround, an atomic
 * group and a recursion, and it is the one whose worst case is exponential in
 * the subject length. GRX_Limits::max_steps and max_backtrack are not
 * optional extras here - they are the only thing standing between a pattern
 * such as `(a+)+$` and a process that never returns.
 *
 * Status: stub. Reports GRX_ERR_UNSUPPORTED until the compiler produces a
 * program to run.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>

#include "exec_internal.h"

GRX_Result grx_exec_backtrack(
    const GRX_ExecRequest * request, int * out_matched) {
  if (!request || !out_matched) {
    return GRX_ERR_INVALID;
  }

  *out_matched = 0;
  return GRX_ERR_UNSUPPORTED;
}
