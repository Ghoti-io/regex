/**
 * @file
 *
 * The search request: the window, the four flags, and the search-all loop.
 *
 * These are the parts of a match that the *pattern* cannot express. A regex
 * says what to look for; it cannot say that the buffer it is being run
 * against is the middle of a larger text, or that an empty match is not
 * wanted this time, or what should happen next after one. Every defect this
 * file is written against has the same shape: a caller loops, and the loop
 * either never terminates or silently skips a match.
 *
 * The iteration expectations are not invented. Perl 5 and Node 22 were asked
 * the same questions and their answers are quoted beside the tests that
 * encode them, because the whole point of GRX_IterationRule is that each
 * dialect's own engine is the authority on its own rule.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <string>
#include <vector>

#include "test_helpers.h"

#include "../../src/compile/compile_internal.h"
#include "../../src/exec/exec_internal.h"

namespace {

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
   * Put the program on a different dialect's iteration rule.
   *
   * The rule is a field of the compiled program precisely so that the
   * engines never ask which dialect they are running, and setting that field
   * is therefore the whole of what a Go or Rust pattern would do differently
   * here. Perl and PCRE2 have front ends now and could be compiled instead;
   * this stays because GRX_ITERATE_ADVANCE_SKIP_ABUTTING still has no
   * spelling anywhere.
   */
  void set_iteration(GRX_IterationRule rule) { regex_->program.iteration = rule; }

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

/** "start..end" for the whole match, or "none". */
std::string span_of(GRX_Match * match, int matched) {
  if (!matched) {
    return "none";
  }
  GRX_Capture capture {};
  if (grx_match_span(match, &capture) != GRX_OK) {
    return "error";
  }
  return std::to_string(capture.start) + ".." + std::to_string(capture.end);
}

/** Every match of `pattern` in `subject`, as "a..b a..b", via search_next. */
std::string iterate(const char * pattern, const std::string & subject,
    GRX_IterationRule rule, GRX_Engine engine = GRX_ENGINE_AUTO) {
  Regex regex(pattern);
  if (!regex.ok()) {
    return "compile failed";
  }
  regex.set_iteration(rule);
  Match match(regex);

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.engine = engine;

  std::string out;
  int matched = 0;
  GRX_Result result = grx_regex_search_ex(regex.get(), subject.data(),
      subject.size(), &options, match.get(), &matched);

  // A loop that cannot run away even if the rule under test is wrong: the
  // cap is far above any correct answer for these subjects, and a test that
  // hangs tells you far less than one that fails.
  for (int guard = 0; result == GRX_OK && matched && guard < 64; guard++) {
    if (!out.empty()) {
      out += " ";
    }
    out += span_of(match.get(), matched);
    result = grx_regex_search_next(regex.get(), subject.data(),
        subject.size(), &options, match.get(), &matched);
  }
  if (result != GRX_OK) {
    return "error";
  }
  return out;
}

} // namespace

// --------------------------------------------------------------------------
// The options struct itself
// --------------------------------------------------------------------------

TEST(SearchOptions, DefaultsAreAWholeSubjectSearch) {
  GRX_SearchOptions options;
  grx_search_options_default(&options);
  EXPECT_EQ(options.begin, 0u);
  EXPECT_EQ(options.end, GRX_NPOS);
  EXPECT_EQ(options.flags, (uint32_t)GRX_SEARCH_NONE);
  EXPECT_EQ(options.engine, GRX_ENGINE_AUTO);
  EXPECT_EQ(options.limits, nullptr);

  grx_search_options_default(nullptr); // Must not crash.
}

TEST(SearchOptions, NullOptionsMeansTheDefaults) {
  Regex regex("b+");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  int matched = 0;
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "aabbc", 5, nullptr, match.get(),
                &matched),
      GRX_OK);
  EXPECT_EQ(span_of(match.get(), matched), "2..4");
}

TEST(SearchOptions, AnUnknownFlagIsRejected) {
  // An unknown bit is a caller who compiled against a later version, and
  // silently ignoring it would mean honouring none of what they asked for.
  Regex regex("a");
  ASSERT_TRUE(regex.ok());
  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.flags = 0x80000000u;

  int matched = 0;
  EXPECT_EQ(grx_regex_search_ex(regex.get(), "a", 1, &options, nullptr,
                &matched),
      GRX_ERR_INVALID);
}

TEST(SearchOptions, AWindowOutsideTheSubjectIsRejected) {
  Regex regex("a");
  ASSERT_TRUE(regex.ok());
  GRX_SearchOptions options;
  grx_search_options_default(&options);

  int matched = 0;
  options.end = 9;
  EXPECT_EQ(grx_regex_search_ex(regex.get(), "abc", 3, &options, nullptr,
                &matched),
      GRX_ERR_INVALID);

  grx_search_options_default(&options);
  options.begin = 2;
  options.end = 1;
  EXPECT_EQ(grx_regex_search_ex(regex.get(), "abc", 3, &options, nullptr,
                &matched),
      GRX_ERR_INVALID);
}

