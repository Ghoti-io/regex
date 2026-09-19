/**
 * @file
 *
 * Private declarations shared by the two execution engines.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_SRC_EXEC_EXEC_INTERNAL_H
#define GHOTI_IO_GRX_SRC_EXEC_EXEC_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/compile.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/exec.h>
#include <stddef.h>

#include "../compile/compile_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The mutable state of a match.
 *
 * Declared here because both engines write it; the public headers see only an
 * opaque GRX_Match.
 */
struct GRX_Match {
  const GRX_Allocator * allocator; ///< The allocator it came from.
  const GRX_Regex * regex;         ///< The regex it was created for.
  GRX_Capture * captures;          ///< count entries; index 0 is the whole
                                   ///< match.
  size_t count;                    ///< Capture spans, including group 0.
  GRX_Engine engine;               ///< Which engine ran the last attempt.
  int matched;                     ///< Whether the last attempt matched.
};

/**
 * @brief One attempt, in the form both engines take.
 *
 * Passed by pointer rather than as nine arguments, because the two engines
 * must agree on every one of them and a positional mismatch between them
 * would be a defect no compiler could see.
 */
typedef struct GRX_ExecRequest {
  const GRX_Regex * regex;  ///< The compiled regex. Never NULL.
  const char * subject;     ///< The bytes to search.
  size_t length;            ///< Length of `subject` in bytes.
  size_t start;             ///< Byte offset to begin at.
  int anchored;             ///< Non-zero to match only at `start`.
  const GRX_Limits * limits; ///< Caps to apply. Never NULL.
  GRX_Match * match;        ///< Receives the spans. May be NULL.
} GRX_ExecRequest;

/**
 * @brief Whether a program uses a construct the Pike VM cannot run.
 *
 * A backreference, a lookaround, an atomic group or a recursion each need
 * state the lockstep simulation does not carry, so a program containing one
 * has to run on the backtracking engine - and a caller who asked for
 * GRX_ENGINE_PIKE by name has to be told GRX_ERR_UNSUPPORTED rather than
 * quietly given the engine whose worst case is exponential.
 *
 * @param regex The compiled regex. NULL returns 0.
 * @return Non-zero when only the backtracking engine can run it.
 */
int grx_exec_program_needs_backtracking(const GRX_Regex * regex);

/**
 * @brief Run one attempt on the Pike VM: lockstep, linear in the subject.
 *
 * @param request The attempt. Never NULL.
 * @param out_matched Receives non-zero when a match was found. Never NULL.
 * @return GRX_OK or a failure code.
 */
GRX_Result grx_exec_pike(const GRX_ExecRequest * request, int * out_matched);

/**
 * @brief Run one attempt on the backtracking engine.
 *
 * @param request The attempt. Never NULL.
 * @param out_matched Receives non-zero when a match was found. Never NULL.
 * @return GRX_OK, GRX_ERR_LIMIT when max_steps or max_backtrack was reached,
 *   or another failure code.
 */
GRX_Result grx_exec_backtrack(
    const GRX_ExecRequest * request, int * out_matched);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_EXEC_EXEC_INTERNAL_H
