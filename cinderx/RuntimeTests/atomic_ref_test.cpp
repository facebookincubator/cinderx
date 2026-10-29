// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/python.h"

#include "internal/pycore_code.h"

#include <gtest/gtest.h>

#include "cinderx/Common/atomic_ref.h"

#include <atomic>
#include <cstdint>
#include <type_traits>

using namespace cinderx;

namespace {

struct Instruction {
  uint8_t opcode;
  uint8_t arg;

  bool operator==(const Instruction&) const = default;
};

struct NoDefaultConstructor {
  NoDefaultConstructor() = delete;
  explicit NoDefaultConstructor(uint32_t value) : value{value} {}
  uint32_t value;
};

static_assert(AtomicRef<uint8_t>::kIsAlwaysLockFree);
static_assert(AtomicRef<uint64_t>::kIsAlwaysLockFree);
static_assert(AtomicRef<_Py_CODEUNIT>::kIsAlwaysLockFree);
static_assert(std::is_trivially_copyable_v<NoDefaultConstructor>);

#if !defined(__cpp_lib_atomic_ref) || __cpp_lib_atomic_ref < 201806L
static_assert(
    detail::atomicMemoryOrder(std::memory_order_relaxed) == __ATOMIC_RELAXED);
static_assert(
    detail::atomicMemoryOrder(std::memory_order_consume) == __ATOMIC_CONSUME);
static_assert(
    detail::atomicMemoryOrder(std::memory_order_acquire) == __ATOMIC_ACQUIRE);
static_assert(
    detail::atomicMemoryOrder(std::memory_order_release) == __ATOMIC_RELEASE);
static_assert(
    detail::atomicMemoryOrder(std::memory_order_acq_rel) == __ATOMIC_ACQ_REL);
static_assert(
    detail::atomicMemoryOrder(std::memory_order_seq_cst) == __ATOMIC_SEQ_CST);
#endif

TEST(AtomicRefTest, LoadStoreInteger) {
  alignas(AtomicRef<uint64_t>::kRequiredAlignment) uint64_t value = 0;
  const AtomicRef<uint64_t> ref{value};

  ref.store(0x123456789abcdef0, std::memory_order_relaxed);
  EXPECT_EQ(ref.load(std::memory_order_relaxed), 0x123456789abcdef0);
  ref.store(42, std::memory_order_release);
  EXPECT_EQ(ref.load(std::memory_order_acquire), 42);
}

TEST(AtomicRefTest, LoadStoreAggregateWithWeakDeclaredAlignment) {
  alignas(AtomicRef<Instruction>::kRequiredAlignment) Instruction value{3, 17};
  const AtomicRef<Instruction> ref{value};

  EXPECT_EQ(ref.load(std::memory_order_relaxed), (Instruction{3, 17}));
  ref.store(Instruction{9, 42}, std::memory_order_relaxed);
  EXPECT_EQ(ref.load(std::memory_order_relaxed), (Instruction{9, 42}));
}

TEST(AtomicRefTest, LoadWithoutDefaultConstructor) {
  alignas(AtomicRef<NoDefaultConstructor>::kRequiredAlignment)
      NoDefaultConstructor value{42};
  const AtomicRef<NoDefaultConstructor> ref{value};

  EXPECT_EQ(ref.load(std::memory_order_relaxed).value, 42);
}

TEST(AtomicRefTest, LoadStoreCodeUnit) {
  alignas(AtomicRef<_Py_CODEUNIT>::kRequiredAlignment) _Py_CODEUNIT value =
      _Py_MAKE_CODEUNIT(3, 17);
  const AtomicRef<_Py_CODEUNIT> ref{value};

  ref.store(_Py_MAKE_CODEUNIT(9, 42), std::memory_order_relaxed);
  _Py_CODEUNIT loaded = ref.load(std::memory_order_relaxed);
  EXPECT_EQ(_Py_OPCODE(loaded), 9);
  EXPECT_EQ(_Py_OPARG(loaded), 42);
}

TEST(AtomicRefTest, LoadStorePointer) {
  int first = 1;
  int second = 2;
  alignas(AtomicRef<int*>::kRequiredAlignment) int* value = &first;
  const AtomicRef<int*> ref{value};

  EXPECT_EQ(ref.load(std::memory_order_acquire), &first);
  ref.store(&second, std::memory_order_relaxed);
  EXPECT_EQ(ref.load(std::memory_order_relaxed), &second);
}

int vectorcallTarget(int value) {
  return value + 1;
}

TEST(AtomicRefTest, LoadStoreFunctionPointer) {
  using Function = int (*)(int);
  alignas(AtomicRef<Function>::kRequiredAlignment) Function value = nullptr;
  const AtomicRef<Function> ref{value};

  ref.store(vectorcallTarget, std::memory_order_relaxed);
  EXPECT_EQ(ref.load(std::memory_order_relaxed), vectorcallTarget);
}

TEST(AtomicRefTest, CompareExchangeFailureUpdatesExpected) {
  alignas(AtomicRef<uint8_t>::kRequiredAlignment) uint8_t value = 3;
  const AtomicRef<uint8_t> ref{value};
  uint8_t expected = 1;

  EXPECT_FALSE(ref.compareExchangeWeak(
      expected, 7, std::memory_order_acquire, std::memory_order_relaxed));
  EXPECT_EQ(expected, 3);
  EXPECT_EQ(ref.load(std::memory_order_relaxed), 3);
}

TEST(AtomicRefTest, CompareExchangeSuccessPreservesOtherBits) {
  constexpr uint8_t kLockBit = 0x10;
  alignas(AtomicRef<uint8_t>::kRequiredAlignment) uint8_t value = 3;
  const AtomicRef<uint8_t> ref{value};
  uint8_t expected = ref.load(std::memory_order_relaxed);
  bool acquired = false;
  for (int attempt = 0; attempt < 1000 && !acquired; attempt++) {
    acquired = ref.compareExchangeWeak(
        expected,
        static_cast<uint8_t>(expected | kLockBit),
        std::memory_order_acquire,
        std::memory_order_relaxed);
  }

  ASSERT_TRUE(acquired);
  EXPECT_EQ(expected, 3);
  EXPECT_EQ(ref.load(std::memory_order_relaxed), 3 | kLockBit);
}

TEST(AtomicRefTest, FetchAndReturnsOldValueAndPreservesOtherBits) {
  constexpr uint8_t kLockBit = 0x10;
  alignas(AtomicRef<uint8_t>::kRequiredAlignment) uint8_t value = 3 | kLockBit;
  const AtomicRef<uint8_t> ref{value};

  EXPECT_EQ(
      ref.fetchAnd(static_cast<uint8_t>(~kLockBit), std::memory_order_release),
      3 | kLockBit);
  EXPECT_EQ(ref.load(std::memory_order_relaxed), 3);
}

} // namespace