// --------------------------------------------------------------------------
// The window
// --------------------------------------------------------------------------

TEST(SearchWindow, EndIsWhereTheSubjectEnds) {
  // Not merely where the search stops looking: `$` holds at `end`, and
  // nothing past it is readable. This is what lets a caller match one field
  // of a larger record without copying it out.
  Regex regex("b$");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.end = 2;

  int matched = 0;
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "abc", 3, &options, match.get(),
                &matched),
      GRX_OK);
  EXPECT_EQ(span_of(match.get(), matched), "1..2");

  // And without the window the same pattern does not match at all.
  grx_search_options_default(&options);
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "abc", 3, &options, match.get(),
                &matched),
      GRX_OK);
  EXPECT_FALSE(matched);
}

TEST(SearchWindow, NothingPastEndIsVisible) {
  Regex regex("c");
  ASSERT_TRUE(regex.ok());

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.end = 2;

  int matched = 1;
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "abc", 3, &options, nullptr,
                &matched),
      GRX_OK);
  EXPECT_FALSE(matched);
}

TEST(SearchWindow, LookbehindStillSeesBeforeBegin) {
  // The asymmetry the header documents. `begin` says where a match may
  // start, not where the text starts, so a lookbehind reads through it.
  Regex regex("(?<=ab)c");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.begin = 2;

  int matched = 0;
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "abc", 3, &options, match.get(),
                &matched),
      GRX_OK);
  EXPECT_EQ(span_of(match.get(), matched), "2..3");
}

TEST(SearchWindow, CaretStillMeansOffsetZeroNotBegin) {
  // The other half of the same asymmetry, and the one that matters for
  // iteration: the second call of a search-all loop must not report `^` as
  // holding wherever the first match happened to stop.
  Regex regex("^b");
  ASSERT_TRUE(regex.ok());

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.begin = 1;

  int matched = 1;
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "abc", 3, &options, nullptr,
                &matched),
      GRX_OK);
  EXPECT_FALSE(matched);
}

TEST(SearchWindow, BeginIsWhereAnAnchoredMatchMustStart) {
  Regex regex("b");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.begin = 1;

  int matched = 0;
  ASSERT_EQ(grx_regex_match_ex(regex.get(), "abc", 3, &options, match.get(),
                &matched),
      GRX_OK);
  EXPECT_EQ(span_of(match.get(), matched), "1..2");

  options.begin = 0;
  ASSERT_EQ(grx_regex_match_ex(regex.get(), "abc", 3, &options, match.get(),
                &matched),
      GRX_OK);
  EXPECT_FALSE(matched);
}

// --------------------------------------------------------------------------
// NOTBOL and NOTEOL
// --------------------------------------------------------------------------

