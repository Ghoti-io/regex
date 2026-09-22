/**
 * @file
 *
 * `(?C...)` and the function a caller registers for it.
 *
 * A callout was accepted and inert here for as long as this library has had
 * PCRE2's grammar, which gave the right *answer* - PCRE2 with no function
 * registered also matches the same subjects - and no feature. What was
 * missing was the only thing a callout is for: a way to be told where the
 * match is while it is running.
 *
 * Three things are tested, and they are different in kind.
 *
 * The **payload** is what the parser read: a number, or a string with its
 * delimiters removed and any doubled delimiter collapsed. `(?C"a""b")` is
 * the case that says why the offset and the length do not describe the same
 * bytes.
 *
 * The **sequence** is which callouts fire, in which order, at which
 * positions. That is backtracking order, and it is checked against
 * pcre2test's own `no_start_optimize` output - with the optimisations on,
 * PCRE2 skips starting positions and so skips callouts, which is a
 * difference in its start-up analysis and not in the construct.
 *
 * The **control** is what the function can say back: nothing, fail this
 * path, or stop. The three are separate because the middle one is an
 * outcome the search carries on from and the last one is a failure the
 * caller gets back.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <string>
#include <vector>

#include "test_helpers.h"

namespace {

/** A compiled regex that frees itself. */
class Regex {
public:
  explicit Regex(const char * pattern, uint32_t options = GRX_OPT_UTF) {
    result_ = grx_regex_compile(pattern, GRX_SYNTAX_PCRE, options, &regex_);
  }
  Regex(const Regex &) = delete;
  Regex & operator=(const Regex &) = delete;
  ~Regex() { grx_regex_free(regex_); }

  GRX_Regex * get() const { return regex_; }
  GRX_Result result() const { return result_; }

private:
  GRX_Regex * regex_ = nullptr;
  GRX_Result result_ = GRX_ERR_INTERNAL;
};

/** One callout, recorded as it arrived. */
struct Report {
  uint32_t number;
  std::string text;
  bool has_text;
  size_t string_offset;
  size_t string_length;
  size_t pattern_offset;
  size_t start;
  size_t position;
  std::string subject;
  size_t count;
  std::vector<GRX_Capture> captures;
  std::string mark;
  bool has_mark;
};

/**
 * What a test registers: a log, and an optional rule for what to say back.
 *
 * `fail_at` and `stop_at` are one-based counts of callouts, so that a test
 * says "the second one" rather than reaching for a position that a change
 * in the engine's order would quietly move.
 */
struct Recorder {
  std::vector<Report> reports;
  size_t fail_at = 0;
  size_t stop_at = 0;
  GRX_Result stop_with = GRX_ERR_LIMIT;
};

GRX_Result record(const GRX_Callout * callout, int * out_fail, void * data) {
  Recorder * recorder = static_cast<Recorder *>(data);
  Report report;
  report.number = callout->number;
  report.has_text = callout->string != nullptr;
  report.text = report.has_text
      ? std::string(callout->string, callout->string_length)
      : std::string();
  report.string_offset = callout->string_offset;
  report.string_length = callout->string_length;
  report.pattern_offset = callout->pattern_offset;
  report.start = callout->start;
  report.position = callout->position;
  report.subject = std::string(callout->subject, callout->subject_length);
  report.count = callout->count;
  report.captures.assign(
      callout->captures, callout->captures + callout->count);
  report.has_mark = callout->mark != nullptr;
  report.mark = report.has_mark ? std::string(callout->mark) : std::string();
  recorder->reports.push_back(report);

  size_t seen = recorder->reports.size();
  if (recorder->fail_at == seen) {
    *out_fail = 1;
  }
  if (recorder->stop_at == seen) {
    return recorder->stop_with;
  }
  return GRX_OK;
}

/** Search `subject` with `recorder` registered, and report what happened. */
GRX_Result search(const Regex & regex, const std::string & subject,
    Recorder * recorder, int * out_matched, GRX_Match * match = nullptr,
    GRX_Engine engine = GRX_ENGINE_AUTO) {
  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.engine = engine;
  options.callout = recorder ? record : nullptr;
  options.callout_data = recorder;
  return grx_regex_search_ex(regex.get(), subject.data(), subject.size(),
      &options, match, out_matched);
}

