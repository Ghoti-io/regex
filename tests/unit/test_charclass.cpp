/**
 * @file
 *
 * Character-class membership.
 *
 * grx_charclass_add_range() is a stub, so these build the range array by hand
 * and test the search over it. That is the part whose contract is already
 * settled: the ranges are sorted and disjoint, and membership is a binary
 * search over them.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>

#include "test_helpers.h"

#include "../../src/charclass/charclass_internal.h"

namespace {

/** A class over a fixed, caller-owned range array. */
GRX_CharClass make_class(GRX_CharRange * ranges, size_t count, int negated) {
  GRX_CharClass cls {};
  cls.allocator = nullptr; // Nothing to free; the ranges are on the stack.
  cls.ranges = ranges;
  cls.count = count;
  cls.capacity = count;
  cls.negated = negated;
  return cls;
}

} // namespace

TEST(CharClass, ContainsFindsEveryRange) {
  GRX_CharRange ranges[] = {{'0', '9'}, {'A', 'Z'}, {'a', 'z'}};
  GRX_CharClass cls = make_class(ranges, 3, 0);

  EXPECT_TRUE(grx_charclass_contains(&cls, '0'));
  EXPECT_TRUE(grx_charclass_contains(&cls, '5'));
  EXPECT_TRUE(grx_charclass_contains(&cls, '9'));
  EXPECT_TRUE(grx_charclass_contains(&cls, 'A'));
  EXPECT_TRUE(grx_charclass_contains(&cls, 'z'));

  EXPECT_FALSE(grx_charclass_contains(&cls, '/'));
  EXPECT_FALSE(grx_charclass_contains(&cls, ':'));
  EXPECT_FALSE(grx_charclass_contains(&cls, '_'));
  EXPECT_FALSE(grx_charclass_contains(&cls, 0x1F41F));
}

TEST(CharClass, NegationInvertsTheAnswer) {
  GRX_CharRange ranges[] = {{'a', 'z'}};
  GRX_CharClass cls = make_class(ranges, 1, 1);

  EXPECT_FALSE(grx_charclass_contains(&cls, 'a'));
  EXPECT_TRUE(grx_charclass_contains(&cls, 'A'));
}

TEST(CharClass, EmptyClassMatchesNothing) {
  GRX_CharClass cls = make_class(nullptr, 0, 0);
  EXPECT_FALSE(grx_charclass_contains(&cls, 'a'));

  // And a negated empty class matches everything, which is what `[^\x00-\x{10FFFF}]`
  // inverted amounts to.
  GRX_CharClass negated = make_class(nullptr, 0, 1);
  EXPECT_TRUE(grx_charclass_contains(&negated, 'a'));
}

TEST(CharClass, SingleCodePointRangeIsFound) {
  // A one-code-point range is low == high, which is the case a binary search
  // written with the wrong comparison silently misses.
  GRX_CharRange ranges[] = {{'a', 'a'}, {'c', 'c'}, {'e', 'e'}};
  GRX_CharClass cls = make_class(ranges, 3, 0);

  EXPECT_TRUE(grx_charclass_contains(&cls, 'a'));
  EXPECT_FALSE(grx_charclass_contains(&cls, 'b'));
  EXPECT_TRUE(grx_charclass_contains(&cls, 'c'));
  EXPECT_FALSE(grx_charclass_contains(&cls, 'd'));
  EXPECT_TRUE(grx_charclass_contains(&cls, 'e'));
}

TEST(CharClass, ContainsToleratesNull) {
  EXPECT_FALSE(grx_charclass_contains(nullptr, 'a'));
  grx_charclass_clear(nullptr);
}

TEST(CharClass, AddRangeRejectsAnInvertedRange) {
  GRX_CharClass cls {};
  EXPECT_EQ(grx_charclass_add_range(&cls, 'z', 'a', nullptr), GRX_ERR_INVALID);
  EXPECT_EQ(grx_charclass_add_range(nullptr, 'a', 'z', nullptr),
      GRX_ERR_INVALID);
}

TEST(CharClass, AddRangeIsStillAStub) {
  // STUB: delete when the parser needs it.
  GRX_CharClass cls {};
  EXPECT_EQ(grx_charclass_add_range(&cls, 'a', 'z', nullptr),
      GRX_ERR_UNSUPPORTED);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
