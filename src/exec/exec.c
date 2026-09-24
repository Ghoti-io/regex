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
 * The match object, and the public entry points that choose an engine.
 *
 * Engine selection reads one field - GRX_Facts::is_regular - rather than
 * scanning the program a second time. A second opinion here would be a second
 * place to be wrong, and the failure it would cause (a program routed to the
 * lockstep engine that the lockstep engine cannot run) shows up far from its
 * cause.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/exec.h>
#include <ghoti.io/regex/unicode.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "../core/core_internal.h"
#include "../unicode/unicode_internal.h"
#include "exec_internal.h"

void grx_search_options_default(GRX_SearchOptions * out_options) {
  if (!out_options) {
    return;
  }
  *out_options = (GRX_SearchOptions) {
    .begin = 0,
    .end = GRX_NPOS,
    .flags = GRX_SEARCH_NONE,
    .engine = GRX_ENGINE_AUTO,
    .limits = NULL,
  };
}

GRX_Result grx_match_create(const GRX_Regex * regex,
    const GRX_Allocator * allocator, GRX_Match ** out_match) {
  if (!regex || !out_match) {
    return GRX_ERR_INVALID;
  }
  *out_match = NULL;

  if (!allocator) {
    allocator = grx_allocator_default();
  }

  // One more than the capture count: group 0 is the whole match.
  size_t count = grx_regex_capture_count(regex) + 1;

  GRX_Match * match = gcu_allocator_malloc(allocator, sizeof(GRX_Match));
  if (!match) {
    return GRX_ERR_OOM;
  }

  match->captures = gcu_allocator_calloc(allocator, count,
      sizeof(GRX_Capture));
  if (!match->captures) {
    gcu_allocator_free(allocator, match);
    return GRX_ERR_OOM;
  }

  match->allocator = allocator;
  match->regex = regex;
  match->count = count;
  match->engine = GRX_ENGINE_COUNT;
  match->matched = 0;
  match->steps = 0;
  match->mark = GRX_INDEX_NONE;
  grx_error_clear(&match->error);
  for (size_t i = 0; i < count; i++) {
    match->captures[i] = (GRX_Capture) {GRX_NPOS, GRX_NPOS};
  }

  *out_match = match;
  return GRX_OK;
}

/**
 * The shared body of every entry point: validate, choose an engine, hand the
 * request to it.
 *
 * The window arrives already resolved - `end` is the effective subject length
 * and `begin` the offset to start at - so that the three public forms differ
 * only in what they put in the options, and there is one place where a search
 * can be got wrong.
 */
/**
 * The screen column of every byte offset, counting from one.
 *
 * Vim's `\%23v` is what wants it. Zero where an offset has no column of
 * its own: inside a character, and at a combining character that follows
 * something - `\%2v` against "a" U+0301 "x" holds at the "x" and not at
 * the combining character, which is measured. A real column is never zero,
 * so the two cannot be confused.
 *
 * The tabstop is eight, which is vim's default for the option the answer
 * depends on; see documentation/dialects.md section 6.
 */
/**
 * The base of the cluster the character before `position` belongs to.
 *
 * Vim's word assertions ask what class the character before a position is
 * in, and in a dialect where a base character and the composing characters
 * after it are one character that question is about the *base*: `\>` holds
 * between U+65E5 U+0301 and "x" there, where the mark alone would say the
 * classes are the same and no boundary falls. Vim asks it the same way, by
 * stepping back over composing characters to the head of the character.
 *
 * A run of composing characters at the very start of the window is its own
 * base - a mark with nothing before it is a character of its own, which is
 * the rule everywhere else in this model too.
 *
 * @param subject The text.
 * @param start Where the window begins; nothing before it is read.
 * @param position The position to look back from.
 * @param utf Non-zero when the subject is UTF-8.
 * @param out_codepoint Receives the base.
 * @return Non-zero when there was a character to read.
 */
