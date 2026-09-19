/**
 * @file
 *
 * The diagnostic catalogue, and the one place an error is built.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <set>
#include <string>

#include "test_helpers.h"

#include "../../src/core/core_internal.h"

TEST(Diagnostic, CatalogueIsComplete) {
  // A GRX_Diag constant added without a row in diag.c leaves a zeroed entry,
  // which grx_diag_string() reports as unknown. This is the test that turns
  // that from a message nobody reads into a build that fails.
  for (int i = 0; i < GRX_DIAG_COUNT; i++) {
    const char * text = grx_diag_string((GRX_Diag)i);
    ASSERT_NE(text, nullptr) << "diagnostic " << i;
    EXPECT_STRNE(text, "unknown diagnostic")
        << "diagnostic " << i << " has no row in the catalogue";
    EXPECT_GT(std::strlen(text), 0u) << "diagnostic " << i;
  }
}

TEST(Diagnostic, EveryTextIsDistinct) {
  // Two diagnostics with the same words are two diagnostics a user cannot
  // tell apart, which defeats the reason for having more than one.
  std::set<std::string> seen;
  for (int i = 0; i < GRX_DIAG_COUNT; i++) {
    const char * text = grx_diag_string((GRX_Diag)i);
    EXPECT_TRUE(seen.insert(text).second)
        << "\"" << text << "\" is used by more than one diagnostic";
  }
}

TEST(Diagnostic, OutOfRangeIsNamedRatherThanIndexed) {
  EXPECT_STREQ(grx_diag_string(GRX_DIAG_COUNT), "unknown diagnostic");
  EXPECT_STREQ(grx_diag_string((GRX_Diag)9999), "unknown diagnostic");
  EXPECT_STREQ(grx_diag_string((GRX_Diag)-1), "unknown diagnostic");
}

TEST(Diagnostic, EachImpliesExactlyOneResultCode) {
  // A caller that has chosen a diagnostic has already chosen the code, and
  // grx_diag_result() is what stops the two drifting apart at a call site.
  for (int i = 0; i < GRX_DIAG_COUNT; i++) {
    GRX_Result code = grx_diag_result((GRX_Diag)i);
    EXPECT_GE(code, GRX_OK) << "diagnostic " << i;
    EXPECT_LT(code, GRX_RESULT_COUNT) << "diagnostic " << i;
  }

  EXPECT_EQ(grx_diag_result(GRX_DIAG_NONE), GRX_OK);
  EXPECT_EQ(grx_diag_result(GRX_DIAG_COUNT), GRX_ERR_INTERNAL);
}

TEST(Diagnostic, OnlyTheNoneDiagnosticMeansSuccess) {
  for (int i = 1; i < GRX_DIAG_COUNT; i++) {
    EXPECT_NE(grx_diag_result((GRX_Diag)i), GRX_OK)
        << grx_diag_string((GRX_Diag)i) << " reports success";
  }
}

TEST(Diagnostic, EveryLimitDiagnosticReportsLimit) {
  // documentation/design.md section 6.2: a limit is reported as
  // GRX_ERR_LIMIT with the field named. The naming is the catalogue's job;
  // that the code is GRX_ERR_LIMIT is this one's.
  const GRX_Diag limits[] = {
    GRX_DIAG_LIMIT_PATTERN_LENGTH,
    GRX_DIAG_LIMIT_NESTING_DEPTH,
    GRX_DIAG_LIMIT_NODES,
    GRX_DIAG_LIMIT_PROGRAM_SIZE,
    GRX_DIAG_LIMIT_CAPTURES,
    GRX_DIAG_LIMIT_REPEAT_COUNT,
    GRX_DIAG_LIMIT_CLASS_RANGES,
    GRX_DIAG_LIMIT_LOOKBEHIND_LENGTH,
    GRX_DIAG_LIMIT_RECURSION_DEPTH,
    GRX_DIAG_LIMIT_SUBJECT_LENGTH,
    GRX_DIAG_LIMIT_STEPS,
    GRX_DIAG_LIMIT_BACKTRACK,
    GRX_DIAG_LIMIT_MATCH_MEMORY,
  };

  for (GRX_Diag diag : limits) {
    EXPECT_EQ(grx_diag_result(diag), GRX_ERR_LIMIT)
        << grx_diag_string(diag);
    // The message has to name the field, or a caller cannot tell which limit
    // to raise.
    EXPECT_NE(std::string(grx_diag_string(diag)).find("max_"),
        std::string::npos)
        << "\"" << grx_diag_string(diag) << "\" does not name its field";
  }
}

TEST(ErrorSet, FillsEveryField) {
  GRX_Error error;
  grx_error_clear(&error);

  GRX_Result returned = grx_error_set(
      &error, GRX_ERR_SYNTAX, GRX_DIAG_UNMATCHED_OPEN_PAREN, 4, 1);

  EXPECT_EQ(returned, GRX_ERR_SYNTAX);
  EXPECT_EQ(error.code, GRX_ERR_SYNTAX);
  EXPECT_EQ(error.diag, GRX_DIAG_UNMATCHED_OPEN_PAREN);
  EXPECT_EQ(error.offset, 4u);
  EXPECT_EQ(error.length, 1u);
  EXPECT_EQ(std::string(error.message),
      std::string(grx_diag_string(GRX_DIAG_UNMATCHED_OPEN_PAREN))
          + " at offset 4");
}

TEST(ErrorSet, OmitsThePositionWhenThereIsNone) {
  // A match-time limit has no offset into the pattern, and a message that
  // said "at offset 18446744073709551615" would be worse than one that says
  // nothing about where.
  GRX_Error error;
  grx_error_set(&error, GRX_ERR_LIMIT, GRX_DIAG_LIMIT_STEPS, GRX_NPOS, 0);

  EXPECT_EQ(error.offset, GRX_NPOS);
  EXPECT_STREQ(error.message, grx_diag_string(GRX_DIAG_LIMIT_STEPS));
  EXPECT_EQ(std::string(error.message).find("offset"), std::string::npos);
}

TEST(ErrorSet, ReturnsItsCodeSoACallerCanReturnThrough) {
  GRX_Error error;
  EXPECT_EQ(grx_error_set(&error, GRX_ERR_OOM, GRX_DIAG_OUT_OF_MEMORY,
                GRX_NPOS, 0),
      GRX_ERR_OOM);
}

TEST(ErrorSet, TakesNullAndStillReturnsTheCode) {
  // The public API's out_error is optional, and this is what makes that cost
  // nothing at each call site.
  EXPECT_EQ(grx_error_set(nullptr, GRX_ERR_SYNTAX,
                GRX_DIAG_TRAILING_BACKSLASH, 0, 1),
      GRX_ERR_SYNTAX);
}

TEST(ErrorSet, EveryMessageFitsTheBufferAndIsTerminated) {
  // The buffer is fixed and the catalogue is not: a diagnostic reworded more
  // helpfully later still has to fit. No entry is anywhere near the limit
  // today - the longest is well under half of it - so this does not test a
  // truncation that can currently happen; it tests that one still could not
  // overrun if it did, and it sweeps the whole catalogue so that the entry
  // which first gets long is the one that fails.
  //
  // The largest representable offset is the worst case, since it contributes
  // twenty digits on top of the text.
  for (int i = 0; i < GRX_DIAG_COUNT; i++) {
    GRX_Error error;
    std::memset(error.message, 'x', sizeof(error.message));

    grx_error_set(&error, grx_diag_result((GRX_Diag)i), (GRX_Diag)i,
        (size_t)-2, 1);

    EXPECT_LT(std::strlen(error.message), sizeof(error.message))
        << "diagnostic " << i << " (" << grx_diag_string((GRX_Diag)i)
        << ") did not terminate inside the buffer";
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