/** The (number, position) pairs, which is what pcre2test's trace prints. */
std::vector<std::pair<uint32_t, size_t>> trace(const Recorder & recorder) {
  std::vector<std::pair<uint32_t, size_t>> out;
  for (const Report & report : recorder.reports) {
    out.emplace_back(report.number, report.position);
  }
  return out;
}

// --------------------------------------------------------------------------
// The payload
// --------------------------------------------------------------------------

TEST(Callout, NumberReachesTheFunction) {
  Regex regex("a(?C7)b");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "ab", &recorder, &matched), GRX_OK);
  EXPECT_TRUE(matched);
  ASSERT_EQ(recorder.reports.size(), 1u);
  EXPECT_EQ(recorder.reports[0].number, 7u);
  EXPECT_FALSE(recorder.reports[0].has_text);
}

TEST(Callout, BareCalloutIsNumberZero) {
  // pcre2test prints `0` for `(?C)`, not a distinct "no number" marker, and
  // `(?C0)` is the same callout written out.
  Regex bare("a(?C)b");
  Regex zero("a(?C0)b");
  ASSERT_EQ(bare.result(), GRX_OK);
  ASSERT_EQ(zero.result(), GRX_OK);
  Recorder from_bare;
  Recorder from_zero;
  int matched = 0;
  ASSERT_EQ(search(bare, "ab", &from_bare, &matched), GRX_OK);
  ASSERT_EQ(search(zero, "ab", &from_zero, &matched), GRX_OK);
  ASSERT_EQ(from_bare.reports.size(), 1u);
  ASSERT_EQ(from_zero.reports.size(), 1u);
  EXPECT_EQ(from_bare.reports[0].number, 0u);
  EXPECT_EQ(from_zero.reports[0].number, 0u);
  EXPECT_FALSE(from_bare.reports[0].has_text);
}

TEST(Callout, StringReachesTheFunctionWithoutItsDelimiters) {
  Regex regex("(?C\"hello\")x");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "x", &recorder, &matched), GRX_OK);
  ASSERT_EQ(recorder.reports.size(), 1u);
  EXPECT_TRUE(recorder.reports[0].has_text);
  EXPECT_EQ(recorder.reports[0].text, "hello");
  EXPECT_EQ(recorder.reports[0].number, 0u);
  // `(`, `?`, `C`, `"` - so the body begins at 4, which is the offset
  // pcre2test prints as `Callout (4)`.
  EXPECT_EQ(recorder.reports[0].string_offset, 4u);
  EXPECT_EQ(recorder.reports[0].string_length, 5u);
}

TEST(Callout, EmptyStringIsAStringAndNotANumber) {
  Regex regex("x(?C\"\")y");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "xy", &recorder, &matched), GRX_OK);
  ASSERT_EQ(recorder.reports.size(), 1u);
  EXPECT_TRUE(recorder.reports[0].has_text);
  EXPECT_EQ(recorder.reports[0].string_length, 0u);
  EXPECT_EQ(recorder.reports[0].string_offset, 5u);
}

TEST(Callout, EveryDelimiterPcre2AcceptsIsAccepted) {
  // pcre2pattern's list, and the one asymmetric pair: `{` closes with `}`.
  const char * const patterns[] = {
    "(?C`ab`)x", "(?C'ab')x", "(?C\"ab\")x", "(?C^ab^)x",
    "(?C%ab%)x", "(?C#ab#)x", "(?C$ab$)x", "(?C{ab})x",
  };
  for (const char * pattern : patterns) {
    Regex regex(pattern);
    ASSERT_EQ(regex.result(), GRX_OK) << pattern;
    Recorder recorder;
    int matched = 0;
    ASSERT_EQ(search(regex, "x", &recorder, &matched), GRX_OK) << pattern;
    ASSERT_EQ(recorder.reports.size(), 1u) << pattern;
    EXPECT_EQ(recorder.reports[0].text, "ab") << pattern;
  }
}

