/**
 * @file
 *
 * The compile entry points.
 *
 * The argument contract, which is the part that does not change as the
 * pipeline behind it fills in. What a compile *produces* is
 * tests/unit/test_lower.cpp; what it matches is checked against Node by
 * tools/oracle/match_diff.py.
 *
 * The dialect used here is deliberately one that is not built. Every call
 * below is about arguments rather than about patterns, and using a dialect
 * with a front end would make the tests depend on that front end's rules.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <chrono>
#include <cstdio>
#include <string>

#include "test_helpers.h"

TEST(Compile, NullArgumentsAreInvalid) {
  GRX_Regex * regex = nullptr;
  EXPECT_EQ(
      grx_regex_compile(nullptr, GRX_SYNTAX_PCRE, GRX_OPT_NONE, &regex),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_compile("a", GRX_SYNTAX_PCRE, GRX_OPT_NONE, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_compile_with_allocator(nullptr, 4, GRX_SYNTAX_PCRE,
                GRX_OPT_NONE, nullptr, nullptr, nullptr, &regex),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_compile_pattern(nullptr, nullptr, nullptr, nullptr,
                &regex),
      GRX_ERR_INVALID);
  EXPECT_EQ(regex, nullptr);
}

// A dialect this library names and has not built. Named once so that
// building the next one changes this line rather than four call sites: these
// said POSIX until WP-23 built it and Python until WP-30 did.
static const GRX_Syntax kUnbuiltDialect = GRX_SYNTAX_JAVA;

TEST(Compile, OutputIsNullOnFailure) {
  GRX_Regex * regex = (GRX_Regex *)0x1;
  EXPECT_NE(grx_regex_compile("a", kUnbuiltDialect, GRX_OPT_NONE, &regex),
      GRX_OK);
  EXPECT_EQ(regex, nullptr);
}

TEST(Compile, ReportsThePositionOfTheFailure) {
  // Every failing compile fills in the error structure when one is supplied.
  // A caller showing a user where their pattern went wrong needs that to be
  // unconditional, not a property of some error paths.
  GRX_Error error;
  GRX_Regex * regex = nullptr;
  GRX_Result result = grx_regex_compile_with_allocator("a", 1,
      kUnbuiltDialect, GRX_OPT_NONE, nullptr, nullptr, &error, &regex);

  ASSERT_NE(result, GRX_OK);
  EXPECT_EQ(error.code, result);
  EXPECT_NE(error.message[0], '\0');
}

TEST(Compile, ADialectThatIsNotBuiltSaysSo) {
  // design.md section 4: a dialect that accepts everything is a bug. A
  // pattern of an unbuilt dialect is refused rather than read with somebody
  // else's rules and pronounced valid. `a` is a valid pattern in every
  // dialect here, so the only thing that can refuse it is the absence of a
  // front end. This named POSIX until WP-23 built it and Python until WP-30
  // did, which is the shape of staleness the test below guards.
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  EXPECT_EQ(grx_regex_compile_with_allocator("a", 1, kUnbuiltDialect,
                GRX_OPT_NONE, nullptr, nullptr, &error, &regex),
      GRX_ERR_UNSUPPORTED);
  EXPECT_EQ(error.diag, GRX_DIAG_DIALECT_NOT_IMPLEMENTED);
  EXPECT_EQ(regex, nullptr);
}

TEST(Compile, ThePerlFamilyIsBuilt) {
  // The other half of the rule above, and the one that goes stale silently:
  // a dialect this library *does* read has to be reachable through the same
  // call, or "not implemented" is being reported for a front end that exists.
  for (GRX_Syntax syntax : {GRX_SYNTAX_PCRE, GRX_SYNTAX_PERL}) {
    GRX_Regex * regex = nullptr;
    GRX_Error error;
    EXPECT_EQ(grx_regex_compile_with_allocator("a(?i:b)c", 8, syntax,
                  GRX_OPT_NONE, nullptr, nullptr, &error, &regex),
        GRX_OK)
        << grx_syntax_name(syntax) << ": " << error.message;
    grx_regex_free(regex);
  }
}

TEST(Compile, TheCommonPathProducesARunnableProgram) {
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile("a(b|c)*d", GRX_SYNTAX_ECMASCRIPT, GRX_OPT_NONE,
                &regex),
      GRX_OK);
  ASSERT_NE(regex, nullptr);
  EXPECT_EQ(grx_regex_syntax(regex), GRX_SYNTAX_ECMASCRIPT);
  EXPECT_EQ(grx_regex_capture_count(regex), 1u);
  EXPECT_GT(grx_regex_program_size(regex), 0u);
  grx_regex_free(regex);
}

TEST(Compile, AnAlreadyParsedPatternCanBeCompiledMoreThanOnce) {
  // The pattern is read, not consumed: a caller linting a pattern and then
  // compiling it should not have to parse it twice.
  GRX_Pattern * parsed = nullptr;
  ASSERT_EQ(grx_pattern_parse("[a-z]+", GRX_SYNTAX_ECMASCRIPT, GRX_OPT_NONE,
                &parsed),
      GRX_OK);

  GRX_Regex * first = nullptr;
  GRX_Regex * second = nullptr;
  ASSERT_EQ(
      grx_regex_compile_pattern(parsed, nullptr, nullptr, nullptr, &first),
      GRX_OK);
  ASSERT_EQ(
      grx_regex_compile_pattern(parsed, nullptr, nullptr, nullptr, &second),
      GRX_OK);
  EXPECT_EQ(grx_regex_program_size(first), grx_regex_program_size(second));

  grx_regex_free(first);
  grx_regex_free(second);
  grx_pattern_free(parsed);
}

TEST(Compile, AGroupsLengthIsMeasuredOncePerGroupNotOncePerReference) {
  // `group_span()` in src/ir/analyze.c answers "how long can what this group
  // captured be", and `walk()` asks it for every reference. It had a cycle
  // guard and no memo, so a group referenced twice was measured twice - and
  // when the body doing the referencing was itself a group being measured, the
  // work doubled a level.
  //
  // The shape below is the cheapest witness: group 1 is a literal, and every
  // group after it is two references to the one before. Measured on the build
  // that had no memo, 24 groups took 6.68 seconds and 22 took 2.43 - a factor
  // of 2.1 a level - so the 40 groups here would have taken about nine hours.
  // A 1,514-byte pattern from the fuzzer took 354 seconds and was then
  // rejected as malformed, which is notes section 14i.
  //
  // A wall clock in a test is usually a bad idea and here it is the only
  // honest instrument: the defect is time, not allocation or output, and the
  // two implementations are nine hours apart. Five seconds is therefore not a
  // tolerance to argue about - it is thirty times what a memoised compile of
  // this pattern needs on a slow machine and a millionth of what the
  // unmemoised one needs.
  std::string pattern = "(a)";
  for (int group = 1; group < 40; group++) {
    pattern += "(\\" + std::to_string(group) + "\\" + std::to_string(group) + ")";
  }

  GRX_Regex * regex = nullptr;
  auto start = std::chrono::steady_clock::now();
  GRX_Result result = grx_regex_compile(
      pattern.c_str(), GRX_SYNTAX_ECMASCRIPT, GRX_OPT_NONE, &regex);
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(result, GRX_OK);
  EXPECT_NE(regex, nullptr);
  grx_regex_free(regex);

  auto seconds
      = std::chrono::duration_cast<std::chrono::duration<double>>(elapsed)
            .count();
  EXPECT_LT(seconds, 5.0) << pattern.size() << "-byte pattern with 40 groups "
                             "took " << seconds << " s to compile";
}

TEST(Compile, AllocatesNothingOnAFailedCompile) {
  grxtest::CountingAllocator allocator;

  GRX_Regex * regex = nullptr;
  (void)grx_regex_compile_with_allocator("a", 1, kUnbuiltDialect,
      GRX_OPT_NONE, nullptr, allocator.get(), nullptr, &regex);

  EXPECT_EQ(allocator.live(), 0);
}

TEST(Regex, AccessorsTolerateNull) {
  EXPECT_EQ(grx_regex_capture_count(nullptr), 0u);
  EXPECT_EQ(grx_regex_capture_name(nullptr, 1), nullptr);
  EXPECT_EQ(grx_regex_syntax(nullptr), GRX_SYNTAX_COUNT);
  EXPECT_EQ(grx_regex_program_size(nullptr), 0u);
  EXPECT_EQ(grx_regex_dump(nullptr, stderr), GRX_ERR_INVALID);
  grx_regex_free(nullptr);

  size_t index = 0;
  EXPECT_EQ(grx_regex_capture_index(nullptr, "name", &index), GRX_ERR_INVALID);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
