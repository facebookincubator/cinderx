// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include <stddef.h>
#include <stdint.h>

#ifdef Py_GIL_DISABLED
#include "cinderx/Common/pyatomic.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Extra data attached to a code object.
typedef struct CodeExtra {
  union {
    struct {
      // Number of times the code object has been called.
      uint32_t calls;
      // Estimated number of bytecode code units executed in interpreted loops.
      uint32_t interpreted_bytecodes;
    } counters;
    // Used for unallocated free list code extras
    struct CodeExtra* next;
  };

  // Borrowed pointer to the cinderx::jit::NestedCompileData for this code
  // object, or NULL if the JIT hasn't registered one.  The JIT context owns
  // the data; keeping a pointer here is what lets any function creation path
  // find it.  See cinderx/Jit/nested_compile.h.
  void* jit_nested_compile_data;
} CodeExtra;

// These counters are approximate hotness signals. Their thresholds are well
// below UINT32_MAX, so overflow is acceptable. Under FT-Python, relaxed
// atomics avoid data races without synchronizing threads.
#ifdef Py_GIL_DISABLED

static inline void Ci_code_extra_incr_calls(CodeExtra* extra) {
  Ci_atomic_add_uint32_relaxed(&extra->counters.calls, 1);
}

static inline uint32_t Ci_code_extra_get_calls(const CodeExtra* extra) {
  return Ci_atomic_load_uint32_relaxed(&extra->counters.calls);
}

static inline void Ci_code_extra_add_interpreted_bytecodes(
    CodeExtra* extra,
    uint32_t bytecodes) {
  Ci_atomic_add_uint32_relaxed(
      &extra->counters.interpreted_bytecodes, bytecodes);
}

static inline uint32_t Ci_code_extra_get_interpreted_bytecodes(
    const CodeExtra* extra) {
  return Ci_atomic_load_uint32_relaxed(&extra->counters.interpreted_bytecodes);
}

// Acquire/release ordering publishes the initialized NestedCompileData to
// readers, which under free-threading can be on a thread that doesn't hold any
// of the JIT's locks.
static inline void* Ci_code_extra_get_nested_compile_data(
    const CodeExtra* extra) {
  return _Py_atomic_load_ptr_acquire(&extra->jit_nested_compile_data);
}

static inline void Ci_code_extra_set_nested_compile_data(
    CodeExtra* extra,
    void* data) {
  _Py_atomic_store_ptr_release(&extra->jit_nested_compile_data, data);
}

static inline void Ci_code_extra_clear_nested_compile_data(
    CodeExtra* extra,
    void* data) {
  _Py_atomic_compare_exchange_ptr(&extra->jit_nested_compile_data, &data, NULL);
}

#else

static inline void Ci_code_extra_incr_calls(CodeExtra* extra) {
  extra->counters.calls += 1;
}

static inline uint32_t Ci_code_extra_get_calls(const CodeExtra* extra) {
  return extra->counters.calls;
}

static inline void Ci_code_extra_add_interpreted_bytecodes(
    CodeExtra* extra,
    uint32_t bytecodes) {
  extra->counters.interpreted_bytecodes += bytecodes;
}

static inline uint32_t Ci_code_extra_get_interpreted_bytecodes(
    const CodeExtra* extra) {
  return extra->counters.interpreted_bytecodes;
}

static inline void* Ci_code_extra_get_nested_compile_data(
    const CodeExtra* extra) {
  return extra->jit_nested_compile_data;
}

static inline void Ci_code_extra_set_nested_compile_data(
    CodeExtra* extra,
    void* data) {
  extra->jit_nested_compile_data = data;
}

static inline void Ci_code_extra_clear_nested_compile_data(
    CodeExtra* extra,
    void* data) {
  if (extra->jit_nested_compile_data == data) {
    extra->jit_nested_compile_data = NULL;
  }
}

#endif

#ifdef __cplusplus
}
#endif