TEST(Callout, ADoubledDelimiterIsOneCharacterOfTheString) {
  // pcre2test 10.46 on `/(?C"a""b")x/` prints `Callout (4): "a"b"`, which is
  // the string `a"b` beginning at offset 4. The offset and the length
  // therefore describe different spans - three bytes of string taken from
  // four bytes of pattern - and that is why neither can be computed from
  // the other.
  Regex regex("(?C\"a\"\"b\")x");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "x", &recorder, &matched), GRX_OK);
  ASSERT_EQ(recorder.reports.size(), 1u);
  EXPECT_EQ(recorder.reports[0].text, "a\"b");
  EXPECT_EQ(recorder.reports[0].string_length, 3u);
  EXPECT_EQ(recorder.reports[0].string_offset, 4u);
}

TEST(Callout, ADoubledBraceIsOneCharacterToo) {
  // `{` is the delimiter whose closer is a different character, so the
  // doubling rule is about the *closer*: `}}` is one `}`, and `{` inside
  // needs no escape.
  Regex regex("(?C{a}}b})x");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "x", &recorder, &matched), GRX_OK);
  ASSERT_EQ(recorder.reports.size(), 1u);
  EXPECT_EQ(recorder.reports[0].text, "a}b");
}

TEST(Callout, ATrailingDoubledDelimiterLeavesTheStringUnterminated) {
  // The rule read greedily, which is what makes it a rule rather than a
  // preference: pcre2test refuses `/(?C"a"")x/` with "missing terminating
  // delimiter" rather than reading the string as `a"`.
  Regex regex("(?C\"a\"\")x");
  EXPECT_EQ(regex.result(), GRX_ERR_SYNTAX);
}

TEST(Callout, PatternOffsetIsPastTheCalloutsOwnParenthesis) {
  // PCRE2's `pattern_position`. It is the next byte in every case,
  // including the ones where the next item is a `)` or the end.
  Regex regex("a(?C1)b(?C2)");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "ab", &recorder, &matched), GRX_OK);
  ASSERT_EQ(recorder.reports.size(), 2u);
  EXPECT_EQ(recorder.reports[0].pattern_offset, 6u);
  EXPECT_EQ(recorder.reports[1].pattern_offset, 12u);
}

// --------------------------------------------------------------------------
// The sequence
// --------------------------------------------------------------------------

TEST(Callout, FiresAtEveryStartingPositionTheSearchTries) {
  // pcre2test `/(?C1)a/no_start_optimize` against "zz" prints callout 1 at
  // offsets 0, 1 and 2 and then reports no match. The optimisations are the
  // difference: with them on, PCRE2's required-code-unit test rejects the
  // subject before the first callout.
  Regex regex("(?C1)a");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "zz", &recorder, &matched), GRX_OK);
  EXPECT_FALSE(matched);
  EXPECT_EQ(trace(recorder),
      (std::vector<std::pair<uint32_t, size_t>>{{1, 0}, {1, 1}, {1, 2}}));
}

TEST(Callout, FiresInBacktrackingOrderNotProgramOrder) {
  // `/(?C1)a(?C2)/no_start_optimize` against "zza": callout 1 at 0, 1 and 2,
  // then callout 2 at 3. The second only fires on the path that got past
  // the `a`, which is what makes the order backtracking order.
  Regex regex("(?C1)a(?C2)");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "zza", &recorder, &matched), GRX_OK);
  EXPECT_TRUE(matched);
  EXPECT_EQ(trace(recorder),
      (std::vector<std::pair<uint32_t, size_t>>{
        {1, 0}, {1, 1}, {1, 2}, {2, 3}}));
}

TEST(Callout, FiresOnceForEachAlternativeTried) {
  // `/(?C1)a|(?C2)b/` against "b": both, at offset 0, in the order the
  // alternation prefers them.
  Regex regex("(?C1)a|(?C2)b");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "b", &recorder, &matched), GRX_OK);
  EXPECT_TRUE(matched);
  EXPECT_EQ(trace(recorder),
      (std::vector<std::pair<uint32_t, size_t>>{{1, 0}, {2, 0}}));
}

