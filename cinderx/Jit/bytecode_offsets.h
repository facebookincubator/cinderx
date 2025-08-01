// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#if PY_VERSION_HEX >= 0x030D0000
#include "internal/pycore_code.h"
#endif

#include "cinderx/Common/log.h"

#include <concepts>
#include <limits>
#include <ostream>
#include <type_traits>

namespace jit {

/*
 * BCOffsetBase is used to define two related types: BCOffset and BCIndex.
 * BCOffset holds a byte offset into a code object, while BCIndex holds an
 * instruction index into a code object.
 *
 * They are both simple wrappers for an integral value (int, in the current
 * implementation, assuming the JIT won't have to deal with code objects
 * containing more than 2 GiB of bytecode), and support common comparison and
 * arithmetic operations. Conversion to or from a raw int must be explicit, but
 * implicit conversion between BCOffset and BCIndex is allowed, with appropriate
 * adjustments made to the value.
 */
template <typename T>
class BCOffsetBase {
 public:
  constexpr BCOffsetBase() = default;

  template <class TInt>
  explicit BCOffsetBase(TInt value) : value_{check(value)} {}

  // Explicit accessor for the underlying value.
  constexpr int value() const {
    return value_;
  }

  // Arithmetic operators.
  T operator+(Py_ssize_t other) const {
    return T{value() + other};
  }

  T operator-(Py_ssize_t other) const {
    return T{value() - other};
  }

  int operator-(T other) const {
    return value_ - other.value();
  }

  T operator*(Py_ssize_t other) const {
    return T{value() * other};
  }

  T& operator++() {
    value_++;
    return asT();
  }

  T operator++(int) {
    T old = asT();
    operator++();
    return old;
  }

  T& operator--() {
    value_--;
    return asT();
  }

  T operator--(int) {
    T old = asT();
    operator--();
    return old;
  }

 private:
  template <class TInt>
  int check(TInt v) {
    static_assert(std::is_integral_v<TInt>, "BCOffsetBase is an integer");
    JIT_DCHECK(
        v <= std::numeric_limits<int>::max(), "Overflow converting from {}", v);
    // Checking that an unsigned value is greater or equal than a negative value
    // would produce a compiler warning.
    if constexpr (std::is_signed_v<TInt>) {
      JIT_DCHECK(
          v >= std::numeric_limits<int>::min(),
          "Underflow converting from {}",
          v);
    }
    return static_cast<int>(v);
  }

  T& asT() {
    return static_cast<T&>(*this);
  }

  const T& asT() const {
    return static_cast<const T&>(*this);
  }

  int value_{0};
};

class BCIndex;

class BCOffset : public BCOffsetBase<BCOffset> {
 public:
  using BCOffsetBase::BCOffsetBase;

  /* implicit */ BCOffset(BCIndex idx);

  BCIndex asIndex() const;

  // Comparison operators.

  constexpr std::strong_ordering operator<=>(const BCOffset& other) const {
    return value() <=> other.value();
  }

  template <std::integral TInt>
  constexpr std::strong_ordering operator<=>(const TInt& other) const {
    return value() <=> other;
  }

  constexpr bool operator==(const BCOffset& other) const {
    return value() == other.value();
  }

  template <std::integral TInt>
  constexpr bool operator==(const TInt& other) const {
    return value() == other;
  }
};

class BCIndex : public BCOffsetBase<BCIndex> {
 public:
  using BCOffsetBase::BCOffsetBase;

  /* implicit */ BCIndex(BCOffset offset);

  BCOffset asOffset() const;

  // Comparison operators.

  constexpr std::strong_ordering operator<=>(const BCIndex& other) const {
    return value() <=> other.value();
  }

  template <std::integral TInt>
  constexpr std::strong_ordering operator<=>(const TInt& other) const {
    return value() <=> other;
  }

  constexpr bool operator==(const BCIndex& other) const {
    return value() == other.value();
  }

  template <std::integral TInt>
  constexpr bool operator==(const TInt& other) const {
    return value() == other;
  }
};

inline BCOffset::BCOffset(BCIndex idx)
    : BCOffset{idx.value() * int{sizeof(_Py_CODEUNIT)}} {}

inline BCIndex::BCIndex(BCOffset offset)
    : BCIndex{offset.value() / int{sizeof(_Py_CODEUNIT)}} {}

inline BCIndex BCOffset::asIndex() const {
  return BCIndex{*this};
}

inline BCOffset BCIndex::asOffset() const {
  return BCOffset{*this};
}

inline BCOffset operator+(const BCOffset& a, const BCOffset& b) {
  return BCOffset{a.value() + b.value()};
}

// Convenience operators for array access and printing.
inline _Py_CODEUNIT* operator+(_Py_CODEUNIT* code, BCIndex index) {
  return code + index.value();
}

inline std::ostream& operator<<(std::ostream& os, jit::BCOffset offset) {
  return os << offset.value();
}

inline std::ostream& operator<<(std::ostream& os, jit::BCIndex index) {
  return os << index.value();
}

inline auto format_as(const jit::BCOffset& offset) {
  return offset.value();
}

inline auto format_as(const jit::BCIndex& idx) {
  return idx.value();
}

} // namespace jit

template <>
struct std::hash<jit::BCOffset> {
  size_t operator()(const jit ::BCOffset& offset) const {
    return std::hash<int>{}(offset.value());
  }
};

template <>
struct std::hash<jit::BCIndex> {
  size_t operator()(const jit::BCIndex& index) const {
    return std::hash<int>{}(index.value());
  }
};
