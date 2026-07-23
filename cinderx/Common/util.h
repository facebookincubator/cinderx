// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#ifdef Py_GIL_DISABLED
#include "internal/pycore_stackref.h"
#endif

#include "cinderx/Common/define.h"
#include "cinderx/Common/log.h"

#include <atomic>
#include <bit>
#include <charconv>
#include <concepts>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

#define DISALLOW_COPY_AND_ASSIGN(klass) \
  klass(const klass&) = delete;         \
  klass& operator=(const klass&) = delete

#define DISALLOW_MOVE_AND_ASSIGN(klass) \
  klass(klass&&) = delete;              \
  klass& operator=(klass&&) = delete

#define UNUSED __attribute__((unused))

// This is for non-test builds. define FRIEND_TEST here so we don't have to
// include the googletest header in our headers to be tested.
#ifndef FRIEND_TEST
#define FRIEND_TEST(test_case_name, test_name) friend class test_case_name
#endif

namespace cinderx {

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

// Tagged PyObject values follow CPython's _PyStackRef tagging scheme in
// free-threaded builds. GIL builds keep using plain PyObject* values.
#ifdef Py_GIL_DISABLED
using TaggedPyObject = _PyStackRef;
constexpr uintptr_t kDeferredRcTag = Py_TAG_DEFERRED;
constexpr uintptr_t kPyObjectPtrTag = Py_TAG_PTR;
constexpr uintptr_t kPyObjectTagBits = Py_TAG_BITS;
#else
using TaggedPyObject = PyObject*;
constexpr uintptr_t kDeferredRcTag = 0;
constexpr uintptr_t kPyObjectPtrTag = 0;
constexpr uintptr_t kPyObjectTagBits = 0;
#endif

// `kPyObjectPtrTag` being zero lets us treat an untagged PyObject* as a
// TaggedPyObject with no extra masking — see `untaggedPyObjectRef`.

constexpr uint64_t kDeferredRcTagBit =
    kFreeThreadedBuild ? std::countr_zero(kDeferredRcTag) : 0;

inline uintptr_t taggedPyObjectBits(TaggedPyObject obj) {
#ifdef Py_GIL_DISABLED
  return obj.bits;
#else
  return reinterpret_cast<uintptr_t>(obj);
#endif
}

inline bool isDeferredRcTagged(uint64_t raw) {
  return kPyObjectTagBits != 0 && (raw & kPyObjectTagBits) == kDeferredRcTag;
}

inline bool isDeferredRcTagged(TaggedPyObject obj) {
  return isDeferredRcTagged(taggedPyObjectBits(obj));
}

inline uint64_t stripDeferredRcTag(uint64_t raw) {
  return raw & ~static_cast<uint64_t>(kPyObjectTagBits);
}

inline PyObject* untaggedPyObject(TaggedPyObject obj) {
  return reinterpret_cast<PyObject*>(
      stripDeferredRcTag(taggedPyObjectBits(obj)));
}

inline TaggedPyObject taggedPyObject(
    PyObject* obj,
    [[maybe_unused]] uintptr_t tag) {
#ifdef Py_GIL_DISABLED
  return {reinterpret_cast<uintptr_t>(obj) | tag};
#else
  return obj;
#endif
}

inline TaggedPyObject untaggedPyObjectRef(PyObject* obj) {
  return taggedPyObject(obj, kPyObjectPtrTag);
}

inline TaggedPyObject addDeferredRcTag(PyObject* obj) {
  return taggedPyObject(obj, kDeferredRcTag);
}

} // namespace jit

constexpr int kPointerSize = sizeof(void*);

constexpr size_t kStackAlign = 16;
constexpr size_t kVecDSize = 16;

constexpr int kKiB = 1024;
constexpr int kMiB = kKiB * kKiB;
constexpr int kGiB = kKiB * kKiB * kKiB;

#if defined(__x86_64__) || defined(__aarch64__) || defined(_M_AMD64)
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
  auto result = std::from_chars(&s.front(), (&s.back()) + 1, n);
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