TEST(Callout, FiresOnEveryArrivalNotOnlyTheFirst) {
  // The late memo the plain backtracker arms would skip an (instruction,
  // position) already tried. That is sound for the *match* - a state that
  // failed will fail again - and wrong for this, because a callout is a
  // side effect and the arrivals it skipped are arrivals the caller asked
  // to be told about. exec_backtrack.c suppresses the memo for a search
  // that registered a function, and this is the shape that arms it: with
  // the suppression removed the count below falls from 861 to 41.
  //
  // 861 is not a number this test invented. `a*(?C1)b` against forty a's
  // fires once per (start, length) pair, 41 * 42 / 2 of them, and
  // pcre2test 10.46 prints exactly that many for the same pattern and
  // subject under `no_start_optimize,no_auto_possess` - the two
  // optimisations this library does not have.
  Regex regex("a*(?C1)b");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  std::string subject(40, 'a');
  ASSERT_EQ(search(regex, subject, &recorder, &matched), GRX_OK);
  EXPECT_FALSE(matched);
  EXPECT_EQ(recorder.reports.size(), 861u);
}

TEST(Callout, CapturesSoFarAreVisibleAndUnsetOnesAreNpos) {
  Regex regex("(a)(b)?(?C1)");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "a", &recorder, &matched), GRX_OK);
  EXPECT_TRUE(matched);
  ASSERT_EQ(recorder.reports.size(), 1u);
  const Report & report = recorder.reports[0];
  // Group 0 and two groups: every entry present, which is the difference
  // from PCRE2's capture_top - there the ovector past it holds rubbish and
  // a caller has to consult a count before reading a pair.
  ASSERT_EQ(report.count, 3u);
  EXPECT_EQ(report.captures[1].start, 0u);
  EXPECT_EQ(report.captures[1].end, 1u);
  EXPECT_EQ(report.captures[2].start, GRX_NPOS);
  EXPECT_EQ(report.captures[2].end, GRX_NPOS);
}

TEST(Callout, StartIsTheAttemptAndPositionIsHowFarItGot) {
  Regex regex("ab(?C1)");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "zzab", &recorder, &matched), GRX_OK);
  EXPECT_TRUE(matched);
  ASSERT_EQ(recorder.reports.size(), 1u);
  EXPECT_EQ(recorder.reports[0].start, 2u);
  EXPECT_EQ(recorder.reports[0].position, 4u);
  EXPECT_EQ(recorder.reports[0].subject, "zzab");
}

TEST(Callout, TheMarkStandingHereIsReported) {
  Regex regex("a(*MARK:here)(?C1)b");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "ab", &recorder, &matched), GRX_OK);
  ASSERT_EQ(recorder.reports.size(), 1u);
  EXPECT_TRUE(recorder.reports[0].has_mark);
  EXPECT_EQ(recorder.reports[0].mark, "here");
}

TEST(Callout, TheMarkIsTheLastOneReachedNotTheLastOneStanding) {
  // pcre2api defines the callout block's mark as "the most recently passed
  // (*MARK), (*PRUNE), or (*THEN) item in the match" - the running value,
  // which a branch being abandoned does not take back. That is a different
  // question from the one grx_match_mark() answers after a match, where a
  // mark on a path that was then abandoned is not one.
  //
  // Measured, not assumed: pcre2test on `(?C1)x(*MARK:m)y` against "xaby"
  // reports the mark at every attempt after the first, where a callout
  // standing *before* the `(*MARK:m)` can be on no path that passed it.
  // This library reported nothing there until
  // tools/oracle/callout_diff.py was run for the first time.
  Regex regex("(?C1)x(*MARK:m)y");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "xaby", &recorder, &matched), GRX_OK);
  EXPECT_FALSE(matched);
  ASSERT_EQ(recorder.reports.size(), 5u);
  EXPECT_FALSE(recorder.reports[0].has_mark);
  for (size_t i = 1; i < recorder.reports.size(); i++) {
    EXPECT_TRUE(recorder.reports[i].has_mark) << "report " << i;
    EXPECT_EQ(recorder.reports[i].mark, "m") << "report " << i;
  }
}