int grx_cluster_base_before(const char * subject, size_t start,
    size_t position, int utf, uint32_t * out_codepoint) {
  size_t at = position;
  uint32_t codepoint = 0;
  int seen = 0;
  while (at > start) {
    if (!utf) {
      codepoint = (unsigned char)subject[at - 1];
      at--;
      seen = 1;
      break;
    }
    size_t width = grx_unicode_utf8_decode_prev(subject, at, &codepoint);
    if (!width) {
      return 0;
    }
    at -= width;
    seen = 1;
    if (grx_display_cell_width(codepoint, 0) != 0) {
      break;
    }
  }
  if (!seen) {
    return 0;
  }
  *out_codepoint = codepoint;
  return 1;
}

static uint32_t * build_columns(const GRX_Regex * regex,
    const char * subject, size_t end, const GRX_Limits * limits) {
  (void)limits;
  size_t slots = end + 1;
  if (slots > GRX_NPOS / sizeof(uint32_t)) {
    return NULL;
  }
  uint32_t * columns
      = gcu_allocator_calloc(regex->allocator, slots, sizeof(uint32_t));
  if (!columns) {
    return NULL;
  }

  size_t column = 1;
  size_t at = 0;
  int first = 1;
  while (at <= end) {
    columns[at] = column > UINT32_MAX ? UINT32_MAX : (uint32_t)column;
    if (at == end) {
      break;
    }
    uint32_t codepoint = 0;
    size_t width = grx_unicode_utf8_decode(subject + at, end - at, &codepoint);
    if (!width) {
      // Not a character, so it is drawn as the byte it is. One cell, which
      // is what vim shows for a stray byte too.
      codepoint = (uint32_t)(unsigned char)subject[at];
      width = 1;
    }
    size_t next = grx_display_column_after(codepoint, column, 8, first);
    if (next == column) {
      // A combining character: the offsets it spans have no column, and
      // the one after it keeps the column its base had.
      for (size_t skip = at; skip < at + width; skip++) {
        columns[skip] = 0;
      }
    }
    column = next;
    at += width;
    first = 0;
  }
  return columns;
}

