/**
 * @file
 *
 * The parse entry points.
 *
 * The parser itself is a stub, so these state the argument contract - which
 * is settled - and record the stub's answer where the behaviour is not. The
 * tests marked STUB are the ones to delete when the parser lands; they exist
 * so that "not implemented" is a statement the suite makes out loud rather
 * than a silence.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdio>
#include <cstring>

#include "test_helpers.h"

TEST(Parse, NullArgumentsAreInvalid) {
  GRX_Pattern * pattern = nullptr;
  EXPECT_EQ(grx_pattern_parse(nullptr, GRX_SYNTAX_PCRE, GRX_OPT_NONE,
                &pattern),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_pattern_parse("a", GRX_SYNTAX_PCRE, GRX_OPT_NONE, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_pattern_parse_with_allocator(nullptr, 4, GRX_SYNTAX_PCRE,
                GRX_OPT_NONE, nullptr, nullptr, nullptr, &pattern),
      GRX_ERR_INVALID);
  EXPECT_EQ(pattern, nullptr);
}

TEST(Parse, UnknownDialectIsInvalid) {
  GRX_Pattern * pattern = nullptr;
  EXPECT_EQ(grx_pattern_parse("a", GRX_SYNTAX_COUNT, GRX_OPT_NONE, &pattern),
      GRX_ERR_INVALID);
  EXPECT_EQ(pattern, nullptr);
}

TEST(Parse, OutputIsNullOnFailure) {
  // Nothing is allocated for the caller to free on a failing call
  // (CONVENTIONS.md section 5), so the output pointer has to be cleared even
  // when it arrived holding something.
  GRX_Pattern * pattern = (GRX_Pattern *)0x1;
  EXPECT_NE(grx_pattern_parse("a", GRX_SYNTAX_PCRE, GRX_OPT_NONE, &pattern),
      GRX_OK);
  EXPECT_EQ(pattern, nullptr);
}

TEST(Parse, PatternLongerThanTheLimitIsRejected) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_pattern_length = 4;

  GRX_Error error;
  GRX_Pattern * pattern = nullptr;
  EXPECT_EQ(grx_pattern_parse_with_allocator("aaaaaaaa", 8, GRX_SYNTAX_PCRE,
                GRX_OPT_NONE, &limits, nullptr, &error, &pattern),
      GRX_ERR_LIMIT);
  EXPECT_EQ(error.code, GRX_ERR_LIMIT);
  EXPECT_NE(error.message[0], '\0');
  EXPECT_EQ(pattern, nullptr);
}

TEST(Parse, ErrorIsClearedBeforeTheAttempt) {
  GRX_Error error;
  std::memset(&error, 0xAA, sizeof(error));

  GRX_Pattern * pattern = nullptr;
  (void)grx_pattern_parse_with_allocator("a", 1, GRX_SYNTAX_PCRE,
      GRX_OPT_NONE, nullptr, nullptr, &error, &pattern);

  // Whatever the outcome, the caller never reads a position left over from a
  // previous call.
  EXPECT_LT(std::strlen(error.message), sizeof(error.message));
}

TEST(Parse, StubReportsUnsupportedRatherThanParsingHalfOfIt) {
  // STUB: delete with the parser. A partial parser accepts patterns it should
  // reject, and a suite written against it records the half.
  GRX_Pattern * pattern = nullptr;
  EXPECT_EQ(grx_pattern_parse("a", GRX_SYNTAX_PCRE, GRX_OPT_NONE, &pattern),
      GRX_ERR_UNSUPPORTED);
  EXPECT_EQ(pattern, nullptr);
}

TEST(Pattern, AccessorsTolerateNull) {
  EXPECT_EQ(grx_pattern_syntax(nullptr), GRX_SYNTAX_COUNT);
  EXPECT_EQ(grx_pattern_capture_count(nullptr), 0u);
  EXPECT_EQ(grx_pattern_node_count(nullptr), 0u);
  grx_pattern_free(nullptr);
}

TEST(Pattern, DumpRejectsNull) {
  EXPECT_EQ(grx_pattern_dump(nullptr, stderr), GRX_ERR_INVALID);
}

TEST(Parse, AllocatesNothingOnAFailedParse) {
  grxtest::CountingAllocator allocator;

  GRX_Pattern * pattern = nullptr;
  (void)grx_pattern_parse_with_allocator("a", 1, GRX_SYNTAX_PCRE,
      GRX_OPT_NONE, nullptr, allocator.get(), nullptr, &pattern);

  EXPECT_EQ(allocator.live(), 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
