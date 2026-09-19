/**
 * @file
 *
 * The bit-state engine, and which engine a search gets.
 *
 * The bit-state engine is the backtracker with one addition: an
 * (instruction, position) already tried and failed is not tried again. That
 * addition is worth an exponential-to-linear change on the patterns that
 * matter, and it is unsound for any program whose behaviour at an
 * instruction and position depends on how it got there. Both halves are
 * tested here - that it is fast enough on the patterns a caller would be
 * attacked with, and that it is refused on the programs where the memo would
 * lie.
 *
 * The selection table in exec.c has a row that no ECMAScript pattern can
 * reach today, because "not regular but memoizable" means an atomic group or
 * a possessive quantifier and ECMAScript has neither. That row is reached
 * here by saying so directly, which is what the fact exists to be.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <chrono>
#include <string>

#include "test_helpers.h"

#include "../../src/compile/compile_internal.h"

namespace {

/** What grx_result_string() says, so a test never spells it twice. */
const char * const kUnsupported = "Unsupported feature";
const char * const kLimit = "Limit exceeded";

/** A compiled regex that frees itself. */
class Regex {
public:
  Regex(const char * pattern, uint32_t options = GRX_OPT_UTF) {
    result_ = grx_regex_compile(pattern, GRX_SYNTAX_ECMASCRIPT, options,
        &regex_);
  }
  Regex(const Regex &) = delete;
  Regex & operator=(const Regex &) = delete;
  ~Regex() { grx_regex_free(regex_); }

  GRX_Regex * get() const { return regex_; }
  bool ok() const { return result_ == GRX_OK; }

  /**
   * Say the program is not regular without making it so.
   *
   * The selection table's middle row - not regular, but memoizable - is
   * where an atomic group or a possessive quantifier will land when a
   * dialect that has one arrives. Until then the only way to stand in that
   * row is to set the fact the row is keyed on, and doing that in a test is
   * better than leaving the row untested until WP-18.
   */
  void claim_not_regular() { regex_->facts.is_regular = 0; }

private:
  GRX_Regex * regex_ = nullptr;
  GRX_Result result_ = GRX_ERR_INTERNAL;
};

/** A match object that frees itself. */
class Match {
public:
  explicit Match(const Regex & regex) {
    grx_match_create(regex.get(), nullptr, &match_);
  }
  Match(const Match &) = delete;
  Match & operator=(const Match &) = delete;
  ~Match() { grx_match_destroy(match_); }
  GRX_Match * get() const { return match_; }

private:
  GRX_Match * match_ = nullptr;
};

/** Which engine a search on this regex actually used. */
GRX_Engine engine_for(const Regex & regex, const char * subject) {
  Match match(regex);
  int matched = 0;
  if (grx_regex_search(regex.get(), subject, strlen(subject), 0,
          GRX_ENGINE_AUTO, nullptr, match.get(), &matched)
      != GRX_OK) {
    return GRX_ENGINE_COUNT;
  }
  return grx_match_engine(match.get());
}

/** "start..end" of every group, or "none". */
std::string spans_of(GRX_Match * match, int matched) {
  if (!matched) {
    return "none";
  }
  std::string out;
  for (size_t i = 0; i < grx_match_count(match); i++) {
    GRX_Capture capture {};
    grx_match_group(match, i, &capture);
    if (i) {
      out += " ";
    }
    out += capture.start == GRX_NPOS
        ? "unset"
        : std::to_string(capture.start) + ".." + std::to_string(capture.end);
  }
  return out;
}

/** Search on one named engine and render the result. */
std::string search_on(const Regex & regex, const std::string & subject,
    GRX_Engine engine, const GRX_Limits * limits = nullptr) {
  Match match(regex);
  int matched = 0;
  GRX_Result result = grx_regex_search(regex.get(), subject.data(),
      subject.size(), 0, engine, limits, match.get(), &matched);
  if (result != GRX_OK) {
    return grx_result_string(result);
  }
  return spans_of(match.get(), matched);
}

} // namespace

// --------------------------------------------------------------------------
// The selection table
// --------------------------------------------------------------------------

TEST(EngineSelection, ARegularProgramGoesToThePikeVm) {
  // Both Pike and bit-state are linear here, and AUTO takes Pike: its memory
  // is bounded by the program, where bit-state's is the program times the
  // subject. Preferring bit-state for speed is a Phase 8 decision.
  Regex regex("a+b");
  ASSERT_TRUE(regex.ok());
  EXPECT_EQ(engine_for(regex, "aab"), GRX_ENGINE_PIKE);
}

