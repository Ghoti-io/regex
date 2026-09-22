/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Regex.
 *
 * Ghoti.io Regex is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Regex is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Private declarations shared by the two execution engines.
 */

#ifndef GHOTI_IO_GRX_SRC_EXEC_EXEC_INTERNAL_H
#define GHOTI_IO_GRX_SRC_EXEC_EXEC_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/compile.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/exec.h>
#include <stddef.h>
#include <stdint.h>

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
  size_t steps;                    ///< Instructions the last attempt ran.
  /**
   * The `(*MARK:NAME)` the last attempt passed, as an index, or
   * GRX_INDEX_NONE.
   *
   * An index rather than a name, because the engines compare marks and never
   * read them; grx_match_mark() is where it becomes text, and it has the
   * regex to look the index up in.
   */
  uint32_t mark;

  /**
   * Why the last attempt stopped, when it stopped at a limit.
   *
   * Cleared at the start of every attempt, so it describes the last one
   * and never an older one. GRX_OK with GRX_DIAG_NONE when nothing went
   * wrong, which is what a caller reading it after a plain non-match sees.
   */
  GRX_Error error;
};

/**
 * @brief Whether an empty match counts as a match.
 *
 * Both halves of PCRE2's documented search-all loop, and the mechanism behind
 * every rule in documentation/dialects.md section 5.10: without a way to say
 * "not the empty match you just gave me", a loop over a pattern that can
 * match empty either never advances or has to skip positions a match starts
 * at.
 */
typedef enum {
  GRX_EMPTY_OK = 0,      ///< An empty match is a match.
  GRX_EMPTY_REJECT,      ///< An empty match is never a match.
  GRX_EMPTY_REJECT_AT_START ///< Not when it begins where the search did.
} GRX_EmptyMatchRule;

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
  /**
   * @brief The position `\G` asserts.
   *
   * `start` for every dialect but Perl, and for Perl too except on the one
   * step of a search-all loop that advances past a failure: `\G` is
   * `pos()` there, which a failed attempt does not move. See
   * @ref GRX_SearchStartRule.
   */
  size_t search_start;
  int anchored;             ///< Non-zero to match only at `start`.
  int not_bol;              ///< `^` does not hold at offset 0.
  int not_eol;              ///< `$` does not hold at `length`.
  uint8_t empty_rule;       ///< A @ref GRX_EmptyMatchRule.
  const GRX_Limits * limits; ///< Caps to apply. Never NULL.
  GRX_Match * match;        ///< Receives the spans. May be NULL.
  size_t * out_steps;       ///< Receives the step count. May be NULL.
  /**
   * Receives which limit stopped the run, or GRX_DIAG_NONE. May be NULL.
   *
   * A match-time failure has no offset into the pattern - "ran out of
   * steps" is not a place - so it travels as a diagnostic rather than as
   * a GRX_Error, and exec.c turns it into one on the match object.
   */
  GRX_Diag * out_diag;
  int memoize;              ///< Run the backtracker with a visited bitmap.
} GRX_ExecRequest;

/**
 * @brief Whether a match spanning [begin, end) is one this request accepts.
 *
 * Shared by the two engines so that the rule is written once. An engine calls
 * it where it would otherwise have accepted a match outright.
 *
 * @param request The attempt. Never NULL.
 * @param begin Where the candidate match begins.
 * @param end Where it ends.
 * @return Non-zero when the match is acceptable.
 */
static inline int grx_exec_accepts(
    const GRX_ExecRequest * request, size_t begin, size_t end) {
  if (begin != end) {
    return 1;
  }
  switch ((GRX_EmptyMatchRule)request->empty_rule) {
    case GRX_EMPTY_REJECT:
      return 0;
    case GRX_EMPTY_REJECT_AT_START:
      return begin != request->start;
    case GRX_EMPTY_OK:
    default:
      return 1;
  }
}

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
 * @brief Whether a program's behaviour is a function of (instruction,
 * position) alone.
 *
 * The bit-state engine memoises on exactly that pair, so anything the
 * program carries *between* those two - a captured span a backreference will
 * compare against, a progress register an empty-iteration guard will test -
 * makes the memo unsound: two paths reaching the same instruction at the
 * same position would behave differently, and the second would be wrongly
 * skipped.
 *
 * Lookaround and recursion are excluded for the same reason a sub-run would
 * need a bitmap of its own (documentation/design.md section 3.5.3).
 *
 * @param regex The compiled regex. NULL returns 0.
 * @return Non-zero when the bit-state engine may run it.
 */
int grx_exec_program_is_memoizable(const GRX_Regex * regex);

/**
 * @brief Bytes the bit-state engine's bitmap would need for a subject.
 *
 * One bit per instruction per position, plus the end position. Returns
 * GRX_NPOS when the product overflows, which a caller must treat as "will
 * not fit" rather than as a small number.
 *
 * @param regex The compiled regex. NULL returns GRX_NPOS.
 * @param length The subject length in bytes.
 * @return The size in bytes, or GRX_NPOS on overflow.
 */
size_t grx_exec_bitmap_bytes(const GRX_Regex * regex, size_t length);

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

/**
 * @name The CR LF pair, for the line assertions
 *
 * PCRE2's `(*CRLF)`, `(*ANYCRLF)` and `(*ANY)` make a CR LF pair one line
 * terminator, which a class of code points cannot say. GRX_INST_NEWLINE_CRLF
 * is the flag and these three are what it asks.
 *
 * Bytes, with no decoding: CR and LF are one byte each in UTF-8 and in byte
 * mode alike, and no multi-byte sequence contains either, so the question
 * "do these two positions spell CR LF" is the same question at every level.
 *
 * Here rather than in each engine because both have an `assertion_holds()`
 * and the two have to agree exactly - the Pike VM's already carries a
 * comment saying so, which is the shape a rule written twice takes just
 * before it drifts.
 * @{
 */

/** Whether `position` sits between the CR and the LF of a pair. */
static inline int grx_between_crlf(
    const char * subject, size_t start, size_t end, size_t position) {
  return position > start && position < end && subject[position - 1] == '\r'
      && subject[position] == '\n';
}

/** Whether a CR LF pair ends at `position`. */
static inline int grx_crlf_ends_at(
    const char * subject, size_t start, size_t position) {
  return position >= start + 2 && subject[position - 2] == '\r'
      && subject[position - 1] == '\n';
}

/** Whether a CR LF pair begins at `position`. */
static inline int grx_crlf_begins_at(
    const char * subject, size_t end, size_t position) {
  return position + 1 < end && subject[position] == '\r'
      && subject[position + 1] == '\n';
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_EXEC_EXEC_INTERNAL_H