TEST(Callout, AGroupStillOpenReadsAsUnset) {
  // A group whose start is written and whose end is not has captured
  // nothing yet. `{start, GRX_NPOS}` would be a span whose end a caller
  // could subtract, and GRX_Capture has no way to mark a pair as
  // half-made - so it reads as unset, which is what grx_match_group() says
  // of a group that did not participate.
  //
  // It is pcre2's answer too: `((?C1)a)(b)` reports capture_top 1 there,
  // not 2, and this library said 2 until the differential asked.
  Regex regex("((?C1)a)(b)");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "ab", &recorder, &matched), GRX_OK);
  EXPECT_TRUE(matched);
  ASSERT_EQ(recorder.reports.size(), 1u);
  const Report & report = recorder.reports[0];
  ASSERT_EQ(report.count, 3u);
  // Group 0 and group 1 are both open here, and neither has closed.
  EXPECT_EQ(report.captures[0].start, GRX_NPOS);
  EXPECT_EQ(report.captures[1].start, GRX_NPOS);
  EXPECT_EQ(report.captures[1].end, GRX_NPOS);
}

TEST(Callout, NoMarkIsNullRatherThanEmpty) {
  Regex regex("a(?C1)b");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "ab", &recorder, &matched), GRX_OK);
  ASSERT_EQ(recorder.reports.size(), 1u);
  EXPECT_FALSE(recorder.reports[0].has_mark);
}

TEST(Callout, InsideAnAssertionConditionFiresTwice) {
  // The second place this library's trace differs from pcre2test's, and it
  // is lowering rather than parsing. `(?(?=A)X|Y)` is rewritten as
  // `(?:(?=A)X|(?!A)Y)`, which is exact and compiles the assertion twice -
  // so a callout inside A reports itself twice where pcre2 reports it
  // once. lower.c's comment used to say the cost of that rewrite was
  // "time and not meaning"; a side effect in the body is the exception,
  // and the comment now names it.
  //
  // Pinned so that building the single-run conditional this wants is a
  // change something notices.
  // The subject has to be one the condition *fails* on: where it holds,
  // the positive copy matches and the negative one is never reached, and
  // the two readings agree. "c" takes the else branch, which is the path
  // that runs the assertion a second time.
  Regex regex("(?(?=(?C1)a)ab|c)");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "c", &recorder, &matched), GRX_OK);
  EXPECT_TRUE(matched);
  EXPECT_EQ(trace(recorder),
      (std::vector<std::pair<uint32_t, size_t>>{{1, 0}, {1, 0}}));
}

TEST(Callout, InAConditionsPositionIsNotReported) {
  // The one place this library knows it differs from pcre2test's trace.
  // `(?(?C9)(?=a)b|c)` prints callout 9 there; here the callout is dropped
  // by the parser, because a conditional's children are the condition and
  // the branches positionally and there is no fourth slot to put it in. The
  // *match* is unaffected, which is what the second half of this checks.
  //
  // Pinned rather than left to chance: if the parser ever does carry it
  // through, this test is the record of what changed.
  Regex regex("(?(?C9)(?=a)ab|c)");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  ASSERT_EQ(search(regex, "ab", &recorder, &matched), GRX_OK);
  EXPECT_TRUE(matched);
  EXPECT_TRUE(recorder.reports.empty());
}

// --------------------------------------------------------------------------
// The control
// --------------------------------------------------------------------------

TEST(Callout, WithNoFunctionRegisteredNothingChanges) {
  // PCRE2's rule, and the reason a pattern's callouts cannot change what it
  // matches unless a caller asks them to.
  Regex with("a(?C1)b");
  Regex without("ab");
  ASSERT_EQ(with.result(), GRX_OK);
  ASSERT_EQ(without.result(), GRX_OK);
  int one = 0;
  int two = 0;
  ASSERT_EQ(search(with, "zab", nullptr, &one), GRX_OK);
  ASSERT_EQ(search(without, "zab", nullptr, &two), GRX_OK);
  EXPECT_TRUE(one);
  EXPECT_EQ(one, two);
}

