/**
 * @file
 *
 * The ReDoS corpus.
 *
 * Every record in `tests/data/redos/` is a pattern and subject pair that an
 * unmemoised backtracking engine cannot finish, and each says what *this*
 * backtracker does with it. Most of them it answers: it arms the bit-state
 * memo once a run has cost more than a memoised one could
 * (src/exec/exec_backtrack.c). The two whose body can match empty carry a
 * progress register, which makes a memo keyed on (instruction, position)
 * unsound, so those are still refused.
 *
 * The conformance runner would check the verdict and no more, and that is
 * half the claim. The other half is what makes either outcome worth having:
 *
 * - it has to arrive **quickly**. A limit reached after a minute is not a
 *   defence against a hostile pattern; it is the same outage with a
 *   different ending. Neither is an answer that takes one.
 * - another engine has to **answer**. A library whose only response to
 *   `(a+)+$` is "I gave up" has not solved the problem, it has renamed it.
 *   Where the Pike VM or the bit-state engine can run the program, it must
 *   return a real answer at the same limits, and the same one.
 *
 * The two together are the reason this library has three engines rather than
 * one, so they are tested together rather than left to the runner.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "rxt.h"
#include "test_helpers.h"

namespace {

/**
 * How long one pathological pair may take to be refused.
 *
 * plan.md WP-08 says one second, and that is the number for a release build:
 * the slowest row costs 414 ms there, so the bound has two and a half times
 * the margin and a regression has to be large rather than a busy machine.
 *
 * A sanitized build is about four times slower - measured, not assumed: this
 * test failed at 1.7 seconds a row the first time it ran under
 * ASan+UBSan - so the budget is scaled rather than loosened for everybody.
 * Loosening it would have been the easy fix and the wrong one: the number
 * that matters is the one a caller gets, and that is the release build's.
 */
#ifdef GRX_SANITIZERS
const int kBudgetMilliseconds = 5000;
#else
const int kBudgetMilliseconds = 1000;
#endif

/**
 * Whether this process is running under Valgrind.
 *
 * Valgrind is twenty to fifty times slower and has no compile-time macro to
 * scale a budget by, so the clock assertion is skipped there rather than
 * given a budget so large it would assert nothing. Everything else in this
 * file still runs under it, which is the point of running it under Valgrind
 * at all: the memory the engines allocate on the way to giving up is exactly
 * the memory nothing else in the suite frees under a limit.
 *
 * Detected from LD_PRELOAD, which Valgrind sets to its own preload library.
 * That is the only signal available without linking `valgrind/valgrind.h`,
 * which would make a test depend on a header the build does not require.
 */
bool under_valgrind() {
  const char * preload = std::getenv("LD_PRELOAD");
  return preload && std::strstr(preload, "vgpreload");
}

/** Render a match the way a record's `expect:` field spells it. */
std::string spans_of(const GRX_Match * match, int matched) {
  if (!matched) {
    return "nomatch";
  }
  std::string out;
  for (size_t i = 0; i < grx_match_count(match); i++) {
    GRX_Capture capture {GRX_NPOS, GRX_NPOS};
    grx_match_group(match, i, &capture);
    if (i) {
      out += " ";
    }
    out += capture.start == GRX_NPOS
        ? "-"
        : std::to_string(capture.start) + "-" + std::to_string(capture.end);
  }
  return out;
}

/** The same rendering of what a record says should happen. */
std::string expected_of(const grxtest::Record & record) {
  if (record.expectation == grxtest::Expectation::Limit) {
    return "limit";
  }
  if (record.expectation != grxtest::Expectation::Spans) {
    return "nomatch";
  }
  std::string out;
  for (size_t i = 0; i < record.spans.size(); i++) {
    const grxtest::Span & span = record.spans[i];
    if (i) {
      out += " ";
    }
    if (!span.set) {
      out += "-";
      continue;
    }
    size_t start = span.start == grxtest::kSubjectLength ? record.subject.size()
                                                         : span.start;
    size_t end = span.end == grxtest::kSubjectLength ? record.subject.size()
                                                     : span.end;
    out += std::to_string(start) + "-" + std::to_string(end);
  }
  return out;
}

struct Attempt {
  GRX_Result result;
  int matched;
  long milliseconds;
  size_t steps;
  std::string spans; ///< Rendered the way a record's `expect:` field is.
};

Attempt run(GRX_Regex * regex, const std::string & subject,
    GRX_Engine engine, const GRX_Limits * limits) {
  GRX_Match * match = nullptr;
  Attempt attempt {GRX_ERR_INTERNAL, 0, 0, 0, ""};
  if (grx_match_create(regex, nullptr, &match) != GRX_OK) {
    return attempt;
  }

  auto began = std::chrono::steady_clock::now();
  attempt.result = grx_regex_search(regex, subject.data(), subject.size(), 0,
      engine, limits, match, &attempt.matched);
  attempt.milliseconds
      = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - began)
            .count();
  attempt.steps = grx_match_steps(match);
  attempt.spans = attempt.result == GRX_ERR_LIMIT ? "limit"
      : attempt.result != GRX_OK ? grx_result_string(attempt.result)
                                 : spans_of(match, attempt.matched);
  grx_match_destroy(match);
  return attempt;
}

} // namespace

