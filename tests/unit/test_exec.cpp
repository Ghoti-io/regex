/**
 * @file
 *
 * Engine selection and the match object.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdio>

#include "test_helpers.h"

TEST(Exec, SearchRejectsNullArguments) {
  int matched = 1;
  EXPECT_EQ(grx_regex_search(nullptr, "abc", 3, 0, GRX_ENGINE_AUTO, nullptr,
                nullptr, &matched),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_match(nullptr, "abc", 3, 0, GRX_ENGINE_AUTO, nullptr,
                nullptr, &matched),
      GRX_ERR_INVALID);
}

TEST(Exec, MatchObjectRejectsNullArguments) {
  GRX_Match * match = nullptr;
  EXPECT_EQ(grx_match_create(nullptr, nullptr, &match), GRX_ERR_INVALID);
  EXPECT_EQ(match, nullptr);
}

TEST(Match, AccessorsTolerateNull) {
  EXPECT_EQ(grx_match_count(nullptr), 0u);
  EXPECT_EQ(grx_match_engine(nullptr), GRX_ENGINE_COUNT);
  EXPECT_EQ(grx_match_dump(nullptr, stderr), GRX_ERR_INVALID);
  grx_match_destroy(nullptr);

  GRX_Capture capture;
  EXPECT_EQ(grx_match_group(nullptr, 0, &capture), GRX_ERR_INVALID);
  EXPECT_EQ(grx_match_group_named(nullptr, "name", &capture), GRX_ERR_INVALID);
}

TEST(Capture, UnsetIsDistinctFromEmpty) {
  // A group that did not participate and a group that matched the empty
  // string are different answers, and a caller has to be able to tell them
  // apart. GRX_NPOS is what says "did not participate"; 0..0 is a real span.
  GRX_Capture unset = {GRX_NPOS, GRX_NPOS};
  GRX_Capture empty = {0, 0};

  EXPECT_NE(unset.start, empty.start);
  EXPECT_EQ(empty.start, empty.end);
}

TEST(Exec, UnknownEngineIsInvalid) {
  // Checked before the regex is dereferenced would be wrong: a NULL regex is
  // invalid whatever the engine. This states the order the other way round -
  // an out-of-range engine with a NULL regex is still GRX_ERR_INVALID.
  int matched = 1;
  EXPECT_EQ(grx_regex_search(nullptr, "abc", 3, 0, (GRX_Engine)9999, nullptr,
                nullptr, &matched),
      GRX_ERR_INVALID);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
