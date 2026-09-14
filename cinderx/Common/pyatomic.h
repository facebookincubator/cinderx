// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include <stdint.h>

#ifdef _MSC_VER
#include <intrin.h>
#endif

#ifdef __cplusplus
#define CI_ATOMIC_INLINE inline
#else
#define CI_ATOMIC_INLINE static inline
#endif

CI_ATOMIC_INLINE uint64_t
Ci_atomic_add_uint64_relaxed(uint64_t* obj, uint64_t value) {
#if _Py_USE_GCC_BUILTIN_ATOMICS
  return __atomic_fetch_add(obj, value, __ATOMIC_RELAXED);
#elif defined(__cplusplus) && defined(__cpp_lib_atomic_ref) && \
    __cpp_lib_atomic_ref >= 201806L
  return std::atomic_ref<uint64_t>(*obj).fetch_add(
      value, std::memory_order_relaxed);
#elif !defined(__cplusplus) && defined(__STDC_VERSION__) && \
    __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  _Py_USING_STD;
  return atomic_fetch_add_explicit(
      (_Atomic(uint64_t)*)obj, value, memory_order_relaxed);
#elif defined(_MSC_VER)
#if defined(_M_ARM64)
  Py_BUILD_ASSERT(sizeof(*obj) == sizeof(__int64));
  return (uint64_t)_InterlockedExchangeAdd64_nf(
      (volatile __int64*)obj, (__int64)value);
#else
  // CPython has no relaxed wrapper for this platform. The stronger ordering is
  // safe for an approximate counter.
  return _Py_atomic_add_uint64(obj, value);
#endif
#else
#error "no available pyatomic implementation for this platform/compiler"
#endif
}

CI_ATOMIC_INLINE uint64_t Ci_atomic_load_uint64_relaxed(const uint64_t* obj) {
#if _Py_USE_GCC_BUILTIN_ATOMICS
  return __atomic_load_n(obj, __ATOMIC_RELAXED);
#elif defined(__cplusplus) && defined(__cpp_lib_atomic_ref) && \
    __cpp_lib_atomic_ref >= 201806L
  return std::atomic_ref<uint64_t>(*const_cast<uint64_t*>(obj))
      .load(std::memory_order_relaxed);
#elif !defined(__cplusplus) && defined(__STDC_VERSION__) && \
    __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  _Py_USING_STD;
  return atomic_load_explicit(
      (const _Atomic(uint64_t)*)obj, memory_order_relaxed);
#elif defined(_MSC_VER)
  return *(const volatile uint64_t*)obj;
#else
#error "no available pyatomic implementation for this platform/compiler"
#endif
}

#undef CI_ATOMIC_INLINE
