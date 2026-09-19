/**
 * @file
 *
 * The compile entry points.
 *
 * As with the parser, the compiler is a stub: these state the argument
 * contract, and the tests marked STUB record the stub's answer so that "not
 * implemented" is said out loud.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdio>

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

TEST(Compile, OutputIsNullOnFailure) {
  GRX_Regex * regex = (GRX_Regex *)0x1;
  EXPECT_NE(grx_regex_compile("a", GRX_SYNTAX_PCRE, GRX_OPT_NONE, &regex),
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
      GRX_SYNTAX_PCRE, GRX_OPT_NONE, nullptr, nullptr, &error, &regex);

  ASSERT_NE(result, GRX_OK);
  EXPECT_EQ(error.code, result);
  EXPECT_NE(error.message[0], '\0');
}

TEST(Compile, StubReportsUnsupported) {
  // STUB: delete with the compiler.
  GRX_Regex * regex = nullptr;
  EXPECT_EQ(grx_regex_compile("a", GRX_SYNTAX_PCRE, GRX_OPT_NONE, &regex),
      GRX_ERR_UNSUPPORTED);
}

TEST(Compile, AllocatesNothingOnAFailedCompile) {
  grxtest::CountingAllocator allocator;

  GRX_Regex * regex = nullptr;
  (void)grx_regex_compile_with_allocator("a", 1, GRX_SYNTAX_PCRE,
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