TEST(SearchFlags, NotbolAndNoteolSuppressTheSubjectEnds) {
  Regex caret("^a");
  Regex dollar("c$");
  ASSERT_TRUE(caret.ok());
  ASSERT_TRUE(dollar.ok());

  GRX_SearchOptions options;
  grx_search_options_default(&options);

  int matched = 0;
  ASSERT_EQ(grx_regex_search_ex(caret.get(), "abc", 3, &options, nullptr,
                &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
  options.flags = GRX_SEARCH_NOTBOL;
  ASSERT_EQ(grx_regex_search_ex(caret.get(), "abc", 3, &options, nullptr,
                &matched),
      GRX_OK);
  EXPECT_FALSE(matched);

  grx_search_options_default(&options);
  ASSERT_EQ(grx_regex_search_ex(dollar.get(), "abc", 3, &options, nullptr,
                &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
  options.flags = GRX_SEARCH_NOTEOL;
  ASSERT_EQ(grx_regex_search_ex(dollar.get(), "abc", 3, &options, nullptr,
                &matched),
      GRX_OK);
  EXPECT_FALSE(matched);
}

TEST(SearchFlags, NotbolAndNoteolLeaveLineAnchorsAlone) {
  // They say the buffer's ends are not the text's ends. A `^` that holds
  // because a newline precedes it is holding for a reason wholly inside the
  // buffer, and suppressing it too would be a different and wrong rule.
  Regex caret("^b", GRX_OPT_UTF | GRX_OPT_MULTILINE);
  Regex dollar("a$", GRX_OPT_UTF | GRX_OPT_MULTILINE);
  ASSERT_TRUE(caret.ok());
  ASSERT_TRUE(dollar.ok());

  Match mcaret(caret);
  Match mdollar(dollar);

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.flags = GRX_SEARCH_NOTBOL | GRX_SEARCH_NOTEOL;

  int matched = 0;
  ASSERT_EQ(grx_regex_search_ex(caret.get(), "a\nb", 3, &options,
                mcaret.get(), &matched),
      GRX_OK);
  EXPECT_EQ(span_of(mcaret.get(), matched), "2..3");

  ASSERT_EQ(grx_regex_search_ex(dollar.get(), "a\nb", 3, &options,
                mdollar.get(), &matched),
      GRX_OK);
  EXPECT_EQ(span_of(mdollar.get(), matched), "0..1");
}

TEST(SearchFlags, NotbolAndNoteolDoNotReachTheSubjectAnchors) {
  // `\A` and `^` without multiline mean the same position, and so do `\z`
  // and `$`, and `\Z` and `$`; the IR gives each pair one kind because
  // under an ordinary whole-subject search they are the same assertion.
  // These flags are exactly when they stop being the same: PCRE2_NOTBOL
  // "does not affect \A" in as many words, PCRE2_NOTEOL leaves `\Z` and
  // `\z` alone, and glibc's REG_NOTBOL/REG_NOTEOL leave GNU's `` \` `` and
  // `\'` alone. Two references, one rule, and this library suppressed all
  // six spellings until a generated comparison against pcre2 asked.
  struct Row {
    const char * pattern;
    uint32_t flags;
    bool matches;
  };
  static const Row rows[] = {
    {"\\Aa", GRX_SEARCH_NOTBOL, true},
    {"^a", GRX_SEARCH_NOTBOL, false},
    {"c\\z", GRX_SEARCH_NOTEOL, true},
    {"c\\Z", GRX_SEARCH_NOTEOL, true},
    {"c$", GRX_SEARCH_NOTEOL, false},
    // Both flags at once, so that neither is being read in place of the
    // other, and both anchors in one pattern so the program holds two
    // instructions of the same kind with different flag bits.
    {"\\Aabc\\z", GRX_SEARCH_NOTBOL | GRX_SEARCH_NOTEOL, true},
    {"^abc$", GRX_SEARCH_NOTBOL | GRX_SEARCH_NOTEOL, false},
    {"\\Aabc$", GRX_SEARCH_NOTBOL | GRX_SEARCH_NOTEOL, false},
    {"^abc\\z", GRX_SEARCH_NOTBOL | GRX_SEARCH_NOTEOL, false},
  };

  for (const Row & row : rows) {
    for (GRX_Engine engine : {GRX_ENGINE_PIKE, GRX_ENGINE_BACKTRACK}) {
      GRX_Regex * regex = nullptr;
      ASSERT_EQ(grx_regex_compile(row.pattern, GRX_SYNTAX_PERL, 0, &regex),
          GRX_OK) << row.pattern;

      GRX_SearchOptions options;
      grx_search_options_default(&options);
      options.flags = row.flags;
      options.engine = engine;

      int matched = 0;
      EXPECT_EQ(grx_regex_search_ex(regex, "abc", 3, &options, nullptr,
                    &matched),
          GRX_OK) << row.pattern;
      EXPECT_EQ(matched != 0, row.matches)
          << "/" << row.pattern << "/ with flags " << row.flags
          << " on engine " << (int)engine;
      grx_regex_free(regex);
    }
  }
}

TEST(SearchFlags, BothEnginesAgreeOnNotbolAndNoteol) {
  // The equivalence invariant (documentation/design.md section 3.5.4) covers
  // the search request too: a flag honoured by one engine and ignored by the
  // other is exactly the defect the invariant exists to catch.
  const char * const patterns[] = {"^a", "c$", "^abc$", "a|^a|c$"};
  for (const char * pattern : patterns) {
    Regex regex(pattern);
    ASSERT_TRUE(regex.ok()) << pattern;
    Match pike(regex);
    Match back(regex);

    for (uint32_t flags = 0; flags <= 3u; flags++) {
      GRX_SearchOptions options;
      grx_search_options_default(&options);
      options.flags = flags;

      int pike_matched = 0;
      int back_matched = 0;
      options.engine = GRX_ENGINE_PIKE;
      ASSERT_EQ(grx_regex_search_ex(regex.get(), "abc", 3, &options,
                    pike.get(), &pike_matched),
          GRX_OK);
      options.engine = GRX_ENGINE_BACKTRACK;
      ASSERT_EQ(grx_regex_search_ex(regex.get(), "abc", 3, &options,
                    back.get(), &back_matched),
          GRX_OK);
      EXPECT_EQ(span_of(pike.get(), pike_matched),
          span_of(back.get(), back_matched))
          << "/" << pattern << "/ with flags " << flags;
    }
  }
}

// --------------------------------------------------------------------------
// NOTEMPTY
// --------------------------------------------------------------------------

TEST(SearchFlags, NotemptyRefusesAnEmptyMatchAnywhere) {
  Regex regex("a*");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.flags = GRX_SEARCH_NOTEMPTY;

  int matched = 1;
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "b", 1, &options, match.get(),
                &matched),
      GRX_OK);
  EXPECT_FALSE(matched);

  // But a non-empty match further along is still found.
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "ba", 2, &options, match.get(),
                &matched),
      GRX_OK);
  EXPECT_EQ(span_of(match.get(), matched), "1..2");
}

