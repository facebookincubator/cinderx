// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/codegen/arch/detection.h"

#include <cstddef>
#include <cstdint>

namespace jit {

int frameHeaderSize(BorrowedRef<PyCodeObject> code);

// FrameHeader lives at the beginning of the stack frame for JIT-compiled
// functions.  This is followed by the _PyInterpreterFrame.
struct FrameHeader {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
#if defined(CINDER_AARCH64)
  // Index into the CodeRuntime's deopt metadata array. Used to recover the
  // current bytecode offset for frame introspection (e.g. sys._current_frames).
  // Updated before each instruction that can deopt. On x86-64, we use the
  // IP-based symbolizer approach instead.
  // On aarch64 deopt_idx is placed first so that func/frame_status is adjacent
  // to the _PyInterpreterFrame fields that follow, enabling consecutive stores
  // via StorePair during frame initialization.
  std::size_t deopt_idx;
#endif
  union {
    PyFunctionObject* func;
    uintptr_t frame_status;
  };
#endif
};

#ifdef ENABLE_LIGHTWEIGHT_FRAMES
inline constexpr size_t kFrameHeaderOverhead = sizeof(FrameHeader);
#else
inline constexpr size_t kFrameHeaderOverhead = 0;
#endif

#define JIT_FRAME_INLINED 0x01
#define JIT_FRAME_INITIALIZED 0x02
#define JIT_FRAME_DEOPT_PATCHED 0x04
#define JIT_FRAME_MASK 0x07

} // namespace jit