TEST(ReDoS, EveryPairIsAnsweredOrRefusedQuicklyAndNeverOnlyByOneEngine) {
  grxtest::VectorFile file;
  std::string error;
  ASSERT_TRUE(grxtest::read_vector_file(
      grxtest::data("redos/ecmascript.rxt"), &file, &error))
      << error;
  ASSERT_GT(file.records.size(), 10u)
      << "the corpus is too small to be measuring anything";

  GRX_Limits limits;
  grx_limits_default(&limits);

  size_t answered = 0;
  size_t refused = 0;
  size_t ran = 0;
  long slowest_refusal = 0;
  long slowest_answer = 0;

  // Under Valgrind, five rows rather than seventeen. A refused row costs
  // max_steps - ten million - and memcheck is thirty times slower, so the
  // full corpus was six minutes of a twelve-minute suite and dominated it.
  //
  // Five rather than four, because the fourth row is the first one that is
  // still refused, and the refusal path - backtrack until the limit, then
  // answer on another engine - is the one with memory to leak on the way out.
  // Every other row takes the same two paths and the memory each allocates
  // differs only in size. Nothing here reaches the bit-state engine at all:
  // every one of these patterns is regular, so the safe engine is always the
  // Pike VM, and bit-state's allocation is checked by test_bitstate.cpp,
  // which also runs under Valgrind.
  const size_t rows = under_valgrind() ? 5 : file.records.size();

  for (const grxtest::Record & record : file.records) {
    if (ran++ >= rows) {
      break;
    }
    GRX_Regex * regex = nullptr;
    GRX_Error compile_error;
    grx_error_clear(&compile_error);
    ASSERT_EQ(grx_regex_compile_with_allocator(record.pattern.data(),
                  record.pattern.size(), record.syntax, record.options,
                  &limits, nullptr, &compile_error, &regex),
        GRX_OK)
        << record.pattern << ": " << compile_error.message;

    // Half one: the backtracker does what the record says, inside the
    // budget. A row whose verdict has changed is not a failure of this test
    // to describe - it is a corpus that has gone stale, and saying so is
    // what makes the memo's reach something a person decided rather than
    // something that drifted.
    const std::string expected = expected_of(record);
    Attempt backtrack
        = run(regex, record.subject, GRX_ENGINE_BACKTRACK, &limits);
    EXPECT_EQ(backtrack.spans, expected)
        << "/" << record.pattern << "/ on the backtracker, after "
        << backtrack.steps
        << " steps; the corpus says " << expected
        << " and should be regenerated if that is no longer true";
    if (!under_valgrind()) {
      EXPECT_LT(backtrack.milliseconds, kBudgetMilliseconds)
          << "/" << record.pattern << "/ took " << backtrack.milliseconds
          << " ms";
    }
    if (expected == "limit") {
      refused++;
      slowest_refusal = std::max(slowest_refusal, backtrack.milliseconds);
    }

    // Half two: an engine that can run the program does, at the same
    // limits, without a limit - and says the same thing where the
    // backtracker said anything at all.
    GRX_Facts facts;
    grx_regex_facts(regex, &facts);
    GRX_Engine other = facts.is_regular ? GRX_ENGINE_PIKE
                                        : GRX_ENGINE_BITSTATE;
    Attempt safe = run(regex, record.subject, other, &limits);
    if (safe.result != GRX_ERR_UNSUPPORTED) {
      answered++;
      EXPECT_EQ(safe.result, GRX_OK)
          << "/" << record.pattern << "/ on the "
          << (other == GRX_ENGINE_PIKE ? "Pike VM" : "bit-state engine")
          << " gave " << grx_result_string(safe.result);
      if (!under_valgrind()) {
        EXPECT_LT(safe.milliseconds, kBudgetMilliseconds) << record.pattern;
      }
      EXPECT_LT(safe.steps, backtrack.steps)
          << "/" << record.pattern
          << "/ cost the safe engine as much as the backtracker";
      if (expected != "limit") {
        EXPECT_EQ(safe.spans, backtrack.spans)
            << "/" << record.pattern << "/: the engines disagree";
      }
      slowest_answer = std::max(slowest_answer, safe.milliseconds);
    }

    grx_regex_free(regex);
  }

  // Every pair must be answerable by *something*. A row that only the
  // backtracker can run is a row this library has no defence for, and the
  // corpus is where that would have to be recorded rather than discovered.
  EXPECT_EQ(answered, rows) << "some pair has no engine that can answer it";

  printf("\nredos: %zu of %zu pairs, %zu still refused by the backtracker; "
         "slowest refusal %ld ms, slowest answer %ld ms\n",
      rows, file.records.size(), refused, slowest_refusal, slowest_answer);
}