TEST(SearchFlags, NotemptyAtstartRefusesOnlyAtTheStart) {
  Regex regex("x*");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.flags = GRX_SEARCH_NOTEMPTY_ATSTART;

  int matched = 0;
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "ab", 2, &options, match.get(),
                &matched),
      GRX_OK);
  EXPECT_EQ(span_of(match.get(), matched), "1..1");

  // NOTEMPTY is the stronger of the two and wins when both are given.
  options.flags = GRX_SEARCH_NOTEMPTY | GRX_SEARCH_NOTEMPTY_ATSTART;
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "ab", 2, &options, match.get(),
                &matched),
      GRX_OK);
  EXPECT_FALSE(matched);
}

TEST(SearchFlags, NotemptyLetsALongerAlternativeWinFromTheSamePosition) {
  // The reason rejecting an empty match cannot simply skip the position: a
  // lazy quantifier prefers the empty match, and the non-empty one it was
  // hiding is the answer once the empty one is refused.
  Regex regex("a??");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  GRX_SearchOptions options;
  grx_search_options_default(&options);

  int matched = 0;
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "aa", 2, &options, match.get(),
                &matched),
      GRX_OK);
  EXPECT_EQ(span_of(match.get(), matched), "0..0");

  options.flags = GRX_SEARCH_NOTEMPTY;
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "aa", 2, &options, match.get(),
                &matched),
      GRX_OK);
  EXPECT_EQ(span_of(match.get(), matched), "0..1");
}

TEST(SearchFlags, BothEnginesAgreeOnNotempty) {
  const char * const patterns[] = {"a*", "a??", "(a)|", "x*", "(?:a|)"};
  for (const char * pattern : patterns) {
    Regex regex(pattern);
    ASSERT_TRUE(regex.ok()) << pattern;
    Match pike(regex);
    Match back(regex);

    for (uint32_t flags :
        {(uint32_t)GRX_SEARCH_NOTEMPTY, (uint32_t)GRX_SEARCH_NOTEMPTY_ATSTART}) {
      GRX_SearchOptions options;
      grx_search_options_default(&options);
      options.flags = flags;

      int pike_matched = 0;
      int back_matched = 0;
      options.engine = GRX_ENGINE_PIKE;
      ASSERT_EQ(grx_regex_search_ex(regex.get(), "aab", 3, &options,
                    pike.get(), &pike_matched),
          GRX_OK);
      options.engine = GRX_ENGINE_BACKTRACK;
      ASSERT_EQ(grx_regex_search_ex(regex.get(), "aab", 3, &options,
                    back.get(), &back_matched),
          GRX_OK);
      EXPECT_EQ(span_of(pike.get(), pike_matched),
          span_of(back.get(), back_matched))
          << "/" << pattern << "/ with flags " << flags;
    }
  }
}

TEST(SearchFlags, NotemptyDoesNotReachInsideALookaround) {
  // A lookaround body is compiled as a sub-program ending in its own MATCH.
  // NOTEMPTY is a rule about the *result* of the search, so applying it to
  // that inner MATCH would make every zero-width assertion fail.
  Regex regex("(?=a)a");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.flags = GRX_SEARCH_NOTEMPTY;

  int matched = 0;
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "a", 1, &options, match.get(),
                &matched),
      GRX_OK);
  EXPECT_EQ(span_of(match.get(), matched), "0..1");
}

// --------------------------------------------------------------------------
// The UTF-8 check
// --------------------------------------------------------------------------

TEST(SearchFlags, AnInvalidSubjectIsRefusedBeforeMatching) {
  // Reported up front, not when some engine happens to read that far: the
  // same subject must give the same answer on both engines, and "whichever
  // engine reached the bad byte first" is not an answer.
  Regex regex("a");
  ASSERT_TRUE(regex.ok());

  const char bad[] = {'a', '\xff'};
  GRX_SearchOptions options;
  grx_search_options_default(&options);

  int matched = 1;
  EXPECT_EQ(grx_regex_search_ex(regex.get(), bad, 2, &options, nullptr,
                &matched),
      GRX_ERR_INVALID);
  options.engine = GRX_ENGINE_BACKTRACK;
  EXPECT_EQ(grx_regex_search_ex(regex.get(), bad, 2, &options, nullptr,
                &matched),
      GRX_ERR_INVALID);
}