TEST(EngineSelection, ANonRegularMemoizableProgramGoesToBitState) {
  Regex regex("a+b");
  ASSERT_TRUE(regex.ok());
  regex.claim_not_regular();
  EXPECT_EQ(engine_for(regex, "aab"), GRX_ENGINE_BITSTATE);
}

TEST(EngineSelection, ABackreferenceGoesToTheBacktracker) {
  Regex regex("(a)\\1");
  ASSERT_TRUE(regex.ok());
  EXPECT_EQ(engine_for(regex, "aa"), GRX_ENGINE_BACKTRACK);
}

TEST(EngineSelection, ALookaroundGoesToTheBacktracker) {
  Regex regex("(?=a)ab");
  ASSERT_TRUE(regex.ok());
  EXPECT_EQ(engine_for(regex, "ab"), GRX_ENGINE_BACKTRACK);
}

TEST(EngineSelection, AnEmptyIterationGuardGoesToTheBacktracker) {
  // `(a*)*` carries a progress register, which is per-thread history that
  // (instruction, position) does not capture - so it is regular, the Pike VM
  // takes it, and naming the bit-state engine is refused.
  Regex regex("(a*)*b");
  ASSERT_TRUE(regex.ok());
  EXPECT_EQ(engine_for(regex, "aab"), GRX_ENGINE_PIKE);

  regex.claim_not_regular();
  EXPECT_EQ(engine_for(regex, "aab"), GRX_ENGINE_BACKTRACK);
}

TEST(EngineSelection, NamingAnEngineThatCannotRunItIsRefusedNotSubstituted) {
  const char * const not_regular[] = {"(a)\\1", "(?=a)a", "(?<=a)b"};
  for (const char * pattern : not_regular) {
    Regex regex(pattern);
    ASSERT_TRUE(regex.ok()) << pattern;
    EXPECT_EQ(search_on(regex, "aa", GRX_ENGINE_PIKE), kUnsupported)
        << pattern;
    EXPECT_EQ(search_on(regex, "aa", GRX_ENGINE_BITSTATE), kUnsupported)
        << pattern;
  }

  // Regular but with a progress register: the Pike VM runs it, the memo
  // would be unsound, so bit-state is refused rather than degraded.
  Regex guarded("(a*)*b");
  ASSERT_TRUE(guarded.ok());
  EXPECT_NE(search_on(guarded, "aab", GRX_ENGINE_PIKE), kUnsupported);
  EXPECT_EQ(search_on(guarded, "aab", GRX_ENGINE_BITSTATE), kUnsupported);
}

TEST(EngineSelection, AnUnknownEngineIsStillInvalid) {
  Regex regex("a");
  ASSERT_TRUE(regex.ok());
  int matched = 0;
  EXPECT_EQ(grx_regex_search(regex.get(), "a", 1, 0, GRX_ENGINE_COUNT,
                nullptr, nullptr, &matched),
      GRX_ERR_INVALID);
}

// --------------------------------------------------------------------------
// The memo changes no answer
// --------------------------------------------------------------------------

TEST(BitState, AgreesWithTheOtherEnginesOnEveryGroup) {
  // The equivalence invariant. These are chosen for the shapes where a memo
  // could plausibly change a capture: overlapping alternatives, a lazy
  // quantifier, a nested group, an anchor.
  const char * const patterns[] = {
    "a+b", "(a|aa)+b", "(a)(b)?", "a??b", "^(a+)(b+)$", "[a-c]+",
    "(ab|a)(c|)", "a{2,4}?b", "\\b\\w+\\b", ".*", "(a)|(b)", "x*",
  };
  const char * const subjects[] = {
    "aab", "aaab", "ab", "b", "", "abcabc", "aaaaaaaab", "a b c",
  };

  for (const char * pattern : patterns) {
    Regex regex(pattern);
    ASSERT_TRUE(regex.ok()) << pattern;
    for (const char * subject : subjects) {
      std::string bits = search_on(regex, subject, GRX_ENGINE_BITSTATE);
      if (bits == std::string(kUnsupported)) {
        continue;
      }
      EXPECT_EQ(bits, search_on(regex, subject, GRX_ENGINE_BACKTRACK))
          << "/" << pattern << "/ on \"" << subject << "\"";
      EXPECT_EQ(bits, search_on(regex, subject, GRX_ENGINE_PIKE))
          << "/" << pattern << "/ on \"" << subject << "\"";
    }
  }
}

