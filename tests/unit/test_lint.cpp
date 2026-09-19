/**
 * @file
 *
 * The JSON Schema subset check.
 *
 * Every row here is JSON Schema core section 6.4 read as written. The list
 * it gives is short - characters, `[abc]`, `[a-z]`, `[^abc]`, the five
 * quantifiers and their lazy forms, `^`, `$`, `(...)` and `|` - and a lint
 * that quietly widened it would be a lint nobody could rely on. So `.`,
 * `\d` and `(?:...)` are reported, and the header says why each of the three
 * is not an oversight.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <string>

#include "test_helpers.h"

namespace {

/** A parsed pattern that frees itself. */
class Parsed {
public:
  Parsed(const char * pattern, uint32_t options = GRX_OPT_UTF) {
    GRX_Error error;
    grx_error_clear(&error);
    result_ = grx_pattern_parse_with_allocator(pattern, strlen(pattern),
        GRX_SYNTAX_ECMASCRIPT, options, nullptr, nullptr, &error, &pattern_);
  }
  Parsed(const Parsed &) = delete;
  Parsed & operator=(const Parsed &) = delete;
  ~Parsed() { grx_pattern_free(pattern_); }

  bool ok() const { return result_ == GRX_OK; }
  const GRX_Pattern * get() const { return pattern_; }

private:
  GRX_Pattern * pattern_ = nullptr;
  GRX_Result result_ = GRX_ERR_INTERNAL;
};

/** The finding for a pattern, and where it was. */
GRX_LintReport lint(const char * pattern, uint32_t options = GRX_OPT_UTF) {
  Parsed parsed(pattern, options);
  EXPECT_TRUE(parsed.ok()) << pattern;
  GRX_LintReport report {};
  EXPECT_EQ(grx_pattern_lint(parsed.get(), &report), GRX_OK) << pattern;
  return report;
}

/** Just the finding. */
GRX_Lint finding(const char * pattern, uint32_t options = GRX_OPT_UTF) {
  return lint(pattern, options).finding;
}

} // namespace

TEST(Lint, RejectsNullArguments) {
  GRX_LintReport report {};
  EXPECT_EQ(grx_pattern_lint(nullptr, &report), GRX_ERR_INVALID);

  Parsed parsed("a");
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(grx_pattern_lint(parsed.get(), nullptr), GRX_ERR_INVALID);
}

TEST(Lint, TheWholeListIsInsideTheSubset) {
  // Section 6.4's seven bullets, one pattern each, and nothing else may be
  // added to this test without the specification changing first.
  const char * const inside[] = {
    "abc",                     // individual Unicode characters
    "\xC3\xA9",                // ... including non-ASCII ones
    "[abc]", "[a-z]", "[a-zA-Z0-9]",
    "[^abc]", "[^a-z]",
    "a+", "a*", "a?", "a+?", "a*?", "a??",
    "a{2}", "a{2,5}", "a{2,}", "a{2,5}?", "a{2,}?",
    "^abc", "abc$", "^abc$",
    "(abc)", "a|b", "(a|b)+",
    "^(https?)://([a-z0-9-]+)\\.([a-z]{2,6})$",
  };
  for (const char * pattern : inside) {
    EXPECT_EQ(finding(pattern), GRX_LINT_NONE)
        << pattern << " reported " << grx_lint_string(finding(pattern));
  }
}

TEST(Lint, TheThreeSurprisingOmissions) {
  // Each of these is outside the list as written, and each is a real
  // portability hazard rather than a technicality - except `(?:`, which is
  // reported because the alternative is deciding which bullets were meant
  // loosely.
  EXPECT_EQ(finding("a.c"), GRX_LINT_DOT);
  EXPECT_EQ(finding("\\d+"), GRX_LINT_SHORTHAND);
  EXPECT_EQ(finding("\\w"), GRX_LINT_SHORTHAND);
  EXPECT_EQ(finding("\\s"), GRX_LINT_SHORTHAND);
  EXPECT_EQ(finding("\\D"), GRX_LINT_SHORTHAND);
  EXPECT_EQ(finding("(?:ab)"), GRX_LINT_GROUP);

  // And a shorthand inside a class is the same finding: `[\d]` is no more
  // portable than `\d`.
  EXPECT_EQ(finding("[\\d]"), GRX_LINT_SHORTHAND);
  EXPECT_EQ(finding("[a\\s]"), GRX_LINT_SHORTHAND);
}

TEST(Lint, EveryOtherConstructIsReportedByKind) {
  struct Row {
    const char * pattern;
    GRX_Lint finding;
  };
  const Row rows[] = {
    {"\\p{L}", GRX_LINT_PROPERTY},
    {"[\\p{Lu}]", GRX_LINT_PROPERTY},
    {"a\\b", GRX_LINT_ANCHOR},
    {"a\\B", GRX_LINT_ANCHOR},
    {"(?<n>a)", GRX_LINT_GROUP},
    {"(a)\\1", GRX_LINT_BACKREFERENCE},
    {"(?<n>a)\\k<n>", GRX_LINT_GROUP},
    {"(?=a)b", GRX_LINT_LOOKAROUND},
    {"(?!a)b", GRX_LINT_LOOKAROUND},
    {"(?<=a)b", GRX_LINT_LOOKAROUND},
    {"(?<!a)b", GRX_LINT_LOOKAROUND},
  };
  for (const Row & row : rows) {
    EXPECT_EQ(finding(row.pattern), row.finding)
        << row.pattern << " reported "
        << grx_lint_string(finding(row.pattern));
  }
}