TEST(SearchFlags, NoUtfCheckSkipsTheScanAndTrustsTheCaller) {
  // The promise is not verified, which is the entire point of making it: the
  // backtracker stops at the MATCH after consuming the "a" and never reads
  // the byte that would have failed the scan.
  Regex regex("a");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  const char bad[] = {'a', '\xff'};
  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.flags = GRX_SEARCH_NO_UTF_CHECK;
  options.engine = GRX_ENGINE_BACKTRACK;

  int matched = 0;
  ASSERT_EQ(grx_regex_match_ex(regex.get(), bad, 2, &options, match.get(),
                &matched),
      GRX_OK);
  EXPECT_EQ(span_of(match.get(), matched), "0..1");

  // A caller who breaks the promise is not rewarded with a wrong answer, only
  // with a later one: an engine that does reach the malformed bytes still
  // refuses them rather than matching a pattern against a character the
  // subject does not contain. The Pike VM steps every position in lockstep
  // and so reaches them here.
  options.engine = GRX_ENGINE_PIKE;
  EXPECT_EQ(grx_regex_match_ex(regex.get(), bad, 2, &options, match.get(),
                &matched),
      GRX_ERR_INVALID);
}

TEST(SearchFlags, TheCheckCoversOnlyTheVisibleSubject) {
  // Bytes past `end` are not part of this search and so are not its problem.
  Regex regex("a");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  const char bad[] = {'a', '\xff'};
  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.end = 1;

  int matched = 0;
  ASSERT_EQ(grx_regex_search_ex(regex.get(), bad, 2, &options, match.get(),
                &matched),
      GRX_OK);
  EXPECT_EQ(span_of(match.get(), matched), "0..1");
}

TEST(SearchFlags, EcmascriptChecksTheSubjectWithoutTheUFlag) {
  // Not an oversight: ECMAScript's subject is a sequence of characters in
  // every mode, and `u` changes the grammar and the folding rather than
  // whether the subject is text (GRX_Profile::subject_is_text, and
  // documentation/dialects.md section 6.1). So the check runs here too.
  // A dialect whose subject really is bytes - PCRE2 without PCRE2_UTF - will
  // set that field the other way and this test will need a sibling saying so.
  Regex regex("a", GRX_OPT_NONE);
  ASSERT_TRUE(regex.ok());

  const char bad[] = {'a', '\xff'};
  int matched = 1;
  EXPECT_EQ(
      grx_regex_search_ex(regex.get(), bad, 2, nullptr, nullptr, &matched),
      GRX_ERR_INVALID);
}

// --------------------------------------------------------------------------
// Steps and the convenience accessor
// --------------------------------------------------------------------------

TEST(Match, StepsAreReportedAndReset) {
  Regex regex("a+b");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  EXPECT_EQ(grx_match_steps(nullptr), 0u);
  EXPECT_EQ(grx_match_steps(match.get()), 0u);

  int matched = 0;
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "aaab", 4, nullptr, match.get(),
                &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
  size_t many = grx_match_steps(match.get());
  EXPECT_GT(many, 0u);

  // A shorter subject costs fewer steps, and the count is not cumulative.
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "ab", 2, nullptr, match.get(),
                &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
  EXPECT_LT(grx_match_steps(match.get()), many);
}

TEST(Match, StepsAreReportedEvenWhenTheLimitIsHit) {
  // The count is what max_steps counts, so it has to survive the failure
  // that max_steps causes - otherwise a caller measuring a limit learns
  // nothing from the case that hit it.
  Regex regex("(a+)+b");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 500;

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.limits = &limits;
  options.engine = GRX_ENGINE_BACKTRACK;

  // The leading `b` is a decoy, not decoration. Every match of `(a+)+b`
  // contains one, so a subject of nothing but `a` is settled by the
  // prefilter before an engine runs, and a search that never runs spends no
  // steps to report. Putting it where no match can use it leaves the run
  // intact: the first-byte set steps over position 0.
  const std::string subject = "b" + std::string(40, 'a');
  int matched = 0;
  EXPECT_EQ(grx_regex_search_ex(regex.get(), subject.data(), subject.size(),
                &options, match.get(), &matched),
      GRX_ERR_LIMIT);
  EXPECT_GT(grx_match_steps(match.get()), 0u);
}

