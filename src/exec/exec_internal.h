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
#include <string.h>

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

  /**
   * Where the last attempt was told to start looking.
   *
   * Read by Vim's iteration rule and by nothing else. The loop there has to
   * know whether a match *moved*, and the reported end alone cannot say:
   * `\ze` can pin it back to where the search began, so `\zea` over "aaa"
   * reports 0-0 three times running and vim advances a character each time,
   * where `a\zs` reports 1-1 from a search that began at 0 and vim does
   * not. Without this the second shape is right and the first is a loop
   * that never ends.
   */
  size_t searched_from;
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
  GRX_CalloutFn callout;    ///< Called at each `(?C...)`. May be NULL.
  void * callout_data;      ///< Passed to `callout` untouched.
  /**
   * @brief The screen column of every byte offset, or NULL.
   *
   * `length + 1` entries, built once per search and only for a program
   * carrying GRX_PROGRAM_HAS_SCREEN_COLUMN - Vim's `\%23v` and nothing
   * else. Zero at an offset that has no column of its own, which is one
   * inside a character and one at a combining character that follows
   * something; columns themselves count from one, so zero cannot collide
   * with a real answer.
   *
   * Here rather than computed by the assertion because the assertion is
   * reached at arbitrary positions in arbitrary order, and a walk from the
   * start at each of them would be quadratic in the subject.
   */
  const uint32_t * columns;
} GRX_ExecRequest;

/**
 * @brief The first position at or after `from` whose byte could begin a match.
 *
 * `length` when there is none. The caller must have nothing in flight: this
 * says only that a fresh attempt at a skipped position would have died before
 * consuming anything, which is a statement about starting and not about
 * threads already running.
 *
 * Safe because the set is a superset when it is known at all (prefilter.c), so
 * a position this steps over is one where no attempt could have consumed its
 * first byte. In UTF mode the set holds leading bytes only, so a position it
 * stops at is a character boundary; a subject that is not valid UTF-8 never
 * reaches an engine, exec.c having validated it.
 *
 * @param program The compiled program.
 * @param subject The bytes being searched.
 * @param length Their length.
 * @param from Where to look from.
 * @return The position to try next, which is `from` when nothing is known.
 */
/**
 * @brief The first occurrence of `needle` in `haystack`, or NULL.
 *
 * memchr for a candidate and a comparison to confirm it, rather than
 * `memmem`, which is a GNU extension where this library builds on three
 * platforms. The scan is the work and it is memchr's, which every libc
 * vectorises.
 *
 * **The candidate is the needle's last byte, not its first**, and the reason
 * is the case documentation/design.md section 3.5.5 names as what the
 * literals are for. `aaaaaaaaab` over a subject of `a` has a candidate at
 * every position if the first byte is what is scanned for - the search
 * degrades to a memchr call and a memcmp per byte, measured at 5.2 ns per
 * subject byte, which is barely better than the byte set it replaced. Its
 * last byte occurs nowhere, so scanning for that answers in one pass.
 *
 * The first byte is then checked inline before memcmp, which costs one
 * comparison and bounds the mirror image of that case: a needle whose last
 * byte is common and whose first is rare rejects each candidate without a
 * call.
 *
 * Neither is a guarantee. A needle whose first *and* last bytes are both
 * common is still quadratic in the worst case, and the fix for that is the
 * two-way or Boyer-Moore search design.md already names as the next piece.
 *
 * @param haystack Where to look. May be NULL only when `haystack_length` is 0.
 * @param haystack_length Its length.
 * @param needle What to look for. Never NULL.
 * @param needle_length Its length; zero finds `haystack` itself.
 * @return The first occurrence, or NULL.
 */
static inline const char * grx_exec_find(const char * haystack,
    size_t haystack_length, const char * needle, size_t needle_length) {
  if (!needle_length) {
    return haystack;
  }
  if (!haystack || needle_length > haystack_length) {
    return NULL;
  }
  const size_t last = needle_length - 1;
  const char * at = haystack + last;
  size_t left = haystack_length - last;
  while (left) {
    const char * hit
        = (const char *)memchr(at, (unsigned char)needle[last], left);
    if (!hit) {
      return NULL;
    }
    const char * begin = hit - last;
    if (begin[0] == needle[0] && memcmp(begin, needle, last) == 0) {
      return begin;
    }
    left -= (size_t)(hit - at) + 1;
    at = hit + 1;
  }
  return NULL;
}

