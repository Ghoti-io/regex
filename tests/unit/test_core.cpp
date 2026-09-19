/**
 * @file
 *
 * Result codes, the error structure, the default limits, and the allocator.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>

#include "test_helpers.h"

TEST(Result, EveryCodeHasItsOwnString) {
  // The string table is complete and has no duplicates, so a new result code
  // cannot be added without a description - which is what RESULT_COUNT
  // closing the enum is for (CONVENTIONS.md section 5).
  for (int i = 0; i < GRX_RESULT_COUNT; i++) {
    const char * text = grx_result_string((GRX_Result)i);
    ASSERT_NE(text, nullptr);
    EXPECT_NE(std::strlen(text), 0u);
    EXPECT_STRNE(text, "Unknown error") << "result code " << i;

    for (int j = i + 1; j < GRX_RESULT_COUNT; j++) {
      EXPECT_STRNE(text, grx_result_string((GRX_Result)j))
          << "codes " << i << " and " << j << " share a description";
    }
  }
}

TEST(Result, OutOfRangeCodeIsUnknown) {
  EXPECT_STREQ(grx_result_string(GRX_RESULT_COUNT), "Unknown error");
  EXPECT_STREQ(grx_result_string((GRX_Result)9999), "Unknown error");
}

TEST(Result, SuccessIsZero) {
  // Callers write `if (result)`. That has to mean failure.
  EXPECT_EQ(GRX_OK, 0);
}

TEST(Error, ClearLeavesNoPosition) {
  GRX_Error error;
  std::memset(&error, 0xAA, sizeof(error));

  grx_error_clear(&error);

  EXPECT_EQ(error.code, GRX_OK);
  EXPECT_EQ(error.offset, GRX_NPOS);
  EXPECT_EQ(error.message[0], '\0');
}

TEST(Error, ClearTakesNull) {
  grx_error_clear(nullptr);
}

TEST(Limits, DefaultsBoundEveryRunawayQuantity) {
  GRX_Limits limits;
  std::memset(&limits, 0, sizeof(limits));
  grx_limits_default(&limits);

  // A regular expression is the one input where a small pattern can cost
  // unbounded time, so the defaults may not leave the engine uncapped. Each
  // of these is a promise the fuzzer drives.
  EXPECT_GT(limits.max_pattern_length, 0u);
  EXPECT_GT(limits.max_nesting_depth, 0u);
  EXPECT_GT(limits.max_nodes, 0u);
  EXPECT_GT(limits.max_program_size, 0u);
  EXPECT_GT(limits.max_captures, 0u);
  EXPECT_GT(limits.max_repeat_count, 0u);
  EXPECT_GT(limits.max_class_ranges, 0u);
  EXPECT_GT(limits.max_steps, 0u);
  EXPECT_GT(limits.max_backtrack, 0u);
}

TEST(Limits, DefaultTakesNull) {
  grx_limits_default(nullptr);
}

TEST(Allocator, DefaultIsUsable) {
  const GRX_Allocator * allocator = grx_allocator_default();
  ASSERT_NE(allocator, nullptr);
  EXPECT_NE(allocator->malloc_fn, nullptr);
  EXPECT_NE(allocator->calloc_fn, nullptr);
  EXPECT_NE(allocator->realloc_fn, nullptr);
  EXPECT_NE(allocator->free_fn, nullptr);
}

TEST(Allocator, DefaultIsOneProcessWideInstance) {
  EXPECT_EQ(grx_allocator_default(), grx_allocator_default());
}

TEST(Version, ReportsWhatItWasBuiltAs) {
  // The linked library's version, not the caller's macros. They agree here
  // because the test links this build, and that is the point: if they ever
  // disagree, the generated header and the Makefile have drifted apart.
  ASSERT_NE(grx_version_string(), nullptr);
  EXPECT_STREQ(grx_version_string(), GRX_VERSION_STRING);
  EXPECT_EQ(grx_version_number(), GRX_VERSION_NUMBER);
}

TEST(Version, PacksOneBytePerComponent) {
  EXPECT_EQ(GRX_MAKE_VERSION(1, 2, 3), 0x010203u);
  EXPECT_LT(GRX_MAKE_VERSION(1, 2, 3), GRX_MAKE_VERSION(1, 3, 0));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