TEST(Match, AWindowShorterThanTheShortestMatchCostsNoEngineAtAll) {
  // `[ab]{4}` is the pattern that isolates this prefilter from the other
  // three. Its min_length is 4 and it has neither a literal prefix nor a
  // required literal, so neither of those can answer; the subject is three
  // bytes and every one of them is in the first-byte set, so the byte skip
  // cannot answer either. Whatever settles this search, it is the length.
  Regex regex("[ab]{4}");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  GRX_Facts facts;
  ASSERT_EQ(grx_regex_facts(regex.get(), &facts), GRX_OK);
  ASSERT_EQ(facts.min_length, 4u);
  ASSERT_EQ(facts.literal_prefix, nullptr);
  ASSERT_EQ(facts.required_literal, nullptr);
  ASSERT_EQ(facts.first_bytes_known, 1);

  // A budget of one step is what makes this an assertion rather than a
  // restatement: "no match" and "ran out of budget" are two states a status
  // alone cannot separate, and an engine that starts an attempt cannot finish
  // inside one step. So GRX_OK here says no engine ran.
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 1;

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.limits = &limits;

  int matched = 1;
  EXPECT_EQ(grx_regex_search_ex(regex.get(), "aaa", 3, &options, match.get(),
                &matched),
      GRX_OK);
  EXPECT_FALSE(matched);
  EXPECT_EQ(grx_match_steps(match.get()), 0u);

  // The control is the same pattern and the same budget one byte later: a
  // window a match fits in has to be searched, and then it is the budget that
  // stops it and not the length. Without this the test above passes for a
  // prefilter that refuses everything.
  EXPECT_EQ(grx_regex_search_ex(regex.get(), "aaab", 4, &options, match.get(),
                &matched),
      GRX_ERR_LIMIT);

  // And with room to work in it matches, which is the other direction of the
  // same control.
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "aaab", 4, nullptr, match.get(),
                &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
  EXPECT_GT(grx_match_steps(match.get()), 0u);
}

TEST(Match, TheWindowIsTheSearchWindowAndNotTheSubject) {
  // A caller who hands over a buffer and a window is asking about the window,
  // so that is what has to be measured. The subject is long enough and the
  // window is not, and the answer must come from the window - the mirror image
  // is the bug where a prefilter measures the whole buffer and lets a search
  // run that had no room from the start.
  Regex regex("[ab]{4}");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 1;

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.limits = &limits;
  options.begin = 2;
  options.end = 4;

  int matched = 1;
  EXPECT_EQ(grx_regex_search_ex(regex.get(), "aaaaaa", 6, &options,
                match.get(), &matched),
      GRX_OK);
  EXPECT_FALSE(matched);
  EXPECT_EQ(grx_match_steps(match.get()), 0u);
}

TEST(Match, APatternThatCanMatchEmptyIsNotRefusedForRoom) {
  // min_length of zero, and a zero-length window is a window that match fits
  // in. The comparison is unsigned, so this is not the arm that would break
  // first - but a pattern that can match empty is most patterns, and a
  // prefilter that got this wrong would be wrong about nearly everything.
  Regex regex("a*");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  GRX_Facts facts;
  ASSERT_EQ(grx_regex_facts(regex.get(), &facts), GRX_OK);
  ASSERT_EQ(facts.min_length, 0u);

  int matched = 0;
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "", 0, nullptr, match.get(),
                &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
}

TEST(Match, SpanIsGroupZero) {
  Regex regex("a(b)c");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  GRX_Capture capture {};
  EXPECT_EQ(grx_match_span(nullptr, &capture), GRX_ERR_INVALID);
  EXPECT_EQ(grx_match_span(match.get(), nullptr), GRX_ERR_INVALID);

  int matched = 0;
  ASSERT_EQ(grx_regex_search_ex(regex.get(), "xabc", 4, nullptr, match.get(),
                &matched),
      GRX_OK);
  ASSERT_TRUE(matched);
  ASSERT_EQ(grx_match_span(match.get(), &capture), GRX_OK);
  EXPECT_EQ(capture.start, 1u);
  EXPECT_EQ(capture.end, 4u);
}

// --------------------------------------------------------------------------
// The search-all loop
// --------------------------------------------------------------------------

TEST(SearchNext, RejectsNullArguments) {
  Regex regex("a");
  ASSERT_TRUE(regex.ok());
  Match match(regex);
  int matched = 0;

  EXPECT_EQ(grx_regex_search_next(nullptr, "a", 1, nullptr, match.get(),
                &matched),
      GRX_ERR_INVALID);
  // The match object is not optional here: it is the only record of where
  // the loop had got to.
  EXPECT_EQ(grx_regex_search_next(regex.get(), "a", 1, nullptr, nullptr,
                &matched),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_search_next(regex.get(), "a", 1, nullptr, match.get(),
                nullptr),
      GRX_ERR_INVALID);
}

TEST(SearchNext, WithNoPreviousMatchItIsAnOrdinarySearch) {
  Regex regex("b");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  int matched = 0;
  ASSERT_EQ(grx_regex_search_next(regex.get(), "abc", 3, nullptr, match.get(),
                &matched),
      GRX_OK);
  EXPECT_EQ(span_of(match.get(), matched), "1..2");
}

