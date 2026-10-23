/**
 * @file
 *
 * Public Vim compatibility profiles, checked against forced engines in
 * pinned Vim 9.2.1129. Group expectations use text because Vim's matchlist()
 * does not distinguish an unset group from an empty capture.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "test_helpers.h"

namespace {

class VimProfiles : public ::testing::TestWithParam<GRX_Syntax> {
 protected:
  bool old() const { return GetParam() == GRX_SYNTAX_VIM_BACKTRACK; }

  void expect(const char * pattern, const std::string & subject,
      size_t start, size_t end, const std::vector<std::string> & groups = {},
      GRX_Engine engine = GRX_ENGINE_BACKTRACK) {
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
                  engine, nullptr, match.get(), &matched), GRX_OK);
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
      if (i >= grx_regex_capture_count(regex.get())) {
        EXPECT_EQ(groups[i], "") << "group " << i + 1;
        continue;
      }
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

TEST_P(VimProfiles, CountedRepetitionRetainsItsCountAndBackedgeHistory) {
  struct Row {
    const char * pattern;
    const char * subject;
    size_t old_start;
    size_t nfa_start;
    bool bitstate = true;
  };
  const Row rows[] = {
      {R"(\%(aa\|a\)\{2}aab)", "aaab", 0, GRX_NPOS},
      {R"(\%(a\|aa\)\{2}b)", "aaaab", 1, 0},
      {R"(\%(\%(a\)\{1,2}\)\{2}b)", "aab", GRX_NPOS, 0},
      {R"(\%(\%(a\)\{1,2}\)\{2}b)", "aaab", 0, 0},
      {R"(\%(a\{1,2}\)\{2}b)", "aab", 0, 0},
      {R"(\%(a\{0,2}\)\{2}b)", "b", GRX_NPOS, 0},
      {R"(\%(a\{0,2}\)\{3}b)", "ab", 0, 0},
      {R"(\%(a\{0,2}\)\{1,3}b)", "b", 0, 0, false},
      {R"(\%(\%(a\)\+\)\{2}b)", "aab", 0, 0},
      {R"(\%(\%(a\)\{-2,3}\)\{2,3}b)", "aaaaab", 1, 0},
  };
  for (const Row & row : rows) {
    SCOPED_TRACE(row.pattern);
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(row.pattern, GetParam(), 0, &regex), GRX_OK);
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    for (GRX_Engine engine : {GRX_ENGINE_AUTO, GRX_ENGINE_BACKTRACK,
             GRX_ENGINE_PIKE, GRX_ENGINE_BITSTATE, GRX_ENGINE_DFA}) {
      SCOPED_TRACE(engine);
      int matched = 0;
      GRX_Result result = grx_regex_search(regex, row.subject,
          std::strlen(row.subject), 0, engine, nullptr, match, &matched);
      if (engine == GRX_ENGINE_DFA ||
          (engine == GRX_ENGINE_BITSTATE && !row.bitstate) || (old() &&
              (engine == GRX_ENGINE_PIKE || engine == GRX_ENGINE_BITSTATE))) {
        EXPECT_EQ(result, GRX_ERR_UNSUPPORTED);
        continue;
      }
      ASSERT_EQ(result, GRX_OK);
      const size_t start = old() ? row.old_start : row.nfa_start;
      EXPECT_EQ(matched, start != GRX_NPOS);
      if (matched) {
        GRX_Capture whole;
        ASSERT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
        EXPECT_EQ(whole.start, start);
        EXPECT_EQ(whole.end, std::strlen(row.subject));
      }
      if (old() && engine == GRX_ENGINE_AUTO) {
        EXPECT_EQ(grx_match_engine(match), GRX_ENGINE_BACKTRACK);
      }
    }
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
}

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

TEST_P(VimProfiles, AssertionFrontiersControlCaptureContextsAndMarkers) {
  expect(R"(\(a\ze\)\@=a*)", "aab", 0, 1, {"a"});
  expect(R"(\(a\ze\)\@=aa)", "aab", 0, old() ? 1 : 2, {"a"});
  expect(R"(\(a\ze\)\@=..)", "aab", 0, 1, {"a"});
  expect(R"(\(a\)\@=\(a\)\@=a)", "ab", 0, 1, {old() ? "a" : "", "a"});
  expect(R"(\(a\)\@=\(\1b\)\@=ab)", "ab", old() ? 0 : GRX_NPOS,
      old() ? 2 : GRX_NPOS, {"a", "ab"});
  expect(R"(\(a\)\@=\(\1b\)\@=..)", "ab", 0, 2, {"a", "ab"});
  expect(R"(\(a\)\@=\(a\1\)\@=ab)", "ab", old() ? GRX_NPOS : 0,
      old() ? GRX_NPOS : 2, {"", "a"});
  expect(R"(\(a\)\@=\([ab]\1\)\@=[ab][ab])", "ab", old() ? GRX_NPOS : 0,
      old() ? GRX_NPOS : 2, {"", "a"});
  expect(R"(\(a\)\@=\(.\1\)\@=..)", "ab", old() ? GRX_NPOS : 0,
      old() ? GRX_NPOS : 2, {"", "a"});
  expect(R"(\(a\)\@=\(a\)\@=\%(a\|\)a)", "aab", 0, 2, {"a", "a"});
}

TEST_P(VimProfiles, DeferredAssertionsRetainContinuationOwnedCaptures) {
  expect(R"(\(a\)\@=\(a\))", "ab", 0, 1, {"a", "a"});
  expect(R"(\(a\)\@=\(a\)\1)", "aa", 0, 2, {"a", "a"});
  expect(R"(\(a\)\@=\(a\)\@=\1a)", "aab", 0, 2, {"a", "a"});
  expect(R"(\(\(a\ze\)\@=a\)\@=\(a\)\@=\2)", "ab", 0, 1,
      {old() ? "a" : "", "a", "a"});
  expect(R"(\(x\)\%(a\1\)\@=a)", "xaa", old() ? GRX_NPOS : 0,
      old() ? GRX_NPOS : 2, {"x"});
  expect(R"(\(x\)\(\%(\1\)\@=a\)\@=.)", "xaa", GRX_NPOS, GRX_NPOS);
}

TEST_P(VimProfiles, LookbehindObservesItsReferencesEvaluationOrder) {
  expect(R"(\(a\)\@<=\1)", "aa", 1, old() ? 1 : 2, {"a"});
  expect(R"(\(a\)\@<=\(a\)\1\l)", "aaab", 1, 3, {"a", "a"});
  expect(R"(a\(a\ze\)\@<=aa\ze)", "aaaab", 0, old() ? 1 : 3, {"a"});
  expect(R"(a\(a\ze\)\@<=\zsa*)", "aaaab", 1, 1, {"a"});
  expect(R"(a\(a\ze\)\@<=\zs\%(a*\)a)", "aaaab", 1, old() ? 1 : 4, {"a"});
  expect(
      R"(\(a\zeb\)\@=\zeabx\|ab)", "ab", 0, old() ? 1 : 2, {old() ? "ab" : ""});
}

TEST_P(VimProfiles, ForwardSubrunEndMarkersTrackMandatoryAtomicContinuations) {
  expect(R"(\(\(a\)\@=a\ze\)\@=\%(ab\)\@>)", "ab", 0, old() ? 1 : 2,
      {old() ? "a" : "", "a"});
  expect(R"(\(\(a\ze\)\@=a\ze\)\@=\%(a*\)\@>)", "aab", 0, 1,
      {old() ? "a" : "", "a"});
  expect(R"(\(\%(b\)\@!a\ze\)\@=\%(ab\)\@>)", "ab", 0, old() ? 1 : 2, {"a"});
  expect(R"(\(a\(a\)\@<=\ze\)\@=\%(ab\)\@>)", "ab", 0, 1, {"a", "a"});
  expect(R"(\(\%(a\)\ze\)\@=\%(ab\)\@>)", "ab", 0, 1, {"a"});
}

TEST_P(VimProfiles, FreshLookbehindBodiesCanChangeTheAssertionVerdict) {
  expect(R"(\(a\)\(a\1\)\@<=..)", "aab", old() ? GRX_NPOS : 0,
      old() ? GRX_NPOS : 3, {"", "a"});
  expect(R"(\(a\)\(a\1\)\@<!..)", "aab", old() ? 0 : GRX_NPOS,
      old() ? 3 : GRX_NPOS, {"a", ""});
  expect(R"(\(x\)\(x\)\@<=a)", "xa", 0, 2, {old() ? "x" : "", "x"});
}

TEST_P(
    VimProfiles, SearchStartMarkersAndSuccessiveLookbehindRetainTheirOwners) {
  expect(R"(\(a\zsb\)\@=\m[\u0041]\I\(ab\)\@1<!\|\%<3c)", "ab", old() ? 1 : 0,
      old() ? 1 : 0, {old() ? "ab" : "", ""});
  expect(R"(\(a\)\@<=\(a\)\@<=[a-c]\M)", "ab", 1, 2, {old() ? "a" : "", "a"});
  expect(R"(\m\(a\)\@<=\(\(a\)\@=a\)\@<=\(a\(b\)\@=\)\@<=)", "ab", 1, 1,
      {"a", old() ? "a" : "", "a", "a", "b"});
}

TEST_P(VimProfiles, ClassLeadingLookbehindKeepsItsDeferredCaptureOwner) {
  const char * pattern = R"(\(\w\+\)\@2<=\(a\(c\)\@!\)\@<=\I)";
  for (const char * subject : {"ab", "aaab", "abc", "abcabc"}) {
    expect(pattern, subject, 1, 2, {"a", "a", ""});
  }
  expect(pattern, "xayaz", 2, 3, {old() ? "a" : "xa", "a", ""});
  expect(pattern, "read", 3, 4, {old() ? "a" : "ea", "a", ""});
  for (const char * subject : {"aéb", "a×b"}) {
    expect(pattern, subject, 1, 3, {"a", "a", ""});
  }
  expect(R"(\([a]\)\@2<=\(a\)\@<=[a-c])", "ab", 1, 2, {"a", "a"});
  expect(R"(\(a\)\@2<=\(a\)\@<=[a-c])", "ab", 1, 2, {old() ? "a" : "", "a"});
}

TEST_P(VimProfiles,
    EagerAndDeferredBehindKeepEvaluationAndReportedOwnersSeparate) {
  expect(R"(\(a\+\)\@<=\(a\)\@<=[a-c])", "aaab", 1, 2, {"a", "a"});
  expect(R"(\(a\)\(a\)\@<=.)", "aab", 0, 2, {"a", "a"});
  expect(R"(\(a\)\@=a\(a\1\)\@<=.)", "aab", old() ? 1 : 0, old() ? 3 : 2,
      {"a", old() ? "aa" : "a"});
  expect(R"(\(a\)\@>a\(a\1\)\@<=.)", "aab", 0, 3, {"a", "aa"});
  expect(R"(\(a\)\@>\(a\1\)\@<=.)", "aab", 1, 3, {"a", "aa"});
}

TEST_P(VimProfiles, AtomicCaptureOwnershipPreservesNestedAssertionChildren) {
  expect(R"(\(\%(a\)\@=a\)\@<=\(a\)\@<=\%(a\)\@>)", "aab", 1, 2,
      {old() ? "a" : "", "a"});
  expect(R"(\(\(a\)\@=a\)\@<=\(a\)\@<=\%(a\)\@>)", "aab", 1, 2,
      {old() ? "a" : "", "a", "a"});
  expect(R"(\(\(b\)\@!a\)\@=\(a\)\@<=\%(aa\)\@>)", "aaab", 1, 3,
      {old() ? "a" : "", "", "a"});
  expect(R"(a\(\%(a\)\@>a\)\@<=\(a\)\@<=\%(a\)\@>)", "aaab", 1, 3, {"aa", "a"});
  expect(R"(a\(\%(a\)\@=a\)\@<=\(a\)\@<=\%(a*\)\@>)", "aaab", 0, 3, {"a", "a"});
}

TEST_P(VimProfiles, EarlierAtomicBasesPreserveLaterForwardedCaptureOwners) {
  for (const char * subject : {"aab", "aaab", "aaaab"}) {
    size_t end = std::strlen(subject) - 1;
    expect(R"(\Va\+\(a\+\)\@>\(a\(b\)\@=\)\@<=)", subject, 0, end,
        {"a", "a", "b"});
    expect(R"(a\+\(a\+\)\@>\(a\(c\)\@!\)\@<=.)", subject, 0, end + 1,
        {"a", "a", ""});
    expect(R"(a\+\%(a\+\)\@>\(a\%(b\)\@=\)\@<=\%(b\)\@>)", subject, 0, end + 1,
        {"a"});
  }
}

TEST_P(VimProfiles, RepeatedAssertionsRetainTheCorrectMarkerActivation) {
  expect(R"(\%(a\(a\ze\)\@<=\)\{0,3}a*b)", "aaaab", 0, old() ? 1 : 2, {"a"});
  expect(R"(\%(a\(a\ze\)\@<=\)\{0,3}a*b)", "xaaab", 1, old() ? 2 : 3, {"a"});
}

TEST_P(VimProfiles, AtomicAcceptingFrontiersRetainIncomingPathPriority) {
  expect(R"(a\+\(a\)\@>\(a\)\@<=)", "aaab", 0, old() ? 3 : 2, {"a", "a"});
  expect(R"(a\+\(aa\)\@>\(a\)\@<=)", "aaaaaab", 0, old() ? 6 : 4, {"aa", "a"});
  expect(R"(a\+\(a*\)\@>\(a\(b\)\@=\)\@<=)", "aaab", 0, 3,
      {old() ? "" : "a", "a", "b"});
  expect(R"(\%(a\|aa\)\(a\{1,2}\)\@>\(a\(b\)\@=\)\@<=)", "aaaab",
      old() ? 0 : GRX_NPOS, old() ? 4 : GRX_NPOS, {"aa", "a", "b"});
  expect(R"(a\+\(a\)\@>\%(\))", "aaab", 0, old() ? 3 : 2, {"a"});
  expect(R"(a\+\(a\)\@>\ze)", "aaab", 0, 3, {"a"});
  expect(R"(a\+\(a\)\@>\1)", "aaaab", 0, 4, {"a"});
  expect(R"(a\+\(a\)\@>b)", "aaab", old() ? 0 : GRX_NPOS, old() ? 4 : GRX_NPOS,
      {"a"});
  expect(R"(a\(a\)\@>b)", "aaaab", 2, 5, {"a"});
  expect(R"(\%(aa\|a\)\(a\)\@>b)", "aaaab", 1, 5, {"a"});
  expect(R"(\%(\(a\ze\)\@=\%(a\)\@>\)\{1,3}b)", "aaaab", 1, 4, {"a"});
}

TEST_P(VimProfiles, AtomicFrontierStateBelongsToOnePublicSearchCall) {
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile(R"(a\+\(a\)\@>\(a\)\@<=)", GetParam(), 0, &regex),
      GRX_OK);
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
  for (GRX_Engine engine : {GRX_ENGINE_AUTO, GRX_ENGINE_BACKTRACK}) {
    for (const std::string subject : {"aaab", "aab", "aaaab", "aaab"}) {
      int matched = 0;
      ASSERT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                    engine, nullptr, match, &matched),
          GRX_OK);
      ASSERT_TRUE(matched);
      GRX_Capture whole;
      ASSERT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
      EXPECT_EQ(whole.start, 0u);
      EXPECT_EQ(
          whole.end, !old() && subject.size() == 4 ? 2u : subject.size() - 1);
      GRX_Capture group;
      ASSERT_EQ(grx_match_group(match, 1, &group), GRX_OK);
      EXPECT_EQ(subject.substr(group.start, group.end - group.start), "a");
    }
  }
  grx_match_destroy(match);
  grx_regex_free(regex);
}

TEST_P(VimProfiles, AtomicEntryContextsResetBetweenPublicSearchCalls) {
  const char * patterns[] = {
      R"(\(a\)\%(z\)\@>x\|\%(ab\)\@>)",
      R"(\(\(a\)\%(z\)\@>x\)\|\%(ab\)\@>)",
  };
  for (const char * pattern : patterns) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(pattern, GetParam(), 0, &regex), GRX_OK);
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    for (const std::string subject : {"aab", "ab", "aab"}) {
      SCOPED_TRACE(pattern);
      SCOPED_TRACE(subject);
      int matched = 0;
      ASSERT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                    GRX_ENGINE_BACKTRACK, nullptr, match, &matched),
          GRX_OK);
      ASSERT_TRUE(matched);
      GRX_Capture whole;
      ASSERT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
      EXPECT_EQ(whole.start, subject.size() - 2);
      EXPECT_EQ(whole.end, subject.size());
      for (size_t i = 1; i <= grx_regex_capture_count(regex); i++) {
        GRX_Capture group;
        ASSERT_EQ(grx_match_group(match, i, &group), GRX_OK);
        std::string text;
        if (group.start != GRX_NPOS) {
          ASSERT_LE(group.start, group.end);
          ASSERT_LE(group.end, subject.size());
          text = subject.substr(group.start, group.end - group.start);
        }
        bool captured = !old() && subject.size() == 3 &&
            (grx_regex_capture_count(regex) == 1 || (!old() && i == 2));
        EXPECT_EQ(text, captured ? "a" : "");
      }
    }
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
  expect(R"(\(a\)\%(z\)\@>x\|\%(\1ab\)\@>)", "aab", old() ? 1 : GRX_NPOS,
      old() ? 3 : GRX_NPOS, {""});
  expect(R"(\(a\)\%(z\)\@>x\|\1ab)", "aab", 1, 3, {""});
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

TEST_P(VimProfiles, OptionalIncomingCaptureIdentifiesAtomicCandidate) {
  if (old()) {
    expect(R"(\(a\)\?\%(\1a\)\@>b)", "aaab", 0, 4, {"a"});
  }
  if (!old()) {
    expect(R"(\(a\)\?\%(\1a\)\@>b)", "aaab", 0, 4, {"a"});
  }
}

TEST_P(VimProfiles, SuccessiveAtomicsPreserveReplacedOptionalContext) {
  if (old()) {
    expect(R"(\(a\)\?\%(aa\)\@>\%(a\)\@>b)", "aaab", 0, 4, {""});
  }
  if (!old()) {
    expect(R"(\(a\)\?\%(aa\)\@>\%(a\)\@>b)", "aaab", 0, 4, {"a"});
  }
}

TEST_P(VimProfiles, PostAtomicBehindReadsItsOwnCaptureContext) {
  if (old()) {
    expect(R"(\(a\)\?\%(a\)\@>\(a\1\)\@<=.)", "ab", 0, 2, {"", "a"});
  }
  if (!old()) {
    expect(R"(\(a\)\?\%(a\)\@>\(a\1\)\@<=.)", "ab", GRX_NPOS, GRX_NPOS);
  }
}

TEST_P(VimProfiles, NestedAheadKeepsItsAtomicIncomingContext) {
  if (old()) {
    expect(R"(\(a\)\@=\%(\(a\)\@=a\)\@>b)", "ab", 0, 2, {"a", "a"});
  }
  if (!old()) {
    expect(R"(\(a\)\@=\%(\(a\)\@=a\)\@>b)", "ab", 0, 2, {"a", "a"});
  }
}

TEST_P(VimProfiles, NestedAheadCaptureRemainsVisibleToContinuationReference) {
  if (old()) {
    expect(R"(\(a\)\@=\%(\(a\)\@=a\)\@>\1)", "ab", GRX_NPOS, GRX_NPOS);
  }
  if (!old()) {
    expect(R"(\(a\)\@=\%(\(a\)\@=a\)\@>\1)", "ab", GRX_NPOS, GRX_NPOS);
  }
}

TEST_P(VimProfiles, CompetingAtomicMarkerRetainsItsAcceptingOwner) {
  if (old()) {
    expect(R"(\%(a\|aa\)\%(a\ze\)\@>b)", "aaaaaab", 3, 6);
  }
  if (!old()) {
    expect(R"(\%(a\|aa\)\%(a\ze\)\@>b)", "aaaaaab", 3, 3);
  }
}

TEST_P(VimProfiles, GreedyIncomingCaptureSurvivesNestedAtomicAhead) {
  if (old()) {
    expect(R"(\(a*\)\%(\(a\)\@=a\)\@>b)", "aaab", 0, 4, {"aa", "a"});
  }
  if (!old()) {
    expect(R"(\(a*\)\%(\(a\)\@=a\)\@>b)", "aaab", 0, 4, {"aa", "a"});
  }
}

TEST_P(VimProfiles, NullableAtomicReturnUsesItsCallerCaptureContext) {
  if (old()) {
    expect(R"(\(a*\)\%(a*\)\@>\ze)", "ab", 0, 1, {"a"});
  }
  if (!old()) {
    expect(R"(\(a*\)\%(a*\)\@>\ze)", "ab", 0, 1, {""});
  }
}

TEST_P(VimProfiles, FailedBranchAtomicContextRemainsVisibleToBehind) {
  if (old()) {
    expect(R"(\(a\)\%(z\)\@>x\|\%(a\)\@>\(a\)\@<=)", "ab", 0, 1, {"", "a"});
  }
  if (!old()) {
    expect(R"(\(a\)\%(z\)\@>x\|\%(a\)\@>\(a\)\@<=)", "ab", 0, 1, {"a", "a"});
  }
}

TEST_P(VimProfiles, NewAtomicContextReplacesEarlierSearchCaptureHistory) {
  if (old()) {
    expect(R"(\(a\)\%(z\)\@>x\|\%(\(a\)\@=a\)\@>\%(a\)\@>b)", "aaaab", 2, 5,
        {"", "a"});
  }
  if (!old()) {
    expect(R"(\(a\)\%(z\)\@>x\|\%(\(a\)\@=a\)\@>\%(a\)\@>b)", "aaaab", 2, 5,
        {"", "a"});
  }
}

TEST_P(VimProfiles, AtomicBehindBodyCompletesBeforeCandidateAcceptanceAab) {
  if (old()) {
    expect(R"(\%(a\@<=a\)\@>b)", "aab", 1, 3);
  }
  if (!old()) {
    expect(R"(\%(a\@<=a\)\@>b)", "aab", 1, 3);
  }
}

TEST_P(VimProfiles, AtomicBehindBodyRejectsBeforeCandidateAcceptanceAaab) {
  if (old()) {
    expect(R"(\%(a\@<=a\)\@>b)", "aaab", 2, 4);
  }
  if (!old()) {
    expect(R"(\%(a\@<=a\)\@>b)", "aaab", GRX_NPOS, GRX_NPOS);
  }
}

TEST_P(VimProfiles, AtomicBehindBodyCompletesBeforeCandidateAcceptanceAaaab) {
  if (old()) {
    expect(R"(\%(a\@<=a\)\@>b)", "aaaab", 3, 5);
  }
  if (!old()) {
    expect(R"(\%(a\@<=a\)\@>b)", "aaaab", 3, 5);
  }
}

TEST_P(VimProfiles, AtomicBodyReferenceKeepsItsEntryInputBeforeLiteralTail) {
  expect(R"(\(a\|b\)\%(a\1\)\@>X)", "babX", 0, 4, {"b"});
}

TEST_P(VimProfiles, AtomicBodyAndConstantBehindHaveSeparateCaptureContexts) {
  expect(R"(\(a\|b\)\%(a\1\)\@>\(ab\)\@<=X)", "babX", 0, 4,
      {old() ? "b" : "a", "ab"});
}

TEST_P(VimProfiles, AtomicBodyAndDependentBehindHaveSeparateReferenceInputs) {
  if (old()) {
    expect(R"(\(a\|b\)\%(a\1\)\@>\(a\1\)\@<=X)", "babX", 0, 4, {"b", "ab"});
  }
  if (!old()) {
    expect(R"(\(a\|b\)\%(a\1\)\@>\(a\1\)\@<=X)", "babX", GRX_NPOS, GRX_NPOS);
  }
}

TEST_P(VimProfiles, AtomicContinuationNegativeBehindUsesItsSelectedContext) {
  if (old()) {
    expect(R"(\(a\|b\)\%(a\1\)\@>\%(a\1\)\@<!X)", "babX", GRX_NPOS, GRX_NPOS);
  }
  if (!old()) {
    expect(R"(\(a\|b\)\%(a\1\)\@>\%(a\1\)\@<!X)", "babX", 0, 4, {"a"});
  }
}

TEST_P(VimProfiles, NumberedAtomicBodyWriteOwnsACompleteBehindResult) {
  expect(
      R"(\(a\)\%(z\)\@>x\|\%(\(a\)\)\@>\(a\)\@<=)", "ab", 0, 1, {"", "a", "a"});
}

TEST_P(VimProfiles, AtomicReferenceBodyWithTwoTrailingLiteralsKeepsLaterOwner) {
  expect(R"(\(ab\|ba\)\%(a\1ba\)\@>\(ababa\)\@<=X)", "baababaX", 0, 8,
      {old() ? "ba" : "ab", "ababa"});
}

TEST_P(VimProfiles, ChainedAtomicMarkerRetainsItsCompletedBodyEndOwner) {
  expect(R"(\(a\)\?\%(a\ze\)\@>\%(a\)\@>b)", "aaaab", 1, old() ? 3 : 1, {"a"});
}

TEST_P(VimProfiles, CompetingPrefixRetainsChainedAtomicMarkerEndOwner) {
  expect(R"(\%(a\|aa\)\%(a\ze\)\@>\%(a\)\@>b)", "aaaaaaab", 3, old() ? 6 : 3);
}

TEST_P(VimProfiles, ReferenceBodyCallersKeepTheirClosureThroughASecondAtomic) {
  expect(R"(\(a\)\?\%(\1a\)\@>\%(a\)\@>b)", "aaaaab", 1, 6, {"a"});
}

TEST(VimProfileState, AtomicInputSeedPersistsOnlyWithinOnePublicSearch) {
  GRX_Regex * raw_regex = nullptr;
  ASSERT_EQ(grx_regex_compile(
                R"(\(.\)\?\%(\1a\)\@>b)", GRX_SYNTAX_VIM_NFA, 0, &raw_regex),
      GRX_OK);
  std::unique_ptr<GRX_Regex, decltype(&grx_regex_free)> regex(
      raw_regex, grx_regex_free);
  GRX_Match * raw_match = nullptr;
  ASSERT_EQ(grx_match_create(regex.get(), nullptr, &raw_match), GRX_OK);
  std::unique_ptr<GRX_Match, decltype(&grx_match_destroy)> match(
      raw_match, grx_match_destroy);
  const std::string subject = "baabab";
  for (GRX_Engine engine : {GRX_ENGINE_BACKTRACK, GRX_ENGINE_AUTO}) {
    for (size_t offset : {size_t{0}, size_t{1}, size_t{2}, size_t{0}}) {
      SCOPED_TRACE(offset);
      int matched = 0;
      ASSERT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(),
                    offset, engine, nullptr, match.get(), &matched),
          GRX_OK);
      if (offset == 1) {
        EXPECT_FALSE(matched);
        continue;
      }
      ASSERT_TRUE(matched);
      GRX_Capture whole;
      ASSERT_EQ(grx_match_group(match.get(), 0, &whole), GRX_OK);
      EXPECT_EQ(whole.start, offset == 0 ? 3u : 2u);
      EXPECT_EQ(whole.end, offset == 0 ? 6u : 4u);
      GRX_Capture group;
      ASSERT_EQ(grx_match_group(match.get(), 1, &group), GRX_OK);
      std::string text;
      if (group.start != GRX_NPOS) {
        ASSERT_LE(group.start, group.end);
        ASSERT_LE(group.end, subject.size());
        text = subject.substr(group.start, group.end - group.start);
      }
      EXPECT_EQ(text, offset == 0 ? "b" : "");
    }
  }
}

TEST(VimProfileState, AtomicInputSeedResetsAcrossVimSearchNextCalls) {
  GRX_Regex * raw_regex = nullptr;
  ASSERT_EQ(grx_regex_compile(R"(\(.\)\?\%(\1a\)\@>b)",
                GRX_SYNTAX_VIM_NFA, 0, &raw_regex), GRX_OK);
  std::unique_ptr<GRX_Regex, decltype(&grx_regex_free)> regex(
      raw_regex, grx_regex_free);
  GRX_Match * raw_match = nullptr;
  ASSERT_EQ(grx_match_create(regex.get(), nullptr, &raw_match), GRX_OK);
  std::unique_ptr<GRX_Match, decltype(&grx_match_destroy)> match(
      raw_match, grx_match_destroy);
  const std::string subject = "baababbaabab";
  for (GRX_Engine engine : {GRX_ENGINE_BACKTRACK, GRX_ENGINE_AUTO}) {
    GRX_SearchOptions search;
    grx_search_options_default(&search);
    search.engine = engine;
    int matched = 0;
    ASSERT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(), 0,
                  engine, nullptr, match.get(), &matched), GRX_OK);
    for (size_t start : {3u, 9u}) {
      ASSERT_TRUE(matched);
      GRX_Capture whole;
      ASSERT_EQ(grx_match_group(match.get(), 0, &whole), GRX_OK);
      EXPECT_EQ(whole.start, start);
      EXPECT_EQ(whole.end, start + 3);
      GRX_Capture group;
      ASSERT_EQ(grx_match_group(match.get(), 1, &group), GRX_OK);
      ASSERT_NE(group.start, GRX_NPOS);
      EXPECT_EQ(subject.substr(group.start, group.end - group.start), "b");
      ASSERT_EQ(grx_regex_search_next(regex.get(), subject.data(), subject.size(),
                    &search, match.get(), &matched), GRX_OK);
    }
    EXPECT_FALSE(matched);
  }
}

TEST_P(VimProfiles, ExactOneWrappersPreserveTheEnclosingAssertionActivation) {
  expect(R"(\%(\%(\(a\ze\)\@=a\)\{1}\)\{0,3}b)", "aab",
      0, 2, {"a"});
  expect(R"(\%(\%(\%(\(a\ze\)\@=a\)\{1}\)\{1}\)\{1,3}b)",
      "aaaab", 1, 4, {"a"});
  expect(R"(\%(\%(a\(a\ze\)\@<=\)\{1}\)\{0,3}b)", "aaab",
      0, old() ? 1 : 2, {"a"});
}

TEST_P(VimProfiles, NestedRepeatCopiesKeepDistinctMarkerExitOwnership) {
  expect(R"(\%(\%(a\(a\ze\)\@<=\)\{0,2}\)\{0,3}b)", "aaab",
      0, old() ? 1 : 3, {"a"});
  expect(R"(\%(\%(a\(a\ze\)\@<=\)*\)\{1,3}b)", "ab",
      0, 1, {"a"});
  expect(R"(\%(a\(a\ze\)\@<=\)\{0,4}b)", "aaab",
      0, old() ? 1 : 2, {"a"});
  expect(R"(\%(\%(a\(a\ze\)\@<=\)\{0,3}\)\{0,2}b)", "aaaaab",
      0, old() ? 1 : 4, {"a"});
}

TEST_P(VimProfiles, ExplicitEndFrontiersRetainTheSuccessfulAtomicCaller) {
  expect(R"(a*\zs\(a\)\%(a\)\@>\zeb)", "aaab", 1, 3, {"a"});
  expect(R"(a*\zs\(a\)\%(a*\)\@>\zeb)", "aaaab", 3, 4, {"a"});
}

TEST_P(VimProfiles, CapturesBeforeAReportedStartKeepTheirAtomicReportOwner) {
  expect(R"(\(a*\)\zs\(a\)\%(aa\)\@>\%(a\)\@<=b)", "aaab",
      0, 4, {old() ? "" : "a", "a"});
  expect(R"(\(a*\)\zs\(a\)\%(aa\)\@>\%(a\)\@<=b)", "aaaaaaab",
      4, 8, {old() ? "aaaa" : "aaaaa", "a"});
  expect(R"(\(a*\)\zs\(a\)\%(\1a\)\@>\%(a\)\@<=b)", "aab",
      0, 3, {old() ? "" : "a", "a"});
}

TEST(VimProfileState, MandatoryNestedCopiesSeparateActivationFromMarkerHistory) {
  for (const char * pattern : {
           R"(\%(\%(a\(a\ze\)\@<=\)\{1,2}\)\{2}b)",
           R"(\%(\%(a\(a\ze\)\@<=\)\+\)\{2}b)",
       }) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(pattern, GRX_SYNTAX_VIM_NFA, 0, &regex), GRX_OK);
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    int matched = 0;
    ASSERT_EQ(grx_regex_search(regex, "aab", 3, 0, GRX_ENGINE_BACKTRACK,
                  nullptr, match, &matched), GRX_OK);
    ASSERT_TRUE(matched);
    GRX_Capture whole;
    ASSERT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
    EXPECT_EQ(whole.start, 0u);
    EXPECT_EQ(whole.end, 1u);
    GRX_Capture group;
    ASSERT_EQ(grx_match_group(match, 1, &group), GRX_OK);
    EXPECT_EQ(group.end - group.start, 1u);
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
}

TEST_P(VimProfiles, AtomicOwnershipReportsExactCaptureParticipationAndPositions) {
  struct Row { const char * pattern; const char * subject; size_t start; size_t end;
    std::vector<GRX_Capture> groups; };
  // The column gate pins the identical-character capture independently of
  // its text. The optional x and explicit empty group separate nonparticipation
  // from participation with zero width, including after reusing a Match.
  const Row rows[] = {
      {R"(a*\zs\(\%2ca\)\%(a\)\@>\zeb)", "aaab", 1, 3, {{1, 2}}},
      {R"(^\(x\)\?\(\)\%(ab\)\@>)", "xab", 0, 3, {{0, 1}, {1, 1}}},
      {R"(^\(x\)\?\(\)\%(ab\)\@>)", "ab", 0, 2,
          {{GRX_NPOS, GRX_NPOS}, {0, 0}}},
  };
  for (const Row & row : rows) {
    SCOPED_TRACE(row.pattern);
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(row.pattern, GetParam(), 0, &regex), GRX_OK);
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    for (GRX_Engine engine : {GRX_ENGINE_AUTO, GRX_ENGINE_BACKTRACK}) {
      int matched = 0;
      ASSERT_EQ(grx_regex_search(regex, row.subject, std::strlen(row.subject),
                    0, engine, nullptr, match, &matched), GRX_OK);
      ASSERT_TRUE(matched);
      GRX_Capture whole;
      ASSERT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
      EXPECT_EQ(whole.start, row.start);
      EXPECT_EQ(whole.end, row.end);
      for (size_t i = 0; i < row.groups.size(); ++i) {
        GRX_Capture group;
        ASSERT_EQ(grx_match_group(match, i + 1, &group), GRX_OK);
        EXPECT_EQ(group.start, row.groups[i].start) << "group " << i + 1;
        EXPECT_EQ(group.end, row.groups[i].end) << "group " << i + 1;
      }
    }
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
}

TEST_P(VimProfiles, AtomicDiscoveryUsesUtf8ByteFrontiersAtPublicOffsets) {
  for (const std::string & scalar : {std::string("é"), std::string("€"), std::string("😀")}) {
    const std::string subject = "xy" + scalar + scalar + scalar + "b";
    const std::vector<std::string> patterns = {
        scalar + R"(\+\()" + scalar + R"(\)\@>\()" + scalar + R"(\)\@<=b)",
        scalar + R"(*\zs\()" + scalar + R"(\)\%()" + scalar + R"(\)\@>\zeb)",
        R"(\()" + scalar + R"(\)\?\%(\1)" + scalar + R"(\)\@>b)",
    };
    for (size_t shape = 0; shape < patterns.size(); ++shape) {
      SCOPED_TRACE(patterns[shape]);
      GRX_Regex * regex = nullptr;
      ASSERT_EQ(grx_regex_compile(patterns[shape].c_str(), GetParam(), 0, &regex), GRX_OK);
      GRX_Match * match = nullptr;
      ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
      for (GRX_Engine engine : {GRX_ENGINE_AUTO, GRX_ENGINE_BACKTRACK}) {
        for (size_t offset : {0u, 2u}) {
          SCOPED_TRACE(offset);
          int matched = 0;
          ASSERT_EQ(grx_regex_search(regex, subject.data(), subject.size(), offset,
                        engine, nullptr, match, &matched), GRX_OK);
          if (shape == 0 && !old()) {
            EXPECT_FALSE(matched);
            continue;
          }
          ASSERT_TRUE(matched);
          GRX_Capture whole;
          ASSERT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
          EXPECT_EQ(whole.start, shape == 1 ? 2 + scalar.size() : 2u);
          EXPECT_EQ(whole.end, shape == 1 ? subject.size() - 1 : subject.size());
          for (size_t group = 1; group <= (shape == 0 ? 2u : 1u); ++group) {
            GRX_Capture capture;
            ASSERT_EQ(grx_match_group(match, group, &capture), GRX_OK);
            ASSERT_NE(capture.start, GRX_NPOS);
            ASSERT_LE(capture.start, capture.end);
            ASSERT_LE(capture.end, subject.size());
            EXPECT_EQ(subject.substr(capture.start, capture.end - capture.start), scalar);
          }
        }
      }
      grx_match_destroy(match);
      grx_regex_free(regex);
    }
  }
}

TEST_P(VimProfiles, CombiningClosureMatchesPhysicalAndReportedFrontiers) {
  // Exact paired pinned-reference rows; missing capture texts are empty.
  struct Row {
    const char * pattern;
    const char * subject;
    size_t old_start, old_end, nfa_start, nfa_end;
    std::vector<std::string> old_groups, nfa_groups;
  };
  const Row rows[] = {
      {"\\(a\\)\\@>\\1", "a\u0301a\u0301", GRX_NPOS, GRX_NPOS, 0, 6,
          {"", "", "", "", "", "", "", "", ""},
          {"a", "", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\(\\1\\)\\@>x", "aa\u0301x", GRX_NPOS, GRX_NPOS, GRX_NPOS,
          GRX_NPOS, {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@>a\\1", "aaa\u0301", 0, 3, GRX_NPOS, GRX_NPOS,
          {"a", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@=a\\(\\1\\)\\@=.", "a\u0301a\u0301", GRX_NPOS, GRX_NPOS, 0, 3,
          {"", "", "", "", "", "", "", "", ""},
          {"a", "", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@=a\\(\\1\\)\\@>x", "aa\u0301x", GRX_NPOS, GRX_NPOS, GRX_NPOS,
          GRX_NPOS, {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@>\\(\\1\\)\\@<=x", "a\u0301x", GRX_NPOS, GRX_NPOS, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"a", "a\u0301", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@>.\\1", "aaa\u0301", 0, 3, 0, 5,
          {"a", "", "", "", "", "", "", "", ""},
          {"a", "", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@=\\(aX\\|\\1\\)\\@=.", "a\u0301", GRX_NPOS, GRX_NPOS, 0, 3,
          {"", "", "", "", "", "", "", "", ""},
          {"a", "a", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@=\\(\\1\\|aX\\)\\@=.", "a\u0301", GRX_NPOS, GRX_NPOS, 0, 3,
          {"", "", "", "", "", "", "", "", ""},
          {"a", "a\u0301", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@=\\(aX\\|\\1\\)\\@>x", "a\u0301x", GRX_NPOS, GRX_NPOS, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"a", "a", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@=\\(\\1\\|aX\\)\\@>x", "a\u0301x", GRX_NPOS, GRX_NPOS, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"a", "a\u0301", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@=\\(a\\%9cX\\|\\1\\)\\@=.", "a\u0301", GRX_NPOS, GRX_NPOS, 0,
          3, {"", "", "", "", "", "", "", "", ""},
          {"a", "a", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@=\\(\\%9caX\\|\\1\\)\\@=.", "a\u0301", GRX_NPOS, GRX_NPOS, 0,
          3, {"", "", "", "", "", "", "", "", ""},
          {"a", "a\u0301", "", "", "", "", "", "", ""}},
      {"\\(\\%(a\\)\\)\\@=\\(a\\)\\@=a", "aaab", 0, 1, 0, 1,
          {"a", "a", "", "", "", "", "", "", ""},
          {"", "a", "", "", "", "", "", "", ""}},
      {"\\(\\%(a\\)\\)\\@=\\(a\\)\\@=a", "a\u0301ab", 3, 4, 3, 4,
          {"a", "a", "", "", "", "", "", "", ""},
          {"a", "a", "", "", "", "", "", "", ""}},
      {"\\(a\\+\\)\\@=\\(a\\)\\@=a", "a\u0301aaa", 3, 4, 3, 4,
          {"aaa", "a", "", "", "", "", "", "", ""},
          {"aaa", "a", "", "", "", "", "", "", ""}},
      {"\\(\\%(a\\|b\\)\\)\\@=\\(a\\)\\@=a", "b\u0301a", 3, 4, 3, 4,
          {"a", "a", "", "", "", "", "", "", ""},
          {"", "a", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@=\\1", "a\u0301", GRX_NPOS, GRX_NPOS, 0, 3,
          {"", "", "", "", "", "", "", "", ""},
          {"a", "", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@=.\\1", "a\u0301a\u0301", GRX_NPOS, GRX_NPOS, 0, 6,
          {"", "", "", "", "", "", "", "", ""},
          {"a", "", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@=a\\1", "aa\u0301a", 0, 2, GRX_NPOS, GRX_NPOS,
          {"a", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@=\\(a\\)\\@=\\1", "a\u0301", GRX_NPOS, GRX_NPOS, 0, 3,
          {"", "", "", "", "", "", "", "", ""},
          {"a", "a", "", "", "", "", "", "", ""}},
      {"\\(\\)\\@=\\1", "\u0301", 0, 0, 0, 0,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\(a\\?\\)\\@=\\1.", "a\u0301", 0, 3, 0, 3,
          {"", "", "", "", "", "", "", "", ""},
          {"a", "", "", "", "", "", "", "", ""}},
      {"\\(x\\)\\%(\\(\\(ab\\)\\@>\\)\\@>\\)\\@=.", "xab", 0, 2, 0, 2,
          {"x", "ab", "ab", "", "", "", "", "", ""},
          {"x", "ab", "ab", "", "", "", "", "", ""}},
      {"\\(x\\)\\%(\\(\\(ab\\)\\@>\\)\\@>\\)\\@=.", "xab\u0301", GRX_NPOS,
          GRX_NPOS, 0, 2, {"", "", "", "", "", "", "", "", ""},
          {"x", "ab\u0301", "ab", "", "", "", "", "", ""}},
      {"\\(x\\)\\%(\\(\\(\\(ab\\)\\@>\\)\\@>\\)\\@>\\)\\@=.", "xab", 0, 2, 0, 2,
          {"x", "ab", "ab", "ab", "", "", "", "", ""},
          {"x", "ab", "ab", "ab", "", "", "", "", ""}},
      {"\\(.\\+\\)\\@1<=x", "a\u0301x", GRX_NPOS, GRX_NPOS, 3, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"a\u0301", "", "", "", "", "", "", "", ""}},
      {"\\(.\\+\\)\\@<=x", "ba\u0301x", 4, 5, 4, 5,
          {"a\u0301", "", "", "", "", "", "", "", ""},
          {"ba\u0301", "", "", "", "", "", "", "", ""}},
      {"\\_s", "\n\u0301b", 0, 3, 0, 1, {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\(\\_s\\)b", "\n\u0301b", 0, 4, 0, 4,
          {"\n\u0301", "", "", "", "", "", "", "", ""},
          {"\n", "", "", "", "", "", "", "", ""}},
      {"\\_s\\zs$", "\n\u0301", 3, 3, 1, 3,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\_[ab]b", "\n\u0301b", 0, 4, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\_[^x]b", "\n\u0301b", 0, 4, 1, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"[a\u0301]", "a\u0302\u0301", GRX_NPOS, GRX_NPOS, 0, 5,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"[a\u0301\u0301]", "a\u0301", GRX_NPOS, GRX_NPOS, 0, 3,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"[ab\u0301]", "b", 0, 1, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\u0301b", "a\u0301x", 0, 3, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\u0301\\?b", "a\u0301xb", 0, 3, 4, 5,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\u0301\\?bb", "a\u0301bxb", GRX_NPOS, GRX_NPOS, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\u0301\\?[b]", "a\u0301x", 0, 3, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\Za\u0301$", "a\u0301\u0302", GRX_NPOS, GRX_NPOS, 0, 5,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\Za\u0301b", "a\u0301\u0302b", 0, 6, 0, 6,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\Z\\(a\u0301\\)b", "a\u0301\u0302b", GRX_NPOS, GRX_NPOS, 0, 6,
          {"", "", "", "", "", "", "", "", ""},
          {"a\u0301\u0302", "", "", "", "", "", "", "", ""}},
      {"\\%u0301b", "a\u0301x", 0, 3, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\Za\\%u0301\u0302", "a\u0301", 0, 3, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\w.", "a\u0301b", 0, 4, 0, 4, {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\w*.", "a\u0301b", 0, 3, 0, 4, {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\W*.", "a\u0301b", 0, 4, 0, 3, {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\W\\?.", "a\u0301b", 0, 3, 0, 3, {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\(\\W\\{1}\\).", "a\u0301\u0302b", 0, 6, GRX_NPOS, GRX_NPOS,
          {"a\u0301\u0302", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\(\\W\\)\\{1}.", "a\u0301\u0302b", GRX_NPOS, GRX_NPOS, GRX_NPOS,
          GRX_NPOS, {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\%(a\\|[ab]\\)", "a\u0301b", 0, 1, 3, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"^\\%(a\\|[ab]\\)", "a\u0301", 0, 1, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\%9ca\\|[ab]", "a\u0301b", 0, 1, 0, 3,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"a\\%9c\\|[ab]", "a\u0301b", 0, 1, 3, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"^\\%([ab].\\|a\\nX\\)", "a\n\u0301b", 0, 4, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"^\\%([ab]..\\|a\\nX\\)", "a\n\u0301b", 0, 5, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\_[ab].", "a\n\u0301b", 0, 4, 1, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"^\\%(\\_[ab]\\).", "a\n\u0301b", 0, 4, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\(\\_[^x]\\).", "\n\u0301b", 0, 4, 0, 3,
          {"\n\u0301", "", "", "", "", "", "", "", ""},
          {"\n\u0301", "", "", "", "", "", "", "", ""}},
      {"\\_[^x]\\zs.", "\n\u0301b", 3, 4, 3, 3,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\_[^\\n]\\zs.", "\n\u0301b", 3, 4, 1, 3,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"b.\u0301", "ba\u0301x", GRX_NPOS, GRX_NPOS, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"b.\u0301", "b.\u0301", 0, 4, 0, 4, {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"b.\u0301\\?", "b..\u0301", 0, 5, 0, 1,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"b\\.\u0301", "b.\u0301", 0, 4, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"b\\%u002e\u0301", "b..\u0301", 0, 5, 0, 5,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@>x", "a\u0301x", GRX_NPOS, GRX_NPOS, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"a", "", "", "", "", "", "", "", ""}},
      {"\\(a\\)\\@>\\zsx", "a\u0301x", GRX_NPOS, GRX_NPOS, 3, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"a", "", "", "", "", "", "", "", ""}},
      {"\\(a\\).", "a\u0301x", GRX_NPOS, GRX_NPOS, 0, 3,
          {"", "", "", "", "", "", "", "", ""},
          {"a", "", "", "", "", "", "", "", ""}},
      {"\\([ab].\\|abX\\)\\@=.", "a\u0301", 0, 3, 0, 3,
          {"a\u0301", "", "", "", "", "", "", "", ""},
          {"a\u0301", "", "", "", "", "", "", "", ""}},
      {"^\\([ab]\\)\\(\\%(\\1aX\\|[ab].x\\)\\)\\@=.", "aaa\u0301x", 0, 2,
          GRX_NPOS, GRX_NPOS, {"a", "aa\u0301x", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"^\\([ab]\\)\\(\\%(\\1aX\\|[ab].x\\)\\)\\@=.", "baa\u0301x", 0, 2, 0, 2,
          {"b", "aa\u0301x", "", "", "", "", "", "", ""},
          {"b", "aa\u0301x", "", "", "", "", "", "", ""}},
      {"[\\n].", "\n\u0301b", 0, 4, 0, 4, {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"[\\n]*.", "\n\u0301b", 0, 3, 0, 4, {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"^\\(a\\)\\?a\\?b\\(\\%(\\1aX\\|[ab].x\\)\\)\\@>", "abaa\u0301x", 0, 7,
          GRX_NPOS, GRX_NPOS, {"a", "aa\u0301x", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"^\\(a\\)\\?a\\?b\\(\\%(\\1aX\\|[ab].x\\)\\)\\@>", "abaa\u0301\u0301x",
          0, 9, GRX_NPOS, GRX_NPOS,
          {"a", "aa\u0301\u0301x", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"^\\(a\\)\\?a\\?b\\(\\%([ab].x\\|\\1aX\\)\\)\\@>", "abaa\u0301x", 0, 7,
          GRX_NPOS, GRX_NPOS, {"a", "aa\u0301x", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"^\\(a\\)\\?a\\?b\\(\\%([ab].x\\|\\1aX\\)\\)\\@>", "abaa\u0301\u0301x",
          0, 9, GRX_NPOS, GRX_NPOS,
          {"a", "aa\u0301\u0301x", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"^\\(a\\)\\?a*b\\(\\%(\\1aX\\|[ab].x\\)\\)\\@>", "abaa\u0301x", 0, 7,
          GRX_NPOS, GRX_NPOS, {"a", "aa\u0301x", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"^\\(a\\)\\?a*b\\(\\%(\\1aX\\|[ab].x\\)\\)\\@>", "abaa\u0301\u0301x", 0,
          9, GRX_NPOS, GRX_NPOS,
          {"a", "aa\u0301\u0301x", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"^\\(a\\)\\?a*b\\(\\%([ab].x\\|\\1aX\\)\\)\\@>", "abaa\u0301x", 0, 7,
          GRX_NPOS, GRX_NPOS, {"a", "aa\u0301x", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"^\\(a\\)\\?a*b\\(\\%([ab].x\\|\\1aX\\)\\)\\@>", "abaa\u0301\u0301x", 0,
          9, GRX_NPOS, GRX_NPOS,
          {"a", "aa\u0301\u0301x", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\(a*.\\)", "a\u0301x", 0, 3, 0, 3,
          {"a\u0301", "", "", "", "", "", "", "", ""},
          {"a\u0301", "", "", "", "", "", "", "", ""}},
      {"\\(a*.\\)\\@=.", "a\u0301x", 0, 3, 0, 3,
          {"a\u0301", "", "", "", "", "", "", "", ""},
          {"a\u0301", "", "", "", "", "", "", "", ""}},
      {"\\(a*.\\)\\@>.", "a\u0301x", 0, 4, 0, 4,
          {"a\u0301", "", "", "", "", "", "", "", ""},
          {"a\u0301", "", "", "", "", "", "", "", ""}},
      {"\\(a*.\\)\\@<=x", "a\u0301x", 3, 4, 3, 4,
          {"a\u0301", "", "", "", "", "", "", "", ""},
          {"a\u0301", "", "", "", "", "", "", "", ""}},
      {"\\%(\\(a*.\\)\\@=.\\)\\@=.", "a\u0301x", 0, 3, 0, 3,
          {"a\u0301", "", "", "", "", "", "", "", ""},
          {"a\u0301", "", "", "", "", "", "", "", ""}},
      {"\\(a\\?.\\)", "a\u0301x", 0, 3, 0, 3,
          {"a\u0301", "", "", "", "", "", "", "", ""},
          {"a\u0301", "", "", "", "", "", "", "", ""}},
      {"\\(a\\?.\\)\\@=.", "a\u0301x", 0, 3, 0, 3,
          {"a\u0301", "", "", "", "", "", "", "", ""},
          {"a\u0301", "", "", "", "", "", "", "", ""}},
      {"\\(a\\?.\\)\\@>.", "a\u0301x", 0, 4, 0, 4,
          {"a\u0301", "", "", "", "", "", "", "", ""},
          {"a\u0301", "", "", "", "", "", "", "", ""}},
      {"\\(a\\?.\\)\\@<=x", "a\u0301x", 3, 4, 3, 4,
          {"a\u0301", "", "", "", "", "", "", "", ""},
          {"a\u0301", "", "", "", "", "", "", "", ""}},
      {"\\(a\\?.\\)\\@<=x", "aa\u0301x", 4, 5, 4, 5,
          {"a\u0301", "", "", "", "", "", "", "", ""},
          {"a\u0301", "", "", "", "", "", "", "", ""}},
      {"\\%(\\(a\\?.\\)\\@=.\\)\\@=.", "a\u0301x", 0, 3, 0, 3,
          {"a\u0301", "", "", "", "", "", "", "", ""},
          {"a\u0301", "", "", "", "", "", "", "", ""}},
      {"[a\u0301]\\{1}.", "a\n\u0301b", 0, 4, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"[ab\u0301]\\{1}.", "a\u0301b", 0, 4, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"[^a\u0301]\\{1}.", "\u0301\u0302a", 0, 5, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\%u0301", "\u0302\u0301a", 0, 4, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\%u0301", "\u0301\u0301", 0, 4, 2, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\Z\\%u0301.", "\u0301\u0302a", 0, 4, 0, 5,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\Z\\(\\%u0301\\).", "\u0301\u0302a", 0, 4, 0, 5,
          {"", "", "", "", "", "", "", "", ""},
          {"\u0301\u0302", "", "", "", "", "", "", "", ""}},
      {"\\(\\(a\\zsb\\)\\@>\\)\\@>", "ab", 1, 2, 0, 2,
          {"ab", "ab", "", "", "", "", "", "", ""},
          {"ab", "ab", "", "", "", "", "", "", ""}},
      {"\\(\\(a\\zsb\\)\\@>\\)\\@>", "xab", 2, 3, 1, 3,
          {"ab", "ab", "", "", "", "", "", "", ""},
          {"ab", "ab", "", "", "", "", "", "", ""}},
      {"\\(\\(a\\zsb\\)\\@>\\)\\@>", "abab", 1, 2, 0, 2,
          {"ab", "ab", "", "", "", "", "", "", ""},
          {"ab", "ab", "", "", "", "", "", "", ""}},
      {"\\(\\(a\\zeb\\)\\@>\\)\\@>", "ab", 0, 1, 0, 2,
          {"ab", "ab", "", "", "", "", "", "", ""},
          {"ab", "ab", "", "", "", "", "", "", ""}},
      {"\\(\\(a\\zeb\\)\\@>\\)\\@>", "xab", 1, 2, 1, 3,
          {"ab", "ab", "", "", "", "", "", "", ""},
          {"ab", "ab", "", "", "", "", "", "", ""}},
      {"\\(\\(a\\zeb\\)\\@>\\)\\@>", "abab", 0, 1, 0, 2,
          {"ab", "ab", "", "", "", "", "", "", ""},
          {"ab", "ab", "", "", "", "", "", "", ""}},
  };
  for (const Row & row : rows) {
    for (GRX_Engine engine : {GRX_ENGINE_AUTO, GRX_ENGINE_BACKTRACK}) {
      SCOPED_TRACE(engine);
      expect(row.pattern, row.subject, old() ? row.old_start : row.nfa_start,
          old() ? row.old_end : row.nfa_end,
          old() ? row.old_groups : row.nfa_groups, engine);
    }
  }
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

TEST(VimCombining, FrontierContextsResetOnSameObjectAcrossSubjectsAndWindows) {
  for (const char * ending : {R"(\@=.)", R"(\@>)"}) {
    std::string pattern = R"(^\([ab]\)\(\%(\1aX\|[ab].x\)\))";
    pattern += ending;
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(pattern.c_str(), GRX_SYNTAX_VIM_NFA, 0, &regex),
        GRX_OK);
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    for (const char * subject : {"aaa\u0301x", "baa\u0301x", "bba\u0301x",
             "aba\u0301x", "aaa\u0301x", "baa\u0301x"}) {
      int matched = 0;
      ASSERT_EQ(grx_regex_search(regex, subject, std::strlen(subject), 0,
                    GRX_ENGINE_BACKTRACK, nullptr, match, &matched),
          GRX_OK);
      const bool expected = subject[0] != subject[1];
      ASSERT_EQ(matched, expected);
      if (expected) {
        GRX_Capture whole;
        ASSERT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
        EXPECT_EQ(whole.start, 0u);
        EXPECT_EQ(whole.end, ending[2] == '=' ? 2u : 6u);
      }
    }
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(
      grx_regex_compile(R"(\([ab].\|abX\)\@=.)", GRX_SYNTAX_VIM_NFA, 0, &regex),
      GRX_OK);
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
  const std::string subject = "xa\u0301xa\u0301";
  GRX_SearchOptions search{};
  search.begin = 1;
  search.end = 4;
  search.engine = GRX_ENGINE_BACKTRACK;
  int matched = 0;
  ASSERT_EQ(grx_regex_search_ex(regex, subject.data(), subject.size(), &search,
                match, &matched),
      GRX_OK);
  ASSERT_TRUE(matched);
  GRX_Capture whole;
  ASSERT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
  EXPECT_EQ(whole.start, 1u);
  EXPECT_EQ(whole.end, 4u);
  ASSERT_EQ(grx_regex_search_next(regex, subject.data(), subject.size(),
                &search, match, &matched),
      GRX_OK);
  EXPECT_FALSE(matched);
  search.begin = 4;
  search.end = subject.size();
  ASSERT_EQ(grx_regex_search_ex(regex, subject.data(), subject.size(), &search,
                match, &matched),
      GRX_OK);
  ASSERT_TRUE(matched);
  ASSERT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
  EXPECT_EQ(whole.start, 5u);
  EXPECT_EQ(whole.end, 8u);
  grx_match_destroy(match);
  grx_regex_free(regex);
}

TEST(VimCombining, QueryProgramsExposeEngineRequirementsWithoutPenalizingLiterals) {
  for (bool query : {false, true}) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(query ? R"(\%(a\|[ab]\))" : "a",
        GRX_SYNTAX_VIM_NFA, 0, &regex), GRX_OK);
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    for (GRX_Engine engine : {GRX_ENGINE_AUTO, GRX_ENGINE_BACKTRACK,
             GRX_ENGINE_PIKE, GRX_ENGINE_BITSTATE, GRX_ENGINE_DFA}) {
      int matched = 0;
      const char * subject = query ? "a\u0301b" : "a";
      GRX_Result result = grx_regex_search(regex, subject, std::strlen(subject),
          0, engine, nullptr, match, &matched);
      if (engine == GRX_ENGINE_DFA ||
          (query && engine != GRX_ENGINE_AUTO && engine != GRX_ENGINE_BACKTRACK)) {
        EXPECT_EQ(result, GRX_ERR_UNSUPPORTED);
      }
      else {
        ASSERT_EQ(result, GRX_OK);
        ASSERT_TRUE(matched);
        GRX_Capture whole;
        ASSERT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
        EXPECT_EQ(whole.start, query ? 3u : 0u);
        EXPECT_EQ(whole.end, query ? 4u : 1u);
      }
    }
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
}

TEST(VimCombining, EligibleEnginesUseReachedPriorScalarForLeadingMarkOrigins) {
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile(R"(\%u0301)", GRX_SYNTAX_VIM_NFA, 0, &regex), GRX_OK);
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
  struct Row { const char * subject; bool matched; };
  const Row rows[] = {{"\u0302\u0301", false}, {"\u0301\u0302", false},
      {"\u0301\u0301", true}};
  for (const Row & row : rows) {
    const char * subject = row.subject;
    for (GRX_Engine engine : {GRX_ENGINE_AUTO, GRX_ENGINE_BACKTRACK,
             GRX_ENGINE_PIKE, GRX_ENGINE_BITSTATE}) {
      int matched = 0;
      ASSERT_EQ(grx_regex_search(regex, subject, std::strlen(subject), 0, engine,
          nullptr, match, &matched), GRX_OK);
      ASSERT_EQ(matched, row.matched);
      if (matched) {
        GRX_Capture whole;
        ASSERT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
        EXPECT_EQ(whole.start, 2u);
        EXPECT_EQ(whole.end, 4u);
      }
    }
  }
  grx_match_destroy(match);
  grx_regex_free(regex);
}

TEST(VimProfileState, WholeClusterFailureActivationResetsForEveryPublicSearch) {
  GRX_Regex * raw_regex = nullptr;
  ASSERT_EQ(grx_regex_compile(
                R"(\(a\+\)\@=\(a\)\@=a)", GRX_SYNTAX_VIM_NFA, 0, &raw_regex),
      GRX_OK);
  std::unique_ptr<GRX_Regex, decltype(&grx_regex_free)> regex(
      raw_regex, grx_regex_free);
  GRX_Match * raw_match = nullptr;
  ASSERT_EQ(grx_match_create(regex.get(), nullptr, &raw_match), GRX_OK);
  std::unique_ptr<GRX_Match, decltype(&grx_match_destroy)> match(
      raw_match, grx_match_destroy);
  struct Row {
    const char * subject;
    size_t start, first, last;
    const char * group;
  };
  for (GRX_Engine engine : {GRX_ENGINE_AUTO, GRX_ENGINE_BACKTRACK}) {
    for (const Row & row : {Row{"a\u0301aaa", 0, 3, 4, "aaa"},
             Row{"aaab", 0, 0, 1, ""}, Row{"a\u0301aaa", 3, 3, 4, ""},
             Row{"b\u0301a", 0, 3, 4, ""}, Row{"a\u0301aaa", 0, 3, 4, "aaa"}}) {
      int matched = 0;
      ASSERT_EQ(
          grx_regex_search(regex.get(), row.subject, std::strlen(row.subject),
              row.start, engine, nullptr, match.get(), &matched),
          GRX_OK);
      ASSERT_TRUE(matched);
      GRX_Capture whole, group;
      ASSERT_EQ(grx_match_group(match.get(), 0, &whole), GRX_OK);
      EXPECT_EQ(whole.start, row.first);
      EXPECT_EQ(whole.end, row.last);
      ASSERT_EQ(grx_match_group(match.get(), 1, &group), GRX_OK);
      EXPECT_EQ(group.start == GRX_NPOS
              ? std::string()
              : std::string(row.subject + group.start, group.end - group.start),
          row.group);
    }
  }
}

TEST_P(VimProfiles, LexicalCandidatesAndMarkedBasesUseEffectiveCharacterPolicies) {
  // Forced-engine controls include scalar/run ownership, encoded-length
  // selection, numeric barriers, Unicode folding and both directive orders.
  struct Row {
    const char * pattern;
    const char * subject;
    size_t old_start, old_end, nfa_start, nfa_end;
    std::vector<std::string> old_groups, nfa_groups;
  };
  const Row rows[] = {
      {"\\c\u0301\\?b", "a\u0301B", 0, 3, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\C\u0301\\?b", "a\u0301B", GRX_NPOS, GRX_NPOS, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\C\u0301\\?b", "a\u0301B", 0, 3, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\C\\c\u0301\\?b", "a\u0301B", 0, 3, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\Z\u0301\\?a\u0302b", "a\u0301a\u0301b", 0, 3, 0, 7,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\u0301\\?\u212a", "a\u0301k", 0, 3, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\u0301\\?\u00c9", "a\u0301\u00e9", 0, 3, 0, 5,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\Z\u0301\\?\u0302", "a\u0301bK", 0, 3, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\Z\u0301\\?\\%u0302", "a\u0301bK", 0, 3, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\u0301\\?b\\zs.", "a\u0301bK", 0, 3, 4, 5,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\Z\u0301\\?a\u0302\\(b\\)", "a\u0301a\u0301b", 0, 3, 0, 7,
          {"", "", "", "", "", "", "", "", ""},
          {"b", "", "", "", "", "", "", "", ""}},
      {"\\Z\u0301\\?a\u0302\\%u0062", "a\u0301a\u0301b", 0, 3, 0, 7,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Z\u0301\\?a\u0302b", "a\u0301a\u0301B", GRX_NPOS, GRX_NPOS, 0, 7,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Z\u0301\\?a\u0302b", "a\u0301a\u0302B", 0, 3, 0, 7,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Za\u0302b", "A\u0301b", 0, 4, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Za\u0302b", "a\u0301B", GRX_NPOS, GRX_NPOS, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Za\u0302b", "a\u0301a\u0302B", 3, 7, 3, 7,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Za", "a\u0301b", 0, 3, 0, 3,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Za", "A\u0301b", GRX_NPOS, GRX_NPOS, 0, 3,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Za", "Ab", 0, 1, 0, 1,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Za\u0302", "A\u0302b", 0, 3, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Za\u0302", "A\u0301b", GRX_NPOS, GRX_NPOS, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Z[a\u0302]b", "A\u0301b", GRX_NPOS, GRX_NPOS, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Z\u00e9\u0302b", "\u00c9\u0301b", 0, 5, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Z\u212a\u0302b", "k\u0301b", 0, 4, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Z\\(a\u0302\\)b", "A\u0301b", GRX_NPOS, GRX_NPOS, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Z\\(a\u0302\\)b", "a\u0301B", 0, 4, 0, 4,
          {"a\u0301", "", "", "", "", "", "", "", ""},
          {"a\u0301", "", "", "", "", "", "", "", ""}},
      {"\\c\\Za\u0302\\?b", "a\u0301B", 0, 4, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Za\u0302\\?b", "A\u0301b", GRX_NPOS, GRX_NPOS, 3, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Za\u0302\\{1}b", "a\u0302B", GRX_NPOS, GRX_NPOS, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Za\\%u0302b", "a\u0302B", GRX_NPOS, GRX_NPOS, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Z\\%u0061\u0302b", "a\u0302B", GRX_NPOS, GRX_NPOS, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\ca\u0302", "A\u0302b", 0, 3, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c[a\u0302]", "A\u0302b", 0, 3, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\ca\u0302\\?b", "A\u0302b", GRX_NPOS, GRX_NPOS, 3, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Zab", "ab\u0301", 0, 4, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Zab", "aB\u0301", GRX_NPOS, GRX_NPOS, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\Zab", "a\u0301b\u0301", 0, 6, 0, 6,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\Za\u0302b", "ab\u0301", 0, 4, 0, 4,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\u0301\\?bbb", "a\u0301b", 0, 3, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\u0301\\?bbb", "a\u0301b", GRX_NPOS, GRX_NPOS, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\u0301\\?a\u0302bc", "a\u0301a\u0302", 0, 3, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Z\u0301\\?a\u0302bc", "a\u0301a\u0302", 0, 3, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\u0301\\?\u0302", "a\u0301b", 0, 3, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\u0301\\?\u0302", "a\u0301b", 0, 3, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\\c\\Z\u0301\\?b", "a\u0301B\u0301", 0, 3, 0, 6,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
      {"\u0301\\?bb\\(\\)\u0302", "a\u0301b", GRX_NPOS, GRX_NPOS, GRX_NPOS, GRX_NPOS,
          {"", "", "", "", "", "", "", "", ""},
          {"", "", "", "", "", "", "", "", ""}},
  };
  for (const Row & row : rows) {
    for (GRX_Engine engine : {GRX_ENGINE_AUTO, GRX_ENGINE_BACKTRACK}) {
      SCOPED_TRACE(engine);
      expect(row.pattern, row.subject, old() ? row.old_start : row.nfa_start,
          old() ? row.old_end : row.nfa_end,
          old() ? row.old_groups : row.nfa_groups, engine);
    }
  }
  if (!old()) {
    for (GRX_Engine engine : {GRX_ENGINE_PIKE, GRX_ENGINE_BITSTATE}) {
      expect("\\c\\Za\u0302b", "A\u0301b", GRX_NPOS, GRX_NPOS, {}, engine);
      expect("\\c\\Za\u0302b", "a\u0301B", 0, 4,
          {"", "", "", "", "", "", "", "", ""}, engine);
    }
  }
}

TEST_P(VimProfiles, NonliteralMarkedCollectionsPreserveLexicalMemberKinds) {
  struct Row {
    const char * pattern;
    const char * subject;
    size_t old_start, old_end;
    std::vector<std::string> old_groups;
    size_t nfa_start, nfa_end;
    std::vector<std::string> nfa_groups;
  };
  const Row rows[] = {
      {"[́̂]", "á̂", GRX_NPOS, GRX_NPOS, {}, 0, 5, {}},
      {"[́̂]", "́̂", 0, 4, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-ź]", "b", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 1, {"", "", "", "", "", "", "", "", ""}},
      {"[a-ź]", "1", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-ź]", "b́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 3, {"", "", "", "", "", "", "", "", ""}},
      {"[a-ź]", "b̂́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 5, {"", "", "", "", "", "", "", "", ""}},
      {"[a-ź]", "é́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-ź]", "日́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-ź]", "́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-ź]", "\ń", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\([a-ź]\\)", "b", 0, 1, {"b", "", "", "", "", "", "", "", ""}, 0, 1, {"b", "", "", "", "", "", "", "", ""}},
      {"\\([a-ź]\\)", "1", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\([a-ź]\\)", "b́", 0, 1, {"b", "", "", "", "", "", "", "", ""}, 0, 3, {"b́", "", "", "", "", "", "", "", ""}},
      {"\\([a-ź]\\)", "b̂́", 0, 1, {"b", "", "", "", "", "", "", "", ""}, 0, 5, {"b̂́", "", "", "", "", "", "", "", ""}},
      {"\\([a-ź]\\)", "é́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\([a-ź]\\)", "日́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\([a-ź]\\)", "́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\([a-ź]\\)", "\ń", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-ź]\\+", "b", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 1, {"", "", "", "", "", "", "", "", ""}},
      {"[a-ź]\\+", "1", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-ź]\\+", "b́", 0, 3, {"", "", "", "", "", "", "", "", ""}, 0, 3, {"", "", "", "", "", "", "", "", ""}},
      {"[a-ź]\\+", "b̂́", 0, 5, {"", "", "", "", "", "", "", "", ""}, 0, 5, {"", "", "", "", "", "", "", "", ""}},
      {"[a-ź]\\+", "é́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-ź]\\+", "日́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-ź]\\+", "́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-ź]\\+", "\ń", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\%([a-ź]\\)\\+", "b", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 1, {"", "", "", "", "", "", "", "", ""}},
      {"\\%([a-ź]\\)\\+", "1", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\%([a-ź]\\)\\+", "b́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 3, {"", "", "", "", "", "", "", "", ""}},
      {"\\%([a-ź]\\)\\+", "b̂́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 5, {"", "", "", "", "", "", "", "", ""}},
      {"\\%([a-ź]\\)\\+", "é́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\%([a-ź]\\)\\+", "日́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\%([a-ź]\\)\\+", "́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\%([a-ź]\\)\\+", "\ń", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\_[a-ź]", "b", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 1, {"", "", "", "", "", "", "", "", ""}},
      {"\\_[a-ź]", "1", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\_[a-ź]", "b́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 3, {"", "", "", "", "", "", "", "", ""}},
      {"\\_[a-ź]", "b̂́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 5, {"", "", "", "", "", "", "", "", ""}},
      {"\\_[a-ź]", "é́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\_[a-ź]", "日́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\_[a-ź]", "́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\_[a-ź]", "\ń", 0, 3, {"", "", "", "", "", "", "", "", ""}, GRX_NPOS, GRX_NPOS, {}},
      {"[[:alpha:]́]", "b", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 1, {"", "", "", "", "", "", "", "", ""}},
      {"[[:alpha:]́]", "1", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[[:alpha:]́]", "b́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 3, {"", "", "", "", "", "", "", "", ""}},
      {"[[:alpha:]́]", "b̂́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 5, {"", "", "", "", "", "", "", "", ""}},
      {"[[:alpha:]́]", "é́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[[:alpha:]́]", "日́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[[:alpha:]́]", "́", GRX_NPOS, GRX_NPOS, {}, 0, 2, {"", "", "", "", "", "", "", "", ""}},
      {"[[:alpha:]́]", "\ń", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-á]", "b", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-á]", "1", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-á]", "b́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-á]", "b̂́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-á]", "é́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-á]", "日́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-á]", "́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-á]", "\ń", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[é-ế]", "b", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[é-ế]", "1", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[é-ế]", "b́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[é-ế]", "b̂́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[é-ế]", "é́", 0, 2, {"", "", "", "", "", "", "", "", ""}, 0, 4, {"", "", "", "", "", "", "", "", ""}},
      {"[é-ế]", "日́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[é-ế]", "́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[é-ế]", "\ń", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[日-旦́]", "b", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[日-旦́]", "1", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[日-旦́]", "b́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[日-旦́]", "b̂́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[日-旦́]", "é́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[日-旦́]", "日́", 0, 3, {"", "", "", "", "", "", "", "", ""}, 0, 5, {"", "", "", "", "", "", "", "", ""}},
      {"[日-旦́]", "́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[日-旦́]", "\ń", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-z\\u0301]", "b", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 1, {"", "", "", "", "", "", "", "", ""}},
      {"[a-z\\u0301]", "1", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-z\\u0301]", "b́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 3, {"", "", "", "", "", "", "", "", ""}},
      {"[a-z\\u0301]", "b̂́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 5, {"", "", "", "", "", "", "", "", ""}},
      {"[a-z\\u0301]", "é́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-z\\u0301]", "日́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-z\\u0301]", "́", GRX_NPOS, GRX_NPOS, {}, 0, 2, {"", "", "", "", "", "", "", "", ""}},
      {"[a-z\\u0301]", "\ń", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-ź́]", "b", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 1, {"", "", "", "", "", "", "", "", ""}},
      {"[a-ź́]", "1", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-ź́]", "b́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 3, {"", "", "", "", "", "", "", "", ""}},
      {"[a-ź́]", "b̂́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 5, {"", "", "", "", "", "", "", "", ""}},
      {"[a-ź́]", "é́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-ź́]", "日́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-ź́]", "́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-ź́]", "\ń", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[ba-ź]", "b", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 1, {"", "", "", "", "", "", "", "", ""}},
      {"[ba-ź]", "1", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[ba-ź]", "b́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 3, {"", "", "", "", "", "", "", "", ""}},
      {"[ba-ź]", "b̂́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 5, {"", "", "", "", "", "", "", "", ""}},
      {"[ba-ź]", "é́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[ba-ź]", "日́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[ba-ź]", "́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[ba-ź]", "\ń", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-zb́]", "b", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 1, {"", "", "", "", "", "", "", "", ""}},
      {"[a-zb́]", "1", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-zb́]", "b́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 3, {"", "", "", "", "", "", "", "", ""}},
      {"[a-zb́]", "b̂́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 5, {"", "", "", "", "", "", "", "", ""}},
      {"[a-zb́]", "é́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-zb́]", "日́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-zb́]", "́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[a-zb́]", "\ń", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[^a-ź]", "b", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[^a-ź]", "1", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 1, {"", "", "", "", "", "", "", "", ""}},
      {"[^a-ź]", "b́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[^a-ź]", "b̂́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[^a-ź]", "é́", 0, 2, {"", "", "", "", "", "", "", "", ""}, 0, 4, {"", "", "", "", "", "", "", "", ""}},
      {"[^a-ź]", "日́", 0, 3, {"", "", "", "", "", "", "", "", ""}, 0, 5, {"", "", "", "", "", "", "", "", ""}},
      {"[^a-ź]", "́", 0, 2, {"", "", "", "", "", "", "", "", ""}, 0, 2, {"", "", "", "", "", "", "", "", ""}},
      {"[^a-ź]", "\ń", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 3, {"", "", "", "", "", "", "", "", ""}},
      {"[^[:alpha:]́]", "b", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[^[:alpha:]́]", "1", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 1, {"", "", "", "", "", "", "", "", ""}},
      {"[^[:alpha:]́]", "b́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[^[:alpha:]́]", "b̂́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"[^[:alpha:]́]", "é́", 0, 2, {"", "", "", "", "", "", "", "", ""}, 0, 4, {"", "", "", "", "", "", "", "", ""}},
      {"[^[:alpha:]́]", "日́", 0, 3, {"", "", "", "", "", "", "", "", ""}, 0, 5, {"", "", "", "", "", "", "", "", ""}},
      {"[^[:alpha:]́]", "́", 0, 2, {"", "", "", "", "", "", "", "", ""}, GRX_NPOS, GRX_NPOS, {}},
      {"[^[:alpha:]́]", "\ń", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 3, {"", "", "", "", "", "", "", "", ""}},
      {"\\c[a-ź]", "b", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 1, {"", "", "", "", "", "", "", "", ""}},
      {"\\c[a-ź]", "1", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\c[a-ź]", "b́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 3, {"", "", "", "", "", "", "", "", ""}},
      {"\\c[a-ź]", "b̂́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 5, {"", "", "", "", "", "", "", "", ""}},
      {"\\c[a-ź]", "é́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\c[a-ź]", "日́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\c[a-ź]", "́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\c[a-ź]", "\ń", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\Z[a-ź]", "b", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 1, {"", "", "", "", "", "", "", "", ""}},
      {"\\Z[a-ź]", "1", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\Z[a-ź]", "b́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 3, {"", "", "", "", "", "", "", "", ""}},
      {"\\Z[a-ź]", "b̂́", 0, 1, {"", "", "", "", "", "", "", "", ""}, 0, 5, {"", "", "", "", "", "", "", "", ""}},
      {"\\Z[a-ź]", "é́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\Z[a-ź]", "日́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\Z[a-ź]", "́", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
      {"\\Z[a-ź]", "\ń", GRX_NPOS, GRX_NPOS, {}, GRX_NPOS, GRX_NPOS, {}},
  };
  for (const Row & row : rows) {
    for (GRX_Engine engine : {GRX_ENGINE_AUTO, GRX_ENGINE_BACKTRACK,
             GRX_ENGINE_PIKE, GRX_ENGINE_BITSTATE}) {
      if ((old() || std::strncmp(row.pattern, "\\_", 2) == 0 ||
              std::strcmp(row.pattern, "[́̂]") == 0) &&
          (engine == GRX_ENGINE_PIKE || engine == GRX_ENGINE_BITSTATE))
        continue;
      expect(row.pattern, row.subject, old() ? row.old_start : row.nfa_start,
          old() ? row.old_end : row.nfa_end,
          old() ? row.old_groups : row.nfa_groups, engine);
    }
  }
}