static GRX_Result exec(const GRX_Regex * regex, const char * subject,
    size_t length, const GRX_SearchOptions * options, int anchored,
    GRX_EmptyMatchRule empty_rule, size_t search_start, GRX_Match * match,
    int * out_matched) {
  GRX_SearchOptions defaults;
  if (!options) {
    grx_search_options_default(&defaults);
    options = &defaults;
  }

  size_t end = options->end == GRX_NPOS ? length : options->end;
  if (!regex || !out_matched || (!subject && length) || end > length
      || options->begin > end) {
    return GRX_ERR_INVALID;
  }
  if ((unsigned)options->engine >= (unsigned)GRX_ENGINE_COUNT) {
    return GRX_ERR_INVALID;
  }
  if (options->flags
      & ~(uint32_t)(GRX_SEARCH_NOTBOL | GRX_SEARCH_NOTEOL | GRX_SEARCH_NOTEMPTY
          | GRX_SEARCH_NOTEMPTY_ATSTART | GRX_SEARCH_NO_UTF_CHECK)) {
    return GRX_ERR_INVALID;
  }
  if (match && match->regex != regex) {
    return GRX_ERR_INVALID;
  }
  *out_matched = 0;

  const GRX_Limits * limits = options->limits;
  GRX_Limits limit_defaults;
  if (!limits) {
    grx_limits_default(&limit_defaults);
    limits = &limit_defaults;
  }
  // `(*LIMIT_MATCH=d)` and kin, resolved here because this is the one place
  // a search decides what its budget is. The pattern may lower what the
  // caller allowed and may never raise it: a caller's cap is a policy, and
  // a pattern arriving from outside must not be able to lift it.
  //
  // A budget of zero is not representable in GRX_Limits, where 0 means "no
  // limit", so grx_pattern_limits_apply() answers it rather than storing it
  // - `(*LIMIT_MATCH=0)abc` fails every match, which is pcre2test's answer
  // too.
  GRX_Limits limit_capped;
  if (regex->limits.max_steps != GRX_NPOS
      || regex->limits.max_backtrack != GRX_NPOS
      || regex->limits.max_match_memory != GRX_NPOS) {
    limit_capped = *limits;
    GRX_Diag refused = GRX_DIAG_NONE;
    if (grx_pattern_limits_apply(&regex->limits, &limit_capped, &refused)
        != GRX_OK) {
      return grx_error_set(match ? &match->error : NULL, GRX_ERR_LIMIT,
          refused, GRX_NPOS, 0);
    }
    limits = &limit_capped;
  }
  if (limits->max_subject_length && end > limits->max_subject_length) {
    return grx_error_set(match ? &match->error : NULL, GRX_ERR_LIMIT,
        GRX_DIAG_LIMIT_SUBJECT_LENGTH, GRX_NPOS, 0);
  }

  // The subject is checked once here rather than discovered mid-match by
  // whichever engine happened to read that far (documentation/design.md
  // section 5.1). Everything up to `end` is checked, not just the window,
  // because a lookbehind reads before `begin`.
  if ((regex->program.flags & GRX_PROGRAM_UTF)
      && !(options->flags & GRX_SEARCH_NO_UTF_CHECK)) {
    size_t at = 0;
    GRX_Result valid = grx_utf8_validate(subject, end, &at);
    if (valid != GRX_OK) {
      // The offset is into the *subject*, which is the one match-time
      // diagnostic that has a position at all.
      return grx_error_set(match ? &match->error : NULL, valid,
          GRX_DIAG_INVALID_SUBJECT_UTF8, at, 0);
    }
  }

  // The flags and the caller's rule are the same axis, so they are resolved
  // to one value; NOTEMPTY is the stronger of the two and wins.
  if (options->flags & GRX_SEARCH_NOTEMPTY) {
    empty_rule = GRX_EMPTY_REJECT;
  }
  else if ((options->flags & GRX_SEARCH_NOTEMPTY_ATSTART)
      && empty_rule == GRX_EMPTY_OK) {
    empty_rule = GRX_EMPTY_REJECT_AT_START;
  }

  // Engine selection. The table, and a test per row, is in
  // tests/unit/test_exec.cpp:
  //
  //   | the program is           | AUTO picks  | PIKE       | BITSTATE   |
  //   | ----------------------- | ----------- | ---------- | ---------- |
  //   | regular                  | Pike        | runs it    | runs it    |
  //   | not regular, memoizable  | bit-state   | refused    | runs it    |
  //   | not regular, not memo.   | backtracker | refused    | refused    |
  //
  // AUTO prefers Pike for a regular program rather than the bit-state
  // engine, even though both are linear: Pike's memory is bounded by the
  // *program* and bit-state's by the program times the subject. Choosing
  // bit-state for a regular program is a speed decision, and speed decisions
  // are Phase 8 (documentation/design.md section 3.5.5).
  //
  // The bottom row is the only one with an exponential worst case left. A
  // backtracker asked to run a *memoizable* program arms the same memo
  // partway through, once the run has cost more than a memoised one could
  // (exec_backtrack.c, `memo_after`), so the difference between the middle
  // row and a named GRX_ENGINE_BACKTRACK is when the bitmap is allocated
  // rather than whether the run finishes.
  GRX_Engine engine = options->engine;
  int needs_backtracking = grx_exec_program_needs_backtracking(regex);
  int memoizable = grx_exec_program_is_memoizable(regex);
  // A fourth row, and the only one that is a fact about the *search* rather
  // than about the program: a caller who registered a GRX_CalloutFn against
  // a pattern that has callouts is asking to be told where the match is,
  // one arrival at a time, and that order is backtracking order. The Pike
  // VM has no such order - two paths reaching one instruction are one
  // thread there - and the bit-state engine would skip the arrivals its
  // memo has already seen. So the three branches below are reused rather
  // than a fourth written: AUTO takes the backtracker, and PIKE or BITSTATE
  // named by hand is refused the way every other engine mismatch is.
  //
  // Without a function registered nothing changes, which is PCRE2's rule
  // too, and is what keeps `a(?C1)b` a linear-time pattern for every caller
  // who is not watching it.
  if (options->callout && (regex->program.flags & GRX_PROGRAM_HAS_CALLOUT)) {
    needs_backtracking = 1;
    memoizable = 0;
  }
  if (engine == GRX_ENGINE_AUTO) {
    engine = !needs_backtracking ? GRX_ENGINE_PIKE
        : memoizable             ? GRX_ENGINE_BITSTATE
                                 : GRX_ENGINE_BACKTRACK;
  }
  else if (engine == GRX_ENGINE_PIKE && needs_backtracking) {
    // Asked for by name, so the answer is that it cannot be done, not a
    // silent substitution of the engine that can hang.
    return GRX_ERR_UNSUPPORTED;
  }
  else if (engine == GRX_ENGINE_BITSTATE && !memoizable) {
    // Same rule: the memo would be unsound for this program, and running it
    // without the memo would be the backtracker under another name.
    return GRX_ERR_UNSUPPORTED;
  }

  size_t steps = 0;
  GRX_Diag diag = GRX_DIAG_NONE;
  if (match) {
    for (size_t i = 0; i < match->count; i++) {
      match->captures[i] = (GRX_Capture) {GRX_NPOS, GRX_NPOS};
    }
    match->engine = engine;
    match->matched = 0;
    match->steps = 0;
    match->mark = GRX_INDEX_NONE;
    match->searched_from = options->begin;
    grx_error_clear(&match->error);
  }

  GRX_ExecRequest request = {
    .regex = regex,
    .subject = subject,
    .length = end,
    .start = options->begin,
    .search_start
        = search_start == GRX_NPOS ? options->begin : search_start,
    .anchored = anchored,
    .not_bol = (options->flags & GRX_SEARCH_NOTBOL) != 0,
    .not_eol = (options->flags & GRX_SEARCH_NOTEOL) != 0,
    .empty_rule = (uint8_t)empty_rule,
    .limits = limits,
    .match = match,
    .out_steps = &steps,
    .out_diag = &diag,
    .memoize = engine == GRX_ENGINE_BITSTATE,
    // Passed even when the program has no callout in it: the engine tests
    // for the function where it meets the instruction, and one place that
    // decides whether callouts are live is one place to get it wrong.
    .callout = options->callout,
    .callout_data = options->callout_data,
    .columns = NULL,
  };

  // Vim's `\%23v`, and only ever that: the screen column of every offset,
  // computed once here because the assertion is reached at arbitrary
  // positions in arbitrary order and a walk from the start at each of them
  // would be quadratic in the subject.
  uint32_t * columns = NULL;
  if (regex->program.flags & GRX_PROGRAM_HAS_SCREEN_COLUMN) {
    columns = build_columns(regex, subject, end, limits);
    if (!columns) {
      if (match) {
        grx_error_set(&match->error, GRX_ERR_OOM, GRX_DIAG_OUT_OF_MEMORY,
            GRX_NPOS, 0);
      }
      return GRX_ERR_OOM;
    }
    request.columns = columns;
  }

  GRX_Result result = engine == GRX_ENGINE_PIKE
      ? grx_exec_pike(&request, out_matched)
      : grx_exec_backtrack(&request, out_matched);
  gcu_allocator_free(regex->allocator, columns);

  if (match) {
    match->steps = steps;
    if (result == GRX_OK) {
      match->matched = *out_matched;
    }
    else {
      // A result code alone does not say *which* limit was reached, which is
      // the whole reason this channel exists.
      grx_error_set(&match->error, result, diag, GRX_NPOS, 0);
    }
  }
  return result;
}

