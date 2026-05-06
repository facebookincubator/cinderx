// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/codegen/arch/detection.h"

#include <cstddef>

#if defined(__has_feature)
#define CINDER_TSAN_HAS_FEATURE(x) __has_feature(x)
#else
#define CINDER_TSAN_HAS_FEATURE(x) 0
#endif

#if defined(__SANITIZE_THREAD__) || CINDER_TSAN_HAS_FEATURE(thread_sanitizer)
#define CINDER_TSAN_ENABLED 1
#else
#define CINDER_TSAN_ENABLED 0
#endif

// JIT TSAN instrumentation is currently implemented for Linux x86-64. Other
// platforms can still use TSAN for non-JIT code.
#if CINDER_TSAN_ENABLED && defined(CINDER_X86_64) && defined(__linux__)
#define CINDER_JIT_TSAN_ENABLED 1
#else
#define CINDER_JIT_TSAN_ENABLED 0
#endif

namespace jit::lir {
class OperandBase;
}

namespace jit::codegen {

struct Environ;

inline constexpr bool kCinderJitTsanEnabled = CINDER_JIT_TSAN_ENABLED;

// Current coverage scope: these helpers are only wired into kMove and
// kMoveRelaxed lowering. Other LIR opcodes that can touch memory (for example
// arithmetic, cmp/test, push/pop, div/idiv, and indirect calls) are not TSAN
// instrumented yet.

#if CINDER_JIT_TSAN_ENABLED

// Emit TSAN read instrumentation before a memory load.
// access_size_in_bytes must match the width of the emitted memory access.
void emitTsanRead(
    Environ& env,
    const jit::lir::OperandBase* mem_operand,
    size_t access_size_in_bytes);

// Emit TSAN write instrumentation before a memory store.
// access_size_in_bytes must match the width of the emitted memory access.
void emitTsanWrite(
    Environ& env,
    const jit::lir::OperandBase* mem_operand,
    size_t access_size_in_bytes);

// Try to emit a replacement TSAN atomic read for relaxed atomic loads.
// access_size_in_bytes must match the memory width of the original LIR move.
// Returns false when the caller should emit the move.
bool tryEmitTsanRelaxedAtomicRead(
    Environ& env,
    const jit::lir::OperandBase* output_operand,
    const jit::lir::OperandBase* mem_operand,
    size_t access_size_in_bytes);

// Try to emit a replacement TSAN atomic write for relaxed atomic stores.
// access_size_in_bytes must match the memory width of the original LIR move.
// Returns false when the caller should emit the move.
bool tryEmitTsanRelaxedAtomicWrite(
    Environ& env,
    const jit::lir::OperandBase* mem_operand,
    const jit::lir::OperandBase* value_operand,
    size_t access_size_in_bytes);

#else

inline void emitTsanRead(Environ&, const jit::lir::OperandBase*, size_t) {}

inline void emitTsanWrite(Environ&, const jit::lir::OperandBase*, size_t) {}

inline bool tryEmitTsanRelaxedAtomicRead(
    Environ&,
    const jit::lir::OperandBase*,
    const jit::lir::OperandBase*,
    size_t) {
  return false;
}

inline bool tryEmitTsanRelaxedAtomicWrite(
    Environ&,
    const jit::lir::OperandBase*,
    const jit::lir::OperandBase*,
    size_t) {
  return false;
}

#endif // CINDER_JIT_TSAN_ENABLED

} // namespace jit::codegen
