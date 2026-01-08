// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include <cstdint>
#include <limits>
#include <type_traits>

#ifdef __cplusplus
#include "cinderx/Common/log.h"

#include <charconv>
#include <concepts>
#include <cstdarg>
#include <cstddef>
#include <memory>
#include <optional>
#include <queue>
#include <string_view>
#include <unordered_set>
#include <utility>

#define DISALLOW_COPY_AND_ASSIGN(klass) \
  klass(const klass&) = delete;         \
  klass& operator=(const klass&) = delete

#define UNUSED __attribute__((unused))

extern "C" {
#endif

struct jit_string_t* ss_alloc(void);
void ss_free(struct jit_string_t* ss);
void ss_reset(struct jit_string_t* ss);
int ss_is_empty(const struct jit_string_t* ss);
const char* ss_get_string(const struct jit_string_t* ss);
int ss_vsprintf(struct jit_string_t* ss, const char* format, va_list args);
int ss_sprintf(struct jit_string_t* ss, const char* format, ...);
struct jit_string_t* ss_sprintf_alloc(const char* format, ...);

#ifdef __cplusplus
}

constexpr bool kPyDebug =
#ifdef Py_DEBUG
    true;
#else
    false;
#endif

constexpr bool kPyRefDebug =
#ifdef Py_REF_DEBUG
    true;
#else
    false;
#endif

constexpr bool kImmortalInstances =
#if defined(Py_IMMORTAL_INSTANCES) || PY_VERSION_HEX >= 0x030C0000
    true;
#else
    false;
#endif

struct jit_string_deleter {
  void operator()(jit_string_t* ss) const {
    ss_free(ss);
  }
};

using auto_jit_string_t = std::unique_ptr<jit_string_t, jit_string_deleter>;

const char* ss_get_string(const auto_jit_string_t& ss);

// Loading a method returns up to 2 items, for one of three possible outcomes:
// * A callable plus an object instance (self).
// * A bound method.
// * Error.
//
// Prior to 3.14 in CPython, the first element returned indicated if we had a
// bound method through being nullptr. However, we wanted to use nullptr to
// trigger a deopt for the error case so instead the JIT used Py_None and
// handled this in the runtime.
//
// After 3.14 things are simpler and we always have a callable as the first
// element, so free to use nullptr on error to trigger a deopt.
struct LoadMethodResult {
  LoadMethodResult() = default;
  LoadMethodResult(PyObject* none_or_callable, PyObject* inst_or_callable) {
    if constexpr (PY_VERSION_HEX >= 0x030E0000) {
      if (none_or_callable == nullptr) {
        JIT_CHECK(
            inst_or_callable == nullptr, "Error, both args should be nullptr");
        callable = self_or_null = nullptr;
      } else if (none_or_callable == Py_None) {
        callable = inst_or_callable;
        self_or_null = nullptr;
      } else {
        callable = none_or_callable;
        self_or_null = inst_or_callable;
      }
    } else {
      callable = none_or_callable;
      self_or_null = inst_or_callable;
    }
  }
  PyObject* callable;
  PyObject* self_or_null;
};

// Per-function entry point function to resume a JIT generator. Arguments are:
//   - Generator instance to be resumed.
//   - A value to send in or NULL to raise the current global error on resume.
//   - A boolean indicating if we need to break out of the current yield-from.
//   - The current thread-state instance.
//  Returns result of computation which is a "yielded" value unless the state of
//  the generator is _PyJITGenState_Completed, in which case it is a "return"
//  value. If the return is NULL, an exception has been raised.
using GenResumeFunc = PyObject* (*)(PyObject * gen,
                                    PyObject* send_value,
                                    uint64_t finish_yield_from,
                                    PyThreadState* tstate);