GRX_Result grx_regex_search_ex(const GRX_Regex * regex, const char * subject,
    size_t length, const GRX_SearchOptions * options, GRX_Match * match,
    int * out_matched) {
  return exec(regex, subject, length, options, 0, GRX_EMPTY_OK, GRX_NPOS,
      match, out_matched);
}

GRX_Result grx_regex_match_ex(const GRX_Regex * regex, const char * subject,
    size_t length, const GRX_SearchOptions * options, GRX_Match * match,
    int * out_matched) {
  return exec(regex, subject, length, options, 1, GRX_EMPTY_OK, GRX_NPOS,
      match, out_matched);
}

GRX_Result grx_regex_search(const GRX_Regex * regex, const char * subject,
    size_t length, size_t start, GRX_Engine engine, const GRX_Limits * limits,
    GRX_Match * match, int * out_matched) {
  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.begin = start;
  options.engine = engine;
  options.limits = limits;
  return grx_regex_search_ex(regex, subject, length, &options, match,
      out_matched);
}

GRX_Result grx_regex_match(const GRX_Regex * regex, const char * subject,
    size_t length, size_t start, GRX_Engine engine, const GRX_Limits * limits,
    GRX_Match * match, int * out_matched) {
  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.begin = start;
  options.engine = engine;
  options.limits = limits;
  return grx_regex_match_ex(regex, subject, length, &options, match,
      out_matched);
}

