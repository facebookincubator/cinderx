// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <utility>

namespace cinderx {

// A frozen list is effectively a vector that is dynamically allocated at
// runtime, but then can no longer be resized.
template <typename T>
  requires std::default_initializable<T> && std::copyable<T>
class FrozenList {
 public:
  FrozenList() = default;
  ~FrozenList() {
    if (ptr_) {
      std::destroy_n(ptr_, size_);
      std::allocator<T>{}.deallocate(ptr_, size_);
    }
  }

  // Construct a frozen list from the given initializer list.
  /* implicit */ FrozenList(std::initializer_list<T> values) {
    allocateAndConstruct(values.size(), [&values](T* storage) {
      std::uninitialized_copy(values.begin(), values.end(), storage);
    });
  }

  FrozenList(const FrozenList& other) {
    allocateAndConstruct(other.size_, [&other](T* storage) {
      std::uninitialized_copy(other.begin(), other.end(), storage);
    });
  }

  FrozenList(FrozenList&& other) noexcept
      : ptr_{std::exchange(other.ptr_, nullptr)},
        size_{std::exchange(other.size_, 0)} {}

  FrozenList& operator=(FrozenList&& other) noexcept {
    if (this != &other) {
      ensureUninitialized();

      ptr_ = std::exchange(other.ptr_, nullptr);
      size_ = std::exchange(other.size_, 0);
    }

    return *this;
  }

  FrozenList& operator=(const FrozenList& other) {
    if (this != &other) {
      allocateAndConstruct(other.size_, [&other](T* storage) {
        std::uninitialized_copy(other.begin(), other.end(), storage);
      });
    }

    return *this;
  }

  // The size of the list.
  size_t size() const {
    return size_;
  }

  // Initialize the list with size value-initialized elements.
  void initialize(size_t size) {
    allocateAndConstruct(size, [size](T* storage) {
      std::uninitialized_value_construct_n(storage, size);
    });
  }

  // Initialize the list with size copies of the given value.
  void initialize(size_t size, const T& val) {
    allocateAndConstruct(size, [&val, size](T* storage) {
      std::uninitialized_fill_n(storage, size, val);
    });
  }

  // Provide the begin function for immutable range-based for-loop support.
  const T* begin() const {
    return ptr_;
  }

  // Provide the end function for immutable range-based for-loop support.
  const T* end() const {
    return ptr_ + size_;
  }

  // Provide the [] operator for accessing elements by index.
  T& operator[](size_t index) {
    return ptr_[index];
  }

  const T& operator[](size_t index) const {
    return ptr_[index];
  }

  // Like the [] operator, but throws an exception if the index is out of range.
  T& at(size_t index) {
    return const_cast<T&>(std::as_const(*this).at(index));
  }

  const T& at(size_t index) const {
    if (index >= size_) {
      throw std::out_of_range("Index out of range");
    }
    return ptr_[index];
  }

 private:
  // Raise an exception if the list already owns element storage.
  void ensureUninitialized() {
    if (ptr_ != nullptr) {
      throw std::runtime_error("Cannot initialize FrozenList more than once");
    }
  }

  // Construct size elements in newly allocated storage, publishing it only
  // after all construction succeeds. constructElements must destroy any
  // partially constructed elements before throwing; the uninitialized
  // algorithms used by callers provide this guarantee.
  template <typename ConstructElements>
  void allocateAndConstruct(size_t size, ConstructElements constructElements) {
    ensureUninitialized();

    if (size == 0) {
      return;
    }

    T* storage = std::allocator<T>{}.allocate(size);

    try {
      constructElements(storage);
    } catch (...) {
      // constructElements has already destroyed any partially built elements.
      std::allocator<T>{}.deallocate(storage, size);
      throw;
    }

    ptr_ = storage;
    size_ = size;
  }

  T* ptr_{nullptr};
  size_t size_{0};
};

} // namespace cinderx
