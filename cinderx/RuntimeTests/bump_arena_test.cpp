// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/bump_arena.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

using namespace cinderx;

namespace {

struct DestructionRecord {
  int id;
  std::vector<int>* destroyed;

  explicit DestructionRecord(int id, std::vector<int>* destroyed)
      : id{id}, destroyed{destroyed} {}

  ~DestructionRecord() noexcept {
    destroyed->push_back(id);
  }
};

struct alignas(64) AlignedValue {
  int value;
};

struct alignas(kPageSize * 2) OverPageAlignedValue {
  int value;
};

struct LargeSizeTrait {
  static size_t size() {
    return sizeof(AlignedValue) + 128;
  }
};

struct OversizedTrait {
  static size_t size() {
    return std::numeric_limits<size_t>::max();
  }
};

TEST(BumpArenaTest, AllocateStoresDifferentTypes) {
  std::vector<int> destroyed;
  BumpArena arena;

  auto integer = arena.allocate<int>(10);
  auto record = arena.allocate<DestructionRecord>(20, &destroyed);

  EXPECT_EQ(*integer, 10);
  EXPECT_EQ(record->id, 20);
  EXPECT_NE(static_cast<void*>(integer), static_cast<void*>(record));
}

TEST(BumpArenaTest, AllocateHonorsAlignmentAndSizeTrait) {
  BumpArena arena;

  auto value = arena.allocate<AlignedValue, LargeSizeTrait>();
  value->value = 42;

  EXPECT_EQ(
      reinterpret_cast<uintptr_t>(value) % alignof(AlignedValue), uintptr_t{0});
  EXPECT_EQ(value->value, 42);
}

TEST(BumpArenaTest, AllocateHonorsAlignmentLargerThanPageSize) {
  BumpArena arena;

  auto value = arena.allocate<OverPageAlignedValue>();
  value->value = 42;

  EXPECT_EQ(
      reinterpret_cast<uintptr_t>(value) % alignof(OverPageAlignedValue),
      uintptr_t{0});
  EXPECT_EQ(value->value, 42);
}

TEST(BumpArenaTest, AllocateHonorsStricterAlignmentWithinExistingBlock) {
  BumpArena arena;

  for (size_t i = 0; i < kPageSize * 3 + 1; i++) {
    arena.allocate<char>();
  }

  auto value = arena.allocate<OverPageAlignedValue>();

  EXPECT_EQ(
      reinterpret_cast<uintptr_t>(value) % alignof(OverPageAlignedValue),
      uintptr_t{0});
}

TEST(BumpArenaTest, AllocateRejectsSizeTraitThatOverflowsBlockSize) {
  BumpArena arena;

  EXPECT_DEATH((arena.allocate<int, OversizedTrait>()), "roundUp overflow");
}

TEST(BumpArenaTest, DestroyRunsNonTrivialDestructorsInReverseOrder) {
  std::vector<int> destroyed;
  {
    BumpArena arena;
    arena.allocate<DestructionRecord>(1, &destroyed);
    arena.allocate<DestructionRecord>(2, &destroyed);
    arena.allocate<int>(3);

    EXPECT_TRUE(destroyed.empty());
  }

  const std::vector<int> expected{2, 1};
  EXPECT_EQ(destroyed, expected);
}

} // namespace
