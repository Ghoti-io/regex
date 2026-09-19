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
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/exec.h>
#include <ghoti.io/regex/unicode.h>
#include <stddef.h>
#include <stdio.h>

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
static GRX_Result exec(const GRX_Regex * regex, const char * subject,
    size_t length, const GRX_SearchOptions * options, int anchored,
    GRX_EmptyMatchRule empty_rule, GRX_Match * match, int * out_matched) {
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
  if (limits->max_subject_length && end > limits->max_subject_length) {
    return GRX_ERR_LIMIT;
  }

  // The subject is checked once here rather than discovered mid-match by
  // whichever engine happened to read that far (documentation/design.md
  // section 5.1). Everything up to `end` is checked, not just the window,
  // because a lookbehind reads before `begin`.
  if ((regex->program.flags & GRX_PROGRAM_UTF)
      && !(options->flags & GRX_SEARCH_NO_UTF_CHECK)) {
    GRX_Result valid = grx_utf8_validate(subject, end, NULL);
    if (valid != GRX_OK) {
      return valid;
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
  GRX_Engine engine = options->engine;
  int needs_backtracking = grx_exec_program_needs_backtracking(regex);
  int memoizable = grx_exec_program_is_memoizable(regex);
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
  if (match) {
    for (size_t i = 0; i < match->count; i++) {
      match->captures[i] = (GRX_Capture) {GRX_NPOS, GRX_NPOS};
    }
    match->engine = engine;
    match->matched = 0;
    match->steps = 0;
  }

  GRX_ExecRequest request = {
    .regex = regex,
    .subject = subject,
    .length = end,
    .start = options->begin,
    .anchored = anchored,
    .not_bol = (options->flags & GRX_SEARCH_NOTBOL) != 0,
    .not_eol = (options->flags & GRX_SEARCH_NOTEOL) != 0,
    .empty_rule = (uint8_t)empty_rule,
    .limits = limits,
    .match = match,
    .out_steps = &steps,
    .memoize = engine == GRX_ENGINE_BITSTATE,
  };

  GRX_Result result = engine == GRX_ENGINE_PIKE
      ? grx_exec_pike(&request, out_matched)
      : grx_exec_backtrack(&request, out_matched);

  if (match) {
    match->steps = steps;
    if (result == GRX_OK) {
      match->matched = *out_matched;
    }
  }
  return result;
}

GRX_Result grx_regex_search_ex(const GRX_Regex * regex, const char * subject,
    size_t length, const GRX_SearchOptions * options, GRX_Match * match,
    int * out_matched) {
  return exec(regex, subject, length, options, 0, GRX_EMPTY_OK, match,
      out_matched);
}

GRX_Result grx_regex_match_ex(const GRX_Regex * regex, const char * subject,
    size_t length, const GRX_SearchOptions * options, GRX_Match * match,
    int * out_matched) {
  return exec(regex, subject, length, options, 1, GRX_EMPTY_OK, match,
      out_matched);
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
          GRX_EMPTY_REJECT_AT_START, match, out_matched);
      if (result != GRX_OK || *out_matched) {
        return result;
      }
      if (previous.end >= end) {
        return no_further_match(match, out_matched);
      }
      resolved.begin = advance_one(regex, subject, end, previous.end);
      return grx_regex_search_ex(regex, subject, length, &resolved, match,
          out_matched);
    }

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

  return grx_match_group(match, index, out_capture);
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
