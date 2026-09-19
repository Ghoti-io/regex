/**
 * @file
 *
 * The engines: selection, the match object, and the bound that is the whole
 * reason the Pike VM exists.
 *
 * What a pattern matches is checked against Node over tens of thousands of
 * rows by tools/oracle/match_diff.py. These are for what a span comparison
 * cannot see: which engine ran, what a limit does, and that a pattern
 * designed to make a backtracker hang does not make this one hang.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <chrono>
#include <cstdio>
#include <string>

#include "test_helpers.h"

TEST(Exec, SearchRejectsNullArguments) {
  int matched = 1;
  EXPECT_EQ(grx_regex_search(nullptr, "abc", 3, 0, GRX_ENGINE_AUTO, nullptr,
                nullptr, &matched),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_match(nullptr, "abc", 3, 0, GRX_ENGINE_AUTO, nullptr,
                nullptr, &matched),
      GRX_ERR_INVALID);
}

TEST(Exec, MatchObjectRejectsNullArguments) {
  GRX_Match * match = nullptr;
  EXPECT_EQ(grx_match_create(nullptr, nullptr, &match), GRX_ERR_INVALID);
  EXPECT_EQ(match, nullptr);
}

TEST(Match, AccessorsTolerateNull) {
  EXPECT_EQ(grx_match_count(nullptr), 0u);
  EXPECT_EQ(grx_match_engine(nullptr), GRX_ENGINE_COUNT);
  EXPECT_EQ(grx_match_dump(nullptr, stderr), GRX_ERR_INVALID);
  grx_match_destroy(nullptr);

  GRX_Capture capture;
  EXPECT_EQ(grx_match_group(nullptr, 0, &capture), GRX_ERR_INVALID);
  EXPECT_EQ(grx_match_group_named(nullptr, "name", &capture), GRX_ERR_INVALID);
}

TEST(Capture, UnsetIsDistinctFromEmpty) {
  // A group that did not participate and a group that matched the empty
  // string are different answers, and a caller has to be able to tell them
  // apart. GRX_NPOS is what says "did not participate"; 0..0 is a real span.
  GRX_Capture unset = {GRX_NPOS, GRX_NPOS};
  GRX_Capture empty = {0, 0};

  EXPECT_NE(unset.start, empty.start);
  EXPECT_EQ(empty.start, empty.end);
}

TEST(Exec, UnknownEngineIsInvalid) {
  // Checked before the regex is dereferenced would be wrong: a NULL regex is
  // invalid whatever the engine. This states the order the other way round -
  // an out-of-range engine with a NULL regex is still GRX_ERR_INVALID.
  int matched = 1;
  EXPECT_EQ(grx_regex_search(nullptr, "abc", 3, 0, (GRX_Engine)9999, nullptr,
                nullptr, &matched),
      GRX_ERR_INVALID);
}

// --------------------------------------------------------------------------
// Running a program
// --------------------------------------------------------------------------

namespace {

/** A compiled regex that frees itself. */
class Regex {
public:
  Regex(const char * pattern, uint32_t options = 0) {
    result_ = grx_regex_compile(pattern, GRX_SYNTAX_ECMASCRIPT, options,
        &regex_);
  }
  Regex(const Regex &) = delete;
  Regex & operator=(const Regex &) = delete;
  ~Regex() { grx_regex_free(regex_); }

  GRX_Regex * get() const { return regex_; }
  bool ok() const { return result_ == GRX_OK; }

private:
  GRX_Regex * regex_ = nullptr;
  GRX_Result result_ = GRX_ERR_INTERNAL;
};

} // namespace

