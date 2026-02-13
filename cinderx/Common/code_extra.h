// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Extra data attached to a code object.
typedef struct CodeExtra {
  union {
    // Number of times the code object has been called.
    uint64_t calls;
    // Used for unallocated free list code extras
    struct CodeExtra* next;
  };
} CodeExtra;

// Thread-safe accessors for CodeExtra::calls.
// Under FT-Python, these use atomics to avoid data races.
#ifdef Py_GIL_DISABLED

// Note: _Py_atomic_add_uint64 uses seq_cst ordering, which might be stronger
// than needed for the calls counter. On x86-64, this is the same cost as
// relaxed (both emit lock xaddq). On ARM, a relaxed variant would be cheaper
// but there is no _Py_atomic_add_uint64_relaxed.
static inline void Ci_code_extra_incr_calls(CodeExtra* extra) {
  _Py_atomic_add_uint64(&extra->calls, 1);
}

static inline uint64_t Ci_code_extra_get_calls(const CodeExtra* extra) {
  return _Py_atomic_load_uint64_relaxed(&extra->calls);
}

#else

static inline void Ci_code_extra_incr_calls(CodeExtra* extra) {
  extra->calls += 1;
}

static inline uint64_t Ci_code_extra_get_calls(const CodeExtra* extra) {
  return extra->calls;
}

#endif

#ifdef __cplusplus
}
#endif
