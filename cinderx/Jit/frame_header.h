// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/define.h"
#include "cinderx/Common/ref.h"

#include <cstddef>
#include <cstdint>

namespace cinderx::jit {

int frameHeaderSize(BorrowedRef<PyCodeObject> code);

// FrameHeader lives at the beginning of the stack frame for JIT-compiled
// functions.  This is followed by the _PyInterpreterFrame.
struct FrameHeader {
#if defined(CINDER_AARCH64)
  // The thread this frame is running on. AArch64 leaves the return address of
  // an active call in the callee's frame record rather than at a fixed offset
  // from the caller's frame pointer, so recovering the frame's current IP
  // means walking the native stack it lives on. See frame.cpp.
  PyThreadState* tstate;
#endif
#if defined(Py_GIL_DISABLED)
  // Index into the CodeRuntime's deopt metadata array, giving GC an exact
  // active callsite for deferred-RC root scanning. Updated before each
  // instruction that can deopt.
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
