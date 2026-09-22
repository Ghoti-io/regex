/**
 * @file
 *
 * Which dialect a fuzz harness compiles for, and how a campaign pins it.
 *
 * Shared rather than copied, because the copy is what went wrong. The rule
 * below - spend a campaign on the dialects that have a front end, and let
 * `GRX_FUZZ_SYNTAX` pin one - was written for fuzz_pattern in 9bac05f, after
 * a 974,873-run soak turned out to have spent fifteen-sixteenths of itself on
 * dialects that refuse every pattern at the first call. It never reached the
 * other two harnesses, which went on naming GRX_SYNTAX_ECMASCRIPT directly,
 * and nothing said so: a harness that only ever compiles one dialect passes
 * every run it is given.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_TESTS_FUZZ_FUZZ_SYNTAX_H
#define GHOTI_IO_GRX_TESTS_FUZZ_FUZZ_SYNTAX_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <ghoti.io/regex/regex.h>

/**
 * The dialects with a front end, which is what most of a campaign should
 * spend its inputs on.
 *
 * Named rather than derived: a dialect joins this list when it has a reader,
 * and a list that walked GRX_SYNTAX_COUNT would quietly spend most of the
 * run on dialects that refuse every pattern at the first call.
 */
static const GRX_Syntax kBuiltSyntaxes[] = {
  GRX_SYNTAX_ECMASCRIPT,
  GRX_SYNTAX_PERL,
  GRX_SYNTAX_PCRE,
  GRX_SYNTAX_POSIX_BRE,
  GRX_SYNTAX_POSIX_ERE,
  GRX_SYNTAX_GNU_BRE,
  GRX_SYNTAX_GNU_ERE,
};
static const size_t kBuiltSyntaxCount
    = sizeof(kBuiltSyntaxes) / sizeof(kBuiltSyntaxes[0]);

/** GRX_FUZZ_SYNTAX, read once. */
static GRX_Syntax fuzz_forced = GRX_SYNTAX_COUNT;
static int fuzz_forced_read = 0;

static void fuzz_read_forced(void) {
  fuzz_forced_read = 1;
  const char * name = getenv("GRX_FUZZ_SYNTAX");
  if (!name || !*name) {
    return;
  }
  // Abort rather than fall back to the sweep: a campaign pinned to a
  // misspelled dialect would otherwise run the sweep for eight hours and be
  // reported as that dialect's result.
  if (grx_syntax_from_name(name, &fuzz_forced) != GRX_OK) {
    fprintf(stderr, "GRX_FUZZ_SYNTAX: unknown dialect %s\n", name);
    abort();
  }
}

/** Whether a campaign pinned one dialect. */
static int fuzz_syntax_is_pinned(void) {
  if (!fuzz_forced_read) {
    fuzz_read_forced();
  }
  return fuzz_forced != GRX_SYNTAX_COUNT;
}

/**
 * The dialect this input should use: the pinned one, or one chosen from
 * `selector` when no pin is set.
 */
static GRX_Syntax fuzz_pick_syntax(uint32_t selector) {
  if (fuzz_syntax_is_pinned()) {
    return fuzz_forced;
  }
  return kBuiltSyntaxes[selector % kBuiltSyntaxCount];
}

#endif // GHOTI_IO_GRX_TESTS_FUZZ_FUZZ_SYNTAX_H
