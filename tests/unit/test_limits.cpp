/**
 * @file
 *
 * The defaults, against the patterns that have to keep working.
 *
 * Every field of GRX_Limits is a promise, and a promise is only worth making
 * if somebody has checked what real patterns actually need. WP-14's method
 * is two corpora pulling in opposite directions: patterns that must compile
 * and run at the defaults, and pairs that must be refused by them. The
 * second half is tests/conformance/test_redos.cpp. This is the first.
 *
 * `tools/limits/measure.py` reports the whole distribution - median, p99,
 * maximum and the headroom over each default - and is what the numbers in
 * documentation/dialects.md section 7 came from. What is here is the part
 * that has to keep being true: not "the headroom is 7x" but "nothing in the
 * corpus is refused", which is the claim a future default would break.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "test_helpers.h"

#include "../conformance/rxt.h"

namespace {

struct Row {
  std::string flags;
  std::string pattern;
  std::string source;
};

uint32_t options_for(const std::string & flags) {
  uint32_t options = 0;
  for (char f : flags) {
    switch (f) {
      case 'i': options |= GRX_OPT_CASELESS; break;
      case 'm': options |= GRX_OPT_MULTILINE; break;
      case 's': options |= GRX_OPT_DOTALL; break;
      case 'u': options |= GRX_OPT_UTF; break;
      case 'v': options |= GRX_OPT_UNICODE_SETS | GRX_OPT_UTF; break;
      default: break;
    }
  }
  return options;
}

/** `tools/limits/real_world.txt`: flags, a tab, the pattern. */
std::vector<Row> read_real_world() {
  std::vector<Row> rows;
  std::ifstream file(grxtest::repo("tools/limits/real_world.txt"));
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    size_t tab = line.find('\t');
    if (tab == std::string::npos) {
      continue;
    }
    rows.push_back({line.substr(0, tab), line.substr(tab + 1),
        "tools/limits/real_world.txt"});
  }
  return rows;
}

} // namespace

TEST(Limits, EveryRealWorldPatternCompilesAtTheDefaults) {
  // The patterns that appear in JSON Schemas and configuration files. A
  // default below what one of these needs is a default that would reject
  // working software, which is the failure this corpus exists to prevent.
  std::vector<Row> rows = read_real_world();
  ASSERT_GT(rows.size(), 20u)
      << "the corpus was not found or is too small to be measuring anything";

  GRX_Limits limits;
  grx_limits_default(&limits);

  for (const Row & row : rows) {
    GRX_Error error;
    grx_error_clear(&error);
    GRX_Regex * regex = nullptr;
    GRX_Result result = grx_regex_compile_with_allocator(row.pattern.data(),
        row.pattern.size(), GRX_SYNTAX_ECMASCRIPT, options_for(row.flags),
        &limits, nullptr, &error, &regex);
    EXPECT_EQ(result, GRX_OK)
        << "/" << row.pattern << "/" << row.flags << ": " << error.message;
    grx_regex_free(regex);
  }
}

TEST(Limits, EveryConformanceVectorCompilesAtTheDefaults) {
  // The same claim over the vectors, which are a much larger and much
  // stranger corpus: they came from a generator aimed at the grammar's
  // corners rather than at what anybody would write.
  grxtest::VectorFile named;
  grxtest::VectorFile generated;
  std::string error;
  ASSERT_TRUE(grxtest::read_vector_file(
      grxtest::data("vectors/ecmascript/named.rxt"), &named, &error))
      << error;
  ASSERT_TRUE(grxtest::read_vector_file(
      grxtest::data("vectors/ecmascript/generated.rxt"), &generated, &error))
      << error;

  GRX_Limits limits;
  grx_limits_default(&limits);

  size_t compiled = 0;
  size_t refused_as_syntax = 0;
  for (const grxtest::VectorFile * file : {&named, &generated}) {
    for (const grxtest::Record & record : file->records) {
      GRX_Error compile_error;
      grx_error_clear(&compile_error);
      GRX_Regex * regex = nullptr;
      GRX_Result result = grx_regex_compile_with_allocator(
          record.pattern.data(), record.pattern.size(), record.syntax,
          record.options, &limits, nullptr, &compile_error, &regex);
      grx_regex_free(regex);

      if (result == GRX_OK) {
        compiled++;
        continue;
      }
      // A vector whose expectation is a syntax error is meant not to
      // compile. What must never happen is a *limit*.
      EXPECT_NE(result, GRX_ERR_LIMIT)
          << "/" << record.pattern << "/ hit " << compile_error.message;
      refused_as_syntax++;
    }
  }

  EXPECT_GT(compiled, 100u) << "almost nothing compiled; the corpus is wrong";
  printf("\nlimits: %zu vectors compiled at the defaults, %zu refused as "
         "syntax\n",
      compiled, refused_as_syntax);
}

