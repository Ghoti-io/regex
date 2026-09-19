/**
 * @file
 *
 * The growable array behind every table in the library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>

#include "test_helpers.h"

#include "../../src/core/arena_internal.h"

namespace {

/** A struct element, since that is what the arena exists to hold. */
struct Pair {
  uint32_t a;
  uint32_t b;
};

} // namespace

TEST(Arena, StartsEmptyAndAllocatesNothing) {
  grxtest::CountingAllocator allocator;
  GRX_Arena arena;
  grx_arena_init(&arena, allocator.get(), sizeof(Pair), 0, GRX_DIAG_NONE);

  EXPECT_EQ(arena.count, 0u);
  EXPECT_EQ(arena.capacity, 0u);
  EXPECT_EQ(allocator.total(), 0);

  grx_arena_clear(&arena);
}

TEST(Arena, AppendReturnsSequentialIndicesFromZero) {
  GRX_Arena arena;
  grx_arena_init(&arena, nullptr, sizeof(Pair), 0, GRX_DIAG_NONE);

  for (uint32_t i = 0; i < 5; i++) {
    Pair pair = {i, i * 2};
    uint32_t index = GRX_INDEX_NONE;
    ASSERT_EQ(grx_arena_append(&arena, &pair, &index), GRX_OK);
    EXPECT_EQ(index, i);
  }
  EXPECT_EQ(arena.count, 5u);

  grx_arena_clear(&arena);
}

TEST(Arena, GrowthPreservesEveryEarlierElement) {
  // The one property a doubling reallocation can break, and the reason every
  // link in a tree is an index rather than a pointer.
  GRX_Arena arena;
  grx_arena_init(&arena, nullptr, sizeof(Pair), 0, GRX_DIAG_NONE);

  const uint32_t count = 1000;
  for (uint32_t i = 0; i < count; i++) {
    Pair pair = {i, i * 3};
    ASSERT_EQ(grx_arena_append(&arena, &pair, nullptr), GRX_OK);
  }

  for (uint32_t i = 0; i < count; i++) {
    const Pair * pair = GRX_ARENA_AT(const Pair, &arena, i);
    ASSERT_NE(pair, nullptr) << "index " << i;
    EXPECT_EQ(pair->a, i);
    EXPECT_EQ(pair->b, i * 3);
  }

  grx_arena_clear(&arena);
}

TEST(Arena, AppendingNullZeroesTheElement) {
  // How a node is reserved before its fields are known: the slot must be
  // zero rather than whatever the allocator last had there.
  GRX_Arena arena;
  grx_arena_init(&arena, nullptr, sizeof(Pair), 0, GRX_DIAG_NONE);

  uint32_t index = GRX_INDEX_NONE;
  ASSERT_EQ(grx_arena_append(&arena, nullptr, &index), GRX_OK);

  const Pair * pair = GRX_ARENA_AT(const Pair, &arena, index);
  ASSERT_NE(pair, nullptr);
  EXPECT_EQ(pair->a, 0u);
  EXPECT_EQ(pair->b, 0u);

  grx_arena_clear(&arena);
}

TEST(Arena, ReserveMakesRoomWithoutAddingElements) {
  GRX_Arena arena;
  grx_arena_init(&arena, nullptr, sizeof(Pair), 0, GRX_DIAG_NONE);

  ASSERT_EQ(grx_arena_reserve(&arena, 64), GRX_OK);
  EXPECT_GE(arena.capacity, 64u);
  EXPECT_EQ(arena.count, 0u);
  EXPECT_EQ(grx_arena_at(&arena, 0), nullptr);

  grx_arena_clear(&arena);
}

TEST(Arena, LimitRefusesTheAppendThatWouldExceedIt) {
  // A limit is a promise: exceeding it is GRX_ERR_LIMIT, never a truncated
  // result and never a partial write.
  GRX_Arena arena;
  grx_arena_init(&arena, nullptr, sizeof(Pair), 3, GRX_DIAG_LIMIT_NODES);

  Pair pair = {1, 2};
  for (int i = 0; i < 3; i++) {
    ASSERT_EQ(grx_arena_append(&arena, &pair, nullptr), GRX_OK);
  }

  EXPECT_EQ(grx_arena_append(&arena, &pair, nullptr), GRX_ERR_LIMIT);
  EXPECT_EQ(arena.count, 3u) << "a refused append changed the count";
  EXPECT_EQ(arena.diag, GRX_DIAG_LIMIT_NODES)
      << "the arena must carry the diagnostic its limit reports";

  grx_arena_clear(&arena);
}