/**
 * One character forward from `offset`, without running off `end`.
 *
 * "One character" is a code point in UTF mode and a byte otherwise, which is
 * the same rule every consuming instruction uses. A byte that does not begin
 * a valid sequence advances by one, so that a malformed subject cannot make
 * an iteration loop stand still - the search itself will report it.
 */
static size_t advance_one(const GRX_Regex * regex, const char * subject,
    size_t end, size_t offset) {
  if (offset >= end) {
    return end;
  }
  if (!(regex->program.flags & GRX_PROGRAM_UTF)) {
    return offset + 1;
  }
  uint32_t codepoint = 0;
  size_t width
      = grx_unicode_utf8_decode(subject + offset, end - offset, &codepoint);
  return offset + (width ? width : 1);
}

/**
 * Report that the loop is over.
 *
 * Every "there is nowhere left to look" path goes through here rather than
 * returning GRX_OK on its own, because `out_matched` is the caller's only
 * signal and a path that leaves it alone leaves the previous iteration's
 * answer standing - which is a loop that never ends.
 */
static GRX_Result no_further_match(GRX_Match * match, int * out_matched) {
  for (size_t i = 0; i < match->count; i++) {
    match->captures[i] = (GRX_Capture) {GRX_NPOS, GRX_NPOS};
  }
  match->matched = 0;
  match->steps = 0;
  *out_matched = 0;
  return GRX_OK;
}

