// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/codegen/arch/detection.h"

#include <cstddef>
#include <cstdint>

namespace cinderx::jit {

int frameHeaderSize(BorrowedRef<PyCodeObject> code);

// FrameHeader lives at the beginning of the stack frame for JIT-compiled
// functions.  This is followed by the _PyInterpreterFrame.
struct FrameHeader {
#if defined(CINDER_AARCH64) || defined(Py_GIL_DISABLED)
  // Index into the CodeRuntime's deopt metadata array. Used to recover the
  // current bytecode offset for frame introspection (e.g. sys._current_frames).
  // Updated before each instruction that can deopt. In free-threaded builds,
  // this also gives GC an exact active callsite for deferred-RC root scanning.
  // On aarch64 deopt_idx is placed first so that func/frame_status is adjacent
  // to the _PyInterpreterFrame fields that follow, enabling consecutive stores
  // via StorePair during frame initialization.
  std::size_t deopt_idx;
#endif
  union {
#if PY_VERSION_HEX < 0x030E0000
    PyFunctionObject* func;
#endif
    uintptr_t frame_status;
  };
};

inline constexpr size_t kFrameHeaderOverhead = sizeof(FrameHeader);

#define JIT_FRAME_INLINED 0x01
#define JIT_FRAME_INITIALIZED 0x02
#define JIT_FRAME_DEOPT_PATCHED 0x04
#define JIT_FRAME_MASK 0x07

} // namespace cinderx::jit
