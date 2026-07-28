// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Jit/intrusive_list.h"

using cinderx::jit::IntrusiveList;
using cinderx::jit::IntrusiveListNode;

struct Entry : public IntrusiveListNode<Entry> {
  explicit Entry(int value) : value(value) {}

  int value;
};

using EntryList = IntrusiveList<Entry>;

TEST(IntrusiveListTest, NewlyCreatedListIsEmpty) {
  EntryList entries;
  ASSERT_TRUE(entries.isEmpty());
}

TEST(IntrusiveListTest, PushFrontOnEmptyListUpdatesFrontAndBack) {
  EntryList entries;
  Entry entry(100);
  entries.pushFront(entry);
  EXPECT_EQ(entries.front().value, 100);
  EXPECT_EQ(entries.back().value, 100);
  EXPECT_FALSE(entries.isEmpty());
}

TEST(IntrusiveListTest, PushBackOnEmptyListUpdatesFrontAndBack) {
  EntryList entries;
  Entry entry(100);
  entries.pushBack(entry);
  EXPECT_EQ(entries.front().value, 100);
  EXPECT_EQ(entries.back().value, 100);
  EXPECT_FALSE(entries.isEmpty());
}

TEST(IntrusiveListTest, PopFrontUpdatesList) {
  EntryList entries;
  Entry entry1(100);
  entries.pushFront(entry1);
  Entry entry2(200);
  entries.pushFront(entry2);
  Entry entry3(300);
  entries.pushFront(entry3);

  ASSERT_EQ(entries.front().value, 300);
  ASSERT_EQ(entries.back().value, 100);

  entries.popFront();
  ASSERT_EQ(entries.front().value, 200);
  ASSERT_EQ(entries.back().value, 100);

  entries.popFront();
  ASSERT_EQ(entries.front().value, 100);
  ASSERT_EQ(entries.back().value, 100);

  entries.popFront();
  ASSERT_TRUE(entries.isEmpty());
}

TEST(IntrusiveListTest, ExtractFrontUpdatesList) {
  EntryList entries;
  Entry entry1(100);
  entries.pushFront(entry1);
  Entry entry2(200);
  entries.pushFront(entry2);
  Entry entry3(300);
  entries.pushFront(entry3);

  ASSERT_EQ(entries.extractFront().value, 300);
  ASSERT_EQ(entries.extractFront().value, 200);
  ASSERT_EQ(entries.extractFront().value, 100);
  ASSERT_TRUE(entries.isEmpty());
}

TEST(IntrusiveListTest, PopBackUpdatesList) {
  EntryList entries;
  Entry entry1(100);
  entries.pushBack(entry1);
  Entry entry2(200);
  entries.pushBack(entry2);
  Entry entry3(300);
  entries.pushBack(entry3);

  ASSERT_EQ(entries.front().value, 100);
  ASSERT_EQ(entries.back().value, 300);

  entries.popBack();
  ASSERT_EQ(entries.front().value, 100);
  ASSERT_EQ(entries.back().value, 200);

  entries.popBack();
  ASSERT_EQ(entries.front().value, 100);
  ASSERT_EQ(entries.back().value, 100);

  entries.popBack();
  ASSERT_TRUE(entries.isEmpty());
}

TEST(IntrusiveListTest, ExtractBackUpdatesList) {
  EntryList entries;
  Entry entry1(100);
  entries.pushBack(entry1);
  Entry entry2(200);
  entries.pushBack(entry2);
  Entry entry3(300);
  entries.pushBack(entry3);

  ASSERT_EQ(entries.extractBack().value, 300);
  ASSERT_EQ(entries.extractBack().value, 200);
  ASSERT_EQ(entries.extractBack().value, 100);
  ASSERT_TRUE(entries.isEmpty());
}

TEST(IntrusiveListTest, IsForwardIterable) {
  EntryList entries;
  Entry entry1(100);
  entries.pushBack(entry1);
  Entry entry2(200);
  entries.pushBack(entry2);
  Entry entry3(300);
  entries.pushBack(entry3);

  auto it = entries.begin();
  ASSERT_EQ(it->value, 100);
  it++;
  ASSERT_EQ(it->value, 200);
  ++it;
  ASSERT_EQ(it->value, 300);
  it++;
  ASSERT_TRUE(it == entries.end());
}

TEST(IntrusiveListTest, IsReverseIterable) {
  EntryList entries;
  Entry entry1(100);
  entries.pushBack(entry1);
  Entry entry2(200);
  entries.pushBack(entry2);
  Entry entry3(300);
  entries.pushBack(entry3);

  auto it = entries.rbegin();
  ASSERT_EQ(it->value, 300);
  it++;
  ASSERT_EQ(it->value, 200);
  ++it;
  ASSERT_EQ(it->value, 100);
  it++;
  ASSERT_TRUE(it == entries.rend());
}

TEST(IntrusiveListTest, IsDecrementable) {
  EntryList entries;
  Entry entry1(100);
  entries.pushBack(entry1);
  Entry entry2(200);
  entries.pushBack(entry2);
  Entry entry3(300);
  entries.pushBack(entry3);

  auto it = entries.end();
  it--;
  ASSERT_EQ(it->value, 300);
  --it;
  ASSERT_EQ(it->value, 200);
  it--;
  ASSERT_EQ(it->value, 100);
  ASSERT_TRUE(it == entries.begin());
}

TEST(InstrusiveListTest, CanBeUsedInRangeExpressions) {
  EntryList entries;
  Entry entry1(100);
  entries.pushBack(entry1);
  Entry entry2(200);
  entries.pushBack(entry2);
  Entry entry3(300);
  entries.pushBack(entry3);

  int visited[3] = {-1, -1, -1};
  int idx = 0;
  for (Entry& entry : entries) {
    visited[idx] = entry.value;
    idx++;
  }

  EXPECT_EQ(visited[0], 100);
  EXPECT_EQ(visited[1], 200);
  EXPECT_EQ(visited[2], 300);
}

