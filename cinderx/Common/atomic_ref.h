// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/log.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace cinderx {

// A non-owning atomic view of existing storage. The object must outlive every
// view and satisfy kRequiredAlignment. Concurrent accesses must use compatible
// atomic operations; this does not make ordinary accesses to the object safe.
// Only the operations needed by CinderX are exposed.
#if defined(__cpp_lib_atomic_ref) && __cpp_lib_atomic_ref >= 201806L
template <typename T>
class AtomicRef {
 public:
  static_assert(std::is_trivially_copyable_v<T>);
  static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>);

  static constexpr bool kIsAlwaysLockFree =
      std::atomic_ref<T>::is_always_lock_free;
  static constexpr size_t kRequiredAlignment =
      std::atomic_ref<T>::required_alignment;

  explicit AtomicRef(T& object) noexcept : ref_(checkAlignment(object)) {}
  AtomicRef(const AtomicRef&) noexcept = default;
  AtomicRef& operator=(const AtomicRef&) = delete;

  T load(std::memory_order order) const noexcept {
    return ref_.load(order);
  }

  void store(T value, std::memory_order order) const noexcept {
    ref_.store(value, order);
  }

  bool compareExchangeWeak(
      T& expected,
      T desired,
      std::memory_order success,
      std::memory_order failure) const noexcept
    requires(std::is_integral_v<T> || std::is_pointer_v<T>)
  {
    return ref_.compare_exchange_weak(expected, desired, success, failure);
  }

  T fetchAnd(T value, std::memory_order order) const noexcept
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
  {
    return ref_.fetch_and(value, order);
  }

 private:
  static T& checkAlignment(T& object) noexcept {
    JIT_DCHECK(
        reinterpret_cast<uintptr_t>(std::addressof(object)) %
                kRequiredAlignment ==
            0,
        "Unaligned AtomicRef object");
    return object;
  }

  std::atomic_ref<T> ref_;
};
#elif defined(__GNUC__) || defined(__clang__)
namespace detail {

constexpr int atomicMemoryOrder(std::memory_order order) {
  switch (order) {
    case std::memory_order_relaxed:
      return __ATOMIC_RELAXED;
    case std::memory_order_consume:
      return __ATOMIC_CONSUME;
    case std::memory_order_acquire:
      return __ATOMIC_ACQUIRE;
    case std::memory_order_release:
      return __ATOMIC_RELEASE;
    case std::memory_order_acq_rel:
      return __ATOMIC_ACQ_REL;
    case std::memory_order_seq_cst:
      return __ATOMIC_SEQ_CST;
  }
  JIT_ABORT("Invalid atomic memory order");
}

} // namespace detail

template <typename T>
class AtomicRef {
 public:
  static_assert(std::is_trivially_copyable_v<T>);
  static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>);

  static constexpr bool kIsAlwaysLockFree =
      __atomic_always_lock_free(sizeof(T), nullptr);
  // Naturally align power-of-two-sized objects, including aggregates whose
  // declared alignment may be weaker than the atomic operation requires.
  static constexpr size_t kRequiredAlignment = std::has_single_bit(sizeof(T)) &&
          sizeof(T) > alignof(T)
      ? sizeof(T)
      : alignof(T);

  explicit AtomicRef(T& object) noexcept {
    JIT_DCHECK(
        reinterpret_cast<uintptr_t>(std::addressof(object)) %
                kRequiredAlignment ==
            0,
        "Unaligned AtomicRef object");
    object_ = static_cast<T*>(
        __builtin_assume_aligned(std::addressof(object), kRequiredAlignment));
  }
  AtomicRef(const AtomicRef&) noexcept = default;
  AtomicRef& operator=(const AtomicRef&) = delete;

  T load(std::memory_order order) const noexcept {
    alignas(T) std::array<std::byte, sizeof(T)> bytes;
    __atomic_load(
        object_,
        reinterpret_cast<T*>(bytes.data()),
        detail::atomicMemoryOrder(order));
    return std::bit_cast<T>(bytes);
  }

  void store(T value, std::memory_order order) const noexcept {
    __atomic_store(object_, &value, detail::atomicMemoryOrder(order));
  }

  bool compareExchangeWeak(
      T& expected,
      T desired,
      std::memory_order success,
      std::memory_order failure) const noexcept
    requires(std::is_integral_v<T> || std::is_pointer_v<T>)
  {
    return __atomic_compare_exchange(
        object_,
        &expected,
        &desired,
        /*weak=*/true,
        detail::atomicMemoryOrder(success),
        detail::atomicMemoryOrder(failure));
  }

  T fetchAnd(T value, std::memory_order order) const noexcept
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
  {
    return __atomic_fetch_and(object_, value, detail::atomicMemoryOrder(order));
  }

 private:
  T* object_;
};
#else
#error "No available AtomicRef implementation for this compiler/library"
#endif

} // namespace cinderx
