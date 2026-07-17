// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/hugepages.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#ifndef WIN32
#include "sys/mman.h"
#endif

#include <cstddef>
#include <cstdlib>
#include <utility>

namespace cinderx {

template <typename T>
class SlabIterator {
 public:
  SlabIterator() = default;
  SlabIterator(char* ptr, size_t increment)
      : ptr_{ptr}, increment_{increment} {}

  SlabIterator& operator++() {
    ptr_ += increment_;
    return *this;
  }

  SlabIterator operator++(int) {
    auto copy = *this;
    operator++();
    return copy;
  }

  T& operator*() {
    return *reinterpret_cast<T*>(ptr_);
  }

  const T& operator*() const {
    return *reinterpret_cast<const T*>(ptr_);
  }

  bool operator==(const SlabIterator& o) const {
    return ptr_ == o.ptr_;
  }

  bool operator!=(const SlabIterator& o) const {
    return !operator==(o);
  }

 private:
  char* ptr_{nullptr};
  size_t increment_{0};
};

// A slab of memory.  The total size of the slab is defined statically, but the
// size of each individual object within the slab is controlled by the
// `increment_` field.
template <typename T, size_t kSlabSize>
class Slab {
 public:
  using iterator = SlabIterator<T>;

  explicit Slab(
      size_t increment,
      std::shared_ptr<HugePageArena> arena = nullptr)
      : arena_(arena), increment_{increment} {
    JIT_CHECK(
        increment >= sizeof(T),
        "Trying to fit a slab object into too little memory");
    void* ptr = nullptr;
    if (arena_ != nullptr) {
      ptr = arena_->allocate(kSlabSize, kPageSize);
    }

    if (ptr == nullptr) {
      ptr = malloc_aligned(kSlabSize, kPageSize);
      JIT_CHECK(ptr != nullptr, "Failed to allocate {} bytes", kSlabSize);
      owned_base_.reset(static_cast<char*>(ptr));
    }
    base_ = fill_ = static_cast<char*>(ptr);
  }

  Slab(Slab&& other)
      : base_{other.base_},
        arena_(std::move(other.arena_)),
        owned_base_{std::move(other.owned_base_)},
        fill_{other.fill_},
        increment_{other.increment_} {
    other.fill_ = nullptr;
    other.base_ = nullptr;
  }

  ~Slab() {
    for (T& obj : *this) {
      obj.~T();
    }
  }

  // Allocate memory for a new T object. Returns void* because the object is not
  // constructed yet.
  void* allocate() {
    char* new_fill = fill_ + increment_;
    if (new_fill > base_ + kSlabSize) {
      return nullptr;
    }

    char* ptr = fill_;
    fill_ = new_fill;
    return ptr;
  }

#ifndef WIN32
  void mlock() {
    if (::mlock(base_, kSlabSize) < 0) {
      JIT_LOG("Failed to mlock slab at {}", base_);
      return;
    }
    mlocks_++;
  }

  void munlock() {
    // Not a fatal error because we allow ::mlock() and ::munlock() to fail and
    // don't raise that information up further.
    if (mlocks_ == 0) {
      JIT_LOG("Trying to unlock slab more than it has been been locked");
    }

    if (::munlock(base_, kSlabSize) < 0) {
      JIT_LOG("Failed to munlock slab at {}", base_);
      return;
    }
    mlocks_--;
  }
#endif

  iterator begin() const {
    return iterator{base_, increment_};
  }

  iterator end() const {
    return iterator{fill_, increment_};
  }

 private:
  char* base_;
  std::shared_ptr<HugePageArena> arena_;
  unique_aligned_ptr<char> owned_base_;
  char* fill_{nullptr};
  size_t increment_{0};
  size_t mlocks_{0};
};

} // namespace cinderx
