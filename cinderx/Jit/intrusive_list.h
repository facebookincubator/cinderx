// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"

#include <cstddef>
#include <iterator>

/*
 * This file defines a simple, intrusive doubly-linked circular list.
 *
 * To make an object eligible to participate in an intrusive list one must
 * add a public `IntrusiveListNode` member for each list the object may
 * belong to.
 *
 * Then, each container is declared as `IntrusiveList<T, T::&member> list`.
 *
 * For example,
 *
 *     // Instances of entry may particpate in one intrusive list
 *     struct Entry {
 *         Entry(int value) : value(value), node() {}
 *
 *         int value;
 *         IntrusiveListNode node;
 *     };
 *
 *     // Then one can declare and build a list of entries like so
 *     IntrusiveList<Entry, &Entry::node> entries;
 *
 *     Entry entry1(100);
 *     entries.pushBack(entry1);
 *
 *     Entry entry2(200);
 *     entries.pushBack(entry2);
 *
 *     // Prints 1 2
 *     for (auto entry : entries) {
 *         printf("%d ", entry.value);
 *     }
 */

namespace cinderx::jit {

class IntrusiveListNode {
 public:
  IntrusiveListNode() : prev_(this), next_(this) {}

  IntrusiveListNode* prev() const {
    return prev_;
  }

  void setPrev(IntrusiveListNode* prev) {
    prev_ = prev;
  }

  IntrusiveListNode* next() const {
    return next_;
  }

  void setNext(IntrusiveListNode* next) {
    next_ = next;
  }

  void insertBefore(IntrusiveListNode* node) {
    JIT_DCHECK(!isLinked(), "Item is already in a list");
    auto prev_node = node->prev();
    prev_node->setNext(this);
    setPrev(prev_node);
    setNext(node);
    node->setPrev(this);
  }

  void insertAfter(IntrusiveListNode* node) {
    JIT_DCHECK(!isLinked(), "Item is already in a list");
    auto next_node = node->next();
    next_node->setPrev(this);
    setNext(next_node);
    node->setNext(this);
    setPrev(node);
  }

  void unlink() {
    JIT_DCHECK(isLinked(), "Item is not in a list");
    prev()->setNext(next());
    next()->setPrev(prev());
    setNext(this);
    setPrev(this);
  }

  bool isLinked() const {
    return prev_ != this;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(IntrusiveListNode);

  IntrusiveListNode* prev_;
  IntrusiveListNode* next_;
};

template <class T, IntrusiveListNode T::* node_member, bool is_const>
class IntrusiveListIterator;

template <class T, IntrusiveListNode T::* node_member>
class IntrusiveList {
 public:
  // Iterator typedefs
  using iterator = IntrusiveListIterator<T, node_member, false>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_iterator = IntrusiveListIterator<T, node_member, true>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;
  using difference_type = std::ptrdiff_t;
  using size_type = std::size_t;
  using value_type = T;
  using pointer = T*;
  using const_pointer = const T*;
  using reference = T&;
  using const_reference = const T&;

  IntrusiveList() = default;

  bool isEmpty() const {
    return root_.next() == &root_;
  }

  reference front() {
    JIT_DCHECK(!isEmpty(), "list cannot be empty");
    return *getOwner(root_.next());
  }

  const_reference front() const {
    JIT_DCHECK(!isEmpty(), "list cannot be empty");
    return *getOwner(root_.next());
  }

  void pushFront(reference node) {
    (node.*node_member).insertAfter(&root_);
  }

  void popFront() {
    JIT_DCHECK(!isEmpty(), "list cannot be empty");
    root_.next()->unlink();
  }

  reference extractFront() {
    JIT_DCHECK(!isEmpty(), "list cannot be empty");
    IntrusiveListNode* old_front = root_.next();
    old_front->unlink();
    return *getOwner(old_front);
  }

  reference back() {
    JIT_DCHECK(!isEmpty(), "list cannot be empty");
    return *getOwner(root_.prev());
  }

  const_reference back() const {
    JIT_DCHECK(!isEmpty(), "list cannot be empty");
    return *getOwner(root_.prev());
  }

  reference next(reference node) {
    return *getOwner((node.*node_member).next());
  }

  const_reference next(const_reference node) const {
    return *getOwner((node.*node_member).next());
  }

  void pushBack(reference node) {
    (node.*node_member).insertAfter(root_.prev());
  }

  void popBack() {
    JIT_DCHECK(!isEmpty(), "list cannot be empty");
    root_.prev()->unlink();
  }

  reference extractBack() {
    JIT_DCHECK(!isEmpty(), "list cannot be empty");
    IntrusiveListNode* old_back = root_.prev();
    old_back->unlink();
    return *getOwner(old_back);
  }

  void spliceAfter(reference node, IntrusiveList& other) {
    IntrusiveListNode* lnode = &(node.*node_member);
    if (lnode->next() == &other.root_) {
      // node is the last element in other, or other is empty
      return;
    }
    IntrusiveListNode* other_root = &(other.root_);
    IntrusiveListNode* spliced_head = lnode->next();
    IntrusiveListNode* spliced_tail = other_root->prev();
    // Splice the remainder out of the other list
    lnode->setNext(other_root);
    other_root->setPrev(lnode);
    // Insert it into our list
    IntrusiveListNode* tail = root_.prev();
    tail->setNext(spliced_head);
    spliced_head->setPrev(tail);
    spliced_tail->setNext(&root_);
    root_.setPrev(spliced_tail);
  }