TEST(SearchNext, EcmascriptMatchesNode) {
  // Node 22.23, `[...s.matchAll(new RegExp(p, "gu"))]`. These are the rows
  // where the rule is visible: a pattern that can match empty, next to one
  // that cannot.
  EXPECT_EQ(iterate("a", "aaa", GRX_ITERATE_ADVANCE_ONE), "0..1 1..2 2..3");
  EXPECT_EQ(iterate("a*", "aab", GRX_ITERATE_ADVANCE_ONE), "0..2 2..2 3..3");
  EXPECT_EQ(iterate("a*", "ba", GRX_ITERATE_ADVANCE_ONE), "0..0 1..2 2..2");
  EXPECT_EQ(iterate("a*", "baa", GRX_ITERATE_ADVANCE_ONE), "0..0 1..3 3..3");
  EXPECT_EQ(iterate("a*", "ab", GRX_ITERATE_ADVANCE_ONE), "0..1 1..1 2..2");
  EXPECT_EQ(iterate("a??", "aa", GRX_ITERATE_ADVANCE_ONE), "0..0 1..1 2..2");
  EXPECT_EQ(iterate("", "abc", GRX_ITERATE_ADVANCE_ONE),
      "0..0 1..1 2..2 3..3");
  EXPECT_EQ(iterate("b*", "abc", GRX_ITERATE_ADVANCE_ONE),
      "0..0 1..2 2..2 3..3");
}

TEST(SearchNext, PerlRetriesBeforeAdvancing) {
  // Perl 5, `while ($s =~ /$p/g) { push @o, "$-[0]..$+[0]" }`. The rule that
  // makes `a??` produce five matches where ECMAScript produces three: after
  // an empty match Perl asks for a non-empty one at the same offset before
  // giving up on that offset.
  EXPECT_EQ(iterate("a??", "aa", GRX_ITERATE_RETRY_THEN_ADVANCE),
      "0..0 0..1 1..1 1..2 2..2");
  EXPECT_EQ(iterate("a*", "baa", GRX_ITERATE_RETRY_THEN_ADVANCE),
      "0..0 1..3 3..3");
  EXPECT_EQ(iterate("a*", "ab", GRX_ITERATE_RETRY_THEN_ADVANCE),
      "0..1 1..1 2..2");
  EXPECT_EQ(iterate("a*", "aab", GRX_ITERATE_RETRY_THEN_ADVANCE),
      "0..2 2..2 3..3");
}

TEST(SearchNext, PerlPinsBackslashGToThePreviousMatchAndPcre2DoesNot) {
  // Both dialects retry before advancing, and they still disagree, because
  // the iteration rule and what `\G` means are two axes rather than one.
  //
  // `\Ga*` against "baac": both find the empty match at 0, both fail to
  // find a non-empty one there, and both step to 1. PCRE2's `\G` is where
  // the current attempt begins, so it holds at 1 and "aa" matches; perl's is
  // `pos()`, which the failed attempt did not move, so it still holds only
  // at 0 and the loop is over. pcre2test 10.46 reports four matches,
  // `while ("baac" =~ /\Ga*/g)` reports one.
  //
  // Neither is a quirk: PCRE2 does not provide the loop, so its `\G` can
  // only mean the attempt its caller asked for.
  struct Row {
    GRX_Syntax syntax;
    const char * pattern;
    const char * subject;
    const char * expected;
  };
  static const Row rows[] = {
    {GRX_SYNTAX_PERL, "\\Ga*", "baac", "0..0"},
    {GRX_SYNTAX_PCRE, "\\Ga*", "baac", "0..0 1..3 3..3 4..4"},
    {GRX_SYNTAX_PERL, "\\Ga*", "aab", "0..2 2..2"},
    {GRX_SYNTAX_PCRE, "\\Ga*", "aab", "0..2 2..2 3..3"},
    // A `\G` in only one branch anchors nothing, and the two agree again.
    {GRX_SYNTAX_PERL, "(?:\\Ga|b)", "cba", "1..2 2..3"},
    {GRX_SYNTAX_PCRE, "(?:\\Ga|b)", "cba", "1..2 2..3"},
    // And without `\G` the axis has nothing to say.
    {GRX_SYNTAX_PERL, "a*", "baac", "0..0 1..3 3..3 4..4"},
    {GRX_SYNTAX_PCRE, "a*", "baac", "0..0 1..3 3..3 4..4"},
  };

  for (const Row & row : rows) {
    for (GRX_Engine engine : {GRX_ENGINE_PIKE, GRX_ENGINE_BACKTRACK}) {
      GRX_Regex * regex = nullptr;
      ASSERT_EQ(grx_regex_compile(row.pattern, row.syntax, 0, &regex), GRX_OK)
          << row.pattern;
      GRX_Match * match = nullptr;
      ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);

      GRX_SearchOptions options;
      grx_search_options_default(&options);
      options.engine = engine;

      std::string out;
      int matched = 0;
      GRX_Result result = grx_regex_search_ex(regex, row.subject,
          strlen(row.subject), &options, match, &matched);
      for (int guard = 0; result == GRX_OK && matched && guard < 64; guard++) {
        if (!out.empty()) {
          out += " ";
        }
        out += span_of(match, matched);
        result = grx_regex_search_next(regex, row.subject,
            strlen(row.subject), &options, match, &matched);
      }
      EXPECT_EQ(result, GRX_OK);
      EXPECT_EQ(out, row.expected)
          << "/" << row.pattern << "/ on \"" << row.subject
          << "\" engine " << (int)engine;

      grx_match_destroy(match);
      grx_regex_free(regex);
    }
  }
}

