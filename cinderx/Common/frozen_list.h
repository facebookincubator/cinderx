// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <memory>

namespace jit {

// A frozen list is effectively a vector that is dynamically allocated at
// runtime, but then can no longer be resized.
template <typename T>
  requires std::default_initializable<T> && std::copy_constructible<T>
class FrozenList {
 public:
  FrozenList() = default;
  ~FrozenList() = default;

  // Construct a frozen list from the given initializer list.
  /* implicit */ FrozenList(std::initializer_list<T> values) {
    reserve(values.size());
    std::copy(values.begin(), values.end(), ptr_.get());
  }

  FrozenList(const FrozenList& other) {
    reserve(other.size_);
    std::copy(other.begin(), other.end(), ptr_.get());
  }

  FrozenList(FrozenList&& other) noexcept {
    *this = std::move(other);
  }

  FrozenList& operator=(FrozenList&& other) noexcept {
    if (this != &other) {
      ensureUninitialized();

      size_ = other.size_;
      ptr_ = std::move(other.ptr_);

      other.size_ = 0;
      other.ptr_ = nullptr;
    }

    return *this;
  }

  FrozenList& operator=(const FrozenList& other) {
    if (this != &other) {
      reserve(other.size_);
      std::copy(other.begin(), other.end(), ptr_.get());
    }

    return *this;
  }

  // The size of the list.
  size_t size() const {
    return size_;
  }

  // Set the size of the frozen list and build a new pointer to the data, then
  // fill the data with the default value for the type.
  void resize(size_t size) {
    resize(size, T{});
  }

  // Set the size of the frozen list and build a new pointer to the data, then
  // fill the data with a copy of the given value.
  void resize(size_t size, const T& val) {
    reserve(size);
    std::fill(ptr_.get(), ptr_.get() + size, val);
  }

  // Provide the begin function for immutable range-based for-loop support.
  const T* begin() const {
    return ptr_.get();
  }

  // Provide the end function for immutable range-based for-loop support.
  const T* end() const {
    return ptr_.get() + size_;
  }

  // Provide the [] operator for accessing elements by index.
  T& operator[](size_t index) const {
    return ptr_[index];
  }

  // Like the [] operator, but throws an exception if the index is out of range.
  T& at(size_t index) const {
    if (index >= size_) {
      throw std::out_of_range("Index out of range");
    }
    return ptr_[index];
  }

 private:
  // Raise an exception if the list has already been initialized.
  void ensureUninitialized() {
    if (ptr_ != nullptr) {
      throw std::runtime_error("Cannot resize a frozen list twice");
    }
  }

  // Set the size of the frozen list and build a new pointer to the data.
  void reserve(size_t size) {
    ensureUninitialized();
    size_ = size;

    if (size != 0) {
      ptr_ = std::make_unique<T[]>(size);
    }
  }

  size_t size_{0};
  std::unique_ptr<T[]> ptr_;
};

} // namespace jit
