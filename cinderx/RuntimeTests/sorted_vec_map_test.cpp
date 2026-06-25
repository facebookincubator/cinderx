// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Common/sorted_vec_map.h"
#include "cinderx/Common/util.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace cinderx;

using IntMap = SortedVecMap<int, std::string>;

} // namespace

TEST(SortedVecMapTest, EmptyMapHasNoElements) {
  IntMap map;
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(map.size(), 0);
  EXPECT_EQ(map.find(0), map.end());
}

TEST(SortedVecMapTest, EmplaceThenFind) {
  IntMap map;
  map.emplace(2, "two");
  map.emplace(1, "one");

  EXPECT_FALSE(map.empty());
  EXPECT_EQ(map.size(), 2);
  EXPECT_EQ(map.find(1)->second, "one");
  EXPECT_EQ(map.find(2)->second, "two");
  EXPECT_EQ(map.find(3), map.end());
}

TEST(SortedVecMapTest, IteratesInAscendingKeyOrder) {
  IntMap map;
  // Insert out of order to prove iteration order doesn't depend on it.
  map.emplace(5, "five");
  map.emplace(1, "one");
  map.emplace(3, "three");

  std::vector<std::pair<int, std::string>> seen;
  for (const auto& [key, value] : map) {
    seen.emplace_back(key, value);
  }

  const std::vector<std::pair<int, std::string>> expected = {
      {1, "one"}, {3, "three"}, {5, "five"}};
  EXPECT_EQ(seen, expected);
}

TEST(SortedVecMapTest, EmplaceDoesNotOverwriteExistingKey) {
  IntMap map;
  auto [first_it, first_inserted] = map.emplace(1, "one");
  EXPECT_TRUE(first_inserted);

  auto [second_it, second_inserted] = map.emplace(1, "uno");
  EXPECT_FALSE(second_inserted);
  EXPECT_EQ(first_it, second_it);
  EXPECT_EQ(map.size(), 1);
  EXPECT_EQ(map.find(1)->second, "one");
}

TEST(SortedVecMapTest, WorksWithMapGetDefault) {
  IntMap map;
  map.emplace(7, "seven");

  EXPECT_EQ(map_get(map, 7, std::string{"missing"}), "seven");
  EXPECT_EQ(map_get(map, 8, std::string{"missing"}), "missing");
}

TEST(SortedVecMapTest, SupportsMoveOnlyValues) {
  SortedVecMap<int, std::unique_ptr<int>> map;
  map.emplace(2, std::make_unique<int>(20));
  map.emplace(1, std::make_unique<int>(10));

  EXPECT_EQ(*map.find(1)->second, 10);
  EXPECT_EQ(*map.find(2)->second, 20);

  // Iteration stays sorted even after the middle-insert shifted elements.
  std::vector<int> keys;
  for (const auto& [key, value] : map) {
    keys.push_back(key);
  }
  const std::vector<int> expected = {1, 2};
  EXPECT_EQ(keys, expected);
}