TEST(Limits, NestingIsBoundedWellInsideTheSmallestStack) {
  // max_nesting_depth is the C-stack bound and the one limit design.md says
  // a caller should not lift casually. Measured rather than assumed: on the
  // 256 KB stack the fuzzers run under, a pattern nested 480 deep parses and
  // one nested 496 deep overflows - about 525 bytes of stack per level. The
  // default of 128 is therefore a factor of about four inside the smallest
  // stack this library claims to work on, and thirty inside the usual one.
  //
  // What is checked here is the half that does not need a `ulimit`: that the
  // default is enforced exactly, one level either side of it.
  GRX_Limits limits;
  grx_limits_default(&limits);
  ASSERT_GT(limits.max_nesting_depth, 0u);

  const std::string at(limits.max_nesting_depth, '(');
  const std::string close(limits.max_nesting_depth, ')');
  GRX_Pattern * parsed = nullptr;
  GRX_Error error;
  grx_error_clear(&error);

  std::string ok = at + "a" + close;
  EXPECT_EQ(grx_pattern_parse_with_allocator(ok.data(), ok.size(),
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &limits, nullptr, &error,
                &parsed),
      GRX_OK)
      << error.message;
  grx_pattern_free(parsed);
  parsed = nullptr;

  std::string over = at + "(a)" + close;
  EXPECT_EQ(grx_pattern_parse_with_allocator(over.data(), over.size(),
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &limits, nullptr, &error,
                &parsed),
      GRX_ERR_LIMIT);
  EXPECT_EQ(error.diag, GRX_DIAG_LIMIT_NESTING_DEPTH);
  grx_pattern_free(parsed);
}

TEST(Limits, TheCostliestRealPatternIsWellInsideTheProgramLimit) {
  // `\p{RGI_Emoji}` is the largest thing ECMAScript can name: nearly four
  // thousand sequences, which lower to an alternation. It is the pattern
  // that decides max_nodes and max_program_size, and the headroom it leaves
  // is the headroom those two have.
  GRX_Limits limits;
  grx_limits_default(&limits);

  GRX_Regex * regex = nullptr;
  GRX_Error error;
  grx_error_clear(&error);
  const char * pattern = "^\\p{RGI_Emoji}+$";
  ASSERT_EQ(grx_regex_compile_with_allocator(pattern, strlen(pattern),
                GRX_SYNTAX_ECMASCRIPT,
                GRX_OPT_UTF | GRX_OPT_UNICODE_SETS, &limits, nullptr, &error,
                &regex),
      GRX_OK)
      << error.message;

  GRX_Facts facts;
  ASSERT_EQ(grx_regex_facts(regex, &facts), GRX_OK);
  EXPECT_LT(facts.program_size, limits.max_program_size);
  EXPECT_GT(facts.program_size, 10000u)
      << "it should be the costliest thing in the corpus; if it is not, the "
         "measurement in dialects.md section 7 is about a different pattern";
  printf("limits: /%s/v is %zu instructions against a cap of %zu\n", pattern,
      facts.program_size, limits.max_program_size);
  grx_regex_free(regex);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

namespace {

/**
 * One field of GRX_Limits, and an input that exceeds a deliberately tight
 * value of it.
 *
 * The offset rather than a pointer-to-member so the table stays a table: the
 * test sets one field of an otherwise default structure, which is how a
 * caller tunes one.
 */
struct Tunable {
  const char * name;
  size_t offset;
  size_t tight;      ///< A value the input below exceeds.
  const char * pattern;
  const char * subject;
  GRX_Engine engine; ///< The one that enforces it, where that matters.
  GRX_Diag diag;     ///< GRX_DIAG_NONE for the match-time limits.
};

#define LIMIT_FIELD(name) offsetof(GRX_Limits, name)

const Tunable kTunables[] = {
  {"max_pattern_length", LIMIT_FIELD(max_pattern_length), 4, "aaaaa", "aaaaa",
      GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_PATTERN_LENGTH},
  {"max_nesting_depth", LIMIT_FIELD(max_nesting_depth), 2, "((((a))))", "a",
      GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_NESTING_DEPTH},
  {"max_nodes", LIMIT_FIELD(max_nodes), 4, "abcdefgh", "abcdefgh",
      GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_NODES},
  {"max_program_size", LIMIT_FIELD(max_program_size), 4, "abcdefgh",
      "abcdefgh", GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_PROGRAM_SIZE},
  {"max_captures", LIMIT_FIELD(max_captures), 1, "(a)(b)", "ab",
      GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_CAPTURES},
  {"max_repeat_count", LIMIT_FIELD(max_repeat_count), 4, "a{5}", "aaaaa",
      GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_REPEAT_COUNT},
  {"max_class_ranges", LIMIT_FIELD(max_class_ranges), 1, "[a-cx-z]", "b",
      GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_CLASS_RANGES},
  {"max_lookbehind_length", LIMIT_FIELD(max_lookbehind_length), 1,
      "(?<=abcd)x", "abcdx", GRX_ENGINE_BACKTRACK,
      GRX_DIAG_LIMIT_LOOKBEHIND_LENGTH},
  {"max_steps", LIMIT_FIELD(max_steps), 1, "^a+$", "aaaaaaaa",
      GRX_ENGINE_AUTO, GRX_DIAG_NONE},
  {"max_backtrack", LIMIT_FIELD(max_backtrack), 1, "^a+$", "aaaaaaaa",
      GRX_ENGINE_BACKTRACK, GRX_DIAG_NONE},
  {"max_match_memory", LIMIT_FIELD(max_match_memory), 1, "^a+$", "aaaaaaaa",
      GRX_ENGINE_AUTO, GRX_DIAG_NONE},
  {"max_subject_length", LIMIT_FIELD(max_subject_length), 1, "^a+$",
      "aaaaaaaa", GRX_ENGINE_AUTO, GRX_DIAG_NONE},
};

/** What one row does at one value of its field. */
struct Attempt {
  GRX_Result result;
  GRX_Diag diag;
  int matched;
};

Attempt run_with(const Tunable & row, size_t value) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  *(size_t *)((char *)&limits + row.offset) = value;

  Attempt attempt = {GRX_OK, GRX_DIAG_NONE, 0};
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  grx_error_clear(&error);
  attempt.result = grx_regex_compile_with_allocator(row.pattern,
      std::strlen(row.pattern), GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &limits,
      nullptr, &error, &regex);
  if (attempt.result != GRX_OK) {
    attempt.diag = error.diag;
    return attempt;
  }

  GRX_Match * match = nullptr;
  grx_match_create(regex, nullptr, &match);
  attempt.result = grx_regex_search(regex, row.subject,
      std::strlen(row.subject), 0, row.engine, &limits, match,
      &attempt.matched);
  grx_match_destroy(match);
  grx_regex_free(regex);
  return attempt;
}

} // namespace