  void insert(reference r, iterator it) {
    JIT_DCHECK(
        it.list() == this,
        "iterator is for list {}, this == {}",
        reinterpret_cast<void*>(it.list()),
        reinterpret_cast<void*>(this));
    (r.*node_member).insertBefore(it.node());
  }

  // Return an iterator to the given object, assuming it's in this list.
  iterator iterator_to(reference r) {
    return iterator(this, &(r.*node_member));
  }

  const_iterator const_iterator_to(const_reference r) const {
    return const_iterator(this, &(r.*node_member));
  }

  iterator begin() {
    return iterator(this, root_.next());
  }

  const_iterator begin() const {
    return const_iterator(this, root_.next());
  }

  // Return a reverse iterator to the given object, assuming it's in this list.
  reverse_iterator reverse_iterator_to(reference r) {
    // For a reverse iterator r constructed from an iterator i, the
    // relationship &*r == &*(i-1)
    return reverse_iterator(++iterator_to(r));
  }

  const_reverse_iterator const_reverse_iterator_to(const_reference r) const {
    return const_reverse_iterator(++const_iterator_to(r));
  }

  reverse_iterator rbegin() {
    return reverse_iterator(end());
  }

  const_reverse_iterator rbegin() const {
    return const_reverse_iterator(end());
  }

  iterator end() {
    return iterator(this, &root_);
  }

  const_iterator end() const {
    return const_iterator(this, &root_);
  }

  reverse_iterator rend() {
    return reverse_iterator(begin());
  }

  const_reverse_iterator rend() const {
    return const_reverse_iterator(begin());
  }

  const_reverse_iterator crend() const {
    return rend();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(IntrusiveList);

  pointer getOwner(IntrusiveListNode* node) const {
    return reinterpret_cast<pointer>(
        reinterpret_cast<char*>(node) - offsetOfNode());
  }

  const_pointer getOwner(const IntrusiveListNode* node) const {
    return reinterpret_cast<const_pointer>(
        reinterpret_cast<const char*>(node) - offsetOfNode());
  }

  // The offset is a compile-time constant determined entirely by the template
  // parameters.
  //
  // This is technically UB, but all of the compilers that we're going to need
  // to use seem to support it, and it's what other more sophisticated intrusive
  // list classes use.
  static std::size_t offsetOfNode() {
    return reinterpret_cast<char*>(&(static_cast<T*>(nullptr)->*node_member)) -
        static_cast<char*>(nullptr);
  }

  friend class IntrusiveListIterator<T, node_member, true>;
  friend class IntrusiveListIterator<T, node_member, false>;

  IntrusiveListNode root_;
};

template <class T, IntrusiveListNode T::* node_member, bool is_const>
class IntrusiveListIterator {
 public:
  using difference_type = std::ptrdiff_t;
  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = T;
  using list_type = std::conditional_t<
      is_const,
      const IntrusiveList<T, node_member>*,
      IntrusiveList<T, node_member>*>;
  using node_type = std::
      conditional_t<is_const, const IntrusiveListNode*, IntrusiveListNode*>;
  using pointer = std::conditional_t<is_const, const T*, T*>;
  using reference = std::conditional_t<is_const, const T&, T&>;

  IntrusiveListIterator() = default;
  IntrusiveListIterator(list_type list, node_type current)
      : list_(list), current_(current) {}
  IntrusiveListIterator(const IntrusiveListIterator&) = default;
  IntrusiveListIterator& operator=(const IntrusiveListIterator&) = default;

  bool operator==(IntrusiveListIterator const& other) const {
    return current_ == other.current_;
  }

  bool operator!=(IntrusiveListIterator const& other) const {
    return !(*this == other);
  }

  reference operator*() const {
    JIT_DCHECK(current_ != &(list_->root_), "iterator exhausted");
    return *(list_->getOwner(current_));
  }

  pointer operator->() const {
    JIT_DCHECK(current_ != &(list_->root_), "iterator exhausted");
    return list_->getOwner(current_);
  }

  IntrusiveListIterator& operator++() {
    JIT_DCHECK(current_ != &(list_->root_), "iterator exhausted");
    current_ = current_->next();
    return *this;
  }

  IntrusiveListIterator operator++(int) {
    JIT_DCHECK(current_ != &(list_->root_), "iterator exhausted");
    IntrusiveListIterator<T, node_member, is_const> clone(*this);
    operator++();
    return clone;
  }

  IntrusiveListIterator& operator--() {
    JIT_DCHECK(current_ != list_->root_.next(), "iterator exhausted");
    current_ = current_->prev();
    return *this;
  }

  IntrusiveListIterator operator--(int) {
    JIT_DCHECK(current_ != list_->root_.next(), "iterator exhausted");
    IntrusiveListIterator<T, node_member, is_const> clone(*this);
    operator--();
    return clone;
  }

  list_type list() const {
    return list_;
  }

  node_type node() const {
    return current_;
  }

 private:
  list_type list_{};
  node_type current_{};
};

} // namespace cinderx::jit