TEST(Arena, ReserveBeyondTheLimitIsRefused) {
  GRX_Arena arena;
  grx_arena_init(&arena, nullptr, sizeof(Pair), 8, GRX_DIAG_LIMIT_NODES);

  EXPECT_EQ(grx_arena_reserve(&arena, 9), GRX_ERR_LIMIT);
  EXPECT_EQ(arena.capacity, 0u);
  EXPECT_EQ(grx_arena_reserve(&arena, 8), GRX_OK);

  grx_arena_clear(&arena);
}

TEST(Arena, ALimitOfZeroMeansNoLimit) {
  GRX_Arena arena;
  grx_arena_init(&arena, nullptr, sizeof(Pair), 0, GRX_DIAG_NONE);

  Pair pair = {0, 0};
  for (int i = 0; i < 500; i++) {
    ASSERT_EQ(grx_arena_append(&arena, &pair, nullptr), GRX_OK);
  }
  EXPECT_EQ(arena.count, 500u);

  grx_arena_clear(&arena);
}

TEST(Arena, AtRejectsAnIndexAtOrPastTheCount) {
  GRX_Arena arena;
  grx_arena_init(&arena, nullptr, sizeof(Pair), 0, GRX_DIAG_NONE);

  Pair pair = {9, 9};
  ASSERT_EQ(grx_arena_append(&arena, &pair, nullptr), GRX_OK);

  EXPECT_NE(grx_arena_at(&arena, 0), nullptr);
  EXPECT_EQ(grx_arena_at(&arena, 1), nullptr);
  EXPECT_EQ(grx_arena_at(&arena, GRX_INDEX_NONE), nullptr);
  EXPECT_EQ(grx_arena_at(nullptr, 0), nullptr);

  grx_arena_clear(&arena);
}

TEST(Arena, ClearReleasesStorageAndLeavesTheArenaUsable) {
  grxtest::CountingAllocator allocator;
  GRX_Arena arena;
  grx_arena_init(&arena, allocator.get(), sizeof(Pair), 4, GRX_DIAG_NONE);

  Pair pair = {1, 1};
  ASSERT_EQ(grx_arena_append(&arena, &pair, nullptr), GRX_OK);
  ASSERT_GT(allocator.total(), 0);

  grx_arena_clear(&arena);
  EXPECT_EQ(allocator.live(), 0);
  EXPECT_EQ(arena.count, 0u);
  EXPECT_EQ(arena.capacity, 0u);

  // The configuration survives, so the arena may be filled again.
  EXPECT_EQ(arena.element_size, sizeof(Pair));
  EXPECT_EQ(arena.limit, 4u);
  EXPECT_EQ(grx_arena_append(&arena, &pair, nullptr), GRX_OK);

  grx_arena_clear(&arena);
  EXPECT_EQ(allocator.live(), 0);
}

TEST(Arena, EveryByteComesFromTheAllocatorItWasGiven) {
  grxtest::CountingAllocator allocator;
  GRX_Arena arena;
  grx_arena_init(&arena, allocator.get(), sizeof(Pair), 0, GRX_DIAG_NONE);

  Pair pair = {1, 2};
  for (int i = 0; i < 100; i++) {
    ASSERT_EQ(grx_arena_append(&arena, &pair, nullptr), GRX_OK);
  }

  grx_arena_clear(&arena);
  EXPECT_EQ(allocator.live(), 0)
      << "the arena allocated from somewhere it was not told to";
}

TEST(Arena, AReserveTooLargeToRepresentIsRefusedRatherThanWrapped) {
  // Doubling from 8 overflows before it reaches SIZE_MAX, and the byte count
  // overflows after that. Either way the answer has to be a refusal: a
  // wrapped capacity would allocate a small buffer that the arena then
  // believes is a huge one, which is a heap overflow waiting for the next
  // append.
  GRX_Arena arena;
  grx_arena_init(&arena, nullptr, sizeof(Pair), 0, GRX_DIAG_NONE);

  EXPECT_EQ(grx_arena_reserve(&arena, (size_t)-1), GRX_ERR_OOM);
  EXPECT_EQ(arena.capacity, 0u);
  EXPECT_EQ(arena.count, 0u);

  grx_arena_clear(&arena);
}

TEST(Arena, RejectsANullArenaAndAZeroElementSize) {
  GRX_Arena arena;
  grx_arena_init(&arena, nullptr, 0, 0, GRX_DIAG_NONE);

  Pair pair = {0, 0};
  EXPECT_EQ(grx_arena_append(nullptr, &pair, nullptr), GRX_ERR_INVALID);
  EXPECT_EQ(grx_arena_reserve(nullptr, 1), GRX_ERR_INVALID);
  EXPECT_EQ(grx_arena_append(&arena, &pair, nullptr), GRX_ERR_INVALID);
  EXPECT_EQ(grx_arena_reserve(&arena, 1), GRX_ERR_INVALID);

  grx_arena_init(nullptr, nullptr, 1, 0, GRX_DIAG_NONE);
  grx_arena_clear(nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