GRX_Result grx_regex_search_next(const GRX_Regex * regex,
    const char * subject, size_t length, const GRX_SearchOptions * options,
    GRX_Match * match, int * out_matched) {
  if (!regex || !match || !out_matched) {
    return GRX_ERR_INVALID;
  }
  if (match->regex != regex) {
    return GRX_ERR_INVALID;
  }

  GRX_SearchOptions resolved;
  if (options) {
    resolved = *options;
  }
  else {
    grx_search_options_default(&resolved);
  }

  // Nothing to continue from: this is an ordinary first search.
  if (!match->matched || !match->count
      || match->captures[0].start == GRX_NPOS) {
    return grx_regex_search_ex(regex, subject, length, &resolved, match,
        out_matched);
  }

  size_t end = resolved.end == GRX_NPOS ? length : resolved.end;
  if (end > length) {
    return GRX_ERR_INVALID;
  }
  GRX_Capture previous = match->captures[0];
  int was_empty = previous.start == previous.end;

  // The subject was validated by the search that produced the previous
  // match, and this call is documented to run against the same bytes. Paying
  // for the O(n) check again on every step would make a search-all loop
  // quadratic in the subject for no additional safety: an engine that meets
  // a malformed sequence still reports GRX_ERR_INVALID rather than reading
  // past it.
  resolved.flags |= GRX_SEARCH_NO_UTF_CHECK;

  switch ((GRX_IterationRule)regex->program.iteration) {
    case GRX_ITERATE_RETRY_THEN_ADVANCE: {
      if (!was_empty) {
        resolved.begin = previous.end;
        return grx_regex_search_ex(regex, subject, length, &resolved, match,
            out_matched);
      }
      // PCRE2's documented loop: first ask for a non-empty match at exactly
      // the position the empty one was found at, because `a*` at "b" should
      // report the "b"-anchored alternative if there is one before giving up
      // on this position entirely.
      resolved.begin = previous.end;
      GRX_Result result = exec(regex, subject, length, &resolved, 1,
          GRX_EMPTY_REJECT_AT_START, GRX_NPOS, match, out_matched);
      if (result != GRX_OK || *out_matched) {
        return result;
      }
      if (previous.end >= end) {
        return no_further_match(match, out_matched);
      }
      resolved.begin = advance_one(regex, subject, end, previous.end);
      // The one step in this file where `\G` and the start offset part
      // company. Under GRX_SEARCH_START_PREVIOUS_END - Perl's - `pos()` did
      // not move, because the attempt that would have moved it failed, so
      // `\G` stays where the previous match ended while the search itself
      // goes on from one character later. A `\G`-anchored pattern therefore
      // finds nothing more, which is where perl's loop stops; under PCRE2's
      // rule `\G` follows the advance and the loop continues.
      //
      // Only this branch. The two ADVANCE rules below belong to dialects
      // that have no `\G` at all, and pinning it there would be a guess
      // rather than something a reference answered.
      return exec(regex, subject, length, &resolved, 0, GRX_EMPTY_OK,
          regex->program.search_start == GRX_SEARCH_START_PREVIOUS_END
              ? previous.end : GRX_NPOS,
          match, out_matched);
    }

    case GRX_ITERATE_ADVANCE_ONE_STOP_AT_END:
      // Vim's, and it differs from ADVANCE_ONE twice over.
      //
      // The loop ends when a match reaches the end of the subject, so the
      // empty match that would otherwise follow a non-empty one there is
      // not reported: `b*` over "ab" is "<>a<>" in vim and "<>a<><>" in
      // node, perl and `re` alike.
      //
      // And the search goes on from where the last match ended, with one
      // rule about what comes back: **the same span twice is not two
      // matches.** Vim's loop reports a match and looks again from its
      // end; where that finds the span it has just reported, it moves on a
      // character rather than reporting it twice.
      //
      // Two cases that no "did this match stand still" test covers at
      // once, which is what this replaced:
      //
      //   - `a\zs` over "aab" reports an empty span at 1, having consumed
      //     the "a" before it. Looking again from 1 finds the empty span
      //     at *2* - a different match - and vim reports it: "aXaXb".
      //   - `a\?\zs` over "ab" reports an empty span at 1 the same way,
      //     and looking again from 1 finds the *same* span, the optional
      //     `a` having matched nothing. vim reports "aXbX", not "aXXbX".
      //
      // The two attempts are identical and the answers differ, so what
      // decides is the answer and not the attempt. `\ze` is why this is a
      // span test rather than the engine's "no empty match where the
      // search began": `\zea` reports an empty span at 0 having walked a
      // character, so the engine sees a non-empty match there and would
      // hand back the same one forever.
      if (previous.end >= end) {
        return no_further_match(match, out_matched);
      }
      resolved.begin = previous.end;
      GRX_Result again = grx_regex_search_ex(regex, subject, length,
          &resolved, match, out_matched);
      if (again != GRX_OK || !*out_matched
          || match->captures[0].start != previous.start
          || match->captures[0].end != previous.end) {
        return again;
      }
      if (previous.end >= end) {
        return no_further_match(match, out_matched);
      }
      resolved.begin = advance_one(regex, subject, end, previous.end);
      return grx_regex_search_ex(regex, subject, length, &resolved, match,
          out_matched);

    case GRX_ITERATE_ADVANCE_ONE:
      // ECMAScript sets lastIndex to the end of the match and advances by one
      // only when the match was empty (AdvanceStringIndex); an empty match
      // immediately after a non-empty one is reported.
      if (was_empty) {
        if (previous.end >= end) {
          return no_further_match(match, out_matched);
        }
        resolved.begin = advance_one(regex, subject, end, previous.end);
      }
      else {
        resolved.begin = previous.end;
      }
      return grx_regex_search_ex(regex, subject, length, &resolved, match,
          out_matched);

    case GRX_ITERATE_ADVANCE_SKIP_ABUTTING: {
      // Go: "empty matches abutting a preceding match are ignored". The
      // ignoring is done to the *result*, not by the engine: an empty match
      // at the previous end is found, discarded, and the loop moves one
      // character past it. Telling the engine to refuse an empty match at
      // the start instead would be a different rule - it would let the
      // engine report a *non-empty* match beginning where the discarded
      // empty one did, which Go never does because it has already moved on.
      if (was_empty) {
        if (previous.end >= end) {
          return no_further_match(match, out_matched);
        }
        resolved.begin = advance_one(regex, subject, end, previous.end);
        return grx_regex_search_ex(regex, subject, length, &resolved, match,
            out_matched);
      }

      resolved.begin = previous.end;
      GRX_Result result = grx_regex_search_ex(regex, subject, length,
          &resolved, match, out_matched);
      if (result != GRX_OK || !*out_matched) {
        return result;
      }
      GRX_Capture found = match->captures[0];
      if (found.start != found.end || found.start != previous.end) {
        return result;
      }
      // The abutting empty match. Discard it and resume one character on;
      // whatever turns up there cannot abut, so this happens at most once.
      if (previous.end >= end) {
        return no_further_match(match, out_matched);
      }
      resolved.begin = advance_one(regex, subject, end, previous.end);
      return grx_regex_search_ex(regex, subject, length, &resolved, match,
          out_matched);
    }

    case GRX_ITERATE_COUNT:
    default:
      return GRX_ERR_INTERNAL;
  }
}

