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
 *
 * The cost of naming it is that the list can go stale in the other
 * direction, which is a campaign silently not covering a dialect that does
 * have a reader. WP-30 added GRX_SYNTAX_PYTHON here in the same commit that
 * built it; a front end landing without this line is a front end no soak
 * ever fuzzes.
 *
 * `Parse.TheFuzzCampaignAsksAboutEveryBuiltDialect` is what stops that being
 * a matter of remembering. It asserts this list and `grx_frontend_for()`
 * describe the same set, in both directions, in `make test` - which the
 * paragraph above asked for without anything checking it until a second copy
 * of the list, in the campaign driver, did fall a dialect behind.
 */
static const GRX_Syntax kBuiltSyntaxes[] = {
  GRX_SYNTAX_ECMASCRIPT,
  GRX_SYNTAX_PERL,
  GRX_SYNTAX_PCRE,
  GRX_SYNTAX_POSIX_BRE,
  GRX_SYNTAX_POSIX_ERE,
  GRX_SYNTAX_GNU_BRE,
  GRX_SYNTAX_GNU_ERE,
  GRX_SYNTAX_PYTHON,
  GRX_SYNTAX_VIM,
  GRX_SYNTAX_IREGEXP,
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
  // `GRX_FUZZ_SYNTAX=?` prints the list above and exits, so that a campaign
  // can schedule the dialects this harness really fuzzes instead of carrying a
  // copy of the list. The copy is what went wrong: the campaign driver in
  // `.local-regex-soak/campaign/run.sh` named nine dialects for a day after
  // I-Regexp became the tenth, and nothing said so, because a campaign cannot
  // notice a dialect it was never told to ask for. `kBuiltSyntaxes` going
  // stale is a gate - `Parse.TheFuzzCampaignAsksAboutEveryBuiltDialect` - and
  // a second list going stale was silence.
  if (name[0] == '?' && !name[1]) {
    for (size_t i = 0; i < kBuiltSyntaxCount; ++i) {
      printf("dialect: %s\n", grx_syntax_name(kBuiltSyntaxes[i]));
    }
    fflush(stdout);
    exit(0);
  }
  // Abort rather than fall back to the sweep: a campaign pinned to a
  // misspelled dialect would otherwise run the sweep for eight hours and be
  // reported as that dialect's result.
  if (grx_syntax_from_name(name, &fuzz_forced) != GRX_OK) {
    fprintf(stderr, "GRX_FUZZ_SYNTAX: unknown dialect %s\n", name);
    abort();
  }
}

/**
 * Read `GRX_FUZZ_SYNTAX` before libFuzzer loads the corpus.
 *
 * Defined here rather than three times for the same reason the list above is
 * shared: a decision copied into each harness is a decision that stops being
 * one. Every fuzz binary is one harness translation unit plus the library, so
 * this header is included once per binary and this is one definition each.
 *
 * Running before the corpus load is what makes `GRX_FUZZ_SYNTAX=?` answerable
 * as a question - the pattern corpus is 25,000 files, and a list that costs a
 * corpus load is a list a script will hard-code instead. It also moves the
 * abort on a misspelled pin to before that load rather than after it.
 */
extern "C" int LLVMFuzzerInitialize(int * argc, char *** argv) {
  (void)argc;
  (void)argv;
  fuzz_read_forced();
  return 0;
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
