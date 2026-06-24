// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace jit {

// An associative map backed by a single sorted std::vector of key/value pairs.
//
// This is aimed at small maps that are populated once and then read: lookups
// are O(log n) binary searches, iteration visits keys in ascending order with
// good cache locality, and the whole thing is one contiguous allocation.
// Insertion is O(n) because it shifts later elements, which is fine for the
// small sizes we use it for.
template <typename Key, typename Value, typename Compare = std::less<Key>>
class SortedVecMap {
 public:
  using key_type = Key;
  using mapped_type = Value;
  using value_type = std::pair<Key, Value>;
  using container_type = std::vector<value_type>;
  using iterator = typename container_type::iterator;
  using const_iterator = typename container_type::const_iterator;
  using size_type = typename container_type::size_type;

  iterator begin() {
    return data_.begin();
  }

  iterator end() {
    return data_.end();
  }

  const_iterator begin() const {
    return data_.begin();
  }

  const_iterator end() const {
    return data_.end();
  }

  bool empty() const {
    return data_.empty();
  }

  size_type size() const {
    return data_.size();
  }

  iterator find(const Key& key) {
    iterator it = lowerBound(key);
    return matches(it, key) ? it : data_.end();
  }

  const_iterator find(const Key& key) const {
    const_iterator it = lowerBound(key);
    return matches(it, key) ? it : data_.end();
  }

  // Insert a key/value pair, keeping the backing vector sorted by key. If the
  // key is already present nothing is inserted.  Return an iterator to the
  // element with that key and whether a new element was inserted.
  template <typename K, typename V>
  std::pair<iterator, bool> emplace(K&& key, V&& value) {
    iterator it = lowerBound(key);
    if (matches(it, key)) {
      return {it, false};
    }
    iterator inserted =
        data_.emplace(it, std::forward<K>(key), std::forward<V>(value));
    return {inserted, true};
  }

 private:
  bool matches(const_iterator it, const Key& key) const {
    return it != data_.end() && !comp_(key, it->first);
  }

  iterator lowerBound(const Key& key) {
    return std::lower_bound(
        data_.begin(),
        data_.end(),
        key,
        [this](const value_type& a, const Key& b) {
          return comp_(a.first, b);
        });
  }

  const_iterator lowerBound(const Key& key) const {
    return std::lower_bound(
        data_.begin(),
        data_.end(),
        key,
        [this](const value_type& a, const Key& b) {
          return comp_(a.first, b);
        });
  }

  container_type data_;
  [[no_unique_address]] Compare comp_;
};

} // namespace jit