TEST(SearchNext, GoDropsAnEmptyMatchThatAbutsThePreviousOne) {
  // Go is tier 2 and has no front end here yet, so these expectations come
  // from the algorithm its `regexp` package documents and implements - an
  // empty match whose start equals the previous match's end is discarded and
  // the loop moves one character past it - rather than from a live oracle.
  // WP-32 replaces this comment with a differential run against Go itself.
  EXPECT_EQ(iterate("a*", "baa", GRX_ITERATE_ADVANCE_SKIP_ABUTTING),
      "0..0 1..3");
  EXPECT_EQ(iterate("a*", "ab", GRX_ITERATE_ADVANCE_SKIP_ABUTTING),
      "0..1 2..2");
  EXPECT_EQ(iterate("a??", "aa", GRX_ITERATE_ADVANCE_SKIP_ABUTTING),
      "0..0 1..1 2..2");
}

TEST(SearchNext, EveryRuleTerminatesOnAPatternThatMatchesEmptyEverywhere) {
  // The defect this whole field exists to prevent. `guard` in the helper is
  // 64; a rule that failed to advance would return the cap's worth of
  // matches rather than four, and the count is what is checked.
  const std::string subject = "abc";
  for (GRX_IterationRule rule : {GRX_ITERATE_RETRY_THEN_ADVANCE,
           GRX_ITERATE_ADVANCE_ONE, GRX_ITERATE_ADVANCE_SKIP_ABUTTING}) {
    std::string spans = iterate("", subject, rule);
    size_t count = spans.empty() ? 0 : 1;
    for (char c : spans) {
      count += (c == ' ');
    }
    EXPECT_EQ(count, 4u) << "rule " << (int)rule << ": " << spans;
  }
}

TEST(SearchNext, BothEnginesAgree) {
  const char * const patterns[] = {"a", "a*", "a??", "b*", "(a)|(b)", ""};
  const char * const subjects[] = {"aab", "ba", "abc", ""};
  for (const char * pattern : patterns) {
    for (const char * subject : subjects) {
      for (GRX_IterationRule rule : {GRX_ITERATE_RETRY_THEN_ADVANCE,
               GRX_ITERATE_ADVANCE_ONE, GRX_ITERATE_ADVANCE_SKIP_ABUTTING}) {
        EXPECT_EQ(iterate(pattern, subject, rule, GRX_ENGINE_PIKE),
            iterate(pattern, subject, rule, GRX_ENGINE_BACKTRACK))
            << "/" << pattern << "/ on \"" << subject << "\" rule "
            << (int)rule;
      }
    }
  }
}

TEST(SearchNext, TheWindowAndFlagsApplyToEveryStep) {
  // `end` and the flags belong to the loop, not to its first call; `begin`
  // is the one field that is spent once.
  Regex regex("a");
  ASSERT_TRUE(regex.ok());
  Match match(regex);

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.begin = 1;
  options.end = 4;

  std::string spans;
  int matched = 0;
  GRX_Result result = grx_regex_search_ex(regex.get(), "aaaaa", 5, &options,
      match.get(), &matched);
  for (int guard = 0; result == GRX_OK && matched && guard < 16; guard++) {
    if (!spans.empty()) {
      spans += " ";
    }
    spans += span_of(match.get(), matched);
    result = grx_regex_search_next(regex.get(), "aaaaa", 5, &options,
        match.get(), &matched);
  }
  ASSERT_EQ(result, GRX_OK);
  EXPECT_EQ(spans, "1..2 2..3 3..4");
}

TEST(SearchNext, AstralCharactersAdvanceByCodePoint) {
  // "One character" after an empty match is a code point, not a byte: an
  // advance of one byte would land inside a sequence and the next search
  // would report an offset no caller can slice at.
  const std::string subject = "\xF0\x9F\x98\x80\xF0\x9F\x98\x81"; // two emoji
  EXPECT_EQ(iterate("", subject, GRX_ITERATE_ADVANCE_ONE), "0..0 4..4 8..8");
  EXPECT_EQ(iterate("x*", subject, GRX_ITERATE_RETRY_THEN_ADVANCE),
      "0..0 4..4 8..8");
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