static inline size_t grx_exec_skip_to_first_byte(const GRX_Program * program,
    const char * subject, size_t length, size_t from) {
  if (!program->first_bytes_known && !program->literal_prefix_length) {
    return from;
  }
  // A callout is a side effect of *trying* a position, not of matching at one:
  // `(?C1)abc` is documented to fire at every starting position the search
  // tries, and Callout.FiresAtEveryStartingPositionTheSearchTries is that
  // sentence as a test. Skipping a position would quietly stop reporting it,
  // which is the one thing a tracing facility must not do - so a program with
  // a callout anywhere in it does not skip. Its author is watching the search,
  // not timing it.
  //
  // This is a property of the *search*, not of the pattern, which is why it is
  // here and not in prefilter.c: the set is still a true fact about where a
  // match can begin, and GRX_Facts still reports it.
  if (program->flags & GRX_PROGRAM_HAS_CALLOUT) {
    return from;
  }
  // `(*CRLF)` keeps both engines from *beginning* an attempt between a CR and
  // its LF, and this would step straight onto one. The rule is off whenever
  // the pattern names CR or LF itself, and that is the only case where the
  // skip and the rule could contend - so the skip simply does not apply when
  // the rule is live. A pattern under `(*CRLF)` that cannot start with either
  // character is the whole of what this gives up.
  if ((program->flags & GRX_PROGRAM_NEWLINE_CRLF)
      && !(program->flags & GRX_PROGRAM_HAS_CR_OR_LF)) {
    return from;
  }
  // A whole string beats a byte wherever there is one. `aaaaaaaaab` is the
  // case the byte set cannot help with at all - every position in a subject
  // of `a` passes the byte test and exactly one passes this - and it is the
  // case documentation/design.md section 3.5.5 names as what the literals are
  // for. Nothing is lost where both are known: the prefix begins with a byte
  // the set contains, so this skips at least as far.
  //
  // A prefix begins with the first byte of an encoded code point, and a UTF-8
  // continuation byte is never one of those, so a hit cannot land inside a
  // character. Not finding it means no match exists at or after `from`, which
  // `length` is the way to say.
  if (program->literal_prefix_length) {
    if (from >= length) {
      return from;
    }
    const char * found = grx_exec_find(subject + from, length - from,
        program->literal_prefix, program->literal_prefix_length);
    return found ? (size_t)(found - subject) : length;
  }

  if (!program->first_bytes_known) {
    return from;
  }
  while (from < length) {
    unsigned char byte = (unsigned char)subject[from];
    if (program->first_bytes[byte >> 3] & (unsigned char)(1u << (byte & 7u))) {
      break;
    }
    from++;
  }
  return from;
}

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
 * @brief Whether `candidate` divides a match better than `best` does, under
 *   POSIX's rule.
 *
 * The comparison GRX_SUBMATCH_POSIX is made of. Both arrays hold the same
 * groups of the same pattern over the same extent, and differ only in where
 * the boundaries between the groups fell.
 *
 * POSIX's rule is recursive - each subexpression takes the longest span
 * consistent with the whole match and with the subexpressions before it -
 * and group *number* is the order that recursion visits in, because groups
 * are numbered by their opening parenthesis: an enclosing group has a lower
 * number than the groups inside it, and a group to the left a lower number
 * than one to its right. So the rule is a scan, and the first group where
 * the two differ decides.
 *
 * **Ends only.** The scan reads each group's end and prefers the later one;
 * it never looks at a start, and never at group 0. That is not a
 * simplification, it is the correction: preferring an earlier start looks
 * like preferring a longer group and is really preferring a *shorter*
 * something to its left, and the something to its left may have no group
 * around it and so no slot here to be compared. `[ab]a*(a|)` against "aab"
 * is the case that says so. Both references give group 1 the empty match at
 * 2, because `a*` is the leftmost subexpression and takes what it can;
 * comparing starts hands group 1 the span 1-2 instead, which is longer and
 * wrong, and shortens an `a*` that has no tag to defend itself with. A
 * group's start belongs to whatever precedes it, which the engines already
 * settle by their own priority order - greedy first - so it is the length,
 * and only the length, that this decides.
 *
 * A group only one of the two entered is skipped rather than preferred
 * either way: a subexpression that did not participate has no length to
 * compare, and letting it decide reports spans from an iteration that lost.
 * `a(b+|((c)*))+d` against "abd" is that case, where treating a set group as
 * beating an unset one reports group 2 as 1-1 where both references leave it
 * unset.
 *
 * What is left when nothing here decides is the engines' own order, which
 * is why this returns 0 for "no preference" rather than a three-way answer.
 *
 * @param candidate The newly finished division. Never NULL.
 * @param best The one currently held. Never NULL.
 * @param captures How many slots each array holds: two per group, group 0
 *   included.
 * @return Non-zero when `candidate` should displace `best`.
 */
static inline int grx_exec_submatch_better(const size_t * candidate,
    const size_t * best, size_t captures) {
  for (size_t slot = 3; slot < captures; slot += 2) {
    size_t mine = candidate[slot];
    size_t theirs = best[slot];
    if (mine == theirs || mine == GRX_NPOS || theirs == GRX_NPOS) {
      continue;
    }
    return mine > theirs;
  }

  return 0;
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
/**
 * @brief The base of the cluster the character before `position` is in.
 *
 * Steps back over composing characters, which is what Vim's word
 * assertions ask about; see src/exec/exec.c. A run of them at the start of
 * the window is its own base.
 *
 * @param subject The text.
 * @param start Where the window begins.
 * @param position The position to look back from.
 * @param utf Non-zero when the subject is UTF-8.
 * @param out_codepoint Receives the base.
 * @return Non-zero when there was a character to read.
 */
int grx_cluster_base_before(const char * subject, size_t start,
    size_t position, int utf, uint32_t * out_codepoint);

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