TEST(Lint, UnicodeSetsConstructsAreOutsideIt) {
  const uint32_t v = GRX_OPT_UTF | GRX_OPT_UNICODE_SETS;
  EXPECT_EQ(finding("[[a-z]--[aeiou]]", v), GRX_LINT_CLASS);
  EXPECT_EQ(finding("[\\q{ab}]", v), GRX_LINT_CLASS);
  EXPECT_EQ(finding("\\p{RGI_Emoji}", v), GRX_LINT_PROPERTY);
  // But an ordinary class is still ordinary with `v` on.
  EXPECT_EQ(finding("[a-z]", v), GRX_LINT_NONE);
}

TEST(Lint, TheFlagIsCheckedAndReportedLast) {
  // "Regular expressions SHOULD be built with the `u` flag (or equivalent)".
  EXPECT_EQ(finding("abc", GRX_OPT_NONE), GRX_LINT_FLAGS);
  EXPECT_EQ(finding("abc", GRX_OPT_UTF), GRX_LINT_NONE);
  EXPECT_EQ(finding("abc", GRX_OPT_UTF | GRX_OPT_UNICODE_SETS),
      GRX_LINT_NONE);

  // A construct wins over the flag, because a caller fixing one thing at a
  // time wants the thing with an offset first.
  EXPECT_EQ(finding("a.c", GRX_OPT_NONE), GRX_LINT_DOT);
}

TEST(Lint, TheOffsetIsTheLeftmostFindingNotTheFirstWalked) {
  // A tree walk meets a group before the `.` inside it and an alternation's
  // second branch after its first. The offset a caller underlines has to be
  // the leftmost one in the *text*.
  GRX_LintReport report = lint("(a)\\d");
  EXPECT_EQ(report.finding, GRX_LINT_SHORTHAND);
  EXPECT_EQ(report.offset, 3u);

  report = lint("\\d(?:a)");
  EXPECT_EQ(report.finding, GRX_LINT_SHORTHAND);
  EXPECT_EQ(report.offset, 0u);

  report = lint("(?:a)\\d");
  EXPECT_EQ(report.finding, GRX_LINT_GROUP);
  EXPECT_EQ(report.offset, 0u);

  // Inside a lookaround, whose body begins before the next sibling does.
  report = lint("x(?=\\d)y.");
  EXPECT_EQ(report.finding, GRX_LINT_LOOKAROUND);
  EXPECT_EQ(report.offset, 1u);
}

TEST(Lint, TheReportCarriesTheConstructsSpan) {
  GRX_LintReport report = lint("ab\\d+c");
  EXPECT_EQ(report.finding, GRX_LINT_SHORTHAND);
  EXPECT_EQ(report.offset, 2u);
  EXPECT_EQ(report.length, 2u);

  report = lint("a(?=bc)d");
  EXPECT_EQ(report.finding, GRX_LINT_LOOKAROUND);
  EXPECT_EQ(report.offset, 1u);
  EXPECT_EQ(report.length, 6u);
}

TEST(Lint, EveryFindingHasADistinctName) {
  std::string previous;
  for (int i = 0; i < GRX_LINT_COUNT; i++) {
    const char * name = grx_lint_string((GRX_Lint)i);
    ASSERT_NE(name, nullptr) << i;
    EXPECT_GT(strlen(name), 0u) << i;
    EXPECT_STRNE(name, "unknown") << i;
  }
  EXPECT_STREQ(grx_lint_string(GRX_LINT_COUNT), "unknown");
  EXPECT_STREQ(grx_lint_string((GRX_Lint)9999), "unknown");
}

TEST(Lint, ItIsNotTheSameQuestionAsIsRegular) {
  // `is_regular` says whether the Pike VM can run it in linear time.
  // A pattern can be regular and outside the subset - `\d+` is both - and
  // the two answers are for different readers.
  Parsed parsed("\\d+");
  ASSERT_TRUE(parsed.ok());

  GRX_LintReport report {};
  ASSERT_EQ(grx_pattern_lint(parsed.get(), &report), GRX_OK);
  EXPECT_EQ(report.finding, GRX_LINT_SHORTHAND);

  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile_pattern(parsed.get(), nullptr, nullptr, nullptr,
                &regex),
      GRX_OK);
  GRX_Facts facts;
  ASSERT_EQ(grx_regex_facts(regex, &facts), GRX_OK);
  EXPECT_TRUE(facts.is_regular);
  grx_regex_free(regex);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
