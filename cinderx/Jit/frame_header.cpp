// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/frame_header.h"

#include "cinderx/Common/util.h"
#include "cinderx/Jit/config.h"

namespace cinderx::jit {

int frameHeaderSize(BorrowedRef<PyCodeObject> code) {
  if (code->co_flags & kCoFlagsAnyGenerator) {
    return 0;
  }

  return kFrameHeaderOverhead + sizeof(PyObject*) * code->co_framesize;
}

} // namespace cinderx::jit