TEST(Callout, FailingThePathMakesTheAlternativeRun) {
  // `out_fail` is an outcome and not an error: the search carries on and
  // finds the match the other branch gives.
  Regex regex("(?C1)ab|ac");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  recorder.fail_at = 1;
  int matched = 0;
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex.get(), nullptr, &match), GRX_OK);
  ASSERT_EQ(search(regex, "ac", &recorder, &matched, match), GRX_OK);
  EXPECT_TRUE(matched);
  GRX_Capture span = {0, 0};
  ASSERT_EQ(grx_match_span(match, &span), GRX_OK);
  EXPECT_EQ(span.start, 0u);
  EXPECT_EQ(span.end, 2u);
  grx_match_destroy(match);
}

TEST(Callout, FailingEveryPathIsAnOrdinaryNonMatch) {
  Regex regex("(?C1)a");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  recorder.fail_at = 1;
  int matched = 0;
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex.get(), nullptr, &match), GRX_OK);
  // Fails the first callout only, so the later starting positions still
  // run - and the subject has no `a` to match at any of them.
  ASSERT_EQ(search(regex, "zz", &recorder, &matched, match), GRX_OK);
  EXPECT_FALSE(matched);
  const GRX_Error * error = grx_match_error(match);
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->code, GRX_OK);
  EXPECT_EQ(error->diag, GRX_DIAG_NONE);
  grx_match_destroy(match);
}

TEST(Callout, StoppingReturnsTheCalloutsOwnCode) {
  Regex regex("(?C1)a");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  recorder.stop_at = 2;
  recorder.stop_with = GRX_ERR_LIMIT;
  int matched = 0;
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex.get(), nullptr, &match), GRX_OK);
  EXPECT_EQ(search(regex, "zzz", &recorder, &matched, match), GRX_ERR_LIMIT);
  EXPECT_FALSE(matched);
  // Stopped at the second, so the third and fourth positions never ran.
  EXPECT_EQ(recorder.reports.size(), 2u);
  const GRX_Error * error = grx_match_error(match);
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->code, GRX_ERR_LIMIT);
  // The diagnostic is what separates a callout borrowing a code from the
  // library reaching that code itself.
  EXPECT_EQ(error->diag, GRX_DIAG_CALLOUT_STOPPED);
  grx_match_destroy(match);
}

TEST(Callout, StoppingWithACodeOutsideTheEnumIsInvalidArgument) {
  // A caller-supplied function out of contract. Passing the value through
  // would make a public entry point return something that is not a
  // GRX_Result at all.
  //
  // 15 rather than a big round number, and the difference is not cosmetic.
  // C lets a function declared to return an enum return any value of its
  // underlying type, which is the case the library guards against; C++
  // does not, and `(GRX_Result)9999` is undefined behaviour in *this
  // file*. UBSan said so - "load of value 9999, which is not a valid value
  // for type GRX_Result" - the first time this suite ran under it. An
  // unscoped enum whose largest enumerator is 10 has a value range of four
  // bits, so 15 is a value the type genuinely has and is not an enumerator,
  // which is exactly the shape the guard is for and is well defined here.
  Regex regex("(?C1)a");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  recorder.stop_at = 1;
  recorder.stop_with = (GRX_Result)15;
  ASSERT_GE((int)recorder.stop_with, (int)GRX_RESULT_COUNT);
  int matched = 0;
  EXPECT_EQ(search(regex, "a", &recorder, &matched), GRX_ERR_INVALID);
}

// --------------------------------------------------------------------------
// Which engine runs it
// --------------------------------------------------------------------------