TEST(ReDoS, TheStepLimitReachesTheClosureWalkAndNotOnlyTheDispatchLoop) {
  // Every row of the corpus above is a backtracking bomb, and all of it is
  // ECMAScript. Both of those are why this shape was not in it.
  //
  // The Pike VM's cost is its closure walk, and the stall mask made that walk
  // super-linear: a program counter can hold one thread per distinct mask,
  // and `n` potentially-empty loops *in sequence* produce 2^n of them, per
  // subject position. ECMAScript cannot reach it, because its progress-check
  // arm is `fail` and a stalled iteration dies there; the `break` dialects -
  // Perl, POSIX, vim - carry it onwards with the register still equal to the
  // position, which is what multiplies the masks. The corpus being one
  // dialect made the one immune dialect the only one measured.
  //
  // None of that walk was charged to max_steps, which counted the threads
  // that survived into a list and nothing else. This pattern is forty-five
  // bytes. Over sixteen kilobytes of "a" it took forty-six seconds and
  // returned GRX_OK, having charged 163,850 of the ten million steps it was
  // allowed while the closure walked 101,079,031 program counters. There was
  // no value of max_steps that stopped it: the work it does is not the
  // quantity the field was counting.
  //
  // So the assertion is the one this file's docstring already makes for the
  // backtracker - a limit has to arrive, and arrive quickly - made of the
  // engine that is supposed to need no limit at all.
  const std::string pattern = "(a?)*(a?)*(a?)*(a?)*(a?)*(a?)*(a?)*(a?)*(a?)*";
  const std::string subject(256, 'a');

  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile(pattern.c_str(), GRX_SYNTAX_PERL, GRX_OPT_NONE,
                &regex),
      GRX_OK);

  GRX_Limits limits;
  grx_limits_default(&limits);
  // Far above what this subject costs an engine that charges what it does -
  // a plain pattern over 256 bytes is a few thousand steps - and far below
  // the 1,587,969 the closure actually walks here. A budget in that gap is
  // refused only by a count that can see the walk: before the closure was
  // charged this row spent 2,570 steps and answered GRX_OK.
  limits.max_steps = 500000;

  Attempt attempt = run(regex, subject, GRX_ENGINE_PIKE, &limits);
  EXPECT_EQ(attempt.result, GRX_ERR_LIMIT)
      << "the closure walked past max_steps and was not stopped; it spent "
      << attempt.steps << " steps and took " << attempt.milliseconds << " ms";
  EXPECT_GE(attempt.steps, limits.max_steps)
      << "refused without having counted the steps that got it there";
  if (!under_valgrind()) {
    EXPECT_LT(attempt.milliseconds, kBudgetMilliseconds)
        << "the refusal took " << attempt.milliseconds << " ms";
  }

  grx_regex_free(regex);
}

TEST(ReDoS, TheSafeEnginesStayLinearAsTheSubjectGrows) {
  // The refusal above is measured at one subject length. This is the claim
  // that makes the refusal unnecessary: doubling the subject must not square
  // the work on the engine that answers. Steps rather than the clock,
  // because the clock measures the machine.
  struct Row {
    const char * pattern;
    char filler;
  };
  const Row rows[] = {
    {"(a+)+$", 'a'},
    {"(a|aa)+$", 'a'},
    {"(.*a){20}$", 'a'},
    {"(x+x+)+y", 'x'},
  };

  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 0;
  limits.max_backtrack = 0;

  for (const Row & row : rows) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(row.pattern, GRX_SYNTAX_ECMASCRIPT,
                  GRX_OPT_UTF, &regex),
        GRX_OK)
        << row.pattern;


    GRX_Facts facts;
    grx_regex_facts(regex, &facts);
    GRX_Engine engine
        = facts.is_regular ? GRX_ENGINE_PIKE : GRX_ENGINE_BITSTATE;

    size_t previous = 0;
    for (size_t length : {200u, 400u, 800u}) {
      std::string subject(length, row.filler);
      subject += '!';
      Attempt attempt = run(regex, subject, engine, &limits);
      ASSERT_EQ(attempt.result, GRX_OK) << row.pattern;
      if (previous) {
        // Linear is 2x for a doubled subject; `(.*a){20}` is quadratic in
        // the subject by construction, so 8x is the ceiling that separates
        // "polynomial as designed" from "exponential again".
        EXPECT_LT(attempt.steps, previous * 8)
            << "/" << row.pattern << "/ at " << length << " took "
            << attempt.steps << " steps against " << previous
            << " for half as many";
      }
      previous = attempt.steps;
    }

    grx_regex_free(regex);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