TEST(BitState, TheEmptyMatchRuleSurvivesTheMemo) {
  // The one thing the memo's key does not capture is the attempt's own
  // start, which the empty-match rule reads. The argument that this is still
  // sound is in already_tried()'s comment; these are the cases it is about.
  Regex regex("x*");
  ASSERT_TRUE(regex.ok());

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.engine = GRX_ENGINE_BITSTATE;

  // An empty match at the start is refused; the one at the next position is
  // not, and must not have been memoised away by the refusal.
  options.flags = GRX_SEARCH_NOTEMPTY_ATSTART;
  Match match(regex);
  int matched = 0;
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "ab", 2, &options, match.get(),
                &matched),
      GRX_OK);
  EXPECT_EQ(spans_of(match.get(), matched), "1..1");

  // And the three engines still agree about it.
  for (const char * pattern : {"x*", "a*", "(a)?", ""}) {
    Regex each(pattern);
    ASSERT_TRUE(each.ok()) << pattern;
    for (uint32_t flags : {(uint32_t)GRX_SEARCH_NOTEMPTY_ATSTART,
             (uint32_t)GRX_SEARCH_NOTEMPTY}) {
      GRX_SearchOptions probe;
      grx_search_options_default(&probe);
      probe.flags = flags;

      Match bits(each);
      Match back(each);
      int a = 0;
      int b = 0;
      probe.engine = GRX_ENGINE_BITSTATE;
      GRX_Result first = grx_regex_search_ex(each.get(), "aab", 3, &probe,
          bits.get(), &a);
      probe.engine = GRX_ENGINE_BACKTRACK;
      ASSERT_EQ(grx_regex_search_ex(each.get(), "aab", 3, &probe, back.get(),
                    &b),
          GRX_OK);
      if (first == GRX_ERR_UNSUPPORTED) {
        continue;
      }
      ASSERT_EQ(first, GRX_OK) << pattern;
      EXPECT_EQ(spans_of(bits.get(), a), spans_of(back.get(), b))
          << "/" << pattern << "/ with flags " << flags;
    }
  }
}

TEST(BitState, LeftmostFirstSurvivesTheMemo) {
  // The memo skips a state that failed. A state that *succeeded* returned,
  // so the first success found is still the highest-priority one - which is
  // what leftmost-first means and what a memo could quietly destroy.
  Regex regex("(a|ab)(c|bcd)");
  ASSERT_TRUE(regex.ok());
  EXPECT_EQ(search_on(regex, "abcd", GRX_ENGINE_BITSTATE), "0..4 0..1 1..4");
  EXPECT_EQ(search_on(regex, "abcd", GRX_ENGINE_BACKTRACK), "0..4 0..1 1..4");
}

// --------------------------------------------------------------------------
// The memo does something
// --------------------------------------------------------------------------

TEST(BitState, CompletesWhatMakesTheBacktrackerGiveUp) {
  // The point of the engine. Each row is a classic catastrophic
  // backtracking pair - a subject that makes the plain backtracker explore
  // exponentially many ways to fail.
  struct Row {
    const char * pattern;
    std::string subject;
    /**
     * The answer, checked against the Pike VM rather than against Node.
     *
     * Node is the oracle everywhere else in this suite and cannot be one
     * here: V8's engine is a backtracker, and `([a-zA-Z]+)*$` against this
     * subject is exactly the input it cannot finish - it was still running
     * after two minutes and had to be killed. That is the whole reason this
     * engine exists, so it is worth writing down rather than quietly using
     * a different oracle.
     */
    const char * expected;
  };
  const Row rows[] = {
    {"(a+)+b", std::string(40, 'a'), "none"},
    {"(a|aa)+b", std::string(40, 'a'), "none"},
    // These two can still match: the outer star takes zero iterations and
    // `$` holds at the end, so the answer is an empty match there rather
    // than no match. Answering at all is the point.
    {"([a-zA-Z]+)*$", std::string(34, 'a') + "!", "35..35 unset"},
    {"(\\w+\\s?)+$", std::string(34, 'a') + "!", "none"},
  };

  for (const Row & row : rows) {
    Regex regex(row.pattern);
    ASSERT_TRUE(regex.ok()) << row.pattern;

    // The plain backtracker runs out of budget, which is what the budget is
    // for and is not a wrong answer - it is no answer.
    GRX_Limits limits;
    grx_limits_default(&limits);
    limits.max_steps = 2000000;
    EXPECT_EQ(search_on(regex, row.subject, GRX_ENGINE_BACKTRACK, &limits),
        kLimit)
        << row.pattern;

    // The bit-state engine answers, under the same budget, in bounded time.
    auto began = std::chrono::steady_clock::now();
    std::string answer
        = search_on(regex, row.subject, GRX_ENGINE_BITSTATE, &limits);
    auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began);
    EXPECT_EQ(answer, row.expected) << row.pattern;
    EXPECT_LT(took.count(), 2000)
        << row.pattern << " took " << took.count() << " ms";
  }
}