/**
 * Every enforced limit refuses when it is tight, and caps nothing at zero.
 *
 * core.h says "zero means no limit for every field", and that sentence had
 * never been checked. It was also not quite true: `max_lookbehind_length`
 * and `max_recursion_depth` were in the structure and in the documentation
 * while nothing read either of them, so setting one to 1 was as unbounded as
 * setting it to 0. A limit a caller can set and cannot feel is worse than no
 * limit at all, because it is a defence they think they have.
 *
 * Each row also checks that the refusal names its *own* field. A caller who
 * has to raise one has to be told which, and a compile that reported
 * max_nodes when max_program_size was the binding limit would send them to
 * the wrong knob.
 */
TEST(Limits, EveryEnforcedLimitRefusesWhenTightAndCapsNothingAtZero) {
  for (const Tunable & row : kTunables) {
    Attempt tight = run_with(row, row.tight);
    EXPECT_EQ(tight.result, GRX_ERR_LIMIT)
        << row.name << " did not refuse at " << row.tight;
    if (row.diag != GRX_DIAG_NONE) {
      EXPECT_EQ(tight.diag, row.diag)
          << row.name << " reported a different field's diagnostic";
    }

    Attempt lifted = run_with(row, 0);
    EXPECT_EQ(lifted.result, GRX_OK) << row.name << " still capped at zero";
    EXPECT_TRUE(lifted.matched) << row.name << " did not match at zero";
  }
}

/**
 * grx_limits_unlimited() caps nothing, field by field.
 *
 * Compared against the structure rather than against behaviour, because the
 * behaviour is the test above. What this pins is that a field added to
 * GRX_Limits later is also zeroed here - the failure mode being a new limit
 * that "unlimited" quietly still applies.
 */
TEST(Limits, UnlimitedIsEveryFieldZero) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  grx_limits_unlimited(&limits);

  const size_t * fields = (const size_t *)&limits;
  for (size_t i = 0; i < sizeof(GRX_Limits) / sizeof(size_t); i++) {
    EXPECT_EQ(fields[i], (size_t)0) << "field " << i << " of GRX_Limits";
  }

  // And it is a real function, not a memset the caller could have written:
  // NULL is ignored rather than crashing, like every other _default().
  grx_limits_unlimited(nullptr);
}

