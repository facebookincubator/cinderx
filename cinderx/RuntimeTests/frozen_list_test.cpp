// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Common/frozen_list.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace cinderx;

// Records how the list builds and tears down its elements, so tests can tell
// in-place construction from construct-then-copy.
struct Tracked {
  static inline int constructions = 0;
  static inline int copies = 0;
  static inline int live = 0;
  // Copies to allow before the next one throws; -1 to never throw.
  static inline int copies_before_throw = -1;

  Tracked() {
    ++constructions;
    ++live;
  }
  Tracked(const Tracked&) {
    countCopy();
    ++live;
  }
  Tracked& operator=(const Tracked&) {
    countCopy();
    return *this;
  }
  ~Tracked() {
    --live;
  }

  static void reset() {
    constructions = 0;
    copies = 0;
    live = 0;
    copies_before_throw = -1;
  }

 private:
  static void countCopy() {
    if (copies == copies_before_throw) {
      throw std::runtime_error("element copy failed");
    }
    ++copies;
  }
};

} // namespace

TEST(FrozenListTest, EmptyListHasNoElements) {
  FrozenList<int> list;
  EXPECT_EQ(list.size(), 0);
  EXPECT_EQ(list.begin(), list.end());

  list.initialize(0);
  EXPECT_EQ(list.size(), 0);
  EXPECT_EQ(list.begin(), list.end());
}

TEST(FrozenListTest, InitializeFillsWithDefaultValues) {
  FrozenList<int> list;
  list.initialize(3);

  const std::vector<int> expected{0, 0, 0};
  EXPECT_EQ(std::vector<int>(list.begin(), list.end()), expected);
}

TEST(FrozenListTest, InitializeFillsWithGivenValue) {
  FrozenList<std::string> list;
  list.initialize(2, "x");

  const std::vector<std::string> expected{"x", "x"};
  EXPECT_EQ(std::vector<std::string>(list.begin(), list.end()), expected);
}

TEST(FrozenListTest, SubscriptReadsAndWritesElements) {
  FrozenList<int> list;
  list.initialize(3);
  list[0] = 7;
  list[2] = 9;

  const std::vector<int> expected{7, 0, 9};
  EXPECT_EQ(std::vector<int>(list.begin(), list.end()), expected);

  const FrozenList<int>& const_list = list;
  EXPECT_EQ(const_list[0], 7);
}

TEST(FrozenListTest, InitializeTwiceThrows) {
  FrozenList<int> list;
  list.initialize(1);

  EXPECT_THROW(list.initialize(1), std::runtime_error);
}

TEST(FrozenListTest, CopyConstructorCopiesElements) {
  const FrozenList<std::string> source{"a", "b"};
  FrozenList<std::string> copy{source};
  copy[0] = "z";

  // Writing through the copy proves it owns its elements rather than sharing
  // the source's.
  const std::vector<std::string> copied{"z", "b"};
  EXPECT_EQ(std::vector<std::string>(copy.begin(), copy.end()), copied);

  const std::vector<std::string> untouched{"a", "b"};
  EXPECT_EQ(std::vector<std::string>(source.begin(), source.end()), untouched);
}

TEST(FrozenListTest, MoveConstructorTransfersOwnership) {
  FrozenList<std::string> source{"a", "b"};
  const FrozenList<std::string> moved{std::move(source)};

  const std::vector<std::string> expected{"a", "b"};
  EXPECT_EQ(std::vector<std::string>(moved.begin(), moved.end()), expected);

  // Reading the moved-from list is the point of the test: the move
  // constructor empties it, rather than leaving it merely unspecified.
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(source.size(), 0);
}

TEST(FrozenListTest, DestructorDestroysEveryElement) {
  Tracked::reset();
  {
    FrozenList<Tracked> list;
    list.initialize(3);
    EXPECT_EQ(Tracked::live, 3);
  }
  EXPECT_EQ(Tracked::live, 0);
}

// Each element is built exactly once, directly in the list's storage: no
// temporary to copy from and no separate default-initialization pass.
TEST(FrozenListTest, InitializeConstructionCounts) {
  Tracked::reset();
  FrozenList<Tracked> list;
  list.initialize(4);

  EXPECT_EQ(Tracked::constructions, 4);
  EXPECT_EQ(Tracked::copies, 0);
}

// A failed initialize is a no-op: it destroys whatever it managed to build and
// leaves the list uninitialized, so it reports no elements and still accepts a
// later initialize.
TEST(FrozenListTest, InitializeWhenElementCopyThrows) {
  Tracked::reset();
  const Tracked probe;
  Tracked::copies_before_throw = 2;

  FrozenList<Tracked> list;
  EXPECT_THROW(list.initialize(5, probe), std::runtime_error);
  Tracked::copies_before_throw = -1;

  EXPECT_EQ(Tracked::live, 1); // only the probe
  EXPECT_EQ(list.size(), 0);

  list.initialize(2, probe);
  EXPECT_EQ(list.size(), 2);
}