TEST(Exec, SearchFindsTheLeftmostMatchAndMatchRequiresTheStart) {
  Regex regex("b+");
  ASSERT_TRUE(regex.ok());

  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex.get(), nullptr, &match), GRX_OK);

  int matched = 0;
  ASSERT_EQ(grx_regex_search(regex.get(), "aabbc", 5, 0, GRX_ENGINE_AUTO,
                nullptr, match, &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
  GRX_Capture capture;
  ASSERT_EQ(grx_match_group(match, 0, &capture), GRX_OK);
  EXPECT_EQ(capture.start, 2u);
  EXPECT_EQ(capture.end, 4u);
  EXPECT_EQ(grx_match_engine(match), GRX_ENGINE_PIKE);

  // The anchored entry point requires the match to *begin* where it was told,
  // which is what ECMAScript's `y` flag means.
  ASSERT_EQ(grx_regex_match(regex.get(), "aabbc", 5, 0, GRX_ENGINE_AUTO,
                nullptr, match, &matched),
      GRX_OK);
  EXPECT_FALSE(matched);

  ASSERT_EQ(grx_regex_match(regex.get(), "aabbc", 5, 2, GRX_ENGINE_AUTO,
                nullptr, match, &matched),
      GRX_OK);
  EXPECT_TRUE(matched);

  // A search from an offset skips what is before it.
  ASSERT_EQ(grx_regex_search(regex.get(), "aabbc", 5, 3, GRX_ENGINE_AUTO,
                nullptr, match, &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
  ASSERT_EQ(grx_match_group(match, 0, &capture), GRX_OK);
  EXPECT_EQ(capture.start, 3u);

  grx_match_destroy(match);
}

TEST(Exec, AMatchObjectIsOptionalAndSoAreItsSpans) {
  // A caller who only wants to know *whether* it matched should not have to
  // allocate a place to put the spans.
  Regex regex("a(b)c");
  ASSERT_TRUE(regex.ok());

  int matched = 0;
  EXPECT_EQ(grx_regex_search(regex.get(), "xabc", 4, 0, GRX_ENGINE_AUTO,
                nullptr, nullptr, &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
}

TEST(Exec, NamedGroupsComeBackByName) {
  Regex regex("(?<word>[a-z]+)-(?<digits>[0-9]+)");
  ASSERT_TRUE(regex.ok());

  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex.get(), nullptr, &match), GRX_OK);

  int matched = 0;
  ASSERT_EQ(grx_regex_search(regex.get(), "x abc-123 y", 11, 0,
                GRX_ENGINE_AUTO, nullptr, match, &matched),
      GRX_OK);
  ASSERT_TRUE(matched);

  GRX_Capture capture;
  ASSERT_EQ(grx_match_group_named(match, "word", &capture), GRX_OK);
  EXPECT_EQ(capture.start, 2u);
  EXPECT_EQ(capture.end, 5u);
  ASSERT_EQ(grx_match_group_named(match, "digits", &capture), GRX_OK);
  EXPECT_EQ(capture.start, 6u);
  EXPECT_EQ(capture.end, 9u);
  EXPECT_EQ(grx_match_group_named(match, "nosuch", &capture), GRX_ERR_INVALID);

  const std::string dump = grxtest::capture_dump(
      [match](FILE * out) { grx_match_dump(match, out); });
  EXPECT_NE(dump.find("word"), std::string::npos) << dump;

  grx_match_destroy(match);
}

TEST(Exec, AMatchObjectBelongsToOneRegex) {
  // Using one regex's match object with another would write spans sized for
  // the wrong capture count, which is a buffer overrun rather than a wrong
  // answer.
  Regex first("(a)(b)(c)");
  Regex second("x");
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());

  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(first.get(), nullptr, &match), GRX_OK);
  EXPECT_EQ(grx_match_count(match), 4u);

  int matched = 0;
  EXPECT_EQ(grx_regex_search(second.get(), "x", 1, 0, GRX_ENGINE_AUTO,
                nullptr, match, &matched),
      GRX_ERR_INVALID);

  grx_match_destroy(match);
}

TEST(Exec, TheLinearBoundIsWhatThisEngineIsFor) {
  // The patterns every article about catastrophic backtracking opens with.
  // A backtracking engine takes exponential time on each of these; the
  // lockstep simulation takes O(subject x program) because a program counter
  // is occupied at most once per position.
  //
  // The assertion is on *scaling*, not on wall-clock time: an absolute
  // threshold makes a test that fails on a loaded machine and passes on a
  // fast one. Ten times the subject may take appreciably more than ten times
  // as long on a real machine - caches, allocation - so the bar is set at
  // forty, which exponential behaviour clears by many orders of magnitude.
  const char * patterns[] = {"(a|aa)*b", "(a+)+b", "(a*)*b", "(a|a?)*b"};

  for (const char * pattern : patterns) {
    Regex regex(pattern);
    ASSERT_TRUE(regex.ok()) << pattern;

    double timings[2] = {0, 0};
    size_t lengths[2] = {2000, 20000};
    for (int i = 0; i < 2; i++) {
      const std::string subject(lengths[i], 'a');
      int matched = 1;
      auto started = std::chrono::steady_clock::now();
      ASSERT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(),
                    0, GRX_ENGINE_PIKE, nullptr, nullptr, &matched),
          GRX_OK)
          << pattern;
      auto finished = std::chrono::steady_clock::now();
      EXPECT_FALSE(matched) << pattern << " should not match all a's";
      timings[i] = std::chrono::duration<double>(finished - started).count();
    }

    // Guard against a clock too coarse to measure the small case.
    if (timings[0] > 1e-5) {
      EXPECT_LT(timings[1], timings[0] * 40)
          << pattern << ": ten times the subject took "
          << (timings[1] / timings[0]) << " times as long";
    }
  }
}