inline void* malloc_aligned(size_t size, size_t alignment) {
#ifdef WIN32
  return _aligned_malloc(size, alignment);
#else
  void* chunk = nullptr;
  int result = posix_memalign(&chunk, alignment, size);
  if (result) {
    return nullptr;
  }
  return chunk;
#endif
}

inline void free_aligned(void* ptr) {
#ifdef WIN32
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

// std::unique_ptr for objects created with std::malloc() rather than new.
struct FreeDeleter {
  void operator()(void* ptr) const {
    std::free(ptr);
  }
};
template <typename T>
using unique_c_ptr = std::unique_ptr<T, FreeDeleter>;

struct AlignedDeleter {
  void operator()(void* ptr) const {
    free_aligned(ptr);
  }
};
template <typename T>
using unique_aligned_ptr = std::unique_ptr<T, AlignedDeleter>;

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

// RAII wrapper for PyCriticalSection. Under GIL builds the class is empty
// and the constructor/destructor are trivial no-ops.
class CriticalSectionGuard final {
 public:
  explicit CriticalSectionGuard([[maybe_unused]] PyObject* obj) noexcept {
#ifdef Py_GIL_DISABLED
    PyCriticalSection_Begin(&cs_, obj);
#endif
  }
  ~CriticalSectionGuard() {
#ifdef Py_GIL_DISABLED
    PyCriticalSection_End(&cs_);
#endif
  }
  CriticalSectionGuard(const CriticalSectionGuard&) = delete;
  CriticalSectionGuard& operator=(const CriticalSectionGuard&) = delete;
  CriticalSectionGuard(CriticalSectionGuard&&) = delete;
  CriticalSectionGuard& operator=(CriticalSectionGuard&&) = delete;

 private:
#ifdef Py_GIL_DISABLED
  PyCriticalSection cs_;
#endif
};

// Typed, cross-version equivalent of CPython's FT_ATOMIC_LOAD_PTR_ACQUIRE().
template <typename T>
T* ftAtomicLoadPtrAcquire(T*& ptr) noexcept {
  if constexpr (kFreeThreadedBuild) {
#ifdef __cpp_lib_atomic_ref
    return std::atomic_ref<T*>(ptr).load(std::memory_order_acquire);
#else
    return __atomic_load_n(&ptr, __ATOMIC_ACQUIRE);
#endif
  } else {
    return ptr;
  }
}

#define SCOPE_EXIT_INTERNAL2(lname, aname, ...) \
  auto lname = [&]() { __VA_ARGS__; };          \
  cinderx::ScopeExit<decltype(lname)> aname(std::move(lname));

#define SCOPE_EXIT_TOKENPASTE(x, y) SCOPE_EXIT_##x##y

#define SCOPE_EXIT_INTERNAL1(ctr, ...)       \
  SCOPE_EXIT_INTERNAL2(                      \
      SCOPE_EXIT_TOKENPASTE(func_, ctr),     \
      SCOPE_EXIT_TOKENPASTE(instance_, ctr), \
      __VA_ARGS__)

#define SCOPE_EXIT(...) SCOPE_EXIT_INTERNAL1(__COUNTER__, __VA_ARGS__)

// Relaxed atomic store for func->vectorcall for thread-safe writes under
// free-threading and to satisfy TSAN. A release store might be the right
// choice in some cases to publish JIT metadata to readers, but CPython's
// _PyVectorcall_FunctionInline does a plain (non-acquire) load, so
// release/acquire isn't achievable without CPython changes.
// Under the GIL this is unnecessary, but relaxed has no overhead so we skip
// the Py_GIL_DISABLED guard.
inline void setVectorcall(
    BorrowedRef<PyFunctionObject> func,
    vectorcallfunc entry) {
#ifdef __cpp_lib_atomic_ref
  std::atomic_ref<vectorcallfunc>(func->vectorcall)
      .store(entry, std::memory_order_relaxed);
#else
  __atomic_store_n(&func->vectorcall, entry, __ATOMIC_RELAXED);
#endif
}

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

} // namespace cinderx
