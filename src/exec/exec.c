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
#include <stddef.h>
#include <stdio.h>

#include "exec_internal.h"

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
  for (size_t i = 0; i < count; i++) {
    match->captures[i] = (GRX_Capture) {GRX_NPOS, GRX_NPOS};
  }

  *out_match = match;
  return GRX_OK;
}

/**
 * The shared body of grx_regex_search() and grx_regex_match(): validate,
 * choose an engine, hand it the request.
 */
static GRX_Result exec(const GRX_Regex * regex, const char * subject,
    size_t length, size_t start, int anchored, GRX_Engine engine,
    const GRX_Limits * limits, GRX_Match * match, int * out_matched) {
  if (!regex || !out_matched || (!subject && length) || start > length) {
    return GRX_ERR_INVALID;
  }
  if ((unsigned)engine >= (unsigned)GRX_ENGINE_COUNT) {
    return GRX_ERR_INVALID;
  }
  if (match && match->regex != regex) {
    return GRX_ERR_INVALID;
  }
  *out_matched = 0;

  GRX_Limits defaults;
  if (!limits) {
    grx_limits_default(&defaults);
    limits = &defaults;
  }
  if (limits->max_subject_length && length > limits->max_subject_length) {
    return GRX_ERR_LIMIT;
  }

  int needs_backtracking = grx_exec_program_needs_backtracking(regex);
  if (engine == GRX_ENGINE_AUTO) {
    engine = needs_backtracking ? GRX_ENGINE_BACKTRACK : GRX_ENGINE_PIKE;
  }
  else if (engine == GRX_ENGINE_PIKE && needs_backtracking) {
    // Asked for by name, so the answer is that it cannot be done, not a
    // silent substitution of the engine that can hang.
    return GRX_ERR_UNSUPPORTED;
  }

  if (match) {
    for (size_t i = 0; i < match->count; i++) {
      match->captures[i] = (GRX_Capture) {GRX_NPOS, GRX_NPOS};
    }
    match->engine = engine;
    match->matched = 0;
  }

  GRX_ExecRequest request = {
    .regex = regex,
    .subject = subject,
    .length = length,
    .start = start,
    .anchored = anchored,
    .limits = limits,
    .match = match,
  };

  GRX_Result result = engine == GRX_ENGINE_PIKE
      ? grx_exec_pike(&request, out_matched)
      : grx_exec_backtrack(&request, out_matched);

  if (match && result == GRX_OK) {
    match->matched = *out_matched;
  }
  return result;
}

GRX_Result grx_regex_search(const GRX_Regex * regex, const char * subject,
    size_t length, size_t start, GRX_Engine engine, const GRX_Limits * limits,
    GRX_Match * match, int * out_matched) {
  return exec(regex, subject, length, start, 0, engine, limits, match,
      out_matched);
}

GRX_Result grx_regex_match(const GRX_Regex * regex, const char * subject,
    size_t length, size_t start, GRX_Engine engine, const GRX_Limits * limits,
    GRX_Match * match, int * out_matched) {
  return exec(regex, subject, length, start, 1, engine, limits, match,
      out_matched);
}

GRX_Engine grx_match_engine(const GRX_Match * match) {
  return match ? match->engine : GRX_ENGINE_COUNT;
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