TEST(Exec, StepsAreCappedEvenThoughTheBoundIsStructural) {
  // The Pike VM does not need max_steps to terminate - the sparse set does
  // that. The cap is still honoured, because a caller who set one is
  // entitled to have it mean something.
  Regex regex("a*b");
  ASSERT_TRUE(regex.ok());

  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 10;

  const std::string subject(1000, 'a');
  int matched = 1;
  EXPECT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(), 0,
                GRX_ENGINE_AUTO, &limits, nullptr, &matched),
      GRX_ERR_LIMIT);
  EXPECT_FALSE(matched)
      << "a limit is not a match; the caller must not be told otherwise";
}

TEST(Exec, AnInvalidSubjectIsReportedRatherThanMatchedAsBytes) {
  // ECMAScript's subject is text. A byte sequence that is not valid UTF-8 is
  // not text, and matching it as if each byte were a character would report
  // spans that fall inside characters.
  Regex regex("a.c");
  ASSERT_TRUE(regex.ok());

  const std::string bad = std::string("a") + '\xC3' + "c";
  int matched = 1;
  EXPECT_EQ(grx_regex_search(regex.get(), bad.data(), bad.size(), 0,
                GRX_ENGINE_AUTO, nullptr, nullptr, &matched),
      GRX_ERR_INVALID);
}

TEST(Exec, SubjectLengthIsCapped) {
  Regex regex("a");
  ASSERT_TRUE(regex.ok());

  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_subject_length = 4;

  int matched = 0;
  EXPECT_EQ(grx_regex_search(regex.get(), "aaaaaa", 6, 0, GRX_ENGINE_AUTO,
                &limits, nullptr, &matched),
      GRX_ERR_LIMIT);
  EXPECT_EQ(grx_regex_search(regex.get(), "aaaa", 4, 0, GRX_ENGINE_AUTO,
                &limits, nullptr, &matched),
      GRX_OK);
}

TEST(Exec, EverythingIsFreedThroughTheCallersAllocator) {
  grxtest::CountingAllocator allocator;

  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile_with_allocator("(a|b)+c", 7,
                GRX_SYNTAX_ECMASCRIPT, 0, nullptr, allocator.get(), nullptr,
                &regex),
      GRX_OK);

  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, allocator.get(), &match), GRX_OK);

  int matched = 0;
  ASSERT_EQ(grx_regex_search(regex, "xxababc", 7, 0, GRX_ENGINE_AUTO, nullptr,
                match, &matched),
      GRX_OK);
  EXPECT_TRUE(matched);

  grx_match_destroy(match);
  grx_regex_free(regex);
  EXPECT_EQ(allocator.live(), 0) << "the engine leaked its thread state";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