TEST(InstrusiveListTest, CanBeUsedInRangeExpressionsWithConstReference) {
  EntryList entries;
  Entry entry1(100);
  entries.pushBack(entry1);
  Entry entry2(200);
  entries.pushBack(entry2);
  Entry entry3(300);
  entries.pushBack(entry3);

  int visited[3] = {-1, -1, -1};
  int idx = 0;
  for (const auto& entry : entries) {
    visited[idx] = entry.value;
    idx++;
  }

  EXPECT_EQ(visited[0], 100);
  EXPECT_EQ(visited[1], 200);
  EXPECT_EQ(visited[2], 300);
}

TEST(IntrusiveListTest, EmptyListHasZeroSize) {
  EntryList entries;
  EXPECT_EQ(entries.size(), 0u);
}

TEST(IntrusiveListTest, SizeTracksPushAndPop) {
  EntryList entries;
  Entry entry1(100);
  Entry entry2(200);
  Entry entry3(300);

  entries.pushBack(entry1);
  EXPECT_EQ(entries.size(), 1u);
  entries.pushFront(entry2);
  EXPECT_EQ(entries.size(), 2u);
  entries.pushBack(entry3);
  EXPECT_EQ(entries.size(), 3u);

  entries.popFront();
  EXPECT_EQ(entries.size(), 2u);
  entries.popBack();
  EXPECT_EQ(entries.size(), 1u);
  entries.popFront();
  EXPECT_EQ(entries.size(), 0u);
  EXPECT_TRUE(entries.isEmpty());
}

TEST(IntrusiveListTest, SizeTracksExtract) {
  EntryList entries;
  Entry entry1(100);
  Entry entry2(200);
  entries.pushBack(entry1);
  entries.pushBack(entry2);
  EXPECT_EQ(entries.size(), 2u);

  entries.extractFront();
  EXPECT_EQ(entries.size(), 1u);
  entries.extractBack();
  EXPECT_EQ(entries.size(), 0u);
}

TEST(IntrusiveListTest, SizeTracksInsertAndRemove) {
  EntryList entries;
  Entry entry1(100);
  Entry entry2(200);
  Entry entry3(300);
  entries.pushBack(entry1);
  entries.pushBack(entry3);

  // Insert entry2 before entry3.
  entries.insert(entry2, entries.iterator_to(entry3));
  EXPECT_EQ(entries.size(), 3u);
  auto it = entries.begin();
  EXPECT_EQ(it->value, 100);
  EXPECT_EQ((++it)->value, 200);
  EXPECT_EQ((++it)->value, 300);

  entries.remove(entry2);
  EXPECT_EQ(entries.size(), 2u);
  entries.remove(entry1);
  entries.remove(entry3);
  EXPECT_EQ(entries.size(), 0u);
}

TEST(InstrusiveListTest, CanGetReverseIteratorsToElements) {
  EntryList list;
  Entry entry1(100);
  list.pushBack(entry1);
  Entry entry2(200);
  list.pushBack(entry2);
  Entry entry3(300);
  list.pushBack(entry3);

  auto it1 = list.reverse_iterator_to(entry3);
  ASSERT_EQ(it1->value, 300);
  it1++;
  ASSERT_EQ(it1->value, 200);
  ++it1;
  ASSERT_EQ(it1->value, 100);
  it1++;
  ASSERT_EQ(it1, list.rend());

  auto it2 = list.reverse_iterator_to(entry2);
  ASSERT_EQ(it2->value, 200);
  ++it2;
  ASSERT_EQ(it2->value, 100);
  it2++;
  ASSERT_EQ(it2, list.rend());

  auto it3 = list.reverse_iterator_to(entry1);
  ASSERT_EQ(it3->value, 100);
  ++it3;
  ASSERT_EQ(it3, list.rend());
}

namespace {

struct FirstTag {};
struct SecondTag {};

struct MultiEntry : IntrusiveListNode<MultiEntry, FirstTag>,
                    IntrusiveListNode<MultiEntry, SecondTag> {};

} // namespace

TEST(IntrusiveListTest, SupportsMultipleTaggedLists) {
  MultiEntry entry;
  IntrusiveList<MultiEntry, FirstTag> first;
  IntrusiveList<MultiEntry, SecondTag> second;

  first.pushBack(entry);
  second.pushBack(entry);

  EXPECT_EQ(&first.front(), &entry);
  EXPECT_EQ(&second.front(), &entry);

  first.popFront();
  EXPECT_TRUE(first.isEmpty());
  EXPECT_FALSE(second.isEmpty());

  second.popFront();
}

TEST(IntrusiveListTest, TaggedListsAreIndependentlyOrdered) {
  MultiEntry a, b, c;
  IntrusiveList<MultiEntry, FirstTag> first;
  IntrusiveList<MultiEntry, SecondTag> second;

  // Same objects, deliberately different order in each list.
  first.pushBack(a);
  first.pushBack(b);
  first.pushBack(c);

  second.pushBack(c);
  second.pushBack(a);
  second.pushBack(b);

  const std::vector<MultiEntry*> expected_first{&a, &b, &c};
  const std::vector<MultiEntry*> expected_second{&c, &a, &b};

  std::vector<MultiEntry*> got_first;
  for (MultiEntry& e : first) {
    got_first.push_back(&e);
  }
  std::vector<MultiEntry*> got_second;
  for (MultiEntry& e : second) {
    got_second.push_back(&e);
  }

  EXPECT_EQ(got_first, expected_first);
  EXPECT_EQ(got_second, expected_second);
}
