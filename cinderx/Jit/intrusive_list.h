// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"

#include <cstddef>
#include <iterator>

/*
 * This file defines a simple intrusive doubly linked circular list.
 *
 * To store objects of type T in an IntrusiveList<T>, T must publicly inherit
 * from IntrusiveListNode<T>.
 *
 * A tag allows an object to belong to multiple lists at the same time. Define
 * a different tag for each list and inherit from IntrusiveListNode<T, Tag> for
 * each tag. An object can belong to at most one list for each tag.
 *
 * Example:
 *
 *     struct Entry : public IntrusiveListNode<Entry> {
 *       explicit Entry(int value) : value{value} {}
 *
 *       int value;
 *     };
 *
 *     IntrusiveList<Entry> entries;
 *     Entry entry{100};
 *     entries.pushBack(entry);
 *
 * Multiple-list example:
 *
 *     struct PendingTag {};
 *     struct ReadyTag {};
 *
 *     struct Task : public IntrusiveListNode<Task, PendingTag>,
 *                   public IntrusiveListNode<Task, ReadyTag> {};
 *
 *     IntrusiveList<Task, PendingTag> pending;
 *     IntrusiveList<Task, ReadyTag> ready;
 *
 *     Task task;
 *     pending.pushBack(task);
 *     ready.pushBack(task);
 */

namespace cinderx::jit {

template <class T, class Tag>
class IntrusiveList;

template <class T, class Tag, bool IsConst>
class IntrusiveListIterator;

template <class T, class Tag = void>
class IntrusiveListNode {
 public:
  IntrusiveListNode() : prev_(this), next_(this) {}

  bool isLinked() const {
    return prev_ != this;
  }

  void unlink() {
    JIT_DCHECK(isLinked(), "Item is not in a list");
    prev()->setNext(next());
    next()->setPrev(prev());
    setNext(this);
    setPrev(this);
  }

 private:
  friend class IntrusiveList<T, Tag>;
  friend class IntrusiveListIterator<T, Tag, false>;
  friend class IntrusiveListIterator<T, Tag, true>;
  friend T;

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

  DISALLOW_COPY_AND_ASSIGN(IntrusiveListNode);

  IntrusiveListNode* prev_;
  IntrusiveListNode* next_;
};

template <class T, class Tag = void>
class IntrusiveList {
 public:
  // Iterator typedefs
  using NodeType = IntrusiveListNode<T, Tag>;
  using iterator = IntrusiveListIterator<T, Tag, false>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_iterator = IntrusiveListIterator<T, Tag, true>;
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
    node.NodeType::insertAfter(&root_);
  }

  void popFront() {
    JIT_DCHECK(!isEmpty(), "list cannot be empty");
    root_.next()->unlink();
  }

  reference extractFront() {
    JIT_DCHECK(!isEmpty(), "list cannot be empty");
    NodeType* old_front = root_.next();
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
    return *getOwner(node.NodeType::next());
  }

  const_reference next(const_reference node) const {
    return *getOwner(node.NodeType::next());
  }

  void pushBack(reference node) {
    node.NodeType::insertAfter(root_.prev());
  }

  void popBack() {
    JIT_DCHECK(!isEmpty(), "list cannot be empty");
    root_.prev()->unlink();
  }

  reference extractBack() {
    JIT_DCHECK(!isEmpty(), "list cannot be empty");
    NodeType* old_back = root_.prev();
    old_back->unlink();
    return *getOwner(old_back);
  }

  void spliceAfter(reference node, IntrusiveList& other) {
    NodeType* lnode = &node;
    if (lnode->next() == &other.root_) {
      // node is the last element in other, or other is empty
      return;
    }
    NodeType* other_root = &(other.root_);
    NodeType* spliced_head = lnode->next();
    NodeType* spliced_tail = other_root->prev();
    // Splice the remainder out of the other list
    lnode->setNext(other_root);
    other_root->setPrev(lnode);
    // Insert it into our list
    NodeType* tail = root_.prev();
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
    r.NodeType::insertBefore(it.node());
  }

  // Return an iterator to the given object, assuming it's in this list.
  iterator iterator_to(reference r) {
    return iterator(this, &r);
  }

  const_iterator const_iterator_to(const_reference r) const {
    return const_iterator(this, &r);
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

  pointer getOwner(NodeType* node) const {
    return static_cast<T*>(node);
  }

  const_pointer getOwner(const NodeType* node) const {
    return static_cast<T const*>(node);
  }

  friend class IntrusiveListIterator<T, Tag, true>;
  friend class IntrusiveListIterator<T, Tag, false>;

  NodeType root_;
};

template <class T, class Tag, bool is_const>
class IntrusiveListIterator {
 public:
  using difference_type = std::ptrdiff_t;
  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = T;
  using list_type = std::conditional_t<
      is_const,
      const IntrusiveList<T, Tag>*,
      IntrusiveList<T, Tag>*>;
  using node_type = std::conditional_t<
      is_const,
      const IntrusiveListNode<T, Tag>*,
      IntrusiveListNode<T, Tag>*>;
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
    IntrusiveListIterator<T, Tag, is_const> clone(*this);
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
    IntrusiveListIterator<T, Tag, is_const> clone(*this);
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