TEST(BitState, AReDoSPatternWhoseBodyCanMatchEmptyIsStillRefused) {
  // `(a|a?)+b` is the same shape as the rows above and is *not* memoizable,
  // because `a?` can match empty and so the loop carries a progress
  // register. Refusing it is the honest answer: the memo would be unsound,
  // and a caller who needs a bound for this pattern needs the Pike VM, which
  // can run it and is linear.
  Regex regex("(a|a?)+b");
  ASSERT_TRUE(regex.ok());
  const std::string subject(34, 'a');

  EXPECT_EQ(search_on(regex, subject, GRX_ENGINE_BITSTATE), kUnsupported);
  EXPECT_EQ(search_on(regex, subject, GRX_ENGINE_PIKE), "none");
  EXPECT_EQ(engine_for(regex, subject.c_str()), GRX_ENGINE_PIKE);
}

TEST(BitState, StaysLinearAsTheSubjectGrows) {
  // Doubling the subject must not square the work. The step count is the
  // measure rather than the clock, because a clock measures the machine.
  Regex regex("(a|aa)+b");
  ASSERT_TRUE(regex.ok());

  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 0;
  limits.max_backtrack = 0;

  size_t previous = 0;
  for (size_t length : {200u, 400u, 800u}) {
    Match match(regex);
    const std::string subject(length, 'a');
    int matched = 0;
    ASSERT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(), 0,
                  GRX_ENGINE_BITSTATE, &limits, match.get(), &matched),
        GRX_OK);
    EXPECT_FALSE(matched);

    size_t steps = grx_match_steps(match.get());
    if (previous) {
      // Linear would be 2x for a doubled subject. Four times the previous
      // count is a generous ceiling that quadratic behaviour still breaks.
      EXPECT_LT(steps, previous * 4)
          << length << " chars took " << steps << " steps against "
          << previous << " for half as many";
    }
    previous = steps;
  }
}

// --------------------------------------------------------------------------
// The memory budget
// --------------------------------------------------------------------------

TEST(BitState, RefusesABitmapThatWillNotFit) {
  // One bit per instruction per position. A caller who named this engine
  // asked for the linear-time guarantee, so a bitmap that will not fit is
  // GRX_ERR_LIMIT rather than a quiet fall back to the engine that hangs.
  Regex regex("(a|aa)+b");
  ASSERT_TRUE(regex.ok());

  const std::string subject(4096, 'a');

  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_match_memory = 64;
  EXPECT_EQ(search_on(regex, subject, GRX_ENGINE_BITSTATE, &limits),
      kLimit);

  // The same search with room for the bitmap answers.
  limits.max_match_memory = 8 * 1024 * 1024;
  EXPECT_EQ(search_on(regex, subject, GRX_ENGINE_BITSTATE, &limits), "none");

  // And the plain backtracker is unaffected by that budget, because it has
  // no bitmap to fit.
  limits.max_match_memory = 64;
  limits.max_steps = 100000;
  EXPECT_EQ(search_on(regex, "aab", GRX_ENGINE_BACKTRACK, &limits),
      "0..3 1..2");
}

TEST(BitState, TheBudgetBoundsEveryEnginesScratch) {
  // max_match_memory is a cap on what a match costs, not a bit-state
  // setting: the Pike VM's live thread states are scratch too and are
  // bounded by the same number. The backtracker's stack is bounded by
  // max_backtrack instead, so it is the one engine the budget does not
  // reach - and that is a fact worth pinning, because it is the reason a
  // caller cannot bound the backtracker with this field alone.
  Regex regex("a+b");
  ASSERT_TRUE(regex.ok());

  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_match_memory = 1;
  EXPECT_EQ(search_on(regex, "aab", GRX_ENGINE_PIKE, &limits), kLimit);
  EXPECT_EQ(search_on(regex, "aab", GRX_ENGINE_BACKTRACK, &limits), "0..3");
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
