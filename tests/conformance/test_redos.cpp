/**
 * @file
 *
 * The ReDoS corpus.
 *
 * Every record in `tests/data/redos/` is a pattern and subject pair that a
 * backtracking engine cannot finish. The conformance runner would check only
 * that the backtracker reports GRX_ERR_LIMIT, and that is half the claim.
 * The other half is what makes the limit worth having:
 *
 * - it has to arrive **quickly**. A limit reached after a minute is not a
 *   defence against a hostile pattern; it is the same outage with a
 *   different ending.
 * - another engine has to **answer**. A library whose only response to
 *   `(a+)+$` is "I gave up" has not solved the problem, it has renamed it.
 *   Where the Pike VM or the bit-state engine can run the program, it must
 *   return a real answer at the same limits.
 *
 * The two together are the reason this library has three engines rather than
 * one, so they are tested together rather than left to the runner.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <chrono>
#include <string>
#include <vector>

#include "rxt.h"
#include "test_helpers.h"

namespace {

/**
 * How long one pathological pair may take to be refused.
 *
 * plan.md WP-08 says one second. This is the wall clock and so is the one
 * number here that measures the machine as well as the library; it is a
 * ceiling far above what any row costs today (the slowest is milliseconds),
 * set so that a regression has to be an order of magnitude rather than a
 * busy build agent.
 */
const int kBudgetMilliseconds = 1000;

struct Attempt {
  GRX_Result result;
  int matched;
  long milliseconds;
  size_t steps;
};

Attempt run(GRX_Regex * regex, const std::string & subject,
    GRX_Engine engine, const GRX_Limits * limits) {
  GRX_Match * match = nullptr;
  Attempt attempt {GRX_ERR_INTERNAL, 0, 0, 0};
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
  grx_match_destroy(match);
  return attempt;
}

} // namespace

TEST(ReDoS, EveryPairIsRefusedQuicklyAndAnsweredByAnotherEngine) {
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
  long slowest_refusal = 0;
  long slowest_answer = 0;

  for (const grxtest::Record & record : file.records) {
    GRX_Regex * regex = nullptr;
    GRX_Error compile_error;
    grx_error_clear(&compile_error);
    ASSERT_EQ(grx_regex_compile_with_allocator(record.pattern.data(),
                  record.pattern.size(), record.syntax, record.options,
                  &limits, nullptr, &compile_error, &regex),
        GRX_OK)
        << record.pattern << ": " << compile_error.message;

    // Half one: the backtracker gives up, and does so inside the budget.
    Attempt backtrack
        = run(regex, record.subject, GRX_ENGINE_BACKTRACK, &limits);
    EXPECT_EQ(backtrack.result, GRX_ERR_LIMIT)
        << "/" << record.pattern << "/ finished on the backtracker in "
        << backtrack.steps
        << " steps; it is no longer a pathological pair and the corpus "
           "should say so";
    EXPECT_LT(backtrack.milliseconds, kBudgetMilliseconds)
        << "/" << record.pattern << "/ took " << backtrack.milliseconds
        << " ms to be refused";
    slowest_refusal = std::max(slowest_refusal, backtrack.milliseconds);

    // Half two: an engine that can run the program does, at the same
    // limits, without a limit.
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
      EXPECT_LT(safe.milliseconds, kBudgetMilliseconds) << record.pattern;
      EXPECT_LT(safe.steps, backtrack.steps)
          << "/" << record.pattern
          << "/ cost the safe engine as much as the backtracker";
      slowest_answer = std::max(slowest_answer, safe.milliseconds);
    }

    grx_regex_free(regex);
  }

  // Every pair must be answerable by *something*. A row that only the
  // backtracker can run is a row this library has no defence for, and the
  // corpus is where that would have to be recorded rather than discovered.
  EXPECT_EQ(answered, file.records.size())
      << "some pair has no engine that can answer it";

  printf("\nredos: %zu pairs; slowest refusal %ld ms, slowest answer %ld ms\n",
      file.records.size(), slowest_refusal, slowest_answer);
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
