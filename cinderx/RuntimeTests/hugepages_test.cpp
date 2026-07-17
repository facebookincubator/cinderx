// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Common/hugepages.h"

#include <cstring>

using namespace cinderx;

// The arena hands out distinct, correctly-aligned, usable memory across chunk
// boundaries.
TEST(HugePageArenaTest, Allocate) {
  HugePageArena arena;

  void* a = arena.allocate(64, 16);
  void* b = arena.allocate(64, 16);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_NE(a, b);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(a) % 16, 0u);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(b) % 16, 0u);

  // A request larger than a single huge page still succeeds and is writable.
  const size_t big = HugePageArena::kHugePageSize + 4096;
  auto* c = static_cast<char*>(arena.allocate(big, 16));
  ASSERT_NE(c, nullptr);
  memset(c, 0x5a, big);
  EXPECT_EQ(c[0], 0x5a);
  EXPECT_EQ(c[big - 1], 0x5a);
}

// collapseAfterFork touches and re-collapses live chunks without crashing, even
// when the kernel rejects MADV_COLLAPSE.
TEST(HugePageArenaTest, CollapseAfterFork) {
  HugePageArena arena;
  auto* p = static_cast<char*>(arena.allocate(4096, 16));
  ASSERT_NE(p, nullptr);
  EXPECT_NO_FATAL_FAILURE(arena.afterForkChild());
  // Memory remains usable afterwards.
  p[0] = 0x11;
  EXPECT_EQ(p[0], 0x11);
}
