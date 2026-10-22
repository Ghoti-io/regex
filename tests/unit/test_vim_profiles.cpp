/**
 * @file
 *
 * Public Vim compatibility profiles, checked against forced engines in
 * pinned Vim 9.2.1129. Group expectations use text because Vim's matchlist()
 * does not distinguish an unset group from an empty capture.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <memory>
#include <string>
#include <vector>

#include "test_helpers.h"

namespace {

class VimProfiles : public ::testing::TestWithParam<GRX_Syntax> {
 protected:
  bool old() const { return GetParam() == GRX_SYNTAX_VIM_BACKTRACK; }

  void expect(const char * pattern, const std::string & subject,
      size_t start, size_t end, const std::vector<std::string> & groups = {}) {
    SCOPED_TRACE(pattern);
    SCOPED_TRACE(grx_syntax_name(GetParam()));
    GRX_Regex * raw_regex = nullptr;
    ASSERT_EQ(grx_regex_compile(pattern, GetParam(), 0, &raw_regex), GRX_OK);
    std::unique_ptr<GRX_Regex, decltype(&grx_regex_free)> regex(
        raw_regex, grx_regex_free);
    EXPECT_EQ(grx_regex_syntax(regex.get()), GetParam());
    GRX_Match * raw_match = nullptr;
    ASSERT_EQ(grx_match_create(regex.get(), nullptr, &raw_match), GRX_OK);
    std::unique_ptr<GRX_Match, decltype(&grx_match_destroy)> match(
        raw_match, grx_match_destroy);
    int matched = 0;
    ASSERT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(), 0,
        GRX_ENGINE_BACKTRACK, nullptr, match.get(), &matched), GRX_OK);
    if (start == GRX_NPOS) {
      EXPECT_FALSE(matched);
      return;
    }
    ASSERT_TRUE(matched);
    GRX_Capture whole;
    ASSERT_EQ(grx_match_group(match.get(), 0, &whole), GRX_OK);
    EXPECT_EQ(whole.start, start);
    EXPECT_EQ(whole.end, end);
    for (size_t i = 0; i < groups.size(); i++) {
      GRX_Capture group;
      ASSERT_EQ(grx_match_group(match.get(), i + 1, &group), GRX_OK);
      std::string text;
      if (group.start != GRX_NPOS) {
        ASSERT_LE(group.start, group.end);
        ASSERT_LE(group.end, subject.size());
        text = subject.substr(group.start, group.end - group.start);
      }
      EXPECT_EQ(text, groups[i]) << "group " << i + 1;
    }
  }
};

INSTANTIATE_TEST_SUITE_P(Compatibility, VimProfiles, ::testing::Values(
    GRX_SYNTAX_VIM_BACKTRACK, GRX_SYNTAX_VIM_NFA));

TEST_P(VimProfiles, OriginalOptionalSequenceCaseUsesOneReferenceThroughout) {
  expect(R"(\(a\)\@!\|a\%[\_s])", "a\nb", 0, old() ? 2 : 1,
      {old() ? "a" : ""});
}

TEST_P(VimProfiles, OriginalLookbehindAndAtomicCaseUsesOneReferenceThroughout) {
  expect(R"([^ab]\(\w\+\)\@2<=\(a\zsb\)\@>)", "abcabc",
      old() ? 4 : 2, 5, {old() ? "c" : "bc", "ab"});
}

TEST_P(VimProfiles, FailedNegativeAssertionCaptureCanAffectABackreference) {
  expect(R"(\(a\)\@!\|a\1)", "aa", 0, old() ? 2 : 1,
      {old() ? "a" : ""});
  expect(R"(\(a\)\@!\|a)", "a", 0, 1, {old() ? "a" : ""});
}

TEST_P(VimProfiles, FailedBodiesAndOrdinaryAlternativesDiscardPartialCaptures) {
  expect(R"(a\(\(b\)c\)\@!)", "abd", 0, 1, {"", ""});
  expect(R"(\(\(a\)b\)\@=\|a)", "ac", 0, 1, {"", ""});
  expect(R"(\(a\)b\|a)", "ac", 0, 1, {""});
}

TEST_P(VimProfiles, AbandonedPositiveAssertionFollowsItsReference) {
  expect(R"(\(a\)\@=x\|a)", "aa", 0, 1, {old() ? "a" : ""});
}

TEST_P(VimProfiles, NestedNegativeAssertionsFollowTheirCapturePolicy) {
  expect(R"(\(\(a\)\@!\)\@!\|a)", "aa", 0, 0,
      {"", old() ? "a" : ""});
  expect(R"(\(a\)\@!\|\(a\)\@!\|a)", "aa", 0, 1,
      {old() ? "a" : "", old() ? "a" : ""});
}

TEST_P(VimProfiles, FailedNegativeLookbehindDoesNotLeakItsCaptures) {
  expect(R"(a\(a\)\@<!\|a)", "a", 0, 1, {""});
  expect(R"([^ab]\(\w\+\)\@<!\|c)", "abc", 2, 3, {""});
}

TEST_P(VimProfiles, AssertionCaptureLeakDoesNotSurviveAnotherSearchStart) {
  for (const char * subject : {"a", "ab"}) {
    expect(R"(\(a\)\@!\|a\1)", subject, old() ? 1 : 0, 1, {""});
    expect(R"(\%(\(a\)\@!\|a\)\1)", subject,
        old() ? 1 : 0, 1, {""});
  }
}

TEST_P(VimProfiles, PositiveAssertionCapturesResetAtAnotherSearchStart) {
  expect(R"(\(a\)\@=x\|b)", "ab", 1, 2, {""});
  expect(R"(\(a\)\@=x\|b\1)", "aba", 1, 2, {""});
}

TEST_P(VimProfiles, LookbehindChoosesCandidateBeforeQuantifierPreference) {
  for (const char * body : {R"(\w\+)", R"(\w\{-1,})", R"(c\|bc)",
           R"(bc\|c)"}) {
    std::string pattern = std::string(R"([^ab]\()") + body + R"(\)\@2<=)";
    expect(pattern.c_str(), "abcabc", 2, 3, {old() ? "c" : "bc"});
  }
  expect(R"([^ab]\(\w\+\)\@1<=)", "abcabc", 2, 3, {"c"});
  expect(R"([^ab]\(bc\)\@2<=)", "abcabc", 2, 3, {"bc"});
}

TEST_P(VimProfiles, UnboundedLookbehindUsesForwardCandidatePolicy) {
  expect(R"([^ab]\(\w\+\)\@<=)", "abcabc", 2, 3,
      {old() ? "c" : "abc"});
  expect(R"([^ab]\(\w\+\)\@0<=)", "abcabc", 2, 3,
      {old() ? "c" : "abc"});
  expect(R"(\(\([ab]\+\)\([bc]\+\)\)\@<=x)", "abcx", 3, 4,
      {old() ? "bc" : "abc", old() ? "b" : "ab", "c"});
}

TEST_P(VimProfiles, Utf8ByteThresholdAdmitsAStraddlingCharacterOnlyInNfa) {
  for (const std::string & scalar : {std::string("é"), std::string("€"),
           std::string("😀")}) {
    std::string subject = "a" + scalar + "x";
    size_t endpoint = subject.size() - 1;
    for (size_t bound = 1; bound <= scalar.size() + 1; bound++) {
      std::string pattern = R"(\(.\+\)\@)" + std::to_string(bound) + "<=x";
      if (old() && bound < scalar.size()) {
        expect(pattern.c_str(), subject, GRX_NPOS, GRX_NPOS);
      }
      else {
        expect(pattern.c_str(), subject, endpoint, endpoint + 1,
            {!old() && bound > scalar.size() ? "a" + scalar : scalar});
      }
    }
  }
}

TEST_P(VimProfiles, FinalOptionalUnderscoreClassHasItsOwnNewlinePolicy) {
  expect(R"(a\%[\_s])", "a\nb", 0, old() ? 2 : 1);
  expect(R"(a\%[\_s])", "a b", 0, 2);
  expect(R"(a\%[\_s])", "a\tb", 0, 2);
  expect(R"(a\%[\_sb])", "a\nb", 0, 3);
  expect(R"(a\_s\?)", "a\nb", 0, 2);
  expect(R"(a\%[\n])", "a\nb", 0, 2);
}

TEST_P(VimProfiles, AtomicStartAndEndMarkersFollowTheSelectedReference) {
  expect(R"([^ab]\(a\zsb\)\@>)", "abcabc", old() ? 4 : 2, 5, {"ab"});
  expect(R"([^ab]\(a\zeb\)\@>)", "abcabc", 2, old() ? 4 : 5, {"ab"});
  expect(R"([^ab]\(a\zsb\))", "abcabc", 4, 5, {"ab"});
  expect(R"([^ab]a\zs\(b\)\@>)", "abcabc", 4, 5, {"b"});
}

TEST_P(VimProfiles, AssertionEndMarkersAreProvisionalOnlyInNfa) {
  expect(R"(\(ab\zec\)\@=a)", "abc", 0, old() ? 2 : 1, {"abc"});
  expect(R"(\(a\zeb\)\@<=a)", "abab", 2, old() ? 2 : 3, {"ab"});
  expect(R"(ab\(a\zeb\)\@<=)", "ab", 0, 1, {"ab"});
  expect(R"(ab\(a\zeb\)\@<=c)", "abc", 0, old() ? 1 : 3, {"ab"});
}

TEST_P(VimProfiles, ExplicitEndMarkerReplacesTheProvisionalAssertionMarker) {
  expect(R"(\(a\zeb\)\@=\zeab)", "ab", 0, 0, {"ab"});
  expect(R"(\(a\zeb\)\@=\zea\zeb)", "ab", 0, 1, {"ab"});
  // An abandoned explicit end must not restrict the fallback match. The
  // backtracking reference retains a separate postfix-marker artifact here.
  if (!old()) {
    expect(R"(\(a\zeb\)\@=\zeabx\|ab)", "ab", 0, 2, {""});
  }
}

TEST_P(VimProfiles, TrailingEmptyIterationPreservesTheSelectedCaptureRule) {
  expect(R"(\%(a\|\zs\)*)", "aa", old() ? 0 : 2, 2);
  expect(R"(\(a\|\)*)", "aa", 0, 2, {old() ? "a" : ""});
  expect(R"(a\%(\zs\)*b)", "ab", 1, 2);
}

TEST_P(VimProfiles, MandatoryRepeatRetainsItsTrailingEmptyIteration) {
  expect(R"(\%(a\|\zs\)\+)", "aa", 2, 2);
  expect(R"(\%(a\|\zs\)\+)", "ab", 1, 1);
}

TEST_P(VimProfiles, ForwardLookbehindHonorsTheExecutionStepLimit) {
  GRX_Regex * raw_regex = nullptr;
  ASSERT_EQ(grx_regex_compile(R"(\(a\{128}\)\@<=)", GetParam(), 0,
      &raw_regex), GRX_OK);
  std::unique_ptr<GRX_Regex, decltype(&grx_regex_free)> regex(
      raw_regex, grx_regex_free);
  GRX_Match * raw_match = nullptr;
  ASSERT_EQ(grx_match_create(regex.get(), nullptr, &raw_match), GRX_OK);
  std::unique_ptr<GRX_Match, decltype(&grx_match_destroy)> match(
      raw_match, grx_match_destroy);
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 8;
  std::string subject(256, 'a');
  int matched = 0;
  EXPECT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(),
      subject.size(), GRX_ENGINE_BACKTRACK, &limits, match.get(), &matched),
      GRX_ERR_LIMIT);
}

TEST_P(VimProfiles, PublicNameAndCompiledIdentityRoundTrip) {
  const char * name = old() ? "vim-backtrack" : "vim-nfa";
  EXPECT_STREQ(grx_syntax_name(GetParam()), name);
  GRX_Syntax syntax = GRX_SYNTAX_COUNT;
  ASSERT_EQ(grx_syntax_from_name(name, &syntax), GRX_OK);
  EXPECT_EQ(syntax, GetParam());
  EXPECT_NE(grx_syntax_from_name("vim", &syntax), GRX_OK);
  expect("a", "a", 0, 1);
}

TEST_P(VimProfiles, PatternLocalEngineSelectorsAreExplicitlyUnsupported) {
  for (const char * pattern : {R"(\%#=0)", R"(\%#=1)", R"(\%#=2)",
           R"(\%#=0a)", R"(\%#=1a)", R"(\%#=2a)"}) {
    SCOPED_TRACE(pattern);
    GRX_Error error;
    grx_error_clear(&error);
    GRX_Regex * raw_regex = nullptr;
    GRX_Result result = grx_regex_compile_with_allocator(pattern,
        std::strlen(pattern), GetParam(), 0, nullptr, nullptr, &error,
        &raw_regex);
    std::unique_ptr<GRX_Regex, decltype(&grx_regex_free)> regex(
        raw_regex, grx_regex_free);
    EXPECT_EQ(result, GRX_ERR_UNSUPPORTED);
    EXPECT_EQ(error.diag, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED);
    ASSERT_LE(error.offset, std::strlen(pattern));
    EXPECT_LE(error.length, std::strlen(pattern) - error.offset);
    EXPECT_EQ(regex.get(), nullptr);
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