/**
 * An unbounded lookbehind is GRX_NPOS, not zero.
 *
 * `(?<=a+)x` used to report max_lookbehind == 0 - the same answer as a
 * pattern with no lookbehind at all - because the analysis skipped a body
 * whose length it could not bound. That is exactly backwards: the body it
 * could not bound is the one that may read the whole subject. It also made
 * max_lookbehind_length unenforceable, since the one pattern a caller would
 * want the limit to catch was the one reporting that it needed nothing.
 */
TEST(Limits, AnUnboundedLookbehindIsNotZero) {
  struct Row {
    const char * pattern;
    size_t expected;
  };
  const Row rows[] = {
    {"x", 0},
    {"(?<=abcd)x", 4},
    {"(?<=a|bcd)x", 3},
    {"(?<=a{3,7})x", 7},
    {"(?<!abc)x", 3},
    {"(?<=a+)x", GRX_NPOS},
    {"(?<=a*)x", GRX_NPOS},
    {"(?<=(?:ab)+)x", GRX_NPOS},
  };

  for (const Row & row : rows) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(row.pattern, GRX_SYNTAX_ECMASCRIPT,
                  GRX_OPT_UTF, &regex),
        GRX_OK)
        << row.pattern;
    GRX_Facts facts;
    ASSERT_EQ(grx_regex_facts(regex, &facts), GRX_OK);
    EXPECT_EQ(facts.max_lookbehind, row.expected) << row.pattern;
    grx_regex_free(regex);
  }
}

/**
 * The default does not bound a lookbehind, because ECMAScript does not.
 *
 * Now that the field is enforced, a non-zero default would refuse `(?<=a+)x`
 * - valid ECMAScript, and a pattern the conformance corpus contains. The
 * limit is a caller's policy on top of the dialect; the default is to have
 * no policy.
 */
TEST(Limits, TheDefaultDoesNotBoundAnEcmascriptLookbehind) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  EXPECT_EQ(limits.max_lookbehind_length, (size_t)0);

  GRX_Regex * regex = nullptr;
  EXPECT_EQ(grx_regex_compile_with_allocator("(?<=a+)x", 8,
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &limits, nullptr, nullptr,
                &regex),
      GRX_OK);
  grx_regex_free(regex);

  // A caller who *does* set one catches the unbounded body, which is the
  // whole reason to set it.
  limits.max_lookbehind_length = 255;
  regex = nullptr;
  GRX_Error error;
  grx_error_clear(&error);
  EXPECT_EQ(grx_regex_compile_with_allocator("(?<=a+)x", 8,
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &limits, nullptr, &error,
                &regex),
      GRX_ERR_LIMIT);
  EXPECT_EQ(error.diag, GRX_DIAG_LIMIT_LOOKBEHIND_LENGTH);
  EXPECT_EQ(regex, nullptr);

  // And a bounded one under the cap still compiles, so the limit is a bound
  // rather than a ban on lookbehind.
  regex = nullptr;
  EXPECT_EQ(grx_regex_compile_with_allocator("(?<=abcd)x", 10,
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &limits, nullptr, nullptr,
                &regex),
      GRX_OK);
  grx_regex_free(regex);
}

/**
 * max_recursion_depth is reserved, and this is what says so.
 *
 * It has a default of 256 and nothing reads it, because no dialect this
 * library implements has recursion or a subroutine call - those arrive with
 * Perl and PCRE2 in WP-18. Rather than leave that as a comment, this pins
 * it: if any dialect ever compiles one of these into a program that reports
 * has_recursion, the limit has to be enforced at the same time, and this
 * test fails until it is.
 */
TEST(Limits, RecursionDepthIsReservedUntilADialectHasRecursion) {
  const char * recursive[] = {"(a)(?R)", "(a)(?1)", "(a)\\g<1>",
      "(?<n>a)(?&n)", "(a)(?P>1)"};

  for (int syntax = 0; syntax < GRX_SYNTAX_COUNT; syntax++) {
    for (const char * pattern : recursive) {
      GRX_Regex * regex = nullptr;
      if (grx_regex_compile(pattern, (GRX_Syntax)syntax, 0, &regex)
          != GRX_OK) {
        continue;
      }
      GRX_Facts facts;
      ASSERT_EQ(grx_regex_facts(regex, &facts), GRX_OK);
      EXPECT_FALSE(facts.has_recursion)
          << "dialect " << syntax << " compiles " << pattern
          << " into recursion, so max_recursion_depth must now be enforced "
             "in grx_compile_program() alongside max_lookbehind_length";
      grx_regex_free(regex);
    }
  }
}