TEST(Callout, RegisteringAFunctionTakesTheSearchToTheBacktracker) {
  // `a(?C1)b` is a regular program, so without a function AUTO gives it the
  // Pike VM. Registering one is a fact about the search, and it moves.
  Regex regex("a(?C1)b");
  ASSERT_EQ(regex.result(), GRX_OK);
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex.get(), nullptr, &match), GRX_OK);

  int matched = 0;
  ASSERT_EQ(search(regex, "ab", nullptr, &matched, match), GRX_OK);
  EXPECT_EQ(grx_match_engine(match), GRX_ENGINE_PIKE);

  Recorder recorder;
  ASSERT_EQ(search(regex, "ab", &recorder, &matched, match), GRX_OK);
  EXPECT_EQ(grx_match_engine(match), GRX_ENGINE_BACKTRACK);
  EXPECT_EQ(recorder.reports.size(), 1u);
  grx_match_destroy(match);
}

TEST(Callout, NamingTheLockstepEnginesWithAFunctionIsRefused) {
  // The same refusal every other engine mismatch gets: a caller who named
  // an engine asked for that engine's guarantee, and a plausible wrong
  // callout sequence is worse than being told it cannot be done.
  Regex regex("a(?C1)b");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  EXPECT_EQ(
      search(regex, "ab", &recorder, &matched, nullptr, GRX_ENGINE_PIKE),
      GRX_ERR_UNSUPPORTED);
  EXPECT_EQ(
      search(regex, "ab", &recorder, &matched, nullptr, GRX_ENGINE_BITSTATE),
      GRX_ERR_UNSUPPORTED);
  EXPECT_TRUE(recorder.reports.empty());
}

TEST(Callout, NamingThemWithNoFunctionIsFine) {
  // Because then the callout is inert, and the program really is regular.
  Regex regex("a(?C1)b");
  ASSERT_EQ(regex.result(), GRX_OK);
  int matched = 0;
  EXPECT_EQ(search(regex, "ab", nullptr, &matched, nullptr, GRX_ENGINE_PIKE),
      GRX_OK);
  EXPECT_TRUE(matched);
  EXPECT_EQ(
      search(regex, "ab", nullptr, &matched, nullptr, GRX_ENGINE_BITSTATE),
      GRX_OK);
  EXPECT_TRUE(matched);
}

TEST(Callout, AFunctionOnAPatternWithNoCalloutsChangesNothing) {
  // The flag is what keeps this from being a walk of the program on every
  // search, and a caller who registers one function for many patterns must
  // not pay for it on the ones without callouts.
  Regex regex("ab");
  ASSERT_EQ(regex.result(), GRX_OK);
  Recorder recorder;
  int matched = 0;
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex.get(), nullptr, &match), GRX_OK);
  ASSERT_EQ(search(regex, "ab", &recorder, &matched, match), GRX_OK);
  EXPECT_TRUE(matched);
  EXPECT_TRUE(recorder.reports.empty());
  EXPECT_EQ(grx_match_engine(match), GRX_ENGINE_PIKE);
  grx_match_destroy(match);
}

// --------------------------------------------------------------------------
// The grammar around it
// --------------------------------------------------------------------------

TEST(Callout, PerlHasNoCalloutsAtAll) {
  // perl 5.40.1 answers "Sequence (?C...) not recognized in regex" for every
  // spelling, `(?C)` included.
  GRX_Regex * regex = nullptr;
  EXPECT_EQ(grx_regex_compile("a(?C1)b", GRX_SYNTAX_PERL, GRX_OPT_UTF,
                &regex),
      GRX_ERR_SYNTAX);
  grx_regex_free(regex);
}

TEST(Callout, ACalloutMayNotBeQuantified) {
  // pcre2test error 109. A comment is lexically invisible and `a(?#x)*`
  // repeats the `a`; a callout is an atom, and an atom with no extent has
  // nothing to repeat.
  Regex regex("a(?C1)*b");
  EXPECT_EQ(regex.result(), GRX_ERR_SYNTAX);
}

TEST(Callout, TheNumberIsBoundedWherePcre2BoundsIt) {
  Regex ok("(?C255)a");
  Regex over("(?C256)a");
  EXPECT_EQ(ok.result(), GRX_OK);
  EXPECT_EQ(over.result(), GRX_ERR_SYNTAX);
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