GRX_Engine grx_match_engine(const GRX_Match * match) {
  return match ? match->engine : GRX_ENGINE_COUNT;
}

size_t grx_match_steps(const GRX_Match * match) {
  return match ? match->steps : 0;
}

const GRX_Error * grx_match_error(const GRX_Match * match) {
  return match ? &match->error : NULL;
}

GRX_Result grx_match_span(
    const GRX_Match * match, GRX_Capture * out_capture) {
  return grx_match_group(match, 0, out_capture);
}

size_t grx_match_count(const GRX_Match * match) {
  return match ? match->count : 0;
}

GRX_Result grx_match_group(
    const GRX_Match * match, size_t index, GRX_Capture * out_capture) {
  if (!match || !out_capture || index >= match->count) {
    return GRX_ERR_INVALID;
  }

  *out_capture = match->captures[index];
  return GRX_OK;
}

GRX_Result grx_match_group_named(
    const GRX_Match * match, const char * name, GRX_Capture * out_capture) {
  if (!match || !name || !out_capture) {
    return GRX_ERR_INVALID;
  }

  size_t index = 0;
  GRX_Result result = grx_regex_capture_index(match->regex, name, &index);
  if (result != GRX_OK) {
    return result;
  }

  // With duplicate names - Perl always, PCRE2 under `(?J)` - one name stands
  // for several groups and the answer is the first of them that *took part*,
  // not the first of them. `(?<a>x)|(?<a>y)` against "y" has only the second
  // set, and perl reports "y" for it; pcre2_substring_get_byname() says the
  // same, "the first one that is set". Falling back to the first when none
  // is set keeps an unset answer rather than an error, which is what a
  // single-group name would have given.
  if (grx_regex_capture_name(match->regex, index)) {
    for (size_t i = index; i <= grx_regex_capture_count(match->regex); i++) {
      const char * candidate = grx_regex_capture_name(match->regex, i);
      if (!candidate || strcmp(candidate, name) != 0) {
        continue;
      }
      GRX_Capture capture;
      if (grx_match_group(match, i, &capture) == GRX_OK
          && capture.start != GRX_NPOS) {
        *out_capture = capture;
        return GRX_OK;
      }
    }
  }

  return grx_match_group(match, index, out_capture);
}

const char * grx_match_mark(const GRX_Match * match) {
  if (!match || match->mark == GRX_INDEX_NONE || !match->regex
      || !match->regex->mark_names
      || match->mark >= match->regex->mark_count) {
    return NULL;
  }

  return match->regex->mark_names[match->mark];
}

GRX_Result grx_match_dump(const GRX_Match * match, FILE * out) {
  if (!match || !out) {
    return GRX_ERR_INVALID;
  }

  fprintf(out, "match: %s groups=%zu\n", match->matched ? "yes" : "no",
      match->count);

  for (size_t i = 0; i < match->count; i++) {
    const GRX_Capture * capture = &match->captures[i];
    const char * name = grx_regex_capture_name(match->regex, i);
    if (capture->start == GRX_NPOS) {
      fprintf(out, "  %4zu  %-16s unset\n", i, name ? name : "");
    }
    else {
      fprintf(out, "  %4zu  %-16s %zu..%zu\n", i, name ? name : "",
          capture->start, capture->end);
    }
  }

  return GRX_OK;
}

void grx_match_destroy(GRX_Match * match) {
  if (!match) {
    return;
  }

  const GRX_Allocator * allocator = match->allocator;
  if (!allocator) {
    allocator = grx_allocator_default();
  }
  gcu_allocator_free(allocator, match->captures);
  gcu_allocator_free(allocator, match);
}
