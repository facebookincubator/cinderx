// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"

namespace jit {

int frameHeaderSize(BorrowedRef<PyCodeObject> code);

#if PY_VERSION_HEX < 0x030C0000

#include "internal/pycore_shadow_frame_struct.h"

// FrameHeader lives at the beginning of the stack frame for JIT-compiled
// functions. Note these will be garbage in generator objects.
struct FrameHeader {
  JITShadowFrame shadow_frame;
};

void assertShadowCallStackConsistent(PyThreadState* tstate);

const char* shadowFrameKind(_PyShadowFrame* sf);

#else

// FrameHeader lives at the beginning of the stack frame for JIT-compiled
// functions. In 3.12+ this will be followed by the _PyInterpreterFrame.
struct FrameHeader {
  union {
    PyFunctionObject* func;
    uintptr_t rtfs;
  };
};

#define JIT_FRAME_RTFS 0x01
#define JIT_FRAME_INITIALIZED 0x02
#define JIT_FRAME_MASK 0x03

#endif

} // namespace jit