namespace jit {

constexpr int kPointerSize = sizeof(void*);

constexpr size_t kStackAlign = 16;

constexpr int kKiB = 1024;
constexpr int kMiB = kKiB * kKiB;
constexpr int kGiB = kKiB * kKiB * kKiB;

#if defined(__x86_64__) || defined(__aarch64__)
constexpr int kPageSize = 4 * kKiB;
#else
#error Please define kPageSize for the current architecture
#endif

template <typename T>
constexpr bool isPowerOfTwo(T x) {
  return (x & (x - 1)) == 0;
}

template <typename T>
constexpr T roundDown(T x, size_t n) {
  if (n == 0) {
    return n;
  }
  JIT_DCHECK(isPowerOfTwo(n), "Must be 0 or a power of 2");
  return (x & -n);
}

template <typename T>
constexpr T roundUp(T x, size_t n) {
  if (n == 0) {
    return n;
  }
  return roundDown(x + n - 1, n);
}

template <typename T1, typename T2>
  requires std::is_integral_v<T1> && std::is_integral_v<T2>
constexpr std::common_type_t<T1, T1> ceilDiv(T1 a, T2 b) {
  return (a + b - 1) / b;
}

constexpr int kCoFlagsAnyGenerator =
    CO_ASYNC_GENERATOR | CO_COROUTINE | CO_GENERATOR | CO_ITERABLE_COROUTINE;

// If stable pointers are enabled (with a call to setUseStablePointers(true))
// return 0xdeadbeef. Otherwise, return the original pointer.
const void* getStablePointer(const void* ptr);

// Enable or disable pointer sanitization.
void setUseStablePointers(bool enable);

constexpr std::size_t combineHash(std::size_t seed, std::size_t hash) {
  return seed ^ (hash + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

template <class... Args>
constexpr std::size_t
combineHash(std::size_t seed, std::size_t hash, Args&&... args) {
  return combineHash(combineHash(seed, hash), std::forward<Args&&>(args)...);
}

template <class T>
std::optional<T> parseNumber(std::string_view s) {
  T n = 0;
  auto result = std::from_chars(s.begin(), s.end(), n);
  if (result.ec == std::errc{}) {
    return n;
  }
  return std::nullopt;
}

// Return the given PyUnicodeObject as a std::string, or "" if an error occurs.
std::string unicodeAsString(PyObject* str);

// Convert a C++ string into a Python unicode object.  Will return nullptr on
// error.
Ref<> stringAsUnicode(std::string_view str);

inline int popcount(unsigned i) {
  return __builtin_popcount(i);
}

inline int popcount(unsigned long i) {
  return __builtin_popcountl(i);
}

inline int popcount(unsigned long long i) {
  return __builtin_popcountll(i);
}

// Look up an item in the given map. Always abort if key doesn't exist.
template <typename M, typename K>
auto& map_get_strict(M& map, const K& key) {
  auto it = map.find(key);
  JIT_CHECK(it != map.end(), "Key not found in map");
  return it->second;
}

// Look up an item in the given map, aborting if the key doesn't exist. Similar
// to map.at(key) but with a less opaque failure mode.
template <typename M, typename K>
auto& map_get(M& map, const K& key) {
  auto it = map.find(key);
  JIT_DCHECK(it != map.end(), "Key not found in map");
  return it->second;
}

// Look up an item in the given map. If the key doesn't exist, return the
// default value.
template <typename M>
const typename M::mapped_type map_get(
    M& map,
    const typename M::key_type& key,
    const typename M::mapped_type& def) {
  auto it = map.find(key);
  if (it == map.end()) {
    return def;
  }
  return it->second;
}

// A queue that doesn't enqueue items that are already present. Items must be
// hashable with std::hash.
template <typename T>
class Worklist {
 public:
  bool empty() const {
    return queue_.empty();
  }

  const T& front() const {
    JIT_DCHECK(!empty(), "Worklist is empty");
    return queue_.front();
  }

  void push(const T& item) {
    if (set_.insert(item).second) {
      queue_.push(item);
    }
  }

  void pop() {
    set_.erase(front());
    queue_.pop();
  }

 private:
  std::queue<T> queue_;
  std::unordered_set<T> set_;
};

template <int N, std::integral T>
bool fitsSignedInt(T val) {
  static_assert(N > 0 && N <= 64, "N must be between 1 and 64");
  if constexpr (N == 64) {
    return std::cmp_less_equal(val, std::numeric_limits<int64_t>::max()) &&
        std::cmp_greater_equal(val, std::numeric_limits<int64_t>::min());
  } else {
    constexpr int64_t max_val = (int64_t{1} << (N - 1)) - 1;
    constexpr int64_t min_val = -(int64_t{1} << (N - 1));
    return std::cmp_less_equal(val, max_val) &&
        std::cmp_greater_equal(val, min_val);
  }
}

template <int N, typename T>
  requires std::is_pointer_v<T>
bool fitsSignedInt(T val) {
  return fitsSignedInt<N>(reinterpret_cast<intptr_t>(val));
}

// std::unique_ptr for objects created with std::malloc() rather than new.
struct FreeDeleter {
  void operator()(void* ptr) const {
    std::free(ptr);
  }
};
template <typename T>
using unique_c_ptr = std::unique_ptr<T, FreeDeleter>;

template <class T>
class ScopeExit {
 public:
  ScopeExit(T&& action) : lambda_(std::move(action)) {}
  ~ScopeExit() {
    lambda_();
  }

 private:
  T lambda_;
};

#define SCOPE_EXIT_INTERNAL2(lname, aname, ...) \
  auto lname = [&]() { __VA_ARGS__; };          \
  jit::ScopeExit<decltype(lname)> aname(std::move(lname));

#define SCOPE_EXIT_TOKENPASTE(x, y) SCOPE_EXIT_##x##y

#define SCOPE_EXIT_INTERNAL1(ctr, ...)       \
  SCOPE_EXIT_INTERNAL2(                      \
      SCOPE_EXIT_TOKENPASTE(func_, ctr),     \
      SCOPE_EXIT_TOKENPASTE(instance_, ctr), \
      __VA_ARGS__)

#define SCOPE_EXIT(...) SCOPE_EXIT_INTERNAL1(__COUNTER__, __VA_ARGS__)

// Return a crc32 checksum of the bytecode for the given code object.
// A frozen list is effectively a vector that is dynamically allocated at
// runtime, but then can no longer be resized.
template <typename T>
class FrozenList {
 public:
  FrozenList() = default;

  // Make FrozenList copy constructible.
  FrozenList(const FrozenList& other) {
    reserve(other.size_);
    std::copy(other.begin(), other.end(), ptr_.get());
  }

  // Make FrozenList move constructible.
  FrozenList(FrozenList&& other) noexcept {
    *this = std::move(other);
  }

  // Make FrozenList move assignable.
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

  // Construct a frozen list from the given initializer list.
  /* implicit */ FrozenList(std::initializer_list<T> values) {
    reserve(values.size());
    std::copy(values.begin(), values.end(), ptr_.get());
  }

  // Make FrozenList copy assignable.
  FrozenList& operator=(const FrozenList& other) {
    if (this != &other) {
      reserve(other.size_);
      std::copy(other.begin(), other.end(), ptr_.get());
    }

    return *this;
  }

  // Destroy a frozen list.
  ~FrozenList() = default;

  // The size of the list.
  size_t size() const {
    return size_;
  }

  // Set the size of the frozen list and build a new pointer to the data, then
  // fill the data with the default value for the type.
  //
  // In order to call this function, T must be default constructible.
  void resize(size_t size) {
    resize(size, T{});
  }

  // Set the size of the frozen list and build a new pointer to the data, then
  // fill the data with a copy of the given value.
  //
  // In order to call this function, T must be copy constructible.
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
  size_t size_{0};
  std::unique_ptr<T[]> ptr_;

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
};

using FuncVisitor = void (*)(BorrowedRef<PyFunctionObject>);

inline void walkFunctionObjects(FuncVisitor visitor) {
  auto wrapper = [](PyObject* obj, void* arg) {
    if (PyFunction_Check(obj)) {
      BorrowedRef<PyFunctionObject> func{obj};
      reinterpret_cast<FuncVisitor>(arg)(func);
    }
    return 1;
  };

  PyUnstable_GC_VisitObjects(wrapper, reinterpret_cast<void*>(visitor));
}

} // namespace jit

template <typename D, typename S>
inline constexpr D bit_cast(const S& src) {
  static_assert(sizeof(S) == sizeof(D), "src and dst must be the same size");
  static_assert(
      std::is_scalar_v<D> && std::is_scalar_v<S>,
      "both src and dst must be of scalar type.");
  D dst;
  std::memcpy(&dst, &src, sizeof(dst));
  return dst;
}

#endif

// this is for non-test builds. define FRIEND_TEST here so we don't
// have to include the googletest header in our headers to be tested.
#ifndef FRIEND_TEST
#define FRIEND_TEST(test_case_name, test_name) friend class test_case_name
#endif
